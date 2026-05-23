#include "testing/app_host_smoke_test.h"

#include "input/hotkey_manager.h"
#include "observability/logging.h"

namespace voxinsert {
namespace {

constexpr UINT kStepDelayMs = 200;

} // namespace

bool AppHostSmokeTest::ArmTimer(HWND window, const std::shared_ptr<spdlog::logger>& logger, UINT delayMs) noexcept {
    if (SetTimer(window, kTimerId, delayMs, nullptr) != 0) {
        return true;
    }

    logger->error(
        "SetTimer failed for smoke test: {}",
        Utf8FromWide(FormatWin32Error(GetLastError())));
    return false;
}

bool AppHostSmokeTest::ArmInitialTimer(HWND window, const std::shared_ptr<spdlog::logger>& logger) noexcept {
    step_ = 0;
    return ArmTimer(window, logger, kStepDelayMs);
}

bool AppHostSmokeTest::HandleTimer(HWND window, const std::shared_ptr<spdlog::logger>& logger) noexcept {
    switch (step_++) {
    case 0:
        logger->info("smoke-test: simulating toggle start");
        PostMessageW(window, WM_HOTKEY, HotkeyManager::kToggleRecordingHotkeyId, 0);
        return ArmTimer(window, logger, kStepDelayMs);

    case 1:
        logger->info("smoke-test: simulating cancel");
        PostMessageW(window, WM_HOTKEY, HotkeyManager::kCancelRecordingHotkeyId, 0);
        return ArmTimer(window, logger, kStepDelayMs);

    case 2:
        logger->info("smoke-test: simulating toggle start again");
        PostMessageW(window, WM_HOTKEY, HotkeyManager::kToggleRecordingHotkeyId, 0);
        return ArmTimer(window, logger, kStepDelayMs);

    case 3:
        logger->info("smoke-test: simulating toggle stop");
        PostMessageW(window, WM_HOTKEY, HotkeyManager::kToggleRecordingHotkeyId, 0);
        return ArmTimer(window, logger, kStepDelayMs * 2);

    default:
        logger->info("smoke-test sequence complete; closing hidden window");
        DestroyWindow(window);
        return true;
    }
}

} // namespace voxinsert