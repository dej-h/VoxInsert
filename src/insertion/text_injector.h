#pragma once

#include "config/app_config.h"

#include <windows.h>

#include <string>
#include <string_view>

namespace voxinsert {

class TextInjector {
public:
    bool CopyTextToClipboard(
        HWND ownerWindow,
        std::wstring_view text,
        std::wstring& failureReason) const;

    bool InsertText(
        HWND ownerWindow,
        const InsertionConfig& config,
        std::wstring_view text,
        std::wstring& failureReason) const;

private:
    bool ReadClipboardUnicodeText(
        HWND ownerWindow,
        std::wstring& text,
        bool& hasText,
        std::wstring& failureReason) const;

    bool WriteClipboardUnicodeText(
        HWND ownerWindow,
        std::wstring_view text,
        std::wstring& failureReason) const;

    bool SendCtrlV(std::wstring& failureReason) const;
    bool SendEnter(std::wstring& failureReason) const;
};

} // namespace voxinsert