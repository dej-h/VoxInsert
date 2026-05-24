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
constexpr int kAmplitudeEncodingScale = 10000;
constexpr int kBasePillHeight = 28;
constexpr int kBaseMinimumPillWidth = 60;
constexpr int kBasePaddingX = 12;
constexpr int kBaseAnchorSize = 14;
constexpr int kBaseSeparatorHeight = 12;
constexpr int kBaseSeparatorGap = 8;
constexpr int kBaseStateSlotWidth = 32;
constexpr int kBaseAnchorGap = 0;
constexpr int kBaseClockAnchorRightMargin = 12;
constexpr int kBaseTrayAnchorGap = 8;
constexpr int kBaseScreenMargin = 12;
constexpr int kBaseSeparatorWidth = 1;
constexpr float kPillBoundsInset = 0.5f;
constexpr float kPillBoundsTrim = 1.0f;
constexpr float kPillCornerRadius = 14.0f;
constexpr float kBorderPenWidth = 0.5f;

struct RgbColor {
    BYTE red;
    BYTE green;
    BYTE blue;
};

struct PillPalette {
    RgbColor fillColor;
    float fillAlpha;
    RgbColor borderColor;
    float borderAlpha;
    RgbColor separatorColor;
    float separatorAlpha;
};

struct RecordingMeterLayout {
    std::array<float, 5> barProfile;
    float barWidth;
    float barGap;
    float minHeight;
    float maxHeight;
};

struct TranscribingLayout {
    std::array<float, 3> waveHeights;
    float pulsePeriodMs;
    float waveBarWidth;
    float waveBarGap;
    float waveMinHeight;
    float waveMaxHeight;
    float waveBaseScale;
    float waveScaleRange;
    float wavePhaseOffset;
    float accentPenWidth;
    float textPenWidth;
    float textAlpha;
    float arrowXOffset;
    float arrowShaftLength;
    float arrowWingStartX;
    float arrowWingYOffset;
    float arrowWingEndX;
    float textXOffset;
    float textLineYOffset;
    float textTopLength;
    float textMiddleLength;
    float textBottomLength;
};

struct SpinnerLayout {
    int rotationPeriodMs;
    float diameter;
    float penWidth;
    float trackAlpha;
    float arcSpanDegrees;
};

struct CheckmarkLayout {
    float penWidth;
    float startX;
    float midX;
    float midYOffset;
    float endX;
    float endYOffset;
};

struct AlertLayout {
    float penWidth;
    float apexX;
    float apexYOffset;
    float rightX;
    float baseYOffset;
    float exclamationTopYOffset;
    float exclamationBottomYOffset;
    float dotX;
    float dotYOffset;
    float dotSize;
};

constexpr RgbColor kNeutralBackgroundRgb{28, 28, 30};
constexpr RgbColor kNeutralForegroundRgb{255, 255, 255};
constexpr RgbColor kErrorRgb{239, 68, 68};
constexpr RgbColor kWorkingAccentRgb{245, 158, 11};
constexpr RgbColor kDoneAccentRgb{16, 185, 129};

constexpr PillPalette kDefaultPillPalette{
    kNeutralBackgroundRgb,
    0.92f,
    kNeutralForegroundRgb,
    0.14f,
    kNeutralForegroundRgb,
    0.18f,
};

constexpr PillPalette kErrorPillPalette{
    kErrorRgb,
    0.12f,
    kErrorRgb,
    0.30f,
    kErrorRgb,
    0.30f,
};

constexpr RecordingMeterLayout kRecordingMeterLayout{
    {0.42f, 0.68f, 1.0f, 0.68f, 0.42f},
    3.0f,
    2.5f,
    1.75f,
    18.0f,
};

constexpr TranscribingLayout kTranscribingLayout{
    {0.48f, 0.92f, 0.62f},
    220.0f,
    2.2f,
    1.8f,
    4.0f,
    10.0f,
    0.78f,
    0.22f,
    0.95f,
    1.35f,
    1.2f,
    0.72f,
    9.5f,
    5.0f,
    3.1f,
    2.5f,
    5.6f,
    17.5f,
    4.0f,
    8.5f,
    11.0f,
    6.5f,
};

constexpr SpinnerLayout kWorkingSpinnerLayout{
    800,
    13.0f,
    1.5f,
    0.15f,
    110.0f,
};

constexpr CheckmarkLayout kDoneCheckmarkLayout{
    1.7f,
    1.0f,
    5.0f,
    4.0f,
    13.0f,
    4.5f,
};

constexpr AlertLayout kErrorAlertLayout{
    1.6f,
    7.0f,
    7.0f,
    14.0f,
    6.0f,
    2.0f,
    2.0f,
    6.6f,
    4.0f,
    0.8f,
};

Gdiplus::Color ColorWithOpacity(BYTE red, BYTE green, BYTE blue, float alpha, float opacity) {
    const int scaledAlpha = std::clamp(static_cast<int>(alpha * opacity * 255.0f), 0, 255);
    return Gdiplus::Color(static_cast<BYTE>(scaledAlpha), red, green, blue);
}

Gdiplus::Color ColorWithOpacity(const RgbColor& color, float alpha, float opacity) {
    return ColorWithOpacity(color.red, color.green, color.blue, alpha, opacity);
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

    const int encodedAmplitude = std::clamp(static_cast<int>(rms * static_cast<float>(kAmplitudeEncodingScale)), 0, kAmplitudeEncodingScale);
    PostMessageW(window_, kAmplitudeMessage, static_cast<WPARAM>(encodedAmplitude), 0);
}

void StatusPill::Reanchor() {
    if (!enabled_ || window_ == nullptr) {
        return;
    }

    const int width = std::max(
        Scale(kBaseMinimumPillWidth),
        (Scale(kBasePaddingX) * 2) + Scale(kBaseAnchorSize) + Scale(kBaseAnchorGap) +
            Scale(kBaseSeparatorGap) + Scale(kBaseSeparatorWidth) + Scale(kBaseSeparatorGap) + Scale(kBaseStateSlotWidth));
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
        HandleAmplitudeSample(static_cast<float>(wordParam) / static_cast<float>(kAmplitudeEncodingScale));
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
    const PillPalette& palette = isError ? kErrorPillPalette : kDefaultPillPalette;
    const Gdiplus::RectF bounds(
        kPillBoundsInset,
        kPillBoundsInset,
        static_cast<float>(width) - kPillBoundsTrim,
        static_cast<float>(height) - kPillBoundsTrim);
    Gdiplus::GraphicsPath backgroundPath;
    AddRoundedRectangle(backgroundPath, bounds, ScaleF(kPillCornerRadius));

    const auto fillColor = ColorWithOpacity(palette.fillColor, palette.fillAlpha, opacity);
    const auto borderColor = ColorWithOpacity(palette.borderColor, palette.borderAlpha, opacity);
    Gdiplus::SolidBrush fillBrush(fillColor);
    Gdiplus::Pen borderPen(borderColor, ScaleF(kBorderPenWidth));
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
    const auto separatorColor = ColorWithOpacity(palette.separatorColor, palette.separatorAlpha, opacity);
    Gdiplus::Pen separatorPen(separatorColor, ScaleF(static_cast<float>(kBaseSeparatorWidth)));
    graphics.DrawLine(&separatorPen, separatorX, centerY - separatorHeight / 2.0f, separatorX, centerY + separatorHeight / 2.0f);

    const float slotX = separatorX + ScaleF(static_cast<float>(kBaseSeparatorWidth)) + ScaleF(static_cast<float>(kBaseSeparatorGap));

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

        const float barWidth = ScaleF(kRecordingMeterLayout.barWidth);
        const float barGap = ScaleF(kRecordingMeterLayout.barGap);
        const float minHeight = ScaleF(kRecordingMeterLayout.minHeight);
        const float maxHeight = ScaleF(kRecordingMeterLayout.maxHeight);
        Gdiplus::SolidBrush recordingBrush(ColorWithOpacity(kErrorRgb, 1.0f, opacity));

        for (size_t index = 0; index < kRecordingMeterLayout.barProfile.size(); ++index) {
            const float profile = kRecordingMeterLayout.barProfile[index] +
                ((1.0f - kRecordingMeterLayout.barProfile[index]) * displayedAmplitude_);
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
        const float pulsePhase = static_cast<float>(elapsed) / kTranscribingLayout.pulsePeriodMs;
        const auto accentColor = ColorWithOpacity(kWorkingAccentRgb, 1.0f, opacity);
        const auto textColor = ColorWithOpacity(kNeutralForegroundRgb, kTranscribingLayout.textAlpha, opacity);
        Gdiplus::SolidBrush accentBrush(accentColor);
        Gdiplus::Pen accentPen(accentColor, ScaleF(kTranscribingLayout.accentPenWidth));
        accentPen.SetStartCap(Gdiplus::LineCapRound);
        accentPen.SetEndCap(Gdiplus::LineCapRound);
        Gdiplus::Pen textPen(textColor, ScaleF(kTranscribingLayout.textPenWidth));
        textPen.SetStartCap(Gdiplus::LineCapRound);
        textPen.SetEndCap(Gdiplus::LineCapRound);

        const float waveBarWidth = ScaleF(kTranscribingLayout.waveBarWidth);
        const float waveBarGap = ScaleF(kTranscribingLayout.waveBarGap);
        const float waveMinHeight = ScaleF(kTranscribingLayout.waveMinHeight);
        const float waveMaxHeight = ScaleF(kTranscribingLayout.waveMaxHeight);
        for (size_t index = 0; index < kTranscribingLayout.waveHeights.size(); ++index) {
            const float animatedScale = kTranscribingLayout.waveBaseScale +
                kTranscribingLayout.waveScaleRange * std::sin(pulsePhase + static_cast<float>(index) * kTranscribingLayout.wavePhaseOffset);
            const float barHeight = waveMinHeight +
                (waveMaxHeight - waveMinHeight) * kTranscribingLayout.waveHeights[index] * animatedScale;
            const float x = slotX + static_cast<float>(index) * (waveBarWidth + waveBarGap);
            const float y = centerY - barHeight / 2.0f;
            Gdiplus::GraphicsPath barPath;
            AddRoundedRectangle(barPath, Gdiplus::RectF(x, y, waveBarWidth, barHeight), waveBarWidth / 2.0f);
            graphics.FillPath(&accentBrush, &barPath);
        }

        const float arrowX = slotX + ScaleF(kTranscribingLayout.arrowXOffset);
        graphics.DrawLine(&accentPen, arrowX, centerY, arrowX + ScaleF(kTranscribingLayout.arrowShaftLength), centerY);
        graphics.DrawLine(
            &accentPen,
            arrowX + ScaleF(kTranscribingLayout.arrowWingStartX),
            centerY - ScaleF(kTranscribingLayout.arrowWingYOffset),
            arrowX + ScaleF(kTranscribingLayout.arrowWingEndX),
            centerY);
        graphics.DrawLine(
            &accentPen,
            arrowX + ScaleF(kTranscribingLayout.arrowWingStartX),
            centerY + ScaleF(kTranscribingLayout.arrowWingYOffset),
            arrowX + ScaleF(kTranscribingLayout.arrowWingEndX),
            centerY);

        const float textX = slotX + ScaleF(kTranscribingLayout.textXOffset);
        graphics.DrawLine(
            &textPen,
            textX,
            centerY - ScaleF(kTranscribingLayout.textLineYOffset),
            textX + ScaleF(kTranscribingLayout.textTopLength),
            centerY - ScaleF(kTranscribingLayout.textLineYOffset));
        graphics.DrawLine(&textPen, textX, centerY, textX + ScaleF(kTranscribingLayout.textMiddleLength), centerY);
        graphics.DrawLine(
            &textPen,
            textX,
            centerY + ScaleF(kTranscribingLayout.textLineYOffset),
            textX + ScaleF(kTranscribingLayout.textBottomLength),
            centerY + ScaleF(kTranscribingLayout.textLineYOffset));
        return;
    }

    if (state_ == StatusPillState::Working) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - stateStarted_).count();
        const float rotation =
            static_cast<float>(elapsed % kWorkingSpinnerLayout.rotationPeriodMs) /
            static_cast<float>(kWorkingSpinnerLayout.rotationPeriodMs) * 360.0f;
        const float diameter = ScaleF(kWorkingSpinnerLayout.diameter);
        Gdiplus::RectF spinnerRect(slotX, centerY - diameter / 2.0f, diameter, diameter);
        Gdiplus::Pen trackPen(
            ColorWithOpacity(kNeutralForegroundRgb, kWorkingSpinnerLayout.trackAlpha, opacity),
            ScaleF(kWorkingSpinnerLayout.penWidth));
        Gdiplus::Pen headPen(ColorWithOpacity(kWorkingAccentRgb, 1.0f, opacity), ScaleF(kWorkingSpinnerLayout.penWidth));
        graphics.DrawEllipse(&trackPen, spinnerRect);
        graphics.DrawArc(&headPen, spinnerRect, rotation, kWorkingSpinnerLayout.arcSpanDegrees);
        return;
    }

    if (state_ == StatusPillState::Done) {
        Gdiplus::Pen checkPen(ColorWithOpacity(kDoneAccentRgb, 1.0f, opacity), ScaleF(kDoneCheckmarkLayout.penWidth));
        checkPen.SetStartCap(Gdiplus::LineCapRound);
        checkPen.SetEndCap(Gdiplus::LineCapRound);
        graphics.DrawLine(
            &checkPen,
            slotX + ScaleF(kDoneCheckmarkLayout.startX),
            centerY,
            slotX + ScaleF(kDoneCheckmarkLayout.midX),
            centerY + ScaleF(kDoneCheckmarkLayout.midYOffset));
        graphics.DrawLine(
            &checkPen,
            slotX + ScaleF(kDoneCheckmarkLayout.midX),
            centerY + ScaleF(kDoneCheckmarkLayout.midYOffset),
            slotX + ScaleF(kDoneCheckmarkLayout.endX),
            centerY - ScaleF(kDoneCheckmarkLayout.endYOffset));
        return;
    }

    if (state_ == StatusPillState::Error) {
        Gdiplus::Pen alertPen(ColorWithOpacity(kErrorRgb, 1.0f, opacity), ScaleF(kErrorAlertLayout.penWidth));
        alertPen.SetStartCap(Gdiplus::LineCapRound);
        alertPen.SetEndCap(Gdiplus::LineCapRound);
        const Gdiplus::PointF points[3] = {
            {slotX + ScaleF(kErrorAlertLayout.apexX), centerY - ScaleF(kErrorAlertLayout.apexYOffset)},
            {slotX + ScaleF(kErrorAlertLayout.rightX), centerY + ScaleF(kErrorAlertLayout.baseYOffset)},
            {slotX, centerY + ScaleF(kErrorAlertLayout.baseYOffset)}
        };
        graphics.DrawPolygon(&alertPen, points, 3);
        graphics.DrawLine(
            &alertPen,
            slotX + ScaleF(kErrorAlertLayout.apexX),
            centerY - ScaleF(kErrorAlertLayout.exclamationTopYOffset),
            slotX + ScaleF(kErrorAlertLayout.apexX),
            centerY + ScaleF(kErrorAlertLayout.exclamationBottomYOffset));
        graphics.DrawEllipse(
            &alertPen,
            slotX + ScaleF(kErrorAlertLayout.dotX),
            centerY + ScaleF(kErrorAlertLayout.dotYOffset),
            ScaleF(kErrorAlertLayout.dotSize),
            ScaleF(kErrorAlertLayout.dotSize));
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
            const int gap = Scale(kBaseTrayAnchorGap);
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

    const int margin = Scale(kBaseScreenMargin);
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