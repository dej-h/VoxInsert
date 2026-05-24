#include "ui/settings_dialog.h"

#include "resource.h"

#include <commctrl.h>

namespace voxinsert {
namespace {

constexpr wchar_t kDefaultHotkeyHint[] =
    L"Click a hotkey field, then press the shortcut you want. VoxInsert pauses global hotkeys while this dialog is open.";

struct PlacementOption {
    StatusPillPlacement placement;
    const wchar_t* label;
};

constexpr PlacementOption kPlacementOptions[] = {
    {StatusPillPlacement::TrayAnchor, L"Near tray icon"},
    {StatusPillPlacement::ScreenTopLeft, L"Top left"},
    {StatusPillPlacement::ScreenTopRight, L"Top right"},
    {StatusPillPlacement::ScreenBottomLeft, L"Bottom left"},
    {StatusPillPlacement::ScreenBottomRight, L"Bottom right"},
};

BYTE HotkeyFlagsFromModifiers(UINT modifiers) {
    BYTE flags = 0;
    if ((modifiers & MOD_CONTROL) != 0) {
        flags |= HOTKEYF_CONTROL;
    }

    if ((modifiers & MOD_ALT) != 0) {
        flags |= HOTKEYF_ALT;
    }

    if ((modifiers & MOD_SHIFT) != 0) {
        flags |= HOTKEYF_SHIFT;
    }

    return flags;
}

UINT ModifiersFromHotkeyFlags(BYTE flags) {
    UINT modifiers = 0;
    if ((flags & HOTKEYF_CONTROL) != 0) {
        modifiers |= MOD_CONTROL;
    }

    if ((flags & HOTKEYF_ALT) != 0) {
        modifiers |= MOD_ALT;
    }

    if ((flags & HOTKEYF_SHIFT) != 0) {
        modifiers |= MOD_SHIFT;
    }

    return modifiers;
}

WORD HotkeyWordFromBinding(const HotkeyBinding& binding) {
    return MAKEWORD(binding.virtualKey, HotkeyFlagsFromModifiers(binding.modifiers));
}

void SetHotkeyControl(HWND dialog, int controlId, const HotkeyBinding& binding) {
    SendDlgItemMessageW(dialog, controlId, HKM_SETHOTKEY, HotkeyWordFromBinding(binding), 0);
}

bool ReadHotkeyControl(HWND dialog, int controlId, HotkeyBinding& binding, std::wstring& failureReason) {
    const DWORD hotkeyValue = static_cast<DWORD>(SendDlgItemMessageW(dialog, controlId, HKM_GETHOTKEY, 0, 0));
    const UINT virtualKey = LOBYTE(hotkeyValue);
    const UINT modifiers = ModifiersFromHotkeyFlags(HIBYTE(hotkeyValue));
    return TryCreateHotkeyBinding(modifiers, virtualKey, binding, failureReason);
}

void PopulatePlacementCombo(HWND dialog, StatusPillPlacement selectedPlacement) {
    const HWND combo = GetDlgItem(dialog, IDC_SETTINGS_STATUS_PILL_POSITION);
    if (combo == nullptr) {
        return;
    }

    int selectedIndex = 0;
    for (size_t index = 0; index < ARRAYSIZE(kPlacementOptions); ++index) {
        const int itemIndex = static_cast<int>(SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kPlacementOptions[index].label)));
        SendMessageW(combo, CB_SETITEMDATA, itemIndex, static_cast<LPARAM>(kPlacementOptions[index].placement));
        if (kPlacementOptions[index].placement == selectedPlacement) {
            selectedIndex = itemIndex;
        }
    }

    SendMessageW(combo, CB_SETCURSEL, selectedIndex, 0);
}

StatusPillPlacement ReadPlacementSelection(HWND dialog) {
    const int selectedIndex = static_cast<int>(SendDlgItemMessageW(dialog, IDC_SETTINGS_STATUS_PILL_POSITION, CB_GETCURSEL, 0, 0));
    if (selectedIndex == CB_ERR) {
        return StatusPillPlacement::TrayAnchor;
    }

    return static_cast<StatusPillPlacement>(SendDlgItemMessageW(dialog, IDC_SETTINGS_STATUS_PILL_POSITION, CB_GETITEMDATA, selectedIndex, 0));
}

void SetHotkeyHint(HWND dialog, std::wstring_view text) {
    SetDlgItemTextW(dialog, IDC_SETTINGS_HOTKEY_HINT, std::wstring(text).c_str());
}

void UpdatePlacementEnabledState(HWND dialog) {
    const BOOL enabled = IsDlgButtonChecked(dialog, IDC_SETTINGS_SHOW_STATUS_PILL) == BST_CHECKED;
    EnableWindow(GetDlgItem(dialog, IDC_SETTINGS_STATUS_PILL_POSITION_LABEL), enabled);
    EnableWindow(GetDlgItem(dialog, IDC_SETTINGS_STATUS_PILL_POSITION), enabled);
}

std::wstring_view HotkeyFieldLabel(int controlId) {
    switch (controlId) {
    case IDC_SETTINGS_TOGGLE_HOTKEY:
        return L"Toggle recording hotkey";
    case IDC_SETTINGS_CANCEL_HOTKEY:
        return L"Cancel hotkey";
    default:
        return L"Hotkey";
    }
}

void UpdateHotkeyHintForFocus(HWND dialog, int controlId) {
    std::wstring message(HotkeyFieldLabel(controlId));
    message += L": press the shortcut you want now.";
    SetHotkeyHint(dialog, message);
}

void UpdateHotkeyHintForChange(HWND dialog, int controlId) {
    HotkeyBinding binding;
    std::wstring failureReason;
    if (!ReadHotkeyControl(dialog, controlId, binding, failureReason)) {
        UpdateHotkeyHintForFocus(dialog, controlId);
        return;
    }

    std::wstring message(HotkeyFieldLabel(controlId));
    message += L" set to ";
    message += binding.displayName;
    message += L".";
    SetHotkeyHint(dialog, message);
}

INT_PTR CALLBACK SettingsDialogProc(HWND dialog, UINT message, WPARAM wordParam, LPARAM longParam) {
    auto* values = reinterpret_cast<SettingsDialogValues*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));

    switch (message) {
    case WM_INITDIALOG:
        values = reinterpret_cast<SettingsDialogValues*>(longParam);
        SetWindowLongPtrW(dialog, GWLP_USERDATA, longParam);
        CheckDlgButton(dialog, IDC_SETTINGS_START_WITH_WINDOWS, values->autoStartWithWindows ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_SETTINGS_SHOW_STATUS_PILL, values->showStatusPill ? BST_CHECKED : BST_UNCHECKED);
        SetHotkeyControl(dialog, IDC_SETTINGS_TOGGLE_HOTKEY, values->toggleRecordingHotkey);
        SetHotkeyControl(dialog, IDC_SETTINGS_CANCEL_HOTKEY, values->cancelRecordingHotkey);
        PopulatePlacementCombo(dialog, values->statusPillPlacement);
        UpdatePlacementEnabledState(dialog);
        SetHotkeyHint(dialog, kDefaultHotkeyHint);
        return TRUE;

    case WM_COMMAND:
        if (HIWORD(wordParam) == EN_SETFOCUS) {
            switch (LOWORD(wordParam)) {
            case IDC_SETTINGS_TOGGLE_HOTKEY:
            case IDC_SETTINGS_CANCEL_HOTKEY:
                UpdateHotkeyHintForFocus(dialog, LOWORD(wordParam));
                return TRUE;
            }
        }

        if (HIWORD(wordParam) == EN_CHANGE) {
            switch (LOWORD(wordParam)) {
            case IDC_SETTINGS_TOGGLE_HOTKEY:
            case IDC_SETTINGS_CANCEL_HOTKEY:
                UpdateHotkeyHintForChange(dialog, LOWORD(wordParam));
                return TRUE;
            }
        }

        switch (LOWORD(wordParam)) {
        case IDC_SETTINGS_SHOW_STATUS_PILL:
            if (HIWORD(wordParam) == BN_CLICKED) {
                UpdatePlacementEnabledState(dialog);
                return TRUE;
            }
            break;

        case IDOK:
            if (values != nullptr) {
                HotkeyBinding toggleRecordingHotkey;
                std::wstring failureReason;
                if (!ReadHotkeyControl(dialog, IDC_SETTINGS_TOGGLE_HOTKEY, toggleRecordingHotkey, failureReason)) {
                    MessageBoxW(dialog, failureReason.c_str(), L"VoxInsert settings", MB_OK | MB_ICONERROR);
                    return TRUE;
                }

                HotkeyBinding cancelRecordingHotkey;
                if (!ReadHotkeyControl(dialog, IDC_SETTINGS_CANCEL_HOTKEY, cancelRecordingHotkey, failureReason)) {
                    MessageBoxW(dialog, failureReason.c_str(), L"VoxInsert settings", MB_OK | MB_ICONERROR);
                    return TRUE;
                }

                if (toggleRecordingHotkey.virtualKey == cancelRecordingHotkey.virtualKey &&
                    (toggleRecordingHotkey.modifiers & (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN)) ==
                        (cancelRecordingHotkey.modifiers & (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN))) {
                    MessageBoxW(dialog, L"Toggle and cancel hotkeys must be different.", L"VoxInsert settings", MB_OK | MB_ICONERROR);
                    return TRUE;
                }

                values->toggleRecordingHotkey = toggleRecordingHotkey;
                values->cancelRecordingHotkey = cancelRecordingHotkey;
                values->statusPillPlacement = ReadPlacementSelection(dialog);
                values->autoStartWithWindows = IsDlgButtonChecked(dialog, IDC_SETTINGS_START_WITH_WINDOWS) == BST_CHECKED;
                values->showStatusPill = IsDlgButtonChecked(dialog, IDC_SETTINGS_SHOW_STATUS_PILL) == BST_CHECKED;
            }
            EndDialog(dialog, IDOK);
            return TRUE;

        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;
    }

    return FALSE;
}

} // namespace

SettingsDialogResult ShowSettingsDialog(HINSTANCE instance, HWND ownerWindow, SettingsDialogValues& values) {
    const INT_PTR result = DialogBoxParamW(
        instance,
        MAKEINTRESOURCEW(IDD_SETTINGS_DIALOG),
        ownerWindow,
        SettingsDialogProc,
        reinterpret_cast<LPARAM>(&values));

    return result == IDOK ? SettingsDialogResult::Saved : SettingsDialogResult::Cancelled;
}

} // namespace voxinsert