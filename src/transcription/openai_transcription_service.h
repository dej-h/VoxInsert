#pragma once

#include "transcription/transcription_service.h"

namespace voxinsert {

class OpenAiTranscriptionService final : public ITranscriptionService {
public:
    std::string_view ProviderId() const noexcept override;
    bool Transcribe(
        const TranscriptionConfig& config,
        const std::filesystem::path& wavPath,
        std::string& transcript,
        std::wstring& failureReason) const override;
};

} // namespace voxinsert