#include "runtime/app_host_internal.h"

#include "observability/logging.h"

#include <exception>
#include <vector>

namespace voxinsert {

void StartRecording(AppContext& context) {
    std::wstring failureReason;
    if (!context.audioRecorder.Start(
            context.config.audio,
            [&context](float rms) {
                context.statusPill.PostAmplitudeSample(rms);
            },
            failureReason)) {
        ShowRuntimeError(context, L"VoxInsert microphone error", failureReason);
        SetState(context, AppState::Idle);
        return;
    }

    context.logger->info(
        "recording started using the current default microphone: {}",
        Utf8FromWide(context.audioRecorder.ActiveDeviceName()));
    SetState(context, AppState::Recording);
}

void RunPostRecordingPipeline(AppContext& context) noexcept {
    try {
        std::vector<int16_t> samples;
        std::wstring failureReason;
        if (!context.audioRecorder.Stop(samples, failureReason)) {
            CompletePostRecordingWithError(context, L"VoxInsert recording error", failureReason);
            return;
        }

        std::filesystem::path wavPath;
        if (!context.wavWriter.WritePcm16Mono(samples, context.config.audio.sampleRate, wavPath, failureReason)) {
            CompletePostRecordingWithError(context, L"VoxInsert WAV error", failureReason);
            return;
        }

        StoreLastRecordingPath(context, wavPath);
        context.logger->info("recording written to {}", Utf8FromWide(wavPath.wstring()));

        if (context.options.smokeTest) {
            context.logger->info("smoke-test: skipping transcription upload");
            CompletePostRecordingSuccessfully(context, false);
            return;
        }

        if (context.shutdownRequested) {
            return;
        }

        PostMessageW(context.window, kPostRecordingPhaseMessage, static_cast<WPARAM>(static_cast<int>(AppState::Transcribing)), 0);

        std::string transcript;
        if (!context.transcriptionClient.Transcribe(context.config.transcription, wavPath, transcript, failureReason)) {
            CompletePostRecordingWithError(context, L"VoxInsert transcription error", failureReason);
            return;
        }

        context.logger->info("transcription text: {}", transcript);

        const std::wstring transcriptWide = WideFromUtf8(transcript);
        StoreLastTranscript(context, transcriptWide);

        if (context.shutdownRequested) {
            return;
        }

        PostMessageW(context.window, kPostRecordingPhaseMessage, static_cast<WPARAM>(static_cast<int>(AppState::Inserting)), 0);

        if (!context.textInjector.InsertText(context.window, context.config.insertion, transcriptWide, failureReason)) {
            CompletePostRecordingWithError(context, L"VoxInsert insertion error", failureReason);
            return;
        }

        context.logger->info("transcription inserted into the focused text field via clipboard paste");
        CompletePostRecordingSuccessfully(context, true);
    }
    catch (const std::exception& exception) {
        CompletePostRecordingWithError(
            context,
            L"VoxInsert transcription error",
            L"Unexpected background transcription failure: " + WideFromUtf8(exception.what()));
    }
    catch (...) {
        CompletePostRecordingWithError(
            context,
            L"VoxInsert transcription error",
            L"Unexpected background transcription failure.");
    }
}

void StopRecordingAndWriteWav(AppContext& context) {
    SetState(context, AppState::SavingRecording);
    JoinPostRecordingWorker(context);

    try {
        context.postRecordingWorker = std::thread([&context]() {
            RunPostRecordingPipeline(context);
        });
    }
    catch (const std::exception& exception) {
        context.audioRecorder.Cancel();
        std::wstring failureReason = L"Could not start the background transcription worker: ";
        failureReason += WideFromUtf8(exception.what());
        ShowRuntimeError(context, L"VoxInsert transcription error", failureReason);
        SetState(context, AppState::Idle);
    }
}

void HandlePostRecordingPhase(AppContext& context, WPARAM phaseParam) {
    const AppState phase = static_cast<AppState>(static_cast<int>(phaseParam));
    if (phase != AppState::Transcribing && phase != AppState::Inserting) {
        context.logger->warn("unknown post-recording phase received: {}", static_cast<int>(phaseParam));
        return;
    }

    if (context.state == AppState::Idle) {
        context.logger->info("post-recording phase ignored because the app is already idle");
        return;
    }

    SetState(context, phase);
}

void HandlePostRecordingComplete(AppContext& context) {
    PostRecordingResult result;
    if (!TakePostRecordingResult(context, result)) {
        context.logger->warn("post-recording completion message received without a result");
        JoinPostRecordingWorker(context);
        SetState(context, AppState::Idle);
        return;
    }

    JoinPostRecordingWorker(context);

    if (!result.success) {
        ShowRuntimeError(context, result.errorTitle.c_str(), result.failureReason);
        SetState(context, AppState::Idle);
        return;
    }

    if (result.showDone) {
        context.statusPill.SetState(StatusPillState::Done);
    }

    SetState(context, AppState::Idle);
    if (result.showDone) {
        SetTransientTrayStatus(context, TrayStatus::Inserted, {}, kInsertedTrayStatusMs);
    }
}

void CancelRecording(AppContext& context) {
    context.audioRecorder.Cancel();
    context.logger->info("recording cancelled");
    SetState(context, AppState::Idle);
}

void HandleHotkey(AppContext& context, WPARAM hotkeyId) {
    switch (hotkeyId) {
    case HotkeyManager::kToggleRecordingHotkeyId:
        if (context.state == AppState::Idle) {
            StartRecording(context);
        }
        else if (context.state == AppState::Recording) {
            StopRecordingAndWriteWav(context);
        }
        else {
            context.logger->info("toggle hotkey ignored while state is {}", Utf8FromWide(StateLabel(context.state)));
        }
        return;

    case HotkeyManager::kCancelRecordingHotkeyId:
        if (context.state == AppState::Recording) {
            CancelRecording(context);
        }
        else {
            context.logger->info("cancel hotkey ignored while state is {}", Utf8FromWide(StateLabel(context.state)));
        }
        return;
    }

    context.logger->warn("unknown hotkey id received: {}", hotkeyId);
}

} // namespace voxinsert