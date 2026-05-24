#pragma once

#include <windows.h>

#include <memory>
#include <string>

#include <spdlog/logger.h>

namespace voxinsert {

struct AppHostOptions {
    // Exercises the tray host and placeholder hotkey state machine without manual input.
    bool smokeTest = false;
    bool archiveSmokeTest = false;
};

AppHostOptions ParseAppHostOptions();

int RunAppHost(
    HINSTANCE instance,
    int showCommand,
    const std::shared_ptr<spdlog::logger>& logger,
    const AppHostOptions& options);

} // namespace voxinsert