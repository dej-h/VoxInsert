#include "ui/status_pill.h"

#include "observability/logging.h"
#include "resource.h"

#include <shellapi.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace voxinsert {
namespace {

using Clock = std::chrono::steady_clock;

constexpr wchar_t kStatusPillWindowClassName[] = L"VoxInsert.StatusPill";
constexpr UINT_PTR kAnimationTimerId = 1;
constexpr UINT kAnimationFrameMs = 16;
constexpr UINT kAmplitudeMessage = WM_APP + 51;
constexpr float kNoiseFloor = 0.010f;
constexpr float kNoiseCeiling = 0.08f;
constexpr float kSilenceThreshold = 0.08f;
constexpr float kMeterGamma = 0.45f;
constexpr float kMeterAttackAlpha = 0.42f;
constexpr float kMeterReleaseAlpha = 0.14f;
constexpr auto kFadeInDuration = std::chrono::milliseconds(120);
constexpr auto kDefaultFadeOutDuration = std::chrono::milliseconds(200);
constexpr auto kErrorFadeOutDuration = std::chrono::milliseconds(300);
constexpr auto kDoneHoldDuration = std::chrono::milliseconds(600);
constexpr auto kErrorHoldDuration = std::chrono::milliseconds(2400);
constexpr auto kAmplitudeIdleTimeout = std::chrono::milliseconds(120);
constexpr int kBasePillHeight = 28;
constexpr int kBaseMinimumPillWidth = 60;
constexpr int kBasePaddingX = 12;
constexpr int kBaseAnchorSize = 14;
constexpr int kBaseSeparatorHeight = 12;
constexpr int kBaseSeparatorGap = 8;
constexpr int kBaseStateSlotWidth = 32;
constexpr int kBaseAnchorGap = 0;
constexpr int kBaseClockAnchorRightMargin = 12;

Gdiplus::Color ColorWithOpacity(BYTE red, BYTE green, BYTE blue, float alpha, float opacity) {
    const int scaledAlpha = std::clamp(static_cast<int>(alpha * opacity * 255.0f), 0, 255);
    return Gdiplus::Color(static_cast<BYTE>(scaledAlpha), red, green, blue);
}

void AddRoundedRectangle(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rect, float radius) {
    const float diameter = radius * 2.0f;
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(rect.GetRight() - diameter, rect.Y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(rect.GetRight() - diameter, rect.GetBottom() - diameter, diameter, diameter, 0.0f, 90.0f);
    path.AddArc(rect.X, rect.GetBottom() - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

float EaseOut(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - ((1.0f - t) * (1.0f - t));
}

float NormalizeAmplitude(float rms) {
    const float normalized = (std::clamp(rms, 0.0f, 1.0f) - kNoiseFloor) / (kNoiseCeiling - kNoiseFloor);
    const float gated = std::clamp(normalized, 0.0f, 1.0f);
    if (gated < kSilenceThreshold) {
        return 0.0f;
    }

    return std::pow(gated, kMeterGamma);
}

} // namespace

StatusPill::~StatusPill() {
    Destroy();
}

bool StatusPill::Create(
    HINSTANCE instance,
    HWND ownerWindow,
    UINT trayIconId,
    bool enabled,
    StatusPillPlacement placement,
    const std::shared_ptr<spdlog::logger>& logger,
    std::wstring& failureReason) {
    instance_ = instance;
    ownerWindow_ = ownerWindow;
    trayIconId_ = trayIconId;
    enabled_ = enabled;
    placement_ = placement;
    logger_ = logger;

    if (!enabled_) {
        return true;
    }

    Gdiplus::GdiplusStartupInput gdiplusStartupInput{};
    if (Gdiplus::GdiplusStartup(&gdiplusToken_, &gdiplusStartupInput, nullptr) != Gdiplus::Ok) {
        failureReason = L"GdiplusStartup failed while creating the status pill.";
        return false;
    }

    anchorIcon_ = reinterpret_cast<HICON>(LoadImageW(
        instance_,
        MAKEINTRESOURCEW(IDI_TRAY_ICON),
        IMAGE_ICON,
        Scale(kBaseAnchorSize),
        Scale(kBaseAnchorSize),
        LR_DEFAULTCOLOR));

    if (!RegisterWindowClass(failureReason)) {
        return false;
    }

    return CreateWindowHandle(failureReason);
}

void StatusPill::Destroy() noexcept {
    StopAnimationTimer();

    if (window_ != nullptr) {
        DestroyWindow(window_);
        window_ = nullptr;
    }

    if (anchorIcon_ != nullptr) {
        DestroyIcon(anchorIcon_);
        anchorIcon_ = nullptr;
    }

    if (gdiplusToken_ != 0) {
        Gdiplus::GdiplusShutdown(gdiplusToken_);
        gdiplusToken_ = 0;
    }

    visible_ = false;
    enabled_ = false;
}

bool StatusPill::RegisterWindowClass(std::wstring& failureReason) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = StatusPill::WindowProc;
    windowClass.hInstance = instance_;
    windowClass.lpszClassName = kStatusPillWindowClassName;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);

    if (RegisterClassExW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        return true;
    }

    failureReason = L"RegisterClassExW failed for the status pill: ";
    failureReason += FormatWin32Error(GetLastError());
    return false;
}

bool StatusPill::CreateWindowHandle(std::wstring& failureReason) {
    window_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_TRANSPARENT,
        kStatusPillWindowClassName,
        L"VoxInsert.StatusPill",
        WS_POPUP,
        0,
        0,
        Scale(kBaseMinimumPillWidth),
        Scale(kBasePillHeight),
        nullptr,
        nullptr,
        instance_,
        this);

    if (window_ != nullptr) {
        return true;
    }

    failureReason = L"CreateWindowExW failed for the status pill: ";
    failureReason += FormatWin32Error(GetLastError());
    return false;
}

void StatusPill::SetState(StatusPillState state, std::wstring_view errorText) {
    if (!enabled_ || window_ == nullptr) {
        return;
    }

    if (state == StatusPillState::Idle) {
        if (state_ == StatusPillState::Done || state_ == StatusPillState::Error) {
            return;
        }

        BeginFadeOut(kDefaultFadeOutDuration);
        return;
    }

    state_ = state;
    errorText_ = std::wstring(errorText);
    stateStarted_ = Clock::now();
    fadingOut_ = false;

    if (state_ == StatusPillState::Recording) {
        liveAmplitude_ = 0.0f;
        displayedAmplitude_ = 0.0f;
        lastAmplitudeSample_ = {};
    }

    if (state_ == StatusPillState::Done) {
        holdUntil_ = stateStarted_ + kDoneHoldDuration;
    }
    else if (state_ == StatusPillState::Error) {
        holdUntil_ = stateStarted_ + kErrorHoldDuration;
    }

    Reanchor();
    ShowWindow(window_, SW_SHOWNOACTIVATE);
    visible_ = true;
    StartAnimationTimer();
    Render();
}

void StatusPill::PostAmplitudeSample(float rms) noexcept {
    if (!enabled_ || window_ == nullptr) {
        return;
    }

    const int encodedAmplitude = std::clamp(static_cast<int>(rms * 10000.0f), 0, 10000);
    PostMessageW(window_, kAmplitudeMessage, static_cast<WPARAM>(encodedAmplitude), 0);
}

void StatusPill::Reanchor() {
    if (!enabled_ || window_ == nullptr) {
        return;
    }

    const int width = std::max(
        Scale(kBaseMinimumPillWidth),
        (Scale(kBasePaddingX) * 2) + Scale(kBaseAnchorSize) + Scale(kBaseAnchorGap) +
            Scale(kBaseSeparatorGap) + Scale(1) + Scale(kBaseSeparatorGap) + Scale(kBaseStateSlotWidth));
    const int height = Scale(kBasePillHeight);
    const RECT windowRect = CalculateWindowRect(width, height);
    SetWindowPos(
        window_,
        HWND_TOPMOST,
        windowRect.left,
        windowRect.top,
        width,
        height,
        SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

LRESULT CALLBACK StatusPill::WindowProc(HWND window, UINT message, WPARAM wordParam, LPARAM longParam) {
    StatusPill* pill = reinterpret_cast<StatusPill*>(GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        const auto* createStruct = reinterpret_cast<const CREATESTRUCTW*>(longParam);
        pill = static_cast<StatusPill*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pill));
        pill->window_ = window;
        return TRUE;
    }

    if (pill != nullptr) {
        return pill->HandleMessage(message, wordParam, longParam);
    }

    return DefWindowProcW(window, message, wordParam, longParam);
}

LRESULT StatusPill::HandleMessage(UINT message, WPARAM wordParam, LPARAM longParam) {
    (void)longParam;

    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_TIMER:
        if (wordParam == kAnimationTimerId) {
            HandleAnimationTick();
            return 0;
        }
        break;

    case WM_DPICHANGED:
    case WM_DISPLAYCHANGE:
        Reanchor();
        Render();
        return 0;

    case kAmplitudeMessage:
        HandleAmplitudeSample(static_cast<float>(wordParam) / 10000.0f);
        return 0;
    }

    return DefWindowProcW(window_, message, wordParam, longParam);
}

void StatusPill::StartAnimationTimer() {
    if (window_ != nullptr) {
        SetTimer(window_, kAnimationTimerId, kAnimationFrameMs, nullptr);
    }
}

void StatusPill::StopAnimationTimer() {
    if (window_ != nullptr) {
        KillTimer(window_, kAnimationTimerId);
    }
}

void StatusPill::HandleAnimationTick() {
    const auto now = Clock::now();
    if ((state_ == StatusPillState::Done || state_ == StatusPillState::Error) && !fadingOut_ && now >= holdUntil_) {
        BeginFadeOut(state_ == StatusPillState::Error ? kErrorFadeOutDuration : kDefaultFadeOutDuration);
    }

    if (fadingOut_ && now >= fadeStarted_ + fadeDuration_) {
        HideNow();
        return;
    }

    Render();
}

void StatusPill::HandleAmplitudeSample(float rms) {
    liveAmplitude_ = NormalizeAmplitude(rms);
    lastAmplitudeSample_ = Clock::now();
}

void StatusPill::BeginFadeOut(std::chrono::milliseconds duration) {
    if (!visible_ || window_ == nullptr || fadingOut_) {
        return;
    }

    fadingOut_ = true;
    fadeDuration_ = duration;
    fadeStarted_ = Clock::now();
    StartAnimationTimer();
}

void StatusPill::HideNow() {
    ShowWindow(window_, SW_HIDE);
    StopAnimationTimer();
    visible_ = false;
    fadingOut_ = false;
    state_ = StatusPillState::Idle;
    errorText_.clear();
}

void StatusPill::Render() {
    if (!enabled_ || window_ == nullptr || !visible_) {
        return;
    }

    Reanchor();

    RECT rect{};
    GetWindowRect(window_, &rect);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) {
        return;
    }

    const auto now = Clock::now();
    float opacity = 1.0f;
    if (fadingOut_) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - fadeStarted_);
        opacity = 1.0f - EaseOut(static_cast<float>(elapsed.count()) / static_cast<float>(fadeDuration_.count()));
    }
    else {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - stateStarted_);
        opacity = EaseOut(static_cast<float>(elapsed.count()) / static_cast<float>(kFadeInDuration.count()));
    }

    HDC screenDc = GetDC(nullptr);
    HDC memoryDc = CreateCompatibleDC(screenDc);

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screenDc, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);

    DrawPill(memoryDc, width, height, std::clamp(opacity, 0.0f, 1.0f));

    POINT destination{rect.left, rect.top};
    POINT source{0, 0};
    SIZE size{width, height};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(window_, screenDc, &destination, &size, memoryDc, &source, 0, &blend, ULW_ALPHA);

    SelectObject(memoryDc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    ReleaseDC(nullptr, screenDc);
}

void StatusPill::DrawPill(HDC memoryDc, int width, int height, float opacity) {
    Gdiplus::Graphics graphics(memoryDc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.Clear(Gdiplus::Color(0, 0, 0, 0));

    const bool isError = state_ == StatusPillState::Error;
    const Gdiplus::RectF bounds(0.5f, 0.5f, static_cast<float>(width) - 1.0f, static_cast<float>(height) - 1.0f);
    Gdiplus::GraphicsPath backgroundPath;
    AddRoundedRectangle(backgroundPath, bounds, ScaleF(14.0f));

    const auto fillColor = isError
        ? ColorWithOpacity(239, 68, 68, 0.12f, opacity)
        : ColorWithOpacity(28, 28, 30, 0.92f, opacity);
    const auto borderColor = isError
        ? ColorWithOpacity(239, 68, 68, 0.30f, opacity)
        : ColorWithOpacity(255, 255, 255, 0.14f, opacity);
    Gdiplus::SolidBrush fillBrush(fillColor);
    Gdiplus::Pen borderPen(borderColor, ScaleF(0.5f));
    graphics.FillPath(&fillBrush, &backgroundPath);
    graphics.DrawPath(&borderPen, &backgroundPath);

    const float paddingX = ScaleF(static_cast<float>(kBasePaddingX));
    const float anchorSize = ScaleF(static_cast<float>(kBaseAnchorSize));
    const float centerY = static_cast<float>(height) / 2.0f;
    const float anchorX = paddingX;
    const float anchorY = centerY - (anchorSize / 2.0f);

    if (anchorIcon_ != nullptr) {
        Gdiplus::Bitmap iconBitmap(anchorIcon_);
        graphics.DrawImage(&iconBitmap, Gdiplus::RectF(anchorX, anchorY, anchorSize, anchorSize));
    }

    const float separatorX = anchorX + anchorSize + ScaleF(static_cast<float>(kBaseSeparatorGap));
    const float separatorHeight = ScaleF(static_cast<float>(kBaseSeparatorHeight));
    const auto separatorColor = isError
        ? ColorWithOpacity(239, 68, 68, 0.30f, opacity)
        : ColorWithOpacity(255, 255, 255, 0.18f, opacity);
    Gdiplus::Pen separatorPen(separatorColor, ScaleF(1.0f));
    graphics.DrawLine(&separatorPen, separatorX, centerY - separatorHeight / 2.0f, separatorX, centerY + separatorHeight / 2.0f);

    const float slotX = separatorX + ScaleF(1.0f) + ScaleF(static_cast<float>(kBaseSeparatorGap));

    if (state_ == StatusPillState::Recording) {
        const auto now = Clock::now();
        const float targetAmplitude =
            (lastAmplitudeSample_ != Clock::time_point{} && now - lastAmplitudeSample_ <= kAmplitudeIdleTimeout)
            ? liveAmplitude_
            : 0.0f;
        const float smoothingAlpha = targetAmplitude > displayedAmplitude_
            ? kMeterAttackAlpha
            : kMeterReleaseAlpha;
        displayedAmplitude_ += (targetAmplitude - displayedAmplitude_) * smoothingAlpha;

        constexpr std::array<float, 5> kBarProfile = {0.42f, 0.68f, 1.0f, 0.68f, 0.42f};
        const float barWidth = ScaleF(3.0f);
        const float barGap = ScaleF(2.5f);
        const float minHeight = ScaleF(1.75f);
        const float maxHeight = ScaleF(18.0f);
        Gdiplus::SolidBrush recordingBrush(ColorWithOpacity(239, 68, 68, 1.0f, opacity));

        for (size_t index = 0; index < kBarProfile.size(); ++index) {
            const float profile = kBarProfile[index] + ((1.0f - kBarProfile[index]) * displayedAmplitude_);
            const float barLevel = std::clamp(displayedAmplitude_ * profile, 0.0f, 1.0f);
            const float barHeight = minHeight + ((maxHeight - minHeight) * barLevel);
            const float x = slotX + static_cast<float>(index) * (barWidth + barGap);
            const float y = centerY - barHeight / 2.0f;
            Gdiplus::GraphicsPath barPath;
            AddRoundedRectangle(barPath, Gdiplus::RectF(x, y, barWidth, barHeight), barWidth / 2.0f);
            graphics.FillPath(&recordingBrush, &barPath);
        }
        return;
    }

    if (state_ == StatusPillState::Transcribing) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - stateStarted_).count();
        const float pulsePhase = static_cast<float>(elapsed) / 220.0f;
        const auto accentColor = ColorWithOpacity(245, 158, 11, 1.0f, opacity);
        const auto textColor = ColorWithOpacity(255, 255, 255, 0.72f, opacity);
        Gdiplus::SolidBrush accentBrush(accentColor);
        Gdiplus::Pen accentPen(accentColor, ScaleF(1.35f));
        accentPen.SetStartCap(Gdiplus::LineCapRound);
        accentPen.SetEndCap(Gdiplus::LineCapRound);
        Gdiplus::Pen textPen(textColor, ScaleF(1.2f));
        textPen.SetStartCap(Gdiplus::LineCapRound);
        textPen.SetEndCap(Gdiplus::LineCapRound);

        constexpr std::array<float, 3> kWaveHeights = {0.48f, 0.92f, 0.62f};
        const float waveBarWidth = ScaleF(2.2f);
        const float waveBarGap = ScaleF(1.8f);
        const float waveMinHeight = ScaleF(4.0f);
        const float waveMaxHeight = ScaleF(10.0f);
        for (size_t index = 0; index < kWaveHeights.size(); ++index) {
            const float animatedScale = 0.78f + 0.22f * std::sin(pulsePhase + static_cast<float>(index) * 0.95f);
            const float barHeight = waveMinHeight + (waveMaxHeight - waveMinHeight) * kWaveHeights[index] * animatedScale;
            const float x = slotX + static_cast<float>(index) * (waveBarWidth + waveBarGap);
            const float y = centerY - barHeight / 2.0f;
            Gdiplus::GraphicsPath barPath;
            AddRoundedRectangle(barPath, Gdiplus::RectF(x, y, waveBarWidth, barHeight), waveBarWidth / 2.0f);
            graphics.FillPath(&accentBrush, &barPath);
        }

        const float arrowX = slotX + ScaleF(9.5f);
        graphics.DrawLine(&accentPen, arrowX, centerY, arrowX + ScaleF(5.0f), centerY);
        graphics.DrawLine(&accentPen, arrowX + ScaleF(3.1f), centerY - ScaleF(2.5f), arrowX + ScaleF(5.6f), centerY);
        graphics.DrawLine(&accentPen, arrowX + ScaleF(3.1f), centerY + ScaleF(2.5f), arrowX + ScaleF(5.6f), centerY);

        const float textX = slotX + ScaleF(17.5f);
        graphics.DrawLine(&textPen, textX, centerY - ScaleF(4.0f), textX + ScaleF(8.5f), centerY - ScaleF(4.0f));
        graphics.DrawLine(&textPen, textX, centerY, textX + ScaleF(11.0f), centerY);
        graphics.DrawLine(&textPen, textX, centerY + ScaleF(4.0f), textX + ScaleF(6.5f), centerY + ScaleF(4.0f));
        return;
    }

    if (state_ == StatusPillState::Working) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - stateStarted_).count();
        const float rotation = static_cast<float>(elapsed % 800) / 800.0f * 360.0f;
        const float diameter = ScaleF(13.0f);
        Gdiplus::RectF spinnerRect(slotX, centerY - diameter / 2.0f, diameter, diameter);
        Gdiplus::Pen trackPen(ColorWithOpacity(255, 255, 255, 0.15f, opacity), ScaleF(1.5f));
        Gdiplus::Pen headPen(ColorWithOpacity(245, 158, 11, 1.0f, opacity), ScaleF(1.5f));
        graphics.DrawEllipse(&trackPen, spinnerRect);
        graphics.DrawArc(&headPen, spinnerRect, rotation, 110.0f);
        return;
    }

    if (state_ == StatusPillState::Done) {
        Gdiplus::Pen checkPen(ColorWithOpacity(16, 185, 129, 1.0f, opacity), ScaleF(1.7f));
        checkPen.SetStartCap(Gdiplus::LineCapRound);
        checkPen.SetEndCap(Gdiplus::LineCapRound);
        graphics.DrawLine(&checkPen, slotX + ScaleF(1.0f), centerY, slotX + ScaleF(5.0f), centerY + ScaleF(4.0f));
        graphics.DrawLine(&checkPen, slotX + ScaleF(5.0f), centerY + ScaleF(4.0f), slotX + ScaleF(13.0f), centerY - ScaleF(4.5f));
        return;
    }

    if (state_ == StatusPillState::Error) {
        Gdiplus::Pen alertPen(ColorWithOpacity(239, 68, 68, 1.0f, opacity), ScaleF(1.6f));
        alertPen.SetStartCap(Gdiplus::LineCapRound);
        alertPen.SetEndCap(Gdiplus::LineCapRound);
        const Gdiplus::PointF points[3] = {
            {slotX + ScaleF(7.0f), centerY - ScaleF(7.0f)},
            {slotX + ScaleF(14.0f), centerY + ScaleF(6.0f)},
            {slotX, centerY + ScaleF(6.0f)}
        };
        graphics.DrawPolygon(&alertPen, points, 3);
        graphics.DrawLine(&alertPen, slotX + ScaleF(7.0f), centerY - ScaleF(2.0f), slotX + ScaleF(7.0f), centerY + ScaleF(2.0f));
        graphics.DrawEllipse(&alertPen, slotX + ScaleF(6.6f), centerY + ScaleF(4.0f), ScaleF(0.8f), ScaleF(0.8f));
    }
}

RECT StatusPill::CalculateWindowRect(int width, int height) const {
    RECT trayRect{};
    RECT workArea{};
    NOTIFYICONIDENTIFIER identifier{};
    identifier.cbSize = sizeof(identifier);
    identifier.hWnd = ownerWindow_;
    identifier.uID = trayIconId_;

    if (Shell_NotifyIconGetRect(&identifier, &trayRect) == S_OK) {
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        HMONITOR monitor = MonitorFromRect(&trayRect, MONITOR_DEFAULTTONEAREST);
        GetMonitorInfoW(monitor, &monitorInfo);

        workArea = monitorInfo.rcWork;

        if (placement_ == StatusPillPlacement::TrayAnchor) {
            const int gap = Scale(8);
            const int workLeft = static_cast<int>(workArea.left);
            const int workTop = static_cast<int>(workArea.top);
            const int workRight = static_cast<int>(workArea.right);
            const int workBottom = static_cast<int>(workArea.bottom);
            const int centerX = trayRect.left + ((trayRect.right - trayRect.left) / 2);
            const bool horizontalTaskbar = trayRect.top >= workBottom || trayRect.bottom <= workTop;
            const int idealLeft = horizontalTaskbar
                ? workRight - width - Scale(kBaseClockAnchorRightMargin)
                : centerX - (width / 2);
            const int left = std::clamp(idealLeft, workLeft, workRight - width);
            int bottom = trayRect.top - gap;
            if (bottom - height < workTop) {
                bottom = trayRect.bottom + gap + height;
            }

            bottom = std::clamp(bottom, workTop + height, workBottom);
            return RECT{left, bottom - height, left + width, bottom};
        }
    }
    else {
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        HMONITOR monitor = MonitorFromWindow(ownerWindow_, MONITOR_DEFAULTTOPRIMARY);
        GetMonitorInfoW(monitor, &monitorInfo);
        workArea = monitorInfo.rcWork;
    }

    const int margin = Scale(12);
    switch (placement_) {
    case StatusPillPlacement::ScreenTopLeft:
        return RECT{workArea.left + margin, workArea.top + margin, workArea.left + margin + width, workArea.top + margin + height};
    case StatusPillPlacement::ScreenTopRight:
        return RECT{workArea.right - margin - width, workArea.top + margin, workArea.right - margin, workArea.top + margin + height};
    case StatusPillPlacement::ScreenBottomLeft:
        return RECT{workArea.left + margin, workArea.bottom - margin - height, workArea.left + margin + width, workArea.bottom - margin};
    case StatusPillPlacement::ScreenBottomRight:
    case StatusPillPlacement::TrayAnchor:
    default:
        return RECT{workArea.right - margin - width, workArea.bottom - margin - height, workArea.right - margin, workArea.bottom - margin};
    }
}

UINT StatusPill::CurrentDpi() const {
    if (window_ != nullptr) {
        return GetDpiForWindow(window_);
    }

    if (ownerWindow_ != nullptr) {
        return GetDpiForWindow(ownerWindow_);
    }

    return 96;
}

int StatusPill::Scale(int value) const {
    return MulDiv(value, static_cast<int>(CurrentDpi()), 96);
}

float StatusPill::ScaleF(float value) const {
    return value * static_cast<float>(CurrentDpi()) / 96.0f;
}

} // namespace voxinsert