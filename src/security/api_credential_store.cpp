#include "security/api_credential_store.h"

#include "observability/logging.h"

#include <windows.h>
#include <wincred.h>

#include <string>

namespace voxinsert {
namespace {

void TrimTrailingNulls(std::wstring& value) {
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
}

bool ValidateCredentialTarget(std::wstring_view targetName, std::wstring& failureReason) {
    if (!targetName.empty()) {
        return true;
    }

    failureReason = L"The transcription credential target is empty.";
    return false;
}

bool LooksLikeUtf16LeText(const BYTE* bytes, DWORD byteCount) {
    if (bytes == nullptr || byteCount == 0 || (byteCount % sizeof(wchar_t)) != 0) {
        return false;
    }

    for (DWORD index = 1; index < byteCount; index += sizeof(wchar_t)) {
        if (bytes[index] != 0) {
            return false;
        }
    }

    return true;
}

std::wstring DecodeCredentialBlob(const BYTE* bytes, DWORD byteCount) {
    if (LooksLikeUtf16LeText(bytes, byteCount)) {
        const auto* wideCharacters = reinterpret_cast<const wchar_t*>(bytes);
        std::wstring value(wideCharacters, wideCharacters + (byteCount / sizeof(wchar_t)));
        TrimTrailingNulls(value);
        return value;
    }

    std::wstring value = WideFromUtf8(std::string(
        reinterpret_cast<const char*>(bytes),
        reinterpret_cast<const char*>(bytes) + byteCount));
    TrimTrailingNulls(value);
    return value;
}

} // namespace

bool TryReadApiCredentialSecret(
    std::wstring_view targetName,
    std::wstring& secret,
    std::wstring& failureReason) {
    if (!ValidateCredentialTarget(targetName, failureReason)) {
        return false;
    }

    PCREDENTIALW credential = nullptr;
    if (CredReadW(std::wstring(targetName).c_str(), CRED_TYPE_GENERIC, 0, &credential) == 0) {
        const DWORD errorCode = GetLastError();
        if (errorCode == ERROR_NOT_FOUND) {
            failureReason = L"Credential Manager entry '";
            failureReason += std::wstring(targetName);
            failureReason += L"' was not found. Open Settings to create it.";
            return false;
        }

        failureReason = L"CredReadW failed for credential target '";
        failureReason += std::wstring(targetName);
        failureReason += L"': ";
        failureReason += FormatWin32Error(errorCode);
        return false;
    }

    secret = DecodeCredentialBlob(credential->CredentialBlob, credential->CredentialBlobSize);
    CredFree(credential);

    if (secret.empty()) {
        failureReason = L"Credential Manager entry '";
        failureReason += std::wstring(targetName);
        failureReason += L"' does not contain a usable secret.";
        return false;
    }

    return true;
}

bool CheckApiCredentialExists(
    std::wstring_view targetName,
    bool& exists,
    std::wstring& failureReason) {
    exists = false;

    if (!ValidateCredentialTarget(targetName, failureReason)) {
        return false;
    }

    PCREDENTIALW credential = nullptr;
    if (CredReadW(std::wstring(targetName).c_str(), CRED_TYPE_GENERIC, 0, &credential) == 0) {
        const DWORD errorCode = GetLastError();
        if (errorCode == ERROR_NOT_FOUND) {
            return true;
        }

        failureReason = L"CredReadW failed while checking credential target '";
        failureReason += std::wstring(targetName);
        failureReason += L"': ";
        failureReason += FormatWin32Error(errorCode);
        return false;
    }

    exists = true;
    CredFree(credential);
    return true;
}

bool SaveApiCredential(
    std::wstring_view targetName,
    std::wstring_view userName,
    std::wstring_view secret,
    std::wstring& failureReason) {
    if (!ValidateCredentialTarget(targetName, failureReason)) {
        return false;
    }

    if (secret.empty()) {
        failureReason = L"The API key cannot be empty.";
        return false;
    }

    std::string utf8Secret = Utf8FromWide(secret);
    if (utf8Secret.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) {
        SecureZeroMemory(utf8Secret.data(), utf8Secret.size());
        utf8Secret.clear();
        failureReason = L"The API key is too long for Windows Credential Manager generic credential storage.";
        return false;
    }

    std::wstring target(targetName);
    std::wstring user(userName);
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = target.data();
    credential.UserName = user.data();
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.CredentialBlobSize = static_cast<DWORD>(utf8Secret.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(utf8Secret.data());

    const BOOL result = CredWriteW(&credential, 0);
    SecureZeroMemory(utf8Secret.data(), utf8Secret.size());
    utf8Secret.clear();

    if (result != 0) {
        return true;
    }

    failureReason = L"CredWriteW failed for credential target '";
    failureReason += target;
    failureReason += L"': ";
    failureReason += FormatWin32Error(GetLastError());
    return false;
}

bool RemoveApiCredential(
    std::wstring_view targetName,
    bool& removed,
    std::wstring& failureReason) {
    removed = false;

    if (!ValidateCredentialTarget(targetName, failureReason)) {
        return false;
    }

    if (CredDeleteW(std::wstring(targetName).c_str(), CRED_TYPE_GENERIC, 0) != 0) {
        removed = true;
        return true;
    }

    const DWORD errorCode = GetLastError();
    if (errorCode == ERROR_NOT_FOUND) {
        return true;
    }

    failureReason = L"CredDeleteW failed for credential target '";
    failureReason += std::wstring(targetName);
    failureReason += L"': ";
    failureReason += FormatWin32Error(errorCode);
    return false;
}

} // namespace voxinsert