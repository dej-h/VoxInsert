#include "runtime/app_host_internal.h"
#include "runtime/post_recording_workflow.h"

#include "observability/logging.h"

namespace voxinsert {

namespace {

void HandleToggleRequest(AppContext& context, std::wstring_view sourceLabel) {
    if (context.state == AppState::Idle) {
        StartRecording(context);
        return;
    }

    if (context.state == AppState::Recording) {
        StopRecordingAndWriteWav(context);
        return;
    }

    context.logger->info("{} ignored while state is {}", Utf8FromWide(sourceLabel), Utf8FromWide(StateLabel(context.state)));
}

} // namespace

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
    const PostRecordingWorkflowResult result = RunPostRecordingWorkflow({
        .audioRecorder = &context.audioRecorder,
        .wavWriter = &context.wavWriter,
        .audioConfig = &context.config.audio,
        .transcriptionClient = &context.transcriptionClient,
        .transcriptionConfig = &context.config.transcription,
        .textInjector = &context.textInjector,
        .insertionConfig = &context.config.insertion,
        .archiveService = &context.archiveService,
        .archiveConfig = &context.config.archive,
        .logger = context.logger,
        .ownerWindow = context.window,
        .smokeTest = context.options.smokeTest,
        .isShutdownRequested = [&context]() { return context.shutdownRequested.load(); },
        .onPhaseChanged = [&context](PostRecordingPhase phase) {
            AppState nextState = AppState::Transcribing;
            if (phase == PostRecordingPhase::Inserting) {
                nextState = AppState::Inserting;
            }

            PostMessageW(
                context.window,
                kPostRecordingPhaseMessage,
                static_cast<WPARAM>(static_cast<int>(nextState)),
                0);
        },
    });

    if (result.hasRecordingPath) {
        StoreLastRecordingPath(context, result.recordingPath);
    }

    if (result.hasTranscript) {
        StoreLastTranscript(context, result.transcript);
    }

    if (result.status == PostRecordingWorkflowStatus::Cancelled) {
        return;
    }

    if (result.status == PostRecordingWorkflowStatus::Failed) {
        CompletePostRecordingWithError(context, result.errorTitle, result.failureReason);
        return;
    }

    CompletePostRecordingSuccessfully(context, result.showDone);
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
        HandleToggleRequest(context, L"toggle hotkey");
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

void HandleSmtcToggle(AppContext& context) {
    context.logger->info("SMTC Play/Pause received");
    HandleToggleRequest(context, L"SMTC Play/Pause");
}

} // namespace voxinsert