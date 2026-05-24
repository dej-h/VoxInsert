#pragma once

#include "config/app_config.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace voxinsert {

class ITranscriptionService {
public:
    virtual ~ITranscriptionService() = default;

    virtual std::string_view ProviderId() const noexcept = 0;
    virtual bool Transcribe(
        const TranscriptionConfig& config,
        const std::filesystem::path& wavPath,
        std::string& transcript,
        std::wstring& failureReason) const = 0;
};

} // namespace voxinsert