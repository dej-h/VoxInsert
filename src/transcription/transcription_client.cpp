#include "transcription/transcription_client.h"
#include "transcription/mistral_transcription_service.h"
#include "transcription/openai_transcription_service.h"

#include "observability/logging.h"

namespace voxinsert {
namespace {

const ITranscriptionService* ResolveService(std::string_view provider) {
    static const OpenAiTranscriptionService kOpenAiService;
    static const MistralTranscriptionService kMistralService;

    if (provider == kOpenAiService.ProviderId()) {
        return &kOpenAiService;
    }

    if (provider == kMistralService.ProviderId()) {
        return &kMistralService;
    }

    return nullptr;
}

} // namespace

bool TranscriptionClient::Transcribe(
    const TranscriptionConfig& config,
    const std::filesystem::path& wavPath,
    std::string& transcript,
    std::wstring& failureReason) const {
    const ITranscriptionService* service = ResolveService(config.provider);
    if (service == nullptr) {
        failureReason = L"Unsupported transcription provider: ";
        failureReason += WideFromUtf8(config.provider);
        return false;
    }

    return service->Transcribe(config, wavPath, transcript, failureReason);
}

} // namespace voxinsert