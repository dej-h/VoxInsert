#pragma once

#include "config/app_config.h"

#include <string>

namespace voxinsert {

struct AppContext;

bool ApplyRuntimeConfig(AppContext& context, AppConfig nextConfig, std::wstring& failureReason);

} // namespace voxinsert