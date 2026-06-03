#pragma once

#include "config/app_config.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include <spdlog/logger.h>

namespace voxinsert {

enum class StatusPillState {
    Idle,
    Recording,
    Transcribing,
    Working,
    Done,
    Error
};

class StatusPill {
public:
    StatusPill() = default;
    ~StatusPill();

    bool Create(
        HINSTANCE instance,
        HWND ownerWindow,
        UINT trayIconId,
        bool enabled,
        StatusPillPlacement placement,
        const std::shared_ptr<spdlog::logger>& logger,
        std::wstring& failureReason);

    void Destroy() noexcept;
    void SetState(StatusPillState state, std::wstring_view errorText = {});
    void PostAmplitudeSample(float rms) noexcept;
    void Reanchor();

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wordParam, LPARAM longParam);

    LRESULT HandleMessage(UINT message, WPARAM wordParam, LPARAM longParam);
    bool RegisterWindowClass(std::wstring& failureReason);
    bool CreateWindowHandle(std::wstring& failureReason);
    void StartAnimationTimer();
    void StopAnimationTimer();
    void HandleAnimationTick();
    void HandleAmplitudeSample(float rms);
    void BeginFadeOut(std::chrono::milliseconds duration);
    void HideNow();
    void Render();
    void DrawPill(HDC memoryDc, int width, int height, float opacity);
    RECT CalculateWindowRect(int width, int height) const;
    UINT CurrentDpi() const;
    int Scale(int value) const;
    float ScaleF(float value) const;

    HINSTANCE instance_ = nullptr;
    HWND ownerWindow_ = nullptr;
    HWND window_ = nullptr;
    UINT trayIconId_ = 0;
    bool enabled_ = false;
    bool visible_ = false;
    bool fadingOut_ = false;
    StatusPillPlacement placement_ = StatusPillPlacement::TrayAnchor;
    StatusPillState state_ = StatusPillState::Idle;
    std::wstring errorText_;
    std::shared_ptr<spdlog::logger> logger_;
    HICON anchorIcon_ = nullptr;
    ULONG_PTR gdiplusToken_ = 0;
    std::chrono::steady_clock::time_point stateStarted_{};
    std::chrono::steady_clock::time_point fadeStarted_{};
    std::chrono::steady_clock::time_point holdUntil_{};
    std::chrono::steady_clock::time_point lastAmplitudeSample_{};
    std::chrono::milliseconds fadeDuration_{0};
    std::atomic<int> pendingAmplitude_{0};
    std::atomic<bool> amplitudeMessagePending_{false};
    float liveAmplitude_ = 0.0f;
    float displayedAmplitude_ = 0.0f;
};

} // namespace voxinsert
