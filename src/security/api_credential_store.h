#pragma once

#include <string>
#include <string_view>

namespace voxinsert {

bool TryReadApiCredentialSecret(
    std::wstring_view targetName,
    std::wstring& secret,
    std::wstring& failureReason);

bool CheckApiCredentialExists(
    std::wstring_view targetName,
    bool& exists,
    std::wstring& failureReason);

bool SaveApiCredential(
    std::wstring_view targetName,
    std::wstring_view userName,
    std::wstring_view secret,
    std::wstring& failureReason);

bool RemoveApiCredential(
    std::wstring_view targetName,
    bool& removed,
    std::wstring& failureReason);

} // namespace voxinsert