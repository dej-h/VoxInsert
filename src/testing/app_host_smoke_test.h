#pragma once

#include <windows.h>

#include <memory>

#include <spdlog/logger.h>

namespace voxinsert {

// Keeps the host-only smoke sequence out of the production window procedure logic.
class AppHostSmokeTest {
public:
    static constexpr UINT_PTR kTimerId = 1;

    bool ArmInitialTimer(HWND window, const std::shared_ptr<spdlog::logger>& logger) noexcept;
    bool HandleTimer(HWND window, const std::shared_ptr<spdlog::logger>& logger) noexcept;

private:
    bool ArmTimer(HWND window, const std::shared_ptr<spdlog::logger>& logger, UINT delayMs) noexcept;

    unsigned int step_ = 0;
};

} // namespace voxinsert