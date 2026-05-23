#pragma once

#include "config/app_config.h"

#include <filesystem>
#include <string>

namespace voxinsert {

class TranscriptionClient {
public:
    bool Transcribe(
        const TranscriptionConfig& config,
        const std::filesystem::path& wavPath,
        std::string& transcript,
        std::wstring& failureReason) const;
};

} // namespace voxinsert