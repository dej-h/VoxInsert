// Self-contained unit and integration tests for the Phase 1 streaming core:
// the bounded queue, the transcript assembler, and the fake streaming backend.
// No microphone or network is used. The process exits non-zero on any failure.

#include <atomic>
#include <cstdint>
#include <iostream>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "config/app_config.h"
#include "runtime/bounded_queue.h"
#include "runtime/spsc_index_ring.h"
#include "fakes/fake_streaming_transcription_service.h"
#include "transcription/streaming_transcription_service.h"
#include "transcription/transcript_assembler.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void ReportFailure(const char* expr, const char* file, int line) {
    ++g_failures;
    std::cerr << "FAIL: " << expr << " (" << file << ":" << line << ")\n";
}

#define CHECK(cond)                                       \
    do {                                                  \
        ++g_checks;                                       \
        if (!(cond)) {                                    \
            ReportFailure(#cond, __FILE__, __LINE__);     \
        }                                                 \
    } while (false)

#define CHECK_EQ(actual, expected)                                            \
    do {                                                                      \
        ++g_checks;                                                           \
        auto&& a_ = (actual);                                                 \
        auto&& e_ = (expected);                                               \
        if (!(a_ == e_)) {                                                    \
            ++g_failures;                                                     \
            std::cerr << "FAIL: " << #actual << " == " << #expected           \
                      << " (" << __FILE__ << ":" << __LINE__ << ")\n";        \
            std::cerr << "  actual  : " << a_ << "\n";                        \
            std::cerr << "  expected: " << e_ << "\n";                        \
        }                                                                     \
    } while (false)

using voxinsert::BackendTranscriptEvent;
using voxinsert::BackendTranscriptEventKind;
using voxinsert::BlockingBoundedQueue;
using voxinsert::FakeStreamingBackend;
using voxinsert::FakeStreamingScript;
using voxinsert::LongestCommonPrefixUtf8;
using voxinsert::SpscIndexRing;
using voxinsert::StreamingBackendCommand;
using voxinsert::StreamingBackendCommandKind;
using voxinsert::TranscriptAssembler;
using voxinsert::TranscriptionConfig;
using voxinsert::TranscriptPatch;
using voxinsert::TranscriptPatchKind;

// Reconstructs visible text by applying patches exactly per the documented
// consumer contract. Used to prove the patch stream alone is sufficient.
struct PatchConsumer {
    std::string stable;
    std::string unstable;
    std::string finalText;
    bool finalized = false;
    bool sawError = false;

    void Apply(const TranscriptPatch& patch) {
        switch (patch.kind) {
        case TranscriptPatchKind::AppendStableText:
            stable += patch.textUtf8;
            unstable.clear();
            break;
        case TranscriptPatchKind::ReplaceUnstableTail:
            unstable = patch.textUtf8;
            break;
        case TranscriptPatchKind::FinalizeUtterance:
            stable = patch.textUtf8;
            unstable.clear();
            finalText = patch.textUtf8;
            finalized = true;
            break;
        case TranscriptPatchKind::ResetUtterance:
            stable.clear();
            unstable.clear();
            break;
        case TranscriptPatchKind::Error:
            sawError = true;
            break;
        }
    }

    std::string Visible() const { return stable + unstable; }
};

BackendTranscriptEvent MakeDelta(const std::string& utterance, const std::string& text) {
    BackendTranscriptEvent event;
    event.kind = BackendTranscriptEventKind::AppendDelta;
    event.utteranceId = utterance;
    event.textUtf8 = text;
    return event;
}

BackendTranscriptEvent MakeSnapshot(const std::string& utterance, const std::string& text) {
    BackendTranscriptEvent event;
    event.kind = BackendTranscriptEventKind::Snapshot;
    event.utteranceId = utterance;
    event.textUtf8 = text;
    return event;
}

BackendTranscriptEvent MakeFinal(const std::string& utterance, const std::string& text) {
    BackendTranscriptEvent event;
    event.kind = BackendTranscriptEventKind::UtteranceFinalized;
    event.utteranceId = utterance;
    event.textUtf8 = text;
    return event;
}

void TestQueueBasicFifoAndCapacity() {
    BlockingBoundedQueue<int> queue(2);
    CHECK(queue.TryPush(1));
    CHECK(queue.TryPush(2));
    CHECK(!queue.TryPush(3)); // Full.
    CHECK_EQ(queue.Size(), static_cast<size_t>(2));

    std::stop_source source;
    int value = 0;
    CHECK(queue.WaitPop(value, source.get_token()));
    CHECK_EQ(value, 1);
    CHECK(queue.WaitPop(value, source.get_token()));
    CHECK_EQ(value, 2);

    // Now there is room again.
    CHECK(queue.TryPush(3));
    CHECK(queue.WaitPop(value, source.get_token()));
    CHECK_EQ(value, 3);
}

void TestQueueCloseWakesWaiter() {
    BlockingBoundedQueue<int> queue(4);
    std::stop_source source;
    std::atomic<bool> popReturned{false};
    bool popResult = true;

    std::thread worker([&] {
        int value = 0;
        popResult = queue.WaitPop(value, source.get_token());
        popReturned.store(true);
    });

    queue.Close();
    worker.join();
    CHECK(popReturned.load());
    CHECK(!popResult); // Closed + empty -> false.
}

void TestQueueStopTokenWakesWaiter() {
    BlockingBoundedQueue<int> queue(4);
    std::stop_source source;
    bool popResult = true;

    std::thread worker([&] {
        int value = 0;
        popResult = queue.WaitPop(value, source.get_token());
    });

    source.request_stop();
    worker.join();
    CHECK(!popResult); // Stop requested -> false.
}

void TestQueueCrossThreadOrdering() {
    BlockingBoundedQueue<int> queue(8);
    std::stop_source source;
    std::vector<int> received;

    std::thread consumer([&] {
        int value = 0;
        while (queue.WaitPop(value, source.get_token())) {
            received.push_back(value);
        }
    });

    constexpr int kCount = 500;
    for (int i = 0; i < kCount; ++i) {
        while (!queue.TryPush(i)) {
            std::this_thread::yield();
        }
    }
    queue.Close();
    consumer.join();

    CHECK_EQ(received.size(), static_cast<size_t>(kCount));
    bool ordered = true;
    for (int i = 0; i < kCount && i < static_cast<int>(received.size()); ++i) {
        if (received[i] != i) {
            ordered = false;
            break;
        }
    }
    CHECK(ordered);
}

void TestQueueResetReopensForReuse() {
    // Reproduces the "only the first recording streams" bug: the controller
    // reuses one queue across sessions, and Finish() leaves it Closed. Reset()
    // must re-open and drain it so the next session can push/pop again.
    BlockingBoundedQueue<int> queue(4);

    CHECK(queue.TryPush(1));
    CHECK(queue.TryPush(2));
    queue.Close();

    // While closed, producers are rejected (this is the "queue full; dropping
    // backend events" / "append failed" symptom seen on the 2nd recording).
    CHECK(!queue.TryPush(3));
    CHECK(queue.IsClosed());

    queue.Reset();

    // After Reset the leftover items are discarded and the queue accepts work.
    CHECK(!queue.IsClosed());
    CHECK_EQ(queue.Size(), static_cast<size_t>(0));
    CHECK(queue.TryPush(10));
    CHECK(queue.TryPush(11));
    std::stop_source freshSource;
    int popped = 0;
    CHECK(queue.WaitPop(popped, freshSource.get_token()));
    CHECK_EQ(popped, 10);
    CHECK(queue.WaitPop(popped, freshSource.get_token()));
    CHECK_EQ(popped, 11);
}

void TestSpscIndexRingBasicFifoAndCapacity() {
    SpscIndexRing ring(2);

    CHECK_EQ(ring.Capacity(), static_cast<size_t>(2));
    CHECK(ring.TryPush(10));
    CHECK(ring.TryPush(11));
    CHECK(!ring.TryPush(12));
    CHECK_EQ(ring.Size(), static_cast<size_t>(2));

    size_t value = 0;
    CHECK(ring.TryPop(value));
    CHECK_EQ(value, static_cast<size_t>(10));
    CHECK(ring.TryPop(value));
    CHECK_EQ(value, static_cast<size_t>(11));
    CHECK(!ring.TryPop(value));
}

void TestSpscIndexRingResetAndReuse() {
    SpscIndexRing ring(3);
    CHECK(ring.TryPush(1));
    CHECK(ring.TryPush(2));
    ring.Reset();

    CHECK_EQ(ring.Size(), static_cast<size_t>(0));
    CHECK(ring.TryPush(20));
    CHECK(ring.TryPush(21));

    size_t value = 0;
    CHECK(ring.TryPop(value));
    CHECK_EQ(value, static_cast<size_t>(20));
    CHECK(ring.TryPop(value));
    CHECK_EQ(value, static_cast<size_t>(21));
}

void TestSpscIndexRingCrossThreadOrdering() {
    SpscIndexRing ring(8);
    std::atomic<bool> producerDone{false};
    std::vector<size_t> received;

    std::thread consumer([&] {
        while (!producerDone.load() || ring.Size() != 0) {
            size_t value = 0;
            if (ring.TryPop(value)) {
                received.push_back(value);
            }
            else {
                std::this_thread::yield();
            }
        }
    });

    constexpr size_t kCount = 500;
    for (size_t index = 0; index < kCount; ++index) {
        while (!ring.TryPush(index)) {
            std::this_thread::yield();
        }
    }
    producerDone.store(true);
    consumer.join();

    CHECK_EQ(received.size(), kCount);
    bool ordered = true;
    for (size_t index = 0; index < kCount && index < received.size(); ++index) {
        if (received[index] != index) {
            ordered = false;
            break;
        }
    }
    CHECK(ordered);
}

void TestLongestCommonPrefixUtf8() {
    CHECK_EQ(LongestCommonPrefixUtf8("hello", "help"), static_cast<size_t>(3));
    CHECK_EQ(LongestCommonPrefixUtf8("", "abc"), static_cast<size_t>(0));
    CHECK_EQ(LongestCommonPrefixUtf8("abc", "abc"), static_cast<size_t>(3));

    // "café" = 63 61 66 C3 A9. A divergence inside the 2-byte é must back off to
    // the boundary before the multi-byte sequence (length 3).
    const std::string cafe = "caf\xC3\xA9";
    const std::string cafa = "caf\xC3\xA8"; // è shares the lead byte but differs.
    CHECK_EQ(LongestCommonPrefixUtf8(cafe, cafa), static_cast<size_t>(3));

    // Identical multi-byte sequences keep the full prefix.
    CHECK_EQ(LongestCommonPrefixUtf8(cafe, cafe + "x"), cafe.size());
}

void TestAssemblerAppendOnly() {
    TranscriptAssembler assembler;
    PatchConsumer consumer;
    const auto sink = [&](const TranscriptPatch& patch) { consumer.Apply(patch); };

    assembler.Apply(MakeDelta("u1", "Hello"), sink);
    assembler.Apply(MakeDelta("u1", ""), sink);       // Suppressed.
    assembler.Apply(MakeDelta("u1", " world"), sink);
    CHECK_EQ(consumer.Visible(), std::string("Hello world"));
    CHECK_EQ(assembler.VisibleTextFor("u1"), std::string("Hello world"));
    CHECK(!assembler.IsFinalized("u1"));

    assembler.Apply(MakeFinal("u1", "Hello world."), sink);
    CHECK(consumer.finalized);
    CHECK_EQ(consumer.finalText, std::string("Hello world."));
    CHECK(assembler.IsFinalized("u1"));
    CHECK_EQ(assembler.FinalTranscriptUtf8(), std::string("Hello world."));

    // Late deltas after finalization are ignored.
    assembler.Apply(MakeDelta("u1", " extra"), sink);
    CHECK_EQ(assembler.VisibleTextFor("u1"), std::string("Hello world."));
}

void TestAssemblerAppendOnlyFinalizesFromAccumulator() {
    TranscriptAssembler assembler;
    assembler.Apply(MakeDelta("u1", "one "));
    assembler.Apply(MakeDelta("u1", "two"));
    // Finalize with empty text -> use accumulated stable + unstable.
    assembler.Apply(MakeFinal("u1", ""));
    CHECK(assembler.IsFinalized("u1"));
    CHECK_EQ(assembler.VisibleTextFor("u1"), std::string("one two"));
}

void TestAssemblerRevisingSnapshots() {
    TranscriptAssembler assembler;
    PatchConsumer consumer;
    const auto sink = [&](const TranscriptPatch& patch) { consumer.Apply(patch); };

    // First snapshot: everything is an unstable hypothesis.
    assembler.Apply(MakeSnapshot("u1", "the quick"), sink);
    CHECK_EQ(consumer.stable, std::string(""));
    CHECK_EQ(consumer.Visible(), std::string("the quick"));

    // Second snapshot revises the tail; the common prefix "the qui" stays.
    assembler.Apply(MakeSnapshot("u1", "the quint"), sink);
    CHECK_EQ(consumer.Visible(), std::string("the quint"));
    // "the qui" appeared unchanged across two snapshots -> promoted to stable.
    CHECK_EQ(consumer.stable, std::string("the qui"));
    CHECK_EQ(consumer.unstable, std::string("nt"));

    // Third snapshot extends; "the quint" now stable across two snapshots.
    assembler.Apply(MakeSnapshot("u1", "the quint essence"), sink);
    CHECK_EQ(consumer.Visible(), std::string("the quint essence"));
    CHECK_EQ(consumer.stable, std::string("the quint"));
    CHECK_EQ(consumer.unstable, std::string(" essence"));

    assembler.Apply(MakeFinal("u1", "the quintessence"), sink);
    CHECK(consumer.finalized);
    CHECK_EQ(assembler.FinalTranscriptUtf8(), std::string("the quintessence"));
}

void TestAssemblerPreviewTextSeparatesStableAndUnstable() {
    TranscriptAssembler assembler;

    assembler.Apply(MakeSnapshot("u1", "the quick"));
    auto preview = assembler.PreviewText();
    CHECK_EQ(preview.stableUtf8, std::string(""));
    CHECK_EQ(preview.unstableUtf8, std::string("the quick"));
    CHECK(!preview.finalized);

    assembler.Apply(MakeSnapshot("u1", "the quick brown"));
    preview = assembler.PreviewText();
    CHECK_EQ(preview.stableUtf8, std::string("the quick"));
    CHECK_EQ(preview.unstableUtf8, std::string(" brown"));
    CHECK(!preview.finalized);

    assembler.Apply(MakeFinal("u1", "the quick brown fox"));
    preview = assembler.PreviewText();
    CHECK_EQ(preview.stableUtf8, std::string("the quick brown fox"));
    CHECK_EQ(preview.unstableUtf8, std::string(""));
    CHECK(preview.finalized);
}

void TestAssemblerRevisingSnapshotsUtf8Safe() {
    TranscriptAssembler assembler;
    PatchConsumer consumer;
    const auto sink = [&](const TranscriptPatch& patch) { consumer.Apply(patch); };

    const std::string cafe = "ca\xC3\xA9"; // "caé"
    const std::string cafo = "ca\xC3\xB6"; // "caö"

    assembler.Apply(MakeSnapshot("u1", cafe), sink);
    assembler.Apply(MakeSnapshot("u1", cafo), sink);
    CHECK_EQ(consumer.Visible(), cafo);
    // The differing 2-byte char must not be split: stable cannot end mid-codepoint.
    CHECK_EQ(consumer.stable, std::string("ca"));
}

void TestAssemblerMultipleUtterances() {
    TranscriptAssembler assembler;
    assembler.Apply(MakeDelta("u1", "first"));
    assembler.Apply(MakeFinal("u1", "first."));
    assembler.Apply(MakeDelta("u2", "second"));
    assembler.Apply(MakeFinal("u2", "second."));
    CHECK_EQ(assembler.FinalTranscriptUtf8(), std::string("first. second."));
}

// Golden trace mirroring the real OpenAI realtime transcription wire protocol:
// append-only deltas keyed by item_id
// (conversation.item.input_audio_transcription.delta) followed by a completed
// event (.completed) carrying the full transcript. The file-streaming variant
// (transcript.text.delta / transcript.text.done) maps onto the same neutral
// AppendDelta / UtteranceFinalized events, so this also covers that backend.
void TestAssemblerOpenAiRealtimeGoldenTrace() {
    TranscriptAssembler assembler;
    const std::string itemId = "item_ABC123";

    PatchConsumer visible;
    auto sink = [&](const voxinsert::TranscriptPatch& patch) { visible.Apply(patch); };

    assembler.Apply(MakeDelta(itemId, "Insert "), sink);
    assembler.Apply(MakeDelta(itemId, "the "), sink);
    assembler.Apply(MakeDelta(itemId, "streaming "), sink);
    assembler.Apply(MakeDelta(itemId, "transcript."), sink);

    // Pre-final visible text is the concatenation of the deltas.
    CHECK_EQ(visible.Visible(), std::string("Insert the streaming transcript."));
    CHECK_EQ(assembler.VisibleTextFor(itemId), std::string("Insert the streaming transcript."));

    assembler.Apply(MakeFinal(itemId, "Insert the streaming transcript."), sink);
    CHECK(assembler.IsFinalized(itemId));
    CHECK_EQ(assembler.FinalTranscriptUtf8(), std::string("Insert the streaming transcript."));
}

// The .completed transcript is authoritative: when the running delta sum does
// not exactly match the final text, the completed transcript wins.
void TestAssemblerCompletedOverridesDeltas() {
    TranscriptAssembler assembler;
    const std::string itemId = "item_X";
    assembler.Apply(MakeDelta(itemId, "helo wrld"));
    assembler.Apply(MakeFinal(itemId, "Hello, world!"));
    CHECK_EQ(assembler.FinalTranscriptUtf8(), std::string("Hello, world!"));
}

// Drives the fake backend the way the coordinator will: feed audio chunks via
// AppendAudio, route backend events into the assembler, then commit.
std::string RunFakePipeline(
    FakeStreamingBackend& backend,
    const std::string& utterance,
    int audioChunks,
    bool& startOk,
    std::wstring& failureReason) {
    TranscriptAssembler assembler;
    TranscriptionConfig config;

    std::wstring createFailure;
    auto session = backend.CreateSession(
        config,
        "session-1",
        [&](BackendTranscriptEvent event) { assembler.Apply(event); },
        nullptr,
        createFailure);

    startOk = session->Start(failureReason);
    if (!startOk) {
        return {};
    }

    for (int i = 0; i < audioChunks; ++i) {
        StreamingBackendCommand command;
        command.kind = StreamingBackendCommandKind::AppendAudio;
        command.sessionId = "session-1";
        command.utteranceId = utterance;
        command.firstSequence = static_cast<uint64_t>(i);
        command.lastSequence = static_cast<uint64_t>(i);
        std::vector<int16_t> pcm16(256, static_cast<int16_t>(i));
        command.pcm16 = pcm16;
        std::wstring appendFailure;
        if (!session->AppendAudio(command, appendFailure)) {
            failureReason = appendFailure;
        }
    }

    std::wstring commitFailure;
    if (!session->CommitUtterance(utterance, commitFailure)) {
        failureReason = commitFailure;
    }
    session->Close();
    return assembler.FinalTranscriptUtf8();
}

void TestFakeBackendFinalOnCommit() {
    FakeStreamingScript script;
    script.capabilities.appendOnlyDeltas = false;
    script.eventsOnCommit.push_back(MakeFinal("u1", "transcribed on commit"));

    FakeStreamingBackend backend(std::move(script));
    bool startOk = false;
    std::wstring failure;
    const std::string transcript = RunFakePipeline(backend, "u1", 5, startOk, failure);

    CHECK(startOk);
    CHECK_EQ(transcript, std::string("transcribed on commit"));

    const auto& stats = backend.Stats();
    CHECK_EQ(stats->sessionsCreated.load(), 1);
    CHECK_EQ(stats->startCalls.load(), 1);
    CHECK_EQ(stats->appendCalls.load(), 5);
    CHECK_EQ(stats->commitCalls.load(), 1);
    CHECK_EQ(stats->closeCalls.load(), 1);
    CHECK_EQ(stats->appendedSamples.load(), static_cast<uint64_t>(5 * 256));
    CHECK(!stats->rejectedEmptyAppend.load());
}

void TestFakeBackendLivePartials() {
    FakeStreamingScript script;
    script.capabilities.emitsPartialsBeforeCommit = true;
    script.capabilities.appendOnlyDeltas = true;
    script.eventsDuringAppend.push_back(MakeDelta("u1", "live "));
    script.eventsDuringAppend.push_back(MakeDelta("u1", "partial "));
    script.eventsDuringAppend.push_back(MakeDelta("u1", "text"));
    script.eventsOnCommit.push_back(MakeFinal("u1", "live partial text"));

    FakeStreamingBackend backend(std::move(script));
    bool startOk = false;
    std::wstring failure;
    const std::string transcript = RunFakePipeline(backend, "u1", 3, startOk, failure);

    CHECK(startOk);
    CHECK_EQ(transcript, std::string("live partial text"));
    CHECK_EQ(backend.Stats()->appendCalls.load(), 3);
}

void TestFakeBackendStartFailure() {
    FakeStreamingScript script;
    script.failOnStart = true;
    script.startFailureReason = L"no backend";

    FakeStreamingBackend backend(std::move(script));
    bool startOk = true;
    std::wstring failure;
    RunFakePipeline(backend, "u1", 2, startOk, failure);

    CHECK(!startOk);
    CHECK(failure == std::wstring(L"no backend"));
    // No audio should have been appended once start failed.
    CHECK_EQ(backend.Stats()->appendCalls.load(), 0);
}

void TestFakeBackendRejectsEmptyAppend() {
    FakeStreamingScript script;
    FakeStreamingBackend backend(std::move(script));
    TranscriptionConfig config;
    std::wstring createFailure;
    auto session = backend.CreateSession(config, "s", [](BackendTranscriptEvent) {}, nullptr, createFailure);
    std::wstring failure;
    CHECK(session->Start(failure));

    StreamingBackendCommand empty;
    empty.kind = StreamingBackendCommandKind::AppendAudio;
    empty.utteranceId = "u1";
    // pcm16 intentionally empty.
    CHECK(!session->AppendAudio(empty, failure));
    CHECK(backend.Stats()->rejectedEmptyAppend.load());
}

} // namespace

int main() {
    TestQueueBasicFifoAndCapacity();
    TestQueueCloseWakesWaiter();
    TestQueueStopTokenWakesWaiter();
    TestQueueCrossThreadOrdering();
    TestQueueResetReopensForReuse();
    TestSpscIndexRingBasicFifoAndCapacity();
    TestSpscIndexRingResetAndReuse();
    TestSpscIndexRingCrossThreadOrdering();

    TestLongestCommonPrefixUtf8();

    TestAssemblerAppendOnly();
    TestAssemblerAppendOnlyFinalizesFromAccumulator();
    TestAssemblerRevisingSnapshots();
    TestAssemblerPreviewTextSeparatesStableAndUnstable();
    TestAssemblerRevisingSnapshotsUtf8Safe();
    TestAssemblerMultipleUtterances();
    TestAssemblerOpenAiRealtimeGoldenTrace();
    TestAssemblerCompletedOverridesDeltas();

    TestFakeBackendFinalOnCommit();
    TestFakeBackendLivePartials();
    TestFakeBackendStartFailure();
    TestFakeBackendRejectsEmptyAppend();

    std::cout << "Ran " << g_checks << " checks, " << g_failures << " failure(s).\n";
    return g_failures == 0 ? 0 : 1;
}
