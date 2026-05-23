#include "transcription/transcription_client.h"

#include "observability/logging.h"
#include "security/openai_credential_store.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <thread>
#include <vector>

namespace voxinsert {
namespace {

constexpr char kOpenAiTranscriptionUrl[] = "https://api.openai.com/v1/audio/transcriptions";
constexpr int kMaxTranscriptionAttempts = 3;
constexpr int kConnectTimeoutMs = 15000;
constexpr int kRequestTimeoutMs = 120000;
constexpr int kLowSpeedBytesPerSecond = 1;
constexpr int kLowSpeedSeconds = 20;
constexpr auto kInitialRetryDelay = std::chrono::milliseconds(750);

std::string TrimBodyForError(std::string body);

bool ReadFileBytes(const std::filesystem::path& filePath, std::vector<char>& bytes, std::wstring& failureReason) {
    std::error_code errorCode;
    const std::uintmax_t fileSize = std::filesystem::file_size(filePath, errorCode);
    if (errorCode) {
        failureReason = L"Failed to inspect WAV file before upload: ";
        failureReason += filePath.wstring();
        failureReason += L" (";
        failureReason += WideFromUtf8(errorCode.message());
        failureReason += L")";
        return false;
    }

    if (fileSize == 0) {
        failureReason = L"Cannot transcribe an empty WAV file: ";
        failureReason += filePath.wstring();
        return false;
    }

    if (fileSize > static_cast<std::uintmax_t>(std::numeric_limits<size_t>::max())) {
        failureReason = L"The WAV file is too large to upload from memory in this build: ";
        failureReason += filePath.wstring();
        return false;
    }

    std::ifstream inputFile(filePath, std::ios::binary);
    if (!inputFile) {
        failureReason = L"Failed to open WAV file for upload: ";
        failureReason += filePath.wstring();
        return false;
    }

    bytes.resize(static_cast<size_t>(fileSize));
    inputFile.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!inputFile || inputFile.gcount() != static_cast<std::streamsize>(bytes.size())) {
        failureReason = L"Failed to read the complete WAV file before upload: ";
        failureReason += filePath.wstring();
        bytes.clear();
        return false;
    }

    return true;
}

bool IsRetryableHttpStatus(long statusCode) {
    return statusCode == 408 || statusCode == 425 || statusCode == 429 || (statusCode >= 500 && statusCode <= 599);
}

std::wstring BuildHttpFailureReason(const cpr::Response& response) {
    std::wstring reason = L"Transcription failed with HTTP status ";
    reason += std::to_wstring(response.status_code);

    if (!response.reason.empty()) {
        reason += L" (";
        reason += WideFromUtf8(response.reason);
        reason += L")";
    }

    std::string bodyMessage;
    try {
        const auto body = nlohmann::json::parse(response.text);
        const auto errorIt = body.find("error");
        if (errorIt != body.end() && errorIt->is_object()) {
            const auto messageIt = errorIt->find("message");
            if (messageIt != errorIt->end() && messageIt->is_string()) {
                bodyMessage = messageIt->get<std::string>();
            }
        }
    }
    catch (const nlohmann::json::exception&) {
    }

    if (bodyMessage.empty() && !response.text.empty()) {
        bodyMessage = TrimBodyForError(response.text);
    }

    if (!bodyMessage.empty()) {
        reason += L": ";
        reason += WideFromUtf8(bodyMessage);
    }

    return reason;
}

std::wstring BuildTransportFailureReason(const cpr::Response& response) {
    std::wstring reason = L"Transcription request failed before receiving a valid HTTP response";
    if (!response.error.message.empty()) {
        reason += L": ";
        reason += WideFromUtf8(response.error.message);
    }
    return reason;
}

std::wstring BuildRetryFailureReason(std::wstring failureReason, int attemptCount) {
    if (attemptCount <= 1) {
        return failureReason;
    }

    failureReason += L" after ";
    failureReason += std::to_wstring(attemptCount);
    failureReason += L" attempts.";
    return failureReason;
}

std::string TrimBodyForError(std::string body) {
    constexpr size_t kMaxErrorBodyLength = 512;
    if (body.size() <= kMaxErrorBodyLength) {
        return body;
    }

    body.resize(kMaxErrorBodyLength);
    body += "...";
    return body;
}

} // namespace

bool TranscriptionClient::Transcribe(
    const TranscriptionConfig& config,
    const std::filesystem::path& wavPath,
    std::string& transcript,
    std::wstring& failureReason) const {
    if (!std::filesystem::exists(wavPath)) {
        failureReason = L"Cannot transcribe because the WAV file does not exist: ";
        failureReason += wavPath.wstring();
        return false;
    }

    if (config.provider != "openai") {
        failureReason = L"Only the OpenAI transcription provider is supported right now.";
        return false;
    }

    std::wstring apiKeyWide;
    if (!TryReadOpenAiCredentialSecret(config, apiKeyWide, failureReason)) {
        return false;
    }

    std::vector<char> wavBytes;
    if (!ReadFileBytes(wavPath, wavBytes, failureReason)) {
        return false;
    }

    const std::string apiKey = Utf8FromWide(apiKeyWide);
    const cpr::Buffer wavBuffer{wavBytes.begin(), wavBytes.end(), wavPath.filename()};

    std::wstring lastFailureReason;
    for (int attempt = 1; attempt <= kMaxTranscriptionAttempts; ++attempt) {
        const cpr::Multipart multipart{
            {"model", config.model},
            {"language", config.languageHint},
            {"prompt", config.prompt},
            {"response_format", "json"},
            {"file", wavBuffer, "audio/wav"}
        };

        const cpr::Response response = cpr::Post(
            cpr::Url{kOpenAiTranscriptionUrl},
            cpr::Header{{"Authorization", "Bearer " + apiKey}},
            multipart,
            cpr::ConnectTimeout{kConnectTimeoutMs},
            cpr::Timeout{kRequestTimeoutMs},
            cpr::LowSpeed{kLowSpeedBytesPerSecond, std::chrono::seconds{kLowSpeedSeconds}});

        if (response.error.code == cpr::ErrorCode::OK) {
            if (response.status_code >= 200 && response.status_code < 300) {
                try {
                    const auto body = nlohmann::json::parse(response.text);
                    transcript = body.at("text").get<std::string>();
                    return true;
                }
                catch (const nlohmann::json::exception& exception) {
                    failureReason = L"Failed to parse transcription response JSON: ";
                    failureReason += WideFromUtf8(exception.what());
                    return false;
                }
            }

            lastFailureReason = BuildHttpFailureReason(response);
            if (!IsRetryableHttpStatus(response.status_code) || attempt == kMaxTranscriptionAttempts) {
                failureReason = BuildRetryFailureReason(lastFailureReason, attempt);
                return false;
            }
        }
        else {
            lastFailureReason = BuildTransportFailureReason(response);
            if (attempt == kMaxTranscriptionAttempts) {
                failureReason = BuildRetryFailureReason(lastFailureReason, attempt);
                return false;
            }
        }

        std::this_thread::sleep_for(kInitialRetryDelay * attempt);
    }

    failureReason = BuildRetryFailureReason(lastFailureReason, kMaxTranscriptionAttempts);
    return false;
}

} // namespace voxinsert