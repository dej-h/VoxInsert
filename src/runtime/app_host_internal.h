#pragma once

#include "runtime/app_host.h"

#include "archive/archive_service.h"
#include "audio/audio_recorder.h"
#include "audio/wav_writer.h"
#include "config/app_config.h"
#include "input/hotkey_manager.h"
#include "insertion/text_injector.h"
#include "testing/app_host_smoke_test.h"
#include "transcription/transcription_client.h"
#include "ui/status_pill.h"

#include <shellapi.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace voxinsert {

inline constexpr wchar_t kTrayTooltip[] = L"VoxInsert";
inline constexpr UINT kTrayIconId = 1;
inline constexpr UINT kTrayCallbackMessage = WM_APP + 1;
inline constexpr UINT kPostRecordingPhaseMessage = WM_APP + 2;
inline constexpr UINT kPostRecordingCompleteMessage = WM_APP + 3;
inline constexpr UINT_PTR kTrayStatusResetTimerId = 2;
inline constexpr UINT kTrayMenuCommandQuit = 1003;
inline constexpr UINT kTrayMenuCommandReloadConfig = 1004;
inline constexpr UINT kTrayMenuCommandSettings = 1005;
inline constexpr UINT kTrayMenuCommandCopyLastTranscript = 1006;
inline constexpr UINT kTrayMenuCommandReinsertLastTranscript = 1007;
inline constexpr UINT kTrayMenuCommandOpenLastRecordingFolder = 1008;
inline constexpr UINT kInsertedTrayStatusMs = 900;
inline constexpr UINT kErrorTrayStatusMs = 3000;

enum class AppState {
    Idle,
    Recording,
    SavingRecording,
    Transcribing,
    Inserting
};

enum class TrayStatus {
    Idle,
    Recording,
    SavingRecording,
    Transcribing,
    Inserting,
    Inserted,
    Error
};

struct PostRecordingResult {
    bool success = false;
    bool showDone = false;
    std::wstring errorTitle;
    std::wstring failureReason;
};

// Groups the long-lived state owned by the hidden host window and its helper modules.
struct AppContext {
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    std::shared_ptr<spdlog::logger> logger;
    AppHostOptions options{};
    AppConfig config;
    AudioRecorder audioRecorder;
    AppHostSmokeTest smokeTest;
    HotkeyManager hotkeyManager;
    TextInjector textInjector;
    TranscriptionClient transcriptionClient;
    ArchiveService archiveService;
    StatusPill statusPill;
    WavWriter wavWriter;
    std::thread postRecordingWorker;
    std::mutex postRecordingResultMutex;
    std::mutex recentResultMutex;
    PostRecordingResult postRecordingResult;
    bool hasPostRecordingResult = false;
    std::atomic<bool> shutdownRequested = false;
    NOTIFYICONDATAW trayIconData{};
    HICON trayIcon = nullptr;
    bool trayIconAdded = false;
    AppState state = AppState::Idle;
    TrayStatus trayStatus = TrayStatus::Idle;
    std::wstring trayStatusDetail;
    std::filesystem::path lastRecordingPath;
    std::wstring lastTranscript;
    bool hasLastTranscript = false;
    bool settingsDialogOpen = false;
    UINT taskbarCreatedMessage = 0;
    int exitCode = 0;
};

bool SameHotkeyBinding(const HotkeyBinding& left, const HotkeyBinding& right);
std::wstring_view StateLabel(AppState state);
TrayStatus TrayStatusForAppState(AppState state);
bool IsTransientTrayStatus(TrayStatus status);
std::wstring TrayStatusText(const AppContext& context, bool includeErrorDetail);
std::wstring BuildTrayTooltip(const AppContext& context);
std::wstring BuildTrayMenuTitle(const AppContext& context);
bool UpdateTrayTooltip(AppContext& context);
void SetTrayStatus(AppContext& context, TrayStatus status, std::wstring detail = {});
void SetTransientTrayStatus(AppContext& context, TrayStatus status, std::wstring detail, UINT durationMs);
void ResetTrayStatusToCurrentState(AppContext& context);
void SetState(AppContext& context, AppState nextState);
void ShowRuntimeError(AppContext& context, const wchar_t* title, const std::wstring& message);
void ShowRuntimeInfo(AppContext& context, const wchar_t* title, const std::wstring& message);
void JoinPostRecordingWorker(AppContext& context) noexcept;
void StorePostRecordingResult(AppContext& context, PostRecordingResult result);
bool TakePostRecordingResult(AppContext& context, PostRecordingResult& result);
void StoreLastRecordingPath(AppContext& context, std::filesystem::path recordingPath);
bool TryGetLastRecordingPath(AppContext& context, std::filesystem::path& recordingPath);
bool HasLastRecordingPath(AppContext& context);
void StoreLastTranscript(AppContext& context, std::wstring transcript);
bool TryGetLastTranscript(AppContext& context, std::wstring& transcript);
bool HasLastTranscript(AppContext& context);
bool OpenPathInExplorer(HWND ownerWindow, const std::filesystem::path& path, std::wstring& failureReason);
void CompletePostRecordingWithError(AppContext& context, std::wstring errorTitle, std::wstring failureReason);
void CompletePostRecordingSuccessfully(AppContext& context, bool showDone);
bool ApplyStartupRegistrationFromConfig(AppContext& context, std::wstring& failureReason);
void CheckTranscriptionCredentialOnStartup(AppContext& context);
void StartRecording(AppContext& context);
void CopyLastTranscriptFromTray(AppContext& context);
void ReinsertLastTranscriptFromTray(AppContext& context);
void OpenLastRecordingFolderFromTray(AppContext& context);
void RunPostRecordingPipeline(AppContext& context) noexcept;
void StopRecordingAndWriteWav(AppContext& context);
void HandlePostRecordingPhase(AppContext& context, WPARAM phaseParam);
void HandlePostRecordingComplete(AppContext& context);
void CancelRecording(AppContext& context);
void HandleHotkey(AppContext& context, WPARAM hotkeyId);
void RemoveTrayIcon(AppContext& context);
bool AddTrayIcon(AppContext& context);
void RecreateStatusPillIfNeeded(AppContext& context, const UiConfig& previousUiConfig);
void ReloadConfigFromTray(AppContext& context, const wchar_t* successMessage = L"Config reloaded.");
void ShowSettingsDialogFromTray(AppContext& context);
void ShowTrayMenu(AppContext& context);

} // namespace voxinsert