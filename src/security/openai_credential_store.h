#pragma once

#include "config/app_config.h"

#include <windows.h>

#include <string>
#include <string_view>

namespace voxinsert {

enum class OpenAiCredentialPromptResult {
    Saved,
    Cancelled,
    Failed
};

bool TryReadOpenAiCredentialSecret(
    const TranscriptionConfig& config,
    std::wstring& secret,
    std::wstring& failureReason);

bool CheckOpenAiCredentialExists(
    const TranscriptionConfig& config,
    bool& exists,
    std::wstring& failureReason);

bool SaveOpenAiCredential(
    const TranscriptionConfig& config,
    std::wstring_view secret,
    std::wstring& failureReason);

bool RemoveOpenAiCredential(
    const TranscriptionConfig& config,
    bool& removed,
    std::wstring& failureReason);

OpenAiCredentialPromptResult PromptForOpenAiCredential(
    HINSTANCE instance,
    HWND owner,
    const TranscriptionConfig& config,
    std::wstring& failureReason);

} // namespace voxinsert