#include "security/openai_credential_store.h"

#include "observability/logging.h"
#include "resource.h"

#include <windows.h>
#include <wincred.h>

#include <string>
#include <string_view>

namespace voxinsert {
namespace {

constexpr wchar_t kCredentialUserName[] = L"openai";
constexpr wchar_t kCredentialDialogTitle[] = L"VoxInsert OpenAI setup";

struct OpenAiCredentialDialogState {
    std::wstring targetName;
    std::wstring secret;
};

void ClearSecret(std::wstring& value) {
    if (!value.empty()) {
        SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
        value.clear();
    }
}

void ClearSecret(std::string& value) {
    if (!value.empty()) {
        SecureZeroMemory(value.data(), value.size());
        value.clear();
    }
}

void TrimTrailingNulls(std::wstring& value) {
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
}

std::wstring CredentialTargetFromConfig(const TranscriptionConfig& config) {
    return WideFromUtf8(config.credentialTarget);
}

bool ValidateCredentialTarget(std::wstring_view targetName, std::wstring& failureReason) {
    if (!targetName.empty()) {
        return true;
    }

    failureReason = L"The transcription credential target is empty. Set transcription.credential_target in config.json.";
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

INT_PTR CALLBACK OpenAiCredentialDialogProc(HWND dialog, UINT message, WPARAM wordParam, LPARAM longParam) {
    auto* state = reinterpret_cast<OpenAiCredentialDialogState*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));

    switch (message) {
    case WM_INITDIALOG:
        state = reinterpret_cast<OpenAiCredentialDialogState*>(longParam);
        SetWindowLongPtrW(dialog, GWLP_USERDATA, longParam);
        SetDlgItemTextW(dialog, IDC_OPENAI_TARGET_VALUE, state->targetName.c_str());
        SendDlgItemMessageW(dialog, IDC_OPENAI_API_KEY_EDIT, EM_LIMITTEXT, CRED_MAX_CREDENTIAL_BLOB_SIZE, 0);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wordParam)) {
        case IDOK: {
            const UINT characterCount = static_cast<UINT>(
                GetWindowTextLengthW(GetDlgItem(dialog, IDC_OPENAI_API_KEY_EDIT)));
            if (characterCount == 0) {
                MessageBoxW(dialog, L"Enter your OpenAI API key.", kCredentialDialogTitle, MB_OK | MB_ICONWARNING);
                return TRUE;
            }

            std::wstring buffer(static_cast<size_t>(characterCount) + 1, L'\0');
            const UINT writtenCharacters = GetDlgItemTextW(
                dialog,
                IDC_OPENAI_API_KEY_EDIT,
                buffer.data(),
                static_cast<int>(buffer.size()));
            buffer.resize(writtenCharacters);

            if (state != nullptr) {
                ClearSecret(state->secret);
                state->secret = std::move(buffer);
            }

            EndDialog(dialog, IDOK);
            return TRUE;
        }

        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;
    }

    return FALSE;
}

} // namespace

bool TryReadOpenAiCredentialSecret(
    const TranscriptionConfig& config,
    std::wstring& secret,
    std::wstring& failureReason) {
    const std::wstring targetName = CredentialTargetFromConfig(config);
    if (!ValidateCredentialTarget(targetName, failureReason)) {
        return false;
    }

    PCREDENTIALW credential = nullptr;
    if (CredReadW(targetName.c_str(), CRED_TYPE_GENERIC, 0, &credential) == 0) {
        const DWORD errorCode = GetLastError();
        if (errorCode == ERROR_NOT_FOUND) {
            failureReason = L"Credential Manager entry '";
            failureReason += targetName;
            failureReason += L"' was not found. Use Set OpenAI Key from the tray menu to create it.";
            return false;
        }

        failureReason = L"CredReadW failed for credential target '";
        failureReason += targetName;
        failureReason += L"': ";
        failureReason += FormatWin32Error(errorCode);
        return false;
    }

    secret = DecodeCredentialBlob(credential->CredentialBlob, credential->CredentialBlobSize);
    CredFree(credential);

    if (secret.empty()) {
        failureReason = L"Credential Manager entry '";
        failureReason += targetName;
        failureReason += L"' does not contain a usable secret.";
        return false;
    }

    return true;
}

bool CheckOpenAiCredentialExists(
    const TranscriptionConfig& config,
    bool& exists,
    std::wstring& failureReason) {
    exists = false;

    const std::wstring targetName = CredentialTargetFromConfig(config);
    if (!ValidateCredentialTarget(targetName, failureReason)) {
        return false;
    }

    PCREDENTIALW credential = nullptr;
    if (CredReadW(targetName.c_str(), CRED_TYPE_GENERIC, 0, &credential) == 0) {
        const DWORD errorCode = GetLastError();
        if (errorCode == ERROR_NOT_FOUND) {
            return true;
        }

        failureReason = L"CredReadW failed while checking credential target '";
        failureReason += targetName;
        failureReason += L"': ";
        failureReason += FormatWin32Error(errorCode);
        return false;
    }

    exists = true;
    CredFree(credential);
    return true;
}

bool SaveOpenAiCredential(
    const TranscriptionConfig& config,
    std::wstring_view secret,
    std::wstring& failureReason) {
    const std::wstring targetName = CredentialTargetFromConfig(config);
    if (!ValidateCredentialTarget(targetName, failureReason)) {
        return false;
    }

    if (secret.empty()) {
        failureReason = L"The OpenAI API key cannot be empty.";
        return false;
    }

    std::string utf8Secret = Utf8FromWide(secret);
    if (utf8Secret.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) {
        ClearSecret(utf8Secret);
        failureReason = L"The OpenAI API key is too long for Windows Credential Manager generic credential storage.";
        return false;
    }

    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(targetName.c_str());
    credential.UserName = const_cast<LPWSTR>(kCredentialUserName);
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.CredentialBlobSize = static_cast<DWORD>(utf8Secret.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(utf8Secret.data());

    const BOOL result = CredWriteW(&credential, 0);
    ClearSecret(utf8Secret);

    if (result != 0) {
        return true;
    }

    failureReason = L"CredWriteW failed for credential target '";
    failureReason += targetName;
    failureReason += L"': ";
    failureReason += FormatWin32Error(GetLastError());
    return false;
}

bool RemoveOpenAiCredential(
    const TranscriptionConfig& config,
    bool& removed,
    std::wstring& failureReason) {
    removed = false;

    const std::wstring targetName = CredentialTargetFromConfig(config);
    if (!ValidateCredentialTarget(targetName, failureReason)) {
        return false;
    }

    if (CredDeleteW(targetName.c_str(), CRED_TYPE_GENERIC, 0) != 0) {
        removed = true;
        return true;
    }

    const DWORD errorCode = GetLastError();
    if (errorCode == ERROR_NOT_FOUND) {
        return true;
    }

    failureReason = L"CredDeleteW failed for credential target '";
    failureReason += targetName;
    failureReason += L"': ";
    failureReason += FormatWin32Error(errorCode);
    return false;
}

OpenAiCredentialPromptResult PromptForOpenAiCredential(
    HINSTANCE instance,
    HWND owner,
    const TranscriptionConfig& config,
    std::wstring& failureReason) {
    OpenAiCredentialDialogState state{};
    state.targetName = CredentialTargetFromConfig(config);
    if (!ValidateCredentialTarget(state.targetName, failureReason)) {
        return OpenAiCredentialPromptResult::Failed;
    }

    const INT_PTR dialogResult = DialogBoxParamW(
        instance,
        MAKEINTRESOURCEW(IDD_OPENAI_KEY_DIALOG),
        owner,
        OpenAiCredentialDialogProc,
        reinterpret_cast<LPARAM>(&state));

    if (dialogResult == -1) {
        failureReason = L"DialogBoxParamW failed while showing the OpenAI setup dialog: ";
        failureReason += FormatWin32Error(GetLastError());
        ClearSecret(state.secret);
        return OpenAiCredentialPromptResult::Failed;
    }

    if (dialogResult != IDOK) {
        ClearSecret(state.secret);
        return OpenAiCredentialPromptResult::Cancelled;
    }

    const bool saved = SaveOpenAiCredential(config, state.secret, failureReason);
    ClearSecret(state.secret);
    if (!saved) {
        return OpenAiCredentialPromptResult::Failed;
    }

    return OpenAiCredentialPromptResult::Saved;
}

} // namespace voxinsert