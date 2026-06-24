# Streaming Transcription Implementation Plan

## Goal

Move VoxInsert from a strictly finished-file transcription flow to a streaming-ready flow where microphone audio is sent to a transcription backend while the user is still speaking, transcript deltas are assembled into stable final text, and the existing final WAV, transcript persistence, insertion, and latency logging behavior remains intact.

The user-facing target is:

1. Start recording.
2. Capture PCM locally exactly as we do today.
3. Stream PCM chunks to a backend during recording.
4. Receive backend transcript events during recording when the backend supports it.
5. Convert backend events into provider-neutral transcript patches.
6. Keep a live assembled transcript in memory.
7. On stop, finalize the stream.
8. Persist and log the final audio and final transcript through the same paths used today.
9. Insert the final text using the existing insertion behavior unless a later feature explicitly opts into live insertion.

The main latency win is that by the time the user stops recording, most or all of the transcription work has already happened for backends that emit live partials or for sessions that use periodic commits. The final stop path should mostly wait for stream finalization, final transcript assembly, WAV persistence, archive enqueue, and final insertion.

## Latency Model

There are three different latency modes, and the implementation must measure them separately:

1. True live partial mode.
    The backend emits transcript events while audio is still being appended. This is the target experience because the assembled transcript can be nearly complete before stop.

2. Append-before-commit mode.
    The backend accepts audio while recording but only starts returning transcript text after a commit. This still removes end-of-recording upload time, but it does not guarantee the text is ready when the user stops.

3. Periodic commit mode.
    VoxInsert commits bounded fragments during a longer recording, then assembles finalized fragments and manages overlap. This can make final text mostly ready before stop even when the backend needs commit events, but it adds stitching complexity.

The first implementation should support all three at the interface level. The safest production rollout is commit-on-stop first, then true live partial support when the selected backend provides it, then periodic commit after the assembler and fallback behavior are proven.

## Product Decision

The first implementation should stream into VoxInsert-owned state and optionally into a VoxInsert UI preview. It should not rewrite arbitrary focused text boxes live.

Reasons:

- The current insertion path is clipboard plus Ctrl+V.
- Arbitrary Windows text controls do not expose one universal safe replace-range operation.
- Many transcription backends revise their latest words.
- Streaming final text is valuable even before live target-app insertion exists.

Live insertion can be designed later as a separate text-control layer. The streaming transcription pipeline should not depend on that layer.

## Current Behavior To Preserve

The current post-recording workflow in `src/runtime/post_recording_workflow.cpp` establishes behavior that must remain available:

1. Stop the recorder.
2. Receive the full PCM sample vector.
3. Write a WAV file.
4. Store the last recording path.
5. Upload the WAV through `TranscriptionClient`.
6. Store the last transcript.
7. Enqueue archive persistence when enabled.
8. Insert the transcript with `TextInjector`.
9. Log stage timings for stop, WAV write, transcription, insertion, and total time.

Streaming changes where transcription happens, but not the meaning of the final result. The final archived transcript should be the finalized transcript assembled from the stream. The final archived audio should be the same local PCM captured from the microphone, written as a normal WAV file.

## Non-Goals For The First Streaming Pass

- Do not remove the existing finished-file transcription path.
- Do not make live target-app insertion the default.
- Do not send network traffic from the audio capture thread.
- Do not block the hidden window message loop while waiting for audio, network, archive, or insertion work.
- Do not let provider-specific event formats leak into runtime, UI, archive, or insertion code.
- Do not treat partial text as final text until the backend or assembler finalizes it.
- Do not rely on empty audio commits. Empty chunks and empty utterances must be suppressed locally.

## End-State Architecture

The target architecture has these modules:

```text
AppHost UI thread
  -> StreamingRecordingController
      -> AudioCaptureSession
          -> AudioChunkQueue
      -> StreamingTranscriptionCoordinator
          -> ChunkBatcher
          -> CommitStrategy
          -> IStreamingTranscriptionBackend
          -> BackendEventQueue
          -> TranscriptAssembler
          -> TranscriptConsumers
      -> FinalizationBridge
          -> WavWriter
          -> ArchiveService
          -> TextInjector
```

The current `RunPostRecordingWorkflow` remains as the fallback and non-streaming path. The streaming path should be introduced beside it rather than by making the existing synchronous workflow handle every new case.

## Thread Model

### UI Thread

The UI thread owns:

- the hidden host window
- tray menu handling
- hotkey message handling
- app state transitions
- preview window updates, if enabled
- final completion messages

The UI thread must never wait for:

- PortAudio reads
- WebSocket writes
- WebSocket reads
- transcription backend finalization
- WAV writing
- archive work
- clipboard insertion

All cross-thread UI updates must use posted window messages with copied or heap-owned immutable payloads.

### Audio Capture Thread

The audio capture thread owns only the low-level microphone read loop and immediate local capture state.

It may:

- read PCM from PortAudio
- append samples to the retained local PCM vector
- calculate amplitude for the status pill
- create `CapturedAudioChunk` values
- attempt a non-blocking push into the audio chunk queue
- set an atomic streaming backpressure flag if the queue is full

It must not:

- call HTTP or WebSocket APIs
- call transcription code directly
- call archive code
- call clipboard or SendInput APIs
- block indefinitely waiting for the streaming worker
- call into UI code other than the existing post-style amplitude path

### Streaming Coordinator Thread

The streaming coordinator thread owns the streaming session state machine.

It:

- consumes audio chunks from the queue
- applies local gating and batching
- sends append commands to the backend session
- applies commit strategy decisions
- consumes backend transcript events
- feeds the transcript assembler
- emits transcript patches to consumers
- records per-utterance metrics
- produces the final transcript bundle

This thread can block on condition variables and network-layer waits, but it must honor cancellation through `std::stop_token`.

### Backend I/O Thread

Some WebSocket libraries own their own I/O thread or require an event loop. If the selected WebSocket transport needs that, the backend adapter owns that thread behind RAII.

Backend callbacks must not mutate the assembler directly. They should enqueue `BackendTranscriptEvent` values into the coordinator-owned event queue.

### Archive Worker

The existing archive worker remains responsible for archive persistence. The streaming path should enqueue an `ArchiveRequest` with the same final audio and transcript semantics as the existing workflow.

## C++ Safety Rules

Use these rules throughout the implementation:

1. Use `std::jthread` for new worker threads so cancellation and joining are tied to object lifetime.
2. Use `std::stop_token` in every long-running loop.
3. Do not detach threads.
4. Do not store `std::string_view` or `std::wstring_view` across async boundaries.
5. Move owning values through queues. A queued message owns its strings and vectors.
6. Use `std::unique_ptr` for session ownership and RAII wrappers for native handles.
7. Do not call callbacks while holding locks.
8. Keep mutex-protected state small and copy snapshots out before invoking consumers.
9. Use atomics only for simple flags and counters. Use a mutex for compound state.
10. Make queue shutdown explicit so worker threads can unblock during app shutdown.
11. Treat backend callbacks as untrusted timing-wise. They can arrive late, after cancel, or after a newer session starts.
12. Attach a `sessionId` to every cross-thread message and ignore stale messages on the UI thread.

## Core Data Model

### Identifiers

Every streaming run needs stable correlation IDs:

```cpp
using StreamingSessionId = std::string;
using UtteranceId = std::string;
using BackendItemId = std::string;
```

Use string IDs initially because provider item IDs are strings and logs become easier to read. Generate local IDs with a monotonic counter plus timestamp or a UUID helper.

### Audio Format

Keep VoxInsert's internal capture format stable:

```cpp
struct PcmAudioFormat {
    int sampleRate = 16000;
    int channelCount = 1;
    int bitsPerSample = 16;
};
```

Provider adapters handle conversion. For example, OpenAI realtime currently wants 24 kHz mono PCM for `audio/pcm`, while the current recorder captures 16 kHz mono PCM.

### Captured Audio Chunk

The audio producer emits chunks in capture order:

```cpp
struct CapturedAudioChunk {
    StreamingSessionId sessionId;
    UtteranceId utteranceId;
    uint64_t sequence = 0;
    PcmAudioFormat format;
    int64_t startOffsetMs = 0;
    int64_t durationMs = 0;
    float rms = 0.0f;
    bool inputOverflowObserved = false;
    std::vector<int16_t> pcm16;
};
```

Rules:

- `pcm16` must never be empty when sent to a backend.
- Chunks are immutable after they enter a queue.
- Chunks are ordered by `sequence` within an utterance.
- The audio thread may create chunks at the PortAudio read cadence, but the coordinator may batch them into larger backend sends.

### Retained Audio Buffer

The streaming path still needs the full PCM buffer:

```cpp
struct RetainedRecordingAudio {
    PcmAudioFormat format;
    std::vector<int16_t> samples;
    bool streamingBackpressureObserved = false;
    bool inputOverflowObserved = false;
};
```

This buffer is the source of truth for WAV writing and finished-file fallback. Even if streaming fails, local capture should still produce a normal WAV and can still use the current finished-file transcription path.

### Backend Commands

The coordinator sends commands to a backend session:

```cpp
enum class StreamingBackendCommandKind {
    AppendAudio,
    CommitUtterance,
    CancelUtterance,
    CloseSession
};

struct StreamingBackendCommand {
    StreamingBackendCommandKind kind;
    StreamingSessionId sessionId;
    UtteranceId utteranceId;
    uint64_t firstSequence = 0;
    uint64_t lastSequence = 0;
    PcmAudioFormat format;
    int64_t startOffsetMs = 0;
    int64_t durationMs = 0;
    std::vector<int16_t> pcm16;
};
```

The command type is internal to the coordinator/backend boundary. UI and persistence code should never see it.

### Backend Events

Backend adapters normalize provider events into one event model:

```cpp
enum class BackendTranscriptEventKind {
    AppendDelta,
    Snapshot,
    SegmentFinalized,
    UtteranceFinalized,
    SessionReady,
    SessionClosed,
    Failed
};

struct BackendTranscriptEvent {
    BackendTranscriptEventKind kind;
    StreamingSessionId sessionId;
    UtteranceId utteranceId;
    BackendItemId backendItemId;
    uint64_t revision = 0;
    std::string textUtf8;
    std::optional<int64_t> startMs;
    std::optional<int64_t> endMs;
    std::wstring failureReason;
};
```

Provider-specific JSON, WebSocket message names, item IDs, and ordering quirks stop at the backend adapter.

### Transcript Patches

The assembler emits patches that describe changes to VoxInsert's assembled transcript:

```cpp
enum class TranscriptPatchKind {
    AppendStableText,
    ReplaceUnstableTail,
    FinalizeUtterance,
    ResetUtterance,
    Error
};

struct TranscriptPatch {
    TranscriptPatchKind kind;
    StreamingSessionId sessionId;
    UtteranceId utteranceId;
    uint64_t revision = 0;
    size_t replaceStartUtf8 = 0;
    size_t replaceEndUtf8 = 0;
    std::string textUtf8;
    bool isFinal = false;
    float stability = 0.0f;
    std::wstring failureReason;
};
```

Patches are stored in memory for debugging and test replay. The final transcript is derived from assembled state, not from concatenating logs after the fact.

### Assembled Transcript State

```cpp
struct TranscriptAssemblyState {
    StreamingSessionId sessionId;
    UtteranceId utteranceId;
    std::string stablePrefixUtf8;
    std::string unstableTailUtf8;
    std::string finalTextUtf8;
    uint64_t revision = 0;
    bool finalized = false;
};
```

The invariant is:

```text
visible transcript = stablePrefixUtf8 + unstableTailUtf8
final transcript = finalTextUtf8 after finalized == true
```

For append-only providers, `unstableTailUtf8` can remain empty most of the time. For revising providers, the unstable tail is the part that may be replaced.

### Final Result Bundle

When streaming stops, the coordinator returns one final bundle:

```cpp
enum class StreamingWorkflowStatus {
    Succeeded,
    Cancelled,
    Failed,
    FallbackRequired
};

struct StreamingWorkflowResult {
    StreamingWorkflowStatus status = StreamingWorkflowStatus::Failed;
    RetainedRecordingAudio audio;
    std::string transcriptUtf8;
    std::vector<TranscriptPatch> patchTrace;
    bool transcriptFinalizedByStreaming = false;
    bool fallbackUsed = false;
    std::wstring errorTitle;
    std::wstring failureReason;
};
```

The bridge from streaming back into current runtime behavior writes the WAV, stores the last transcript, enqueues archive work, and inserts final text.

## Interface Design

### Audio Capture Interface

Add a streaming-capable capture session rather than overloading `AudioRecorder::Stop` with callbacks that perform backend work.

```cpp
class IAudioChunkSink {
public:
    virtual ~IAudioChunkSink() = default;
    virtual void OnAudioChunk(CapturedAudioChunk chunk) noexcept = 0;
    virtual void OnAudioCaptureWarning(std::wstring warning) noexcept = 0;
};
```

`AudioRecorder` can grow a new method:

```cpp
bool StartStreaming(
    const AudioConfig& config,
    StreamingSessionId sessionId,
    UtteranceId utteranceId,
    AmplitudeCallback amplitudeCallback,
    IAudioChunkSink& chunkSink,
    std::wstring& failureReason);
```

The sink implementation should only enqueue chunks. It should not call backend code inline.

Stop still returns retained samples:

```cpp
bool Stop(RetainedRecordingAudio& audio, std::wstring& failureReason);
```

If changing the existing `Stop(std::vector<int16_t>&, ...)` is too disruptive, add a new overload and keep the old one as a wrapper.

### Streaming Backend Interfaces

The existing `ITranscriptionService` remains the finished-file interface.

Add a new streaming interface:

```cpp
struct StreamingBackendCapabilities {
    bool emitsPartialsBeforeCommit = false;
    bool appendOnlyDeltas = false;
    bool revisingSnapshots = false;
    bool supportsManualCommit = true;
    bool supportsServerTurnDetection = false;
    int requiredSampleRate = 16000;
    int preferredAppendBatchMs = 80;
};

class IStreamingTranscriptionSession {
public:
    virtual ~IStreamingTranscriptionSession() = default;

    virtual bool Start(std::wstring& failureReason) = 0;
    virtual bool AppendAudio(const StreamingBackendCommand& command, std::wstring& failureReason) = 0;
    virtual bool CommitUtterance(const UtteranceId& utteranceId, std::wstring& failureReason) = 0;
    virtual void CancelUtterance(const UtteranceId& utteranceId) noexcept = 0;
    virtual void Close() noexcept = 0;
};

class IStreamingTranscriptionBackend {
public:
    virtual ~IStreamingTranscriptionBackend() = default;

    virtual std::string_view BackendId() const noexcept = 0;
    virtual StreamingBackendCapabilities Capabilities(const TranscriptionConfig& config) const = 0;

    virtual std::unique_ptr<IStreamingTranscriptionSession> CreateSession(
        const TranscriptionConfig& config,
        StreamingSessionId sessionId,
        std::function<void(BackendTranscriptEvent)> onEvent,
        std::shared_ptr<spdlog::logger> logger,
        std::wstring& failureReason) const = 0;
};
```

Important details:

- `onEvent` receives owned values.
- The backend must never call `onEvent` while holding locks that may be needed by `Close`.
- `Close` must be safe to call during shutdown even if the network is already broken.
- `Capabilities` tells the coordinator whether it should expect append-only deltas, revising snapshots, or final-only behavior.

### Streaming Transcription Client

Add `StreamingTranscriptionClient` beside `TranscriptionClient`.

Responsibilities:

- resolve configured streaming provider
- create backend sessions
- expose fallback availability
- keep provider selection out of runtime code

It should not own long-running recording state. That belongs to the coordinator.

### Coordinator Interface

The runtime should talk to one coordinator object:

```cpp
struct StreamingRecordingRequest {
    AudioRecorder* audioRecorder = nullptr;
    WavWriter* wavWriter = nullptr;
    const AudioConfig* audioConfig = nullptr;
    const TranscriptionConfig* transcriptionConfig = nullptr;
    const InsertionConfig* insertionConfig = nullptr;
    TextInjector* textInjector = nullptr;
    ArchiveService* archiveService = nullptr;
    const ArchiveConfig* archiveConfig = nullptr;
    StreamingTranscriptionClient* streamingClient = nullptr;
    TranscriptionClient* fallbackClient = nullptr;
    HWND ownerWindow = nullptr;
    std::shared_ptr<spdlog::logger> logger;
    std::function<bool()> isShutdownRequested;
    std::function<void(TranscriptPatch)> onTranscriptPatch;
    std::function<void(AppState)> onPhaseChanged;
};

class StreamingRecordingController {
public:
    bool Start(const StreamingRecordingRequest& request, std::wstring& failureReason);
    void StopAsync();
    void Cancel() noexcept;
    bool IsActive() const noexcept;
};
```

The controller owns a worker `std::jthread` and posts completion back to the app host. Start should return quickly after capture and backend session startup have succeeded or failed.

### Transcript Consumers

Consumers receive provider-neutral patches:

```cpp
class ITranscriptPatchConsumer {
public:
    virtual ~ITranscriptPatchConsumer() = default;
    virtual void OnTranscriptPatch(TranscriptPatch patch) noexcept = 0;
};
```

Initial consumers:

- preview UI consumer, optional
- in-memory final transcript consumer, required
- debug trace consumer, required in debug logs or tests

Final insertion is not a patch consumer in the first implementation. It receives only the finalized transcript.

## Producer And Consumer Queues

### Queue Type

Add a small blocking bounded queue utility under runtime or a shared utility folder:

```cpp
template <typename T>
class BlockingBoundedQueue {
public:
    explicit BlockingBoundedQueue(size_t capacity);
    bool TryPush(T value);
    bool WaitPop(T& value, std::stop_token stopToken);
    void Close() noexcept;
};
```

Rules:

- Audio producer uses `TryPush` only.
- Coordinator can use `WaitPop`.
- Backend callbacks use `TryPush` with a larger event queue because transcript events are small.
- `Close` wakes waiters.
- The queue stores values, not references.

### Audio Queue Backpressure

Audio is special because losing local audio is unacceptable, but losing live streaming is recoverable.

Policy:

1. The audio thread always retains PCM locally.
2. The audio thread attempts to enqueue the chunk for streaming.
3. If enqueue fails, mark `streamingBackpressureObserved = true`.
4. Do not block the audio thread waiting for queue space.
5. The coordinator logs the backpressure and switches the utterance to fallback-required mode.
6. Final stop uses the retained PCM and the finished-file backend if the streaming transcript cannot be trusted.

### Backend Event Queue Backpressure

Transcript events are much smaller than audio chunks. Use a larger queue. If it fills, the session should fail closed and fall back rather than silently dropping transcript events.

## Audio Capture And Chunking Plan

The current recorder reads up to 256 frames at 16 kHz, about 16 ms. Keep that capture quantum.

Chunking policy:

- capture quantum: 10-20 ms, current 256 frames is fine
- backend append batch: start at 80 ms
- max backend append batch: 160 ms
- min non-empty append: 20 ms
- pre-roll for VAD mode later: 200-300 ms

Implementation steps:

1. Keep the existing `AudioRecorder::Start` and `Stop` path working.
2. Add streaming chunk emission behind a new method or optional sink.
3. Add sequence numbers in the audio thread.
4. Calculate chunk RMS in the audio thread because it is already doing amplitude calculation.
5. Move chunks into the audio queue.
6. Batch chunks in the coordinator, not in the audio thread.

## Commit Strategy

Commit strategy controls when audio is finalized into transcript units.

Add:

```cpp
enum class CommitDecisionKind {
    None,
    CommitCurrentUtterance,
    StartNewUtterance
};

struct CommitDecision {
    CommitDecisionKind kind = CommitDecisionKind::None;
    UtteranceId utteranceId;
    std::wstring reason;
};

class ICommitStrategy {
public:
    virtual ~ICommitStrategy() = default;
    virtual CommitDecision OnAudioBatch(const CapturedAudioChunk& chunk) = 0;
    virtual CommitDecision OnStop() = 0;
    virtual void Reset() noexcept = 0;
};
```

Start with two strategies:

1. Commit-on-stop.
    This is safest and still eliminates upload time for backends that accept append-before-commit. It does not by itself make final text ready before stop when the backend only transcribes after commit.

2. Periodic commit with overlap.
   This enables earlier backend finalization for long dictation, at the cost of stitching and possible boundary errors.

Do not start with open-mic VAD commit. Add it after the streaming path is stable.

Important provider detail:

- Some realtime APIs emit transcript deltas while audio is ongoing.
- Some APIs accept streaming audio but only produce transcript text after commit.
- The coordinator must support both, and logs must distinguish `time_to_first_append`, `time_to_first_delta`, and `time_to_final`.

## Backend Plan

### Finished-File Fallback Backend

The existing `TranscriptionClient` remains the fallback.

Fallback is used when:

- streaming backend cannot start
- audio queue backpressure makes streamed transcript incomplete
- backend connection fails before final transcript
- commit state is ambiguous
- assembler detects invalid event ordering it cannot repair
- user disables streaming in config

Fallback uses retained PCM:

1. write WAV
2. call existing `TranscriptionClient::Transcribe`
3. archive and insert through the existing bridge

### OpenAI Realtime Backend

Add after the interfaces and coordinator are testable with a fake backend.

Expected responsibilities:

- own a WebSocket transcription session
- create a transcription session with the configured model
- resample 16 kHz PCM to 24 kHz PCM when required
- base64 encode audio append payloads if the API transport requires it
- send append events for non-empty audio batches
- send commit events from the commit strategy
- map provider delta events to `BackendTranscriptEventKind::AppendDelta`
- map provider completed events to `UtteranceFinalized`
- preserve backend item IDs for logs and event ordering

The repo currently has `cpr` for HTTP but not a WebSocket dependency. Do not force realtime through `cpr`. Choose a dedicated WebSocket transport dependency or a small WinHTTP/WebSocket wrapper during the backend implementation phase.

### Mistral Realtime Backend

Add after the first realtime backend proves the abstraction.

Expected responsibilities:

- use the realtime transcription model documented in the Mistral note
- send PCM `s16le` in the expected format
- expose target streaming delay through provider config
- map partial and final events to the same backend event model

### Fake Streaming Backend

Build this first.

It should:

- accept audio chunks
- emit scripted deltas or snapshots
- support delayed finalization
- simulate connection failure
- simulate out-of-order final events
- run without microphone or network

This backend makes the coordinator, assembler, queues, and runtime tests deterministic.

## Transcript Assembly Plan

The assembler is the heart of streaming correctness.

It accepts `BackendTranscriptEvent` and emits `TranscriptPatch`.

### Append-Only Deltas

For providers that emit append-only deltas:

1. Keep an accumulator per `utteranceId` and backend item ID.
2. Append each delta exactly as received.
3. Do not insert artificial spaces between deltas.
4. Emit `AppendStableText` if the backend marks deltas as stable.
5. Emit `ReplaceUnstableTail` if the provider says a delta is provisional.
6. On completed, move all remaining text into final text and emit `FinalizeUtterance`.

### Revising Snapshots

For providers that emit snapshots:

1. Keep previous snapshot text.
2. Keep stable prefix and unstable tail.
3. Compare new snapshot to previous visible text.
4. Compute the longest common prefix on UTF-8 code point boundaries.
5. Replace only the unstable tail.
6. Promote stable text after confirmation rules.
7. On final, finalize the complete latest snapshot.

Initial stability rule:

- A prefix becomes stable after appearing unchanged in two consecutive revisions.
- Never split UTF-8 code points.
- Prefer word boundaries when promoting stable text.

### Patch Trace

Keep a bounded patch trace per session:

```cpp
struct TranscriptPatchTrace {
    size_t maxEntries = 512;
    std::vector<TranscriptPatch> patches;
    bool truncated = false;
};
```

Use this for:

- debug logs
- deterministic tests
- optional archive sidecar later

The final archive path should still store the final transcript as before. Patch traces are diagnostic data, not the canonical transcript.

## Persistence Bridge

After the streaming coordinator finishes, reuse the post-recording persistence semantics.

Create a streaming equivalent of the current post-recording bridge:

```cpp
struct FinalizedRecordingRequest {
    RetainedRecordingAudio audio;
    std::string transcriptUtf8;
    bool transcriptFromStreaming = false;
    bool fallbackUsed = false;
    TranscriptPatchTrace patchTrace;
};
```

The bridge performs:

1. Write WAV using `WavWriter::WritePcm16Mono`.
2. Store recording path in app context.
3. Store final transcript in app context.
4. Enqueue archive request using final audio and final transcript.
5. Insert final transcript using `TextInjector::InsertText`.
6. Log latency metrics.

This keeps persistence and insertion behavior stable while replacing only the transcription source.

## Logging And Metrics

Keep the current post-recording latency log and add streaming-specific fields.

Current fields to preserve:

- `stop_recording`
- `wav_write`
- `transcription`
- `insert`
- `total`
- `transcript_utf8_bytes`

For streaming, reinterpret `transcription` as stop-to-final-streaming-transcript wait. Add separate streaming fields:

- `session_id`
- `utterance_id`
- `streaming_enabled`
- `streaming_backend`
- `fallback_used`
- `capture_to_first_append_ms`
- `capture_to_first_delta_ms`
- `stop_to_final_ms`
- `time_to_final_ms`
- `audio_chunks_captured`
- `audio_chunks_sent`
- `audio_bytes_sent`
- `backend_append_batches`
- `backend_commit_count`
- `transcript_revision_count`
- `replace_tail_count`
- `chars_replaced`
- `queue_backpressure_observed`
- `backend_reconnect_count`

Logging levels:

- per-chunk details at `debug`
- per-session summary at `info`
- fallback at `warn`
- hard failures at `error`

Do not log transcript text by default.

## Runtime State Machine

Add states only if needed. The current states are:

- Idle
- Recording
- SavingRecording
- Transcribing
- Inserting

For streaming, use these runtime meanings:

- `Recording`: audio capture and streaming are active.
- `Transcribing`: recording has stopped and the app is waiting for backend finalization or fallback transcription.
- `SavingRecording`: retained audio is being written to WAV.
- `Inserting`: final text is being inserted.

This avoids state churn in the first implementation. If preview UI later needs more detail, add separate tray/status detail strings rather than many app states.

## Main Loop Safety

The hidden window loop remains responsive because:

1. Start returns after setup, not after transcription.
2. Stop posts a request to the streaming controller and returns quickly.
3. Finalization runs on a worker thread.
4. Transcript patches are posted to the UI as copied messages.
5. Completion is posted back through a window message.
6. Shutdown requests close queues and request stop on `std::jthread` workers.

The UI thread may update state and show errors, but it must not wait on worker joins except during controlled shutdown or when ensuring a previous worker is done before starting a new one.

## Config Plan

Add a streaming section under transcription:

```json
"streaming": {
  "enabled": false,
  "provider": "openai_realtime",
  "commit_strategy": "on_stop",
  "append_batch_ms": 80,
  "max_append_batch_ms": 160,
  "enable_preview": true,
  "fallback_to_file_transcription": true,
  "trace_patches": false
}
```

Provider-specific settings can live under existing provider objects or under a new `transcription.streaming_providers` object.

Default `enabled` should be false until the streaming path is production-ready.

## Implementation Phases

### Phase 1: Pure Plumbing With Fake Backend

Add:

- streaming data structs
- bounded queues
- fake backend
- transcript assembler
- coordinator tests

No production behavior changes.

### Phase 2: Streaming Capture Without Network

Add:

- audio chunk emission from the recorder
- retained audio buffer compatibility
- streaming controller lifecycle
- smoke path that records chunks and finalizes through fake backend

The app should still be able to use the existing finished-file path.

### Phase 3: Persistence Bridge

Add:

- final streaming result bridge to WAV writing
- last recording path storage
- last transcript storage
- archive enqueue
- final insertion
- streaming latency summary log

At this point, fake streaming can exercise the full runtime pipeline without a live transcription service.

### Phase 4: First Real Backend

Add OpenAI realtime or another selected realtime backend.

Implementation order:

1. WebSocket transport wrapper.
2. Session startup and close.
3. Append non-empty audio batches.
4. Commit on stop.
5. Parse delta and completed events.
6. Map provider errors to failure reasons.
7. Fallback on ambiguous failure.

### Phase 5: Preview UI

Add a VoxInsert-owned preview surface that consumes `TranscriptPatch` messages. It should display stable and unstable text differently if possible, but it should not affect final persistence.

### Phase 6: Periodic Commit And Lower Latency

After commit-on-stop is reliable, add periodic commit with overlap for long dictation.

This phase needs extra tests for:

- boundary stitching
- out-of-order final events
- duplicate text around chunk overlap
- empty commits
- final transcript ordering

### Phase 7: Optional Live Insertion Layer

Only after streaming transcription is stable, design live target insertion as a separate consumer. It should support controlled adapters and a generic keyboard fallback, but it should not be required for streaming transcription.

## Testing Plan

### Unit Tests

Add tests for:

- bounded queue close behavior
- audio chunk sequencing
- empty chunk suppression
- append batching
- commit-on-stop
- fake backend scripted deltas
- append-only transcript assembly
- revising snapshot transcript assembly
- UTF-8 safe replace boundaries
- fallback-required transitions
- stale session message rejection

### Golden Trace Tests

Store scripted backend event traces for:

- simple append-only dictation
- multiple deltas before final
- revised tail snapshots
- out-of-order final events
- backend failure before commit
- backend failure after partials but before final

Replay traces into the assembler and assert final transcript plus patch sequence.

### Integration Tests

Add integration tests with canned PCM or fake audio chunks:

- fake streaming full pipeline
- fake streaming plus archive enabled
- fake streaming fallback to file transcription
- shutdown while recording
- stop while backend is still finalizing

### Manual Tests

Manual scenarios:

- short dictation
- long dictation
- empty recording
- microphone overflow
- network disconnect mid-recording
- backend unavailable at start
- app quit while streaming
- archive enabled and disabled
- restore clipboard enabled and disabled

## Failure Handling

Failure policy:

- If streaming fails before reliable final transcript, fall back to retained PCM and finished-file transcription when enabled.
- If fallback succeeds, final status is success with `fallback_used=true`.
- If fallback fails, show the fallback failure as the user-facing error and log the original streaming failure as context.
- If user cancels recording, cancel backend utterance and discard transcript.
- If app shuts down, close queues, cancel backend session, join workers, and do not attempt insertion.

Unknown commit state should be treated as not reliable. The retained PCM buffer is the source of truth for recovery.

## Concrete File Plan

Likely new files:

- `src/transcription/streaming_transcription_service.h`
- `src/transcription/streaming_transcription_client.h`
- `src/transcription/streaming_transcription_client.cpp`
- `tests/fakes/fake_streaming_transcription_service.h`
- `tests/fakes/fake_streaming_transcription_service.cpp`
- `src/transcription/transcript_assembler.h`
- `src/transcription/transcript_assembler.cpp`
- `src/runtime/streaming_recording_controller.h`
- `src/runtime/streaming_recording_controller.cpp`
- `src/runtime/bounded_queue.h` or a shared utility equivalent

Likely changed files:

- `src/audio/audio_recorder.h`
- `src/audio/audio_recorder.cpp`
- `src/runtime/app_host_internal.h`
- `src/runtime/app_host_recording.cpp`
- `src/config/app_config.h`
- `src/config/app_config.cpp`
- `config.example.json`
- `CMakeLists.txt`

Do not change `TextInjector` for streaming until the final transcript pipeline is working.

## Review Checklist

Before implementing a real backend, verify:

- The app can still run the current non-streaming workflow.
- Streaming disabled is the default.
- Fake streaming can complete a full record-to-insert flow.
- Audio capture never blocks on network work.
- UI thread never waits for backend finalization.
- All queued messages own their payloads.
- Final WAV writing uses retained local PCM.
- Final transcript storage uses assembled final text.
- Archive enqueue receives final audio and final transcript.
- Logs distinguish streaming success, fallback success, and hard failure.
- Shutdown closes sessions without detached threads or use-after-free risk.

## Open Questions

1. Which backend should be implemented first after the fake backend: OpenAI realtime or Mistral realtime?
2. Should patch traces be archiveable behind a config flag, or only kept in debug logs and tests?
3. Should periodic commit be exposed immediately or kept hidden until commit-on-stop is stable?
4. What should the first preview UI look like: small always-on-top transcript window, settings/debug window, or tray-launched panel?
5. Should streaming provider selection be independent from finished-file provider selection, or should it inherit the current transcription provider when possible?
