#include "insertion/text_injector.h"

#include "observability/logging.h"

#include <windows.h>

#include <chrono>
#include <cstring>
#include <thread>

namespace voxinsert {
namespace {

constexpr int kClipboardOpenAttempts = 5;
constexpr auto kClipboardRetryDelay = std::chrono::milliseconds(50);
constexpr auto kPasteSettleDelay = std::chrono::milliseconds(125);

class ClipboardGuard {
public:
    ClipboardGuard() = default;

    ~ClipboardGuard() {
        if (open_) {
            CloseClipboard();
        }
    }

    void MarkOpen() {
        open_ = true;
    }

    ClipboardGuard(const ClipboardGuard&) = delete;
    ClipboardGuard& operator=(const ClipboardGuard&) = delete;

private:
    bool open_ = false;
};

bool OpenClipboardWithRetry(HWND ownerWindow, std::wstring& failureReason) {
    DWORD lastError = ERROR_SUCCESS;
    for (int attempt = 1; attempt <= kClipboardOpenAttempts; ++attempt) {
        if (OpenClipboard(ownerWindow) != 0) {
            return true;
        }

        lastError = GetLastError();
        if (attempt < kClipboardOpenAttempts) {
            std::this_thread::sleep_for(kClipboardRetryDelay);
        }
    }

    failureReason = L"OpenClipboard failed: ";
    failureReason += FormatWin32Error(lastError);
    return false;
}

bool SendVirtualKeys(const INPUT* inputs, UINT inputCount, std::wstring& failureReason) {
    if (SendInput(inputCount, const_cast<INPUT*>(inputs), sizeof(INPUT)) == inputCount) {
        return true;
    }

    failureReason = L"SendInput failed while sending keystrokes";
    const DWORD errorCode = GetLastError();
    if (errorCode != ERROR_SUCCESS) {
        failureReason += L": ";
        failureReason += FormatWin32Error(errorCode);
    }
    else {
        failureReason += L". The target application may have blocked injected input.";
    }

    return false;
}

} // namespace

bool TextInjector::ReadClipboardUnicodeText(
    HWND ownerWindow,
    std::wstring& text,
    bool& hasText,
    std::wstring& failureReason) const {
    text.clear();
    hasText = false;

    if (IsClipboardFormatAvailable(CF_UNICODETEXT) == 0) {
        return true;
    }

    if (!OpenClipboardWithRetry(ownerWindow, failureReason)) {
        return false;
    }

    ClipboardGuard clipboardGuard;
    clipboardGuard.MarkOpen();

    HANDLE clipboardData = GetClipboardData(CF_UNICODETEXT);
    if (clipboardData == nullptr) {
        failureReason = L"GetClipboardData(CF_UNICODETEXT) failed: ";
        failureReason += FormatWin32Error(GetLastError());
        return false;
    }

    const auto* clipboardText = static_cast<const wchar_t*>(GlobalLock(clipboardData));
    if (clipboardText == nullptr) {
        failureReason = L"GlobalLock failed for clipboard text: ";
        failureReason += FormatWin32Error(GetLastError());
        return false;
    }

    text = clipboardText;
    GlobalUnlock(clipboardData);
    hasText = true;
    return true;
}

bool TextInjector::WriteClipboardUnicodeText(
    HWND ownerWindow,
    std::wstring_view text,
    std::wstring& failureReason) const {
    if (!OpenClipboardWithRetry(ownerWindow, failureReason)) {
        return false;
    }

    ClipboardGuard clipboardGuard;
    clipboardGuard.MarkOpen();

    if (EmptyClipboard() == 0) {
        failureReason = L"EmptyClipboard failed: ";
        failureReason += FormatWin32Error(GetLastError());
        return false;
    }

    const size_t bytesRequired = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memoryHandle = GlobalAlloc(GMEM_MOVEABLE, bytesRequired);
    if (memoryHandle == nullptr) {
        failureReason = L"GlobalAlloc failed while preparing clipboard text: ";
        failureReason += FormatWin32Error(GetLastError());
        return false;
    }

    void* memory = GlobalLock(memoryHandle);
    if (memory == nullptr) {
        const DWORD errorCode = GetLastError();
        GlobalFree(memoryHandle);
        failureReason = L"GlobalLock failed while preparing clipboard text: ";
        failureReason += FormatWin32Error(errorCode);
        return false;
    }

    std::memcpy(memory, text.data(), text.size() * sizeof(wchar_t));
    static_cast<wchar_t*>(memory)[text.size()] = L'\0';
    GlobalUnlock(memoryHandle);

    if (SetClipboardData(CF_UNICODETEXT, memoryHandle) != nullptr) {
        return true;
    }

    const DWORD errorCode = GetLastError();
    GlobalFree(memoryHandle);
    failureReason = L"SetClipboardData(CF_UNICODETEXT) failed: ";
    failureReason += FormatWin32Error(errorCode);
    return false;
}

bool TextInjector::SendCtrlV(std::wstring& failureReason) const {
    INPUT inputs[4]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = 'V';
    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wVk = 'V';
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = VK_CONTROL;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;

    return SendVirtualKeys(inputs, ARRAYSIZE(inputs), failureReason);
}

bool TextInjector::SendEnter(std::wstring& failureReason) const {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_RETURN;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = VK_RETURN;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;

    return SendVirtualKeys(inputs, ARRAYSIZE(inputs), failureReason);
}

bool TextInjector::InsertText(
    HWND ownerWindow,
    const InsertionConfig& config,
    std::wstring_view text,
    std::wstring& failureReason) const {
    if (config.mode != "clipboard_paste") {
        failureReason = L"Only insertion.mode = clipboard_paste is supported right now.";
        return false;
    }

    if (text.empty()) {
        failureReason = L"Cannot insert an empty transcript.";
        return false;
    }

    std::wstring previousClipboardText;
    bool hadPreviousClipboardText = false;
    if (config.restoreClipboard &&
        !ReadClipboardUnicodeText(ownerWindow, previousClipboardText, hadPreviousClipboardText, failureReason)) {
        return false;
    }

    const auto restorePreviousClipboard = [&]() {
        if (!config.restoreClipboard || !hadPreviousClipboardText) {
            return;
        }

        std::wstring restoreFailureReason;
        if (!WriteClipboardUnicodeText(ownerWindow, previousClipboardText, restoreFailureReason)) {
            failureReason += L" Also could not restore the previous clipboard text: ";
            failureReason += restoreFailureReason;
        }
    };

    if (!WriteClipboardUnicodeText(ownerWindow, text, failureReason)) {
        return false;
    }

    if (!SendCtrlV(failureReason)) {
        restorePreviousClipboard();
        return false;
    }

    std::this_thread::sleep_for(kPasteSettleDelay);

    if (config.autoPressEnter) {
        if (!SendEnter(failureReason)) {
            restorePreviousClipboard();
            return false;
        }

        std::this_thread::sleep_for(kPasteSettleDelay);
    }

    if (config.restoreClipboard && hadPreviousClipboardText) {
        std::wstring restoreFailureReason;
        if (!WriteClipboardUnicodeText(ownerWindow, previousClipboardText, restoreFailureReason)) {
            failureReason = L"The transcript was pasted, but the previous clipboard text could not be restored: ";
            failureReason += restoreFailureReason;
            return false;
        }
    }

    return true;
}

} // namespace voxinsert