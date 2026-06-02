#include "ui/settings_dialog.h"

#include "observability/logging.h"
#include "resource.h"
#include "security/api_credential_store.h"

#include <commctrl.h>
#include <shobjidl.h>

#include <algorithm>

namespace voxinsert {
namespace {

constexpr wchar_t kDefaultHotkeyHint[] =
    L"Click a hotkey field, then press the shortcut you want. VoxInsert pauses global hotkeys while this dialog is open.";
constexpr wchar_t kCredentialStatusEnterTarget[] = L"Enter a credential target.";
constexpr wchar_t kCredentialStatusUnavailable[] = L"Unavailable.";
constexpr wchar_t kCredentialStatusPresent[] = L"Present.";
constexpr wchar_t kCredentialStatusMissing[] = L"Missing.";

struct PlacementOption {
    StatusPillPlacement placement;
    const wchar_t* label;
};

struct ProviderOption {
    const wchar_t* id;
    const wchar_t* label;
};

enum class SettingsTab : int {
    General = 0,
    OpenAi = 1,
    Mistral = 2,
    Archive = 3,
};

constexpr PlacementOption kPlacementOptions[] = {
    {StatusPillPlacement::TrayAnchor, L"Near tray icon"},
    {StatusPillPlacement::ScreenTopLeft, L"Top left"},
    {StatusPillPlacement::ScreenTopRight, L"Top right"},
    {StatusPillPlacement::ScreenBottomLeft, L"Bottom left"},
    {StatusPillPlacement::ScreenBottomRight, L"Bottom right"},
};

constexpr ProviderOption kProviderOptions[] = {
    {L"openai", L"OpenAI"},
    {L"mistral", L"Mistral"},
};

constexpr int kGeneralTabControlIds[] = {
    IDC_SETTINGS_TRANSCRIPTION_PROVIDER_LABEL,
    IDC_SETTINGS_TRANSCRIPTION_PROVIDER,
    IDC_SETTINGS_LANGUAGE_HINT_LABEL,
    IDC_SETTINGS_LANGUAGE_HINT,
    IDC_SETTINGS_STREAMING_ENABLED,
    IDC_SETTINGS_START_WITH_WINDOWS,
    IDC_SETTINGS_SHOW_STATUS_PILL,
    IDC_SETTINGS_USE_MEDIA_PLAY_PAUSE_TOGGLE,
    IDC_SETTINGS_TOGGLE_HOTKEY_LABEL,
    IDC_SETTINGS_TOGGLE_HOTKEY,
    IDC_SETTINGS_CANCEL_HOTKEY_LABEL,
    IDC_SETTINGS_CANCEL_HOTKEY,
    IDC_SETTINGS_STATUS_PILL_POSITION_LABEL,
    IDC_SETTINGS_STATUS_PILL_POSITION,
    IDC_SETTINGS_HOTKEY_HINT,
    IDC_SETTINGS_SAVE_HINT,
};

constexpr int kOpenAiTabControlIds[] = {
    IDC_SETTINGS_OPENAI_MODEL_LABEL,
    IDC_SETTINGS_OPENAI_MODEL,
    IDC_SETTINGS_OPENAI_STREAMING_MODEL_LABEL,
    IDC_SETTINGS_OPENAI_STREAMING_MODEL,
    IDC_SETTINGS_OPENAI_TARGET_LABEL,
    IDC_SETTINGS_OPENAI_TARGET,
    IDC_SETTINGS_OPENAI_API_KEY_LABEL,
    IDC_SETTINGS_OPENAI_API_KEY,
    IDC_SETTINGS_OPENAI_REMOVE_KEY,
    IDC_SETTINGS_OPENAI_API_KEY_HINT,
    IDC_SETTINGS_OPENAI_PROMPT_LABEL,
    IDC_SETTINGS_OPENAI_PROMPT,
};

constexpr int kMistralTabControlIds[] = {
    IDC_SETTINGS_MISTRAL_MODEL_LABEL,
    IDC_SETTINGS_MISTRAL_MODEL,
    IDC_SETTINGS_MISTRAL_STREAMING_MODEL_LABEL,
    IDC_SETTINGS_MISTRAL_STREAMING_MODEL,
    IDC_SETTINGS_MISTRAL_TARGET_LABEL,
    IDC_SETTINGS_MISTRAL_TARGET,
    IDC_SETTINGS_MISTRAL_API_KEY_LABEL,
    IDC_SETTINGS_MISTRAL_API_KEY,
    IDC_SETTINGS_MISTRAL_REMOVE_KEY,
    IDC_SETTINGS_MISTRAL_API_KEY_HINT,
    IDC_SETTINGS_MISTRAL_CONTEXT_BIAS_LABEL,
    IDC_SETTINGS_MISTRAL_CONTEXT_BIAS,
};

constexpr int kArchiveTabControlIds[] = {
    IDC_SETTINGS_ARCHIVE_ENABLED,
    IDC_SETTINGS_ARCHIVE_TRANSCRIPT,
    IDC_SETTINGS_ARCHIVE_AUDIO,
    IDC_SETTINGS_ARCHIVE_FOLDER_LABEL,
    IDC_SETTINGS_ARCHIVE_FOLDER,
    IDC_SETTINGS_ARCHIVE_BROWSE,
    IDC_SETTINGS_ARCHIVE_HINT,
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

void SetTextControl(HWND dialog, int controlId, std::wstring_view text) {
    SetDlgItemTextW(dialog, controlId, std::wstring(text).c_str());
}

std::wstring BuildApiKeyHintText(std::wstring_view credentialStatus) {
    if (credentialStatus == kCredentialStatusEnterTarget) {
        return L"Enter a credential target to check whether a stored key exists. Leave API key blank to keep the current key. Check Remove stored key to delete it.";
    }

    std::wstring hint = L"Stored key: ";
    if (credentialStatus.empty()) {
        hint += kCredentialStatusUnavailable;
    }
    else {
        hint += credentialStatus;
    }

    hint += L" Leave API key blank to keep it. Check Remove stored key to delete it.";
    return hint;
}

std::wstring TrimText(std::wstring_view text) {
    size_t first = 0;
    while (first < text.size() && iswspace(text[first]) != 0) {
        ++first;
    }

    size_t last = text.size();
    while (last > first && iswspace(text[last - 1]) != 0) {
        --last;
    }

    return std::wstring(text.substr(first, last - first));
}

std::wstring ReadTextControl(HWND dialog, int controlId) {
    const HWND control = GetDlgItem(dialog, controlId);
    if (control == nullptr) {
        return {};
    }

    const int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<size_t>(copied));
    return text;
}

bool ReadHotkeyControl(HWND dialog, int controlId, HotkeyBinding& binding, std::wstring& failureReason) {
    const DWORD hotkeyValue = static_cast<DWORD>(SendDlgItemMessageW(dialog, controlId, HKM_GETHOTKEY, 0, 0));
    const UINT virtualKey = LOBYTE(hotkeyValue);
    const UINT modifiers = ModifiersFromHotkeyFlags(HIBYTE(hotkeyValue));
    return TryCreateHotkeyBinding(modifiers, virtualKey, binding, failureReason);
}

void SelectProviderCombo(HWND dialog, std::wstring_view selectedProviderId) {
    const HWND combo = GetDlgItem(dialog, IDC_SETTINGS_TRANSCRIPTION_PROVIDER);
    if (combo == nullptr) {
        return;
    }

    int selectedIndex = 0;
    const int itemCount = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    for (int itemIndex = 0; itemIndex < itemCount; ++itemIndex) {
        const LRESULT itemData = SendMessageW(combo, CB_GETITEMDATA, itemIndex, 0);
        if (itemData == CB_ERR) {
            continue;
        }

        const size_t providerIndex = static_cast<size_t>(itemData);
        if (providerIndex < ARRAYSIZE(kProviderOptions) && selectedProviderId == kProviderOptions[providerIndex].id) {
            selectedIndex = itemIndex;
            break;
        }
    }

    SendMessageW(combo, CB_SETCURSEL, selectedIndex, 0);
}

void PopulateSettingsTabs(HWND dialog) {
    const HWND tabControl = GetDlgItem(dialog, IDC_SETTINGS_TABS);
    if (tabControl == nullptr) {
        return;
    }

    const wchar_t* tabLabels[] = {L"General", L"OpenAI", L"Mistral", L"Archive"};
    for (int index = 0; index < static_cast<int>(ARRAYSIZE(tabLabels)); ++index) {
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<LPWSTR>(tabLabels[index]);
        TabCtrl_InsertItem(tabControl, index, &item);
    }

    TabCtrl_SetCurSel(tabControl, 0);
}

void PopulateProviderCombo(HWND dialog, std::wstring_view selectedProviderId) {
    const HWND combo = GetDlgItem(dialog, IDC_SETTINGS_TRANSCRIPTION_PROVIDER);
    if (combo == nullptr) {
        return;
    }

    for (size_t index = 0; index < ARRAYSIZE(kProviderOptions); ++index) {
        const int itemIndex = static_cast<int>(SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kProviderOptions[index].label)));
        SendMessageW(combo, CB_SETITEMDATA, itemIndex, static_cast<LPARAM>(index));
    }

    SelectProviderCombo(dialog, selectedProviderId);
}

std::wstring ReadSelectedProvider(HWND dialog) {
    const int selectedIndex = static_cast<int>(SendDlgItemMessageW(dialog, IDC_SETTINGS_TRANSCRIPTION_PROVIDER, CB_GETCURSEL, 0, 0));
    if (selectedIndex == CB_ERR) {
        return L"openai";
    }

    const auto optionIndex = static_cast<size_t>(SendDlgItemMessageW(dialog, IDC_SETTINGS_TRANSCRIPTION_PROVIDER, CB_GETITEMDATA, selectedIndex, 0));
    if (optionIndex >= ARRAYSIZE(kProviderOptions)) {
        return L"openai";
    }

    return kProviderOptions[optionIndex].id;
}

void SelectPlacementCombo(HWND dialog, StatusPillPlacement selectedPlacement) {
    const HWND combo = GetDlgItem(dialog, IDC_SETTINGS_STATUS_PILL_POSITION);
    if (combo == nullptr) {
        return;
    }

    int selectedIndex = 0;
    const int itemCount = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    for (int itemIndex = 0; itemIndex < itemCount; ++itemIndex) {
        const LRESULT itemData = SendMessageW(combo, CB_GETITEMDATA, itemIndex, 0);
        if (itemData == CB_ERR) {
            continue;
        }

        if (static_cast<StatusPillPlacement>(itemData) == selectedPlacement) {
            selectedIndex = itemIndex;
            break;
        }
    }

    SendMessageW(combo, CB_SETCURSEL, selectedIndex, 0);
}

void PopulatePlacementCombo(HWND dialog, StatusPillPlacement selectedPlacement) {
    const HWND combo = GetDlgItem(dialog, IDC_SETTINGS_STATUS_PILL_POSITION);
    if (combo == nullptr) {
        return;
    }

    for (size_t index = 0; index < ARRAYSIZE(kPlacementOptions); ++index) {
        const int itemIndex = static_cast<int>(SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kPlacementOptions[index].label)));
        SendMessageW(combo, CB_SETITEMDATA, itemIndex, static_cast<LPARAM>(kPlacementOptions[index].placement));
    }

    SelectPlacementCombo(dialog, selectedPlacement);
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

void ShowControlGroup(HWND dialog, const int* controlIds, size_t controlCount, bool visible) {
    for (size_t index = 0; index < controlCount; ++index) {
        if (const HWND control = GetDlgItem(dialog, controlIds[index]); control != nullptr) {
            ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
            EnableWindow(control, visible);
        }
    }
}

SettingsTab CurrentSettingsTab(HWND dialog) {
    const int selectedIndex = TabCtrl_GetCurSel(GetDlgItem(dialog, IDC_SETTINGS_TABS));
    switch (selectedIndex) {
    case 1:
        return SettingsTab::OpenAi;
    case 2:
        return SettingsTab::Mistral;
    case 3:
        return SettingsTab::Archive;
    case 0:
    default:
        return SettingsTab::General;
    }
}

void UpdatePlacementEnabledState(HWND dialog) {
    const BOOL enabled = IsDlgButtonChecked(dialog, IDC_SETTINGS_SHOW_STATUS_PILL) == BST_CHECKED;
    EnableWindow(GetDlgItem(dialog, IDC_SETTINGS_STATUS_PILL_POSITION_LABEL), enabled);
    EnableWindow(GetDlgItem(dialog, IDC_SETTINGS_STATUS_PILL_POSITION), enabled);
}

void UpdateArchiveEnabledState(HWND dialog) {
    const BOOL enabled = IsDlgButtonChecked(dialog, IDC_SETTINGS_ARCHIVE_ENABLED) == BST_CHECKED;
    EnableWindow(GetDlgItem(dialog, IDC_SETTINGS_ARCHIVE_TRANSCRIPT), enabled);
    EnableWindow(GetDlgItem(dialog, IDC_SETTINGS_ARCHIVE_AUDIO), enabled);
    EnableWindow(GetDlgItem(dialog, IDC_SETTINGS_ARCHIVE_FOLDER_LABEL), enabled);
    EnableWindow(GetDlgItem(dialog, IDC_SETTINGS_ARCHIVE_FOLDER), enabled);
    EnableWindow(GetDlgItem(dialog, IDC_SETTINGS_ARCHIVE_BROWSE), enabled);
}

bool BrowseForArchiveFolder(HWND dialog, std::wstring& selectedFolder, std::wstring& failureReason) {
    HRESULT initializeResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool shouldUninitialize = SUCCEEDED(initializeResult);
    if (FAILED(initializeResult) && initializeResult != RPC_E_CHANGED_MODE) {
        failureReason = L"CoInitializeEx failed for folder picker.";
        return false;
    }

    IFileOpenDialog* fileDialog = nullptr;
    HRESULT result = CoCreateInstance(
        CLSID_FileOpenDialog,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&fileDialog));
    if (FAILED(result)) {
        if (shouldUninitialize) {
            CoUninitialize();
        }
        failureReason = L"Could not create folder picker.";
        return false;
    }

    DWORD options = 0;
    if (SUCCEEDED(fileDialog->GetOptions(&options))) {
        fileDialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }

    const std::wstring currentFolder = TrimText(ReadTextControl(dialog, IDC_SETTINGS_ARCHIVE_FOLDER));
    if (!currentFolder.empty()) {
        IShellItem* currentFolderItem = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(currentFolder.c_str(), nullptr, IID_PPV_ARGS(&currentFolderItem)))) {
            fileDialog->SetFolder(currentFolderItem);
            currentFolderItem->Release();
        }
    }

    result = fileDialog->Show(dialog);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        fileDialog->Release();
        if (shouldUninitialize) {
            CoUninitialize();
        }
        return false;
    }

    if (FAILED(result)) {
        fileDialog->Release();
        if (shouldUninitialize) {
            CoUninitialize();
        }
        failureReason = L"Folder picker failed.";
        return false;
    }

    IShellItem* selectedItem = nullptr;
    result = fileDialog->GetResult(&selectedItem);
    if (FAILED(result)) {
        fileDialog->Release();
        if (shouldUninitialize) {
            CoUninitialize();
        }
        failureReason = L"Folder picker did not return a selected folder.";
        return false;
    }

    PWSTR selectedPath = nullptr;
    result = selectedItem->GetDisplayName(SIGDN_FILESYSPATH, &selectedPath);
    selectedItem->Release();
    fileDialog->Release();

    if (FAILED(result) || selectedPath == nullptr) {
        if (shouldUninitialize) {
            CoUninitialize();
        }
        failureReason = L"Could not read the selected folder path.";
        return false;
    }

    selectedFolder = selectedPath;
    CoTaskMemFree(selectedPath);
    if (shouldUninitialize) {
        CoUninitialize();
    }
    return true;
}

void UpdateVisibleSettingsTab(HWND dialog) {
    ShowControlGroup(dialog, kGeneralTabControlIds, ARRAYSIZE(kGeneralTabControlIds), false);
    ShowControlGroup(dialog, kOpenAiTabControlIds, ARRAYSIZE(kOpenAiTabControlIds), false);
    ShowControlGroup(dialog, kMistralTabControlIds, ARRAYSIZE(kMistralTabControlIds), false);
    ShowControlGroup(dialog, kArchiveTabControlIds, ARRAYSIZE(kArchiveTabControlIds), false);

    switch (CurrentSettingsTab(dialog)) {
    case SettingsTab::General:
        ShowControlGroup(dialog, kGeneralTabControlIds, ARRAYSIZE(kGeneralTabControlIds), true);
        UpdatePlacementEnabledState(dialog);
        break;
    case SettingsTab::OpenAi:
        ShowControlGroup(dialog, kOpenAiTabControlIds, ARRAYSIZE(kOpenAiTabControlIds), true);
        break;
    case SettingsTab::Mistral:
        ShowControlGroup(dialog, kMistralTabControlIds, ARRAYSIZE(kMistralTabControlIds), true);
        break;
    case SettingsTab::Archive:
        ShowControlGroup(dialog, kArchiveTabControlIds, ARRAYSIZE(kArchiveTabControlIds), true);
        UpdateArchiveEnabledState(dialog);
        break;
    }
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

std::wstring ResolveCredentialStatus(HWND dialog, int targetControlId) {
    const std::wstring credentialTarget = TrimText(ReadTextControl(dialog, targetControlId));
    if (credentialTarget.empty()) {
        return kCredentialStatusEnterTarget;
    }

    bool exists = false;
    std::wstring failureReason;
    if (!CheckApiCredentialExists(credentialTarget, exists, failureReason)) {
        return kCredentialStatusUnavailable;
    }

    return exists ? kCredentialStatusPresent : kCredentialStatusMissing;
}

void UpdateCredentialStatusHint(HWND dialog, int targetControlId, int hintControlId) {
    SetTextControl(dialog, hintControlId, BuildApiKeyHintText(ResolveCredentialStatus(dialog, targetControlId)));
}

void UpdateAllCredentialStatusHints(HWND dialog) {
    UpdateCredentialStatusHint(dialog, IDC_SETTINGS_OPENAI_TARGET, IDC_SETTINGS_OPENAI_API_KEY_HINT);
    UpdateCredentialStatusHint(dialog, IDC_SETTINGS_MISTRAL_TARGET, IDC_SETTINGS_MISTRAL_API_KEY_HINT);
}

void ApplyDefaultSettingsToDialog(HWND dialog) {
    const AppConfig defaultConfig = DefaultAppConfig();

    SetHotkeyControl(dialog, IDC_SETTINGS_TOGGLE_HOTKEY, defaultConfig.toggleRecordingHotkey);
    SetHotkeyControl(dialog, IDC_SETTINGS_CANCEL_HOTKEY, defaultConfig.cancelRecordingHotkey);
    SelectProviderCombo(dialog, WideFromUtf8(defaultConfig.transcription.provider));
    SetTextControl(dialog, IDC_SETTINGS_LANGUAGE_HINT, WideFromUtf8(defaultConfig.transcription.languageHint));
    CheckDlgButton(dialog, IDC_SETTINGS_STREAMING_ENABLED, defaultConfig.transcription.streaming.enabled ? BST_UNCHECKED : BST_CHECKED);
    SetTextControl(dialog, IDC_SETTINGS_OPENAI_MODEL, WideFromUtf8(defaultConfig.transcription.openAi.model));
    SetTextControl(dialog, IDC_SETTINGS_OPENAI_STREAMING_MODEL, WideFromUtf8(defaultConfig.transcription.openAi.streamingModel));
    SetTextControl(dialog, IDC_SETTINGS_OPENAI_TARGET, WideFromUtf8(defaultConfig.transcription.openAi.credentialTarget));
    SetTextControl(dialog, IDC_SETTINGS_OPENAI_API_KEY, L"");
    SetTextControl(dialog, IDC_SETTINGS_OPENAI_PROMPT, WideFromUtf8(defaultConfig.transcription.openAi.prompt));
    CheckDlgButton(dialog, IDC_SETTINGS_OPENAI_REMOVE_KEY, BST_UNCHECKED);
    SetTextControl(dialog, IDC_SETTINGS_MISTRAL_MODEL, WideFromUtf8(defaultConfig.transcription.mistral.model));
    SetTextControl(dialog, IDC_SETTINGS_MISTRAL_STREAMING_MODEL, WideFromUtf8(defaultConfig.transcription.mistral.streamingModel));
    SetTextControl(dialog, IDC_SETTINGS_MISTRAL_TARGET, WideFromUtf8(defaultConfig.transcription.mistral.credentialTarget));
    SetTextControl(dialog, IDC_SETTINGS_MISTRAL_API_KEY, L"");
    SetTextControl(dialog, IDC_SETTINGS_MISTRAL_CONTEXT_BIAS, WideFromUtf8(defaultConfig.transcription.mistral.contextBias));
    CheckDlgButton(dialog, IDC_SETTINGS_MISTRAL_REMOVE_KEY, BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_SETTINGS_ARCHIVE_ENABLED, defaultConfig.archive.enabled ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_SETTINGS_ARCHIVE_TRANSCRIPT, defaultConfig.archive.persistTranscript ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_SETTINGS_ARCHIVE_AUDIO, defaultConfig.archive.persistAudio ? BST_CHECKED : BST_UNCHECKED);
    SetTextControl(dialog, IDC_SETTINGS_ARCHIVE_FOLDER, defaultConfig.archive.folderPath);
    CheckDlgButton(dialog, IDC_SETTINGS_START_WITH_WINDOWS, defaultConfig.system.autoStartWithWindows ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_SETTINGS_USE_MEDIA_PLAY_PAUSE_TOGGLE, defaultConfig.system.useMediaPlayPauseToggle ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_SETTINGS_SHOW_STATUS_PILL, defaultConfig.ui.showStatusPill ? BST_CHECKED : BST_UNCHECKED);
    SelectPlacementCombo(dialog, defaultConfig.ui.statusPillPlacement);
    UpdatePlacementEnabledState(dialog);
    UpdateArchiveEnabledState(dialog);
    UpdateAllCredentialStatusHints(dialog);
    SetHotkeyHint(dialog, kDefaultHotkeyHint);
}

bool ReadRequiredTrimmedText(
    HWND dialog,
    int controlId,
    const wchar_t* fieldLabel,
    std::wstring& value,
    std::wstring& failureReason) {
    value = TrimText(ReadTextControl(dialog, controlId));
    if (!value.empty()) {
        return true;
    }

    failureReason = fieldLabel;
    failureReason += L" is required.";
    return false;
}

bool ValidateProviderCredentialEdit(
    const wchar_t* providerLabel,
    std::wstring_view apiKey,
    bool removeStoredKey,
    std::wstring& failureReason) {
    if (!(removeStoredKey && !apiKey.empty())) {
        return true;
    }

    failureReason = providerLabel;
    failureReason += L": enter a new API key or choose Remove stored key, but not both.";
    return false;
}

INT_PTR CALLBACK SettingsDialogProc(HWND dialog, UINT message, WPARAM wordParam, LPARAM longParam) {
    auto* values = reinterpret_cast<SettingsDialogValues*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));

    switch (message) {
    case WM_INITDIALOG:
        values = reinterpret_cast<SettingsDialogValues*>(longParam);
        SetWindowLongPtrW(dialog, GWLP_USERDATA, longParam);
        CheckDlgButton(dialog, IDC_SETTINGS_STREAMING_ENABLED, values->streamingEnabled ? BST_UNCHECKED : BST_CHECKED);
        CheckDlgButton(dialog, IDC_SETTINGS_START_WITH_WINDOWS, values->autoStartWithWindows ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_SETTINGS_USE_MEDIA_PLAY_PAUSE_TOGGLE, values->useMediaPlayPauseToggle ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_SETTINGS_SHOW_STATUS_PILL, values->showStatusPill ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_SETTINGS_ARCHIVE_ENABLED, values->archiveEnabled ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_SETTINGS_ARCHIVE_TRANSCRIPT, values->archivePersistTranscript ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_SETTINGS_ARCHIVE_AUDIO, values->archivePersistAudio ? BST_CHECKED : BST_UNCHECKED);
        SetHotkeyControl(dialog, IDC_SETTINGS_TOGGLE_HOTKEY, values->toggleRecordingHotkey);
        SetHotkeyControl(dialog, IDC_SETTINGS_CANCEL_HOTKEY, values->cancelRecordingHotkey);
        PopulateSettingsTabs(dialog);
        PopulateProviderCombo(dialog, values->transcriptionProvider);
        SendDlgItemMessageW(dialog, IDC_SETTINGS_LANGUAGE_HINT, EM_LIMITTEXT, 32, 0);
        SendDlgItemMessageW(dialog, IDC_SETTINGS_OPENAI_MODEL, EM_LIMITTEXT, 128, 0);
        SendDlgItemMessageW(dialog, IDC_SETTINGS_OPENAI_STREAMING_MODEL, EM_LIMITTEXT, 128, 0);
        SendDlgItemMessageW(dialog, IDC_SETTINGS_OPENAI_TARGET, EM_LIMITTEXT, 256, 0);
        SendDlgItemMessageW(dialog, IDC_SETTINGS_OPENAI_API_KEY, EM_LIMITTEXT, 4096, 0);
        SendDlgItemMessageW(dialog, IDC_SETTINGS_OPENAI_PROMPT, EM_LIMITTEXT, 4096, 0);
        SendDlgItemMessageW(dialog, IDC_SETTINGS_MISTRAL_MODEL, EM_LIMITTEXT, 128, 0);
        SendDlgItemMessageW(dialog, IDC_SETTINGS_MISTRAL_STREAMING_MODEL, EM_LIMITTEXT, 128, 0);
        SendDlgItemMessageW(dialog, IDC_SETTINGS_MISTRAL_TARGET, EM_LIMITTEXT, 256, 0);
        SendDlgItemMessageW(dialog, IDC_SETTINGS_MISTRAL_API_KEY, EM_LIMITTEXT, 4096, 0);
        SendDlgItemMessageW(dialog, IDC_SETTINGS_MISTRAL_CONTEXT_BIAS, EM_LIMITTEXT, 4096, 0);
        SendDlgItemMessageW(dialog, IDC_SETTINGS_ARCHIVE_FOLDER, EM_LIMITTEXT, 1024, 0);
        SetTextControl(dialog, IDC_SETTINGS_LANGUAGE_HINT, values->languageHint);
        SetTextControl(dialog, IDC_SETTINGS_OPENAI_MODEL, values->openAiModel);
        SetTextControl(dialog, IDC_SETTINGS_OPENAI_STREAMING_MODEL, values->openAiStreamingModel);
        SetTextControl(dialog, IDC_SETTINGS_OPENAI_TARGET, values->openAiCredentialTarget);
        SetTextControl(dialog, IDC_SETTINGS_OPENAI_API_KEY, values->openAiApiKey);
        SetTextControl(dialog, IDC_SETTINGS_OPENAI_PROMPT, values->openAiPrompt);
        SetTextControl(dialog, IDC_SETTINGS_MISTRAL_MODEL, values->mistralModel);
        SetTextControl(dialog, IDC_SETTINGS_MISTRAL_STREAMING_MODEL, values->mistralStreamingModel);
        SetTextControl(dialog, IDC_SETTINGS_MISTRAL_TARGET, values->mistralCredentialTarget);
        SetTextControl(dialog, IDC_SETTINGS_MISTRAL_API_KEY, values->mistralApiKey);
        SetTextControl(dialog, IDC_SETTINGS_MISTRAL_CONTEXT_BIAS, values->mistralContextBias);
        SetTextControl(dialog, IDC_SETTINGS_ARCHIVE_FOLDER, values->archiveFolderPath);
        CheckDlgButton(dialog, IDC_SETTINGS_OPENAI_REMOVE_KEY, values->removeOpenAiApiKey ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_SETTINGS_MISTRAL_REMOVE_KEY, values->removeMistralApiKey ? BST_CHECKED : BST_UNCHECKED);
        PopulatePlacementCombo(dialog, values->statusPillPlacement);
        UpdateAllCredentialStatusHints(dialog);
        UpdateVisibleSettingsTab(dialog);
        SetHotkeyHint(dialog, kDefaultHotkeyHint);
        return TRUE;

    case WM_NOTIFY:
        if (reinterpret_cast<const NMHDR*>(longParam)->idFrom == IDC_SETTINGS_TABS &&
            reinterpret_cast<const NMHDR*>(longParam)->code == TCN_SELCHANGE) {
            UpdateVisibleSettingsTab(dialog);
            return TRUE;
        }
        break;

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

            case IDC_SETTINGS_OPENAI_TARGET:
                UpdateCredentialStatusHint(dialog, IDC_SETTINGS_OPENAI_TARGET, IDC_SETTINGS_OPENAI_API_KEY_HINT);
                return TRUE;

            case IDC_SETTINGS_MISTRAL_TARGET:
                UpdateCredentialStatusHint(dialog, IDC_SETTINGS_MISTRAL_TARGET, IDC_SETTINGS_MISTRAL_API_KEY_HINT);
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

        case IDC_SETTINGS_ARCHIVE_ENABLED:
            if (HIWORD(wordParam) == BN_CLICKED) {
                UpdateArchiveEnabledState(dialog);
                return TRUE;
            }
            break;

        case IDC_SETTINGS_ARCHIVE_BROWSE:
            if (HIWORD(wordParam) == BN_CLICKED) {
                std::wstring selectedFolder;
                std::wstring browseFailureReason;
                if (BrowseForArchiveFolder(dialog, selectedFolder, browseFailureReason)) {
                    SetTextControl(dialog, IDC_SETTINGS_ARCHIVE_FOLDER, selectedFolder);
                    return TRUE;
                }

                if (!browseFailureReason.empty()) {
                    MessageBoxW(dialog, browseFailureReason.c_str(), L"VoxInsert settings", MB_OK | MB_ICONERROR);
                }
                return TRUE;
            }
            break;

        case IDC_SETTINGS_RESET_DEFAULTS:
            if (HIWORD(wordParam) == BN_CLICKED) {
                ApplyDefaultSettingsToDialog(dialog);
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

                std::wstring failureField;
                if (!ReadRequiredTrimmedText(dialog, IDC_SETTINGS_LANGUAGE_HINT, L"Language hint", values->languageHint, failureField)) {
                    MessageBoxW(dialog, failureField.c_str(), L"VoxInsert settings", MB_OK | MB_ICONERROR);
                    return TRUE;
                }

                if (!ReadRequiredTrimmedText(dialog, IDC_SETTINGS_OPENAI_MODEL, L"OpenAI model", values->openAiModel, failureField)) {
                    MessageBoxW(dialog, failureField.c_str(), L"VoxInsert settings", MB_OK | MB_ICONERROR);
                    return TRUE;
                }

                if (!ReadRequiredTrimmedText(dialog, IDC_SETTINGS_OPENAI_STREAMING_MODEL, L"OpenAI streaming model", values->openAiStreamingModel, failureField)) {
                    MessageBoxW(dialog, failureField.c_str(), L"VoxInsert settings", MB_OK | MB_ICONERROR);
                    return TRUE;
                }

                if (!ReadRequiredTrimmedText(dialog, IDC_SETTINGS_OPENAI_TARGET, L"OpenAI credential target", values->openAiCredentialTarget, failureField)) {
                    MessageBoxW(dialog, failureField.c_str(), L"VoxInsert settings", MB_OK | MB_ICONERROR);
                    return TRUE;
                }

                if (!ReadRequiredTrimmedText(dialog, IDC_SETTINGS_MISTRAL_MODEL, L"Mistral model", values->mistralModel, failureField)) {
                    MessageBoxW(dialog, failureField.c_str(), L"VoxInsert settings", MB_OK | MB_ICONERROR);
                    return TRUE;
                }

                if (!ReadRequiredTrimmedText(dialog, IDC_SETTINGS_MISTRAL_STREAMING_MODEL, L"Mistral streaming model", values->mistralStreamingModel, failureField)) {
                    MessageBoxW(dialog, failureField.c_str(), L"VoxInsert settings", MB_OK | MB_ICONERROR);
                    return TRUE;
                }

                if (!ReadRequiredTrimmedText(dialog, IDC_SETTINGS_MISTRAL_TARGET, L"Mistral credential target", values->mistralCredentialTarget, failureField)) {
                    MessageBoxW(dialog, failureField.c_str(), L"VoxInsert settings", MB_OK | MB_ICONERROR);
                    return TRUE;
                }

                values->transcriptionProvider = ReadSelectedProvider(dialog);
                values->streamingEnabled = IsDlgButtonChecked(dialog, IDC_SETTINGS_STREAMING_ENABLED) != BST_CHECKED;
                values->openAiApiKey = TrimText(ReadTextControl(dialog, IDC_SETTINGS_OPENAI_API_KEY));
                values->mistralApiKey = TrimText(ReadTextControl(dialog, IDC_SETTINGS_MISTRAL_API_KEY));
                values->removeOpenAiApiKey = IsDlgButtonChecked(dialog, IDC_SETTINGS_OPENAI_REMOVE_KEY) == BST_CHECKED;
                values->removeMistralApiKey = IsDlgButtonChecked(dialog, IDC_SETTINGS_MISTRAL_REMOVE_KEY) == BST_CHECKED;
                values->openAiPrompt = ReadTextControl(dialog, IDC_SETTINGS_OPENAI_PROMPT);
                values->mistralContextBias = ReadTextControl(dialog, IDC_SETTINGS_MISTRAL_CONTEXT_BIAS);
                values->archiveEnabled = IsDlgButtonChecked(dialog, IDC_SETTINGS_ARCHIVE_ENABLED) == BST_CHECKED;
                values->archivePersistTranscript = IsDlgButtonChecked(dialog, IDC_SETTINGS_ARCHIVE_TRANSCRIPT) == BST_CHECKED;
                values->archivePersistAudio = IsDlgButtonChecked(dialog, IDC_SETTINGS_ARCHIVE_AUDIO) == BST_CHECKED;
                values->archiveFolderPath = TrimText(ReadTextControl(dialog, IDC_SETTINGS_ARCHIVE_FOLDER));

                if (values->archiveEnabled && !values->archivePersistTranscript && !values->archivePersistAudio) {
                    MessageBoxW(dialog, L"Archive must save transcript text, Opus audio, or both.", L"VoxInsert settings", MB_OK | MB_ICONERROR);
                    return TRUE;
                }

                if (values->archiveEnabled && values->archiveFolderPath.empty()) {
                    MessageBoxW(dialog, L"Archive folder is required when archiving is enabled.", L"VoxInsert settings", MB_OK | MB_ICONERROR);
                    return TRUE;
                }

                if (!ValidateProviderCredentialEdit(L"OpenAI", values->openAiApiKey, values->removeOpenAiApiKey, failureField)) {
                    MessageBoxW(dialog, failureField.c_str(), L"VoxInsert settings", MB_OK | MB_ICONERROR);
                    return TRUE;
                }

                if (!ValidateProviderCredentialEdit(L"Mistral", values->mistralApiKey, values->removeMistralApiKey, failureField)) {
                    MessageBoxW(dialog, failureField.c_str(), L"VoxInsert settings", MB_OK | MB_ICONERROR);
                    return TRUE;
                }

                values->statusPillPlacement = ReadPlacementSelection(dialog);
                values->autoStartWithWindows = IsDlgButtonChecked(dialog, IDC_SETTINGS_START_WITH_WINDOWS) == BST_CHECKED;
                values->useMediaPlayPauseToggle = IsDlgButtonChecked(dialog, IDC_SETTINGS_USE_MEDIA_PLAY_PAUSE_TOGGLE) == BST_CHECKED;
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