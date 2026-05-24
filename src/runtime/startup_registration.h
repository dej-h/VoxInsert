#pragma once

#include "config/app_config.h"

#include <string>

namespace voxinsert {

bool ApplyStartupRegistration(const SystemConfig& config, std::wstring& failureReason);

} // namespace voxinsert