# Headset Media Button Research

## Goal

Research whether VoxInsert can let a user start and stop dictation with a headset or headphone play/pause button, and document the Windows input paths that make this possible or unreliable.

This document is intentionally evidence-driven. It is a research memo, not an implementation plan.

## Current VoxInsert Baseline

- VoxInsert currently uses Windows global hotkeys through `RegisterHotKey` and handles `WM_HOTKEY` in the hidden tray window.
- The current config and settings code only supports `A-Z`, `0-9`, `F1-F24`, and `Escape` as user-configurable hotkeys.
- A future headset feature should therefore be treated as a second input channel adjacent to the existing hotkey settings, not as a small extension of the current hotkey string parser.

## Platform Findings So Far

### 1. `RegisterHotKey` is not the robust foundation

Windows defines media virtual keys such as `VK_MEDIA_PLAY_PAUSE`, so some devices may be capturable through the same mechanism VoxInsert already uses today.

However, the Windows docs do not guarantee that every headset play/pause button is surfaced to desktop apps as a standard virtual key suitable for `RegisterHotKey`. This path is therefore best treated as compatibility, not as the core design assumption.

### 2. `WM_APPCOMMAND` is useful but too high-level to be the primary capture model

Windows exposes media-oriented app commands such as:

- `APPCOMMAND_MEDIA_PLAY_PAUSE`
- `APPCOMMAND_MEDIA_PLAY`
- `APPCOMMAND_MEDIA_PAUSE`
- `APPCOMMAND_MEDIA_RECORD`
- `APPCOMMAND_MIC_ON_OFF_TOGGLE`
- `APPCOMMAND_DICTATE_OR_COMMAND_CONTROL_TOGGLE`

This is evidence that Windows recognizes the semantic class of the button, but `WM_APPCOMMAND` is a higher-level routed window message. The available documentation does not make it the strongest background-capture mechanism for a tray app that wants dependable device-level interception.

### 3. Raw Input is the strongest documented capture path for a Win32 tray app

Windows Raw Input was explicitly designed to expose device data from a broad set of HID devices, including devices beyond the traditional keyboard and mouse.

Important properties for VoxInsert:

- apps must register the HID top-level collections they want
- matching events are delivered via `WM_INPUT`
- background delivery is available with `RIDEV_INPUTSINK`
- hot-plug notifications are available with `RIDEV_DEVNOTIFY`
- the app can distinguish which physical device generated the event

That makes Raw Input the most credible foundation when a headset button is exposed as HID consumer control input.

### 4. Low-level keyboard hooks are not the preferred route

Microsoft's `LowLevelKeyboardProc` guidance explicitly says that in most cases where an app thinks it needs low-level hooks, it should monitor raw input instead because raw input can monitor input more effectively.

For VoxInsert, this is strong evidence against using `WH_KEYBOARD_LL` as the primary design.

### 5. `SystemMediaTransportControls` is aimed at media apps, not dictation utilities

Windows `SystemMediaTransportControls` is the documented integration point for apps that participate as system media sessions and want to receive media button events through that model.

That makes it a poor default fit for VoxInsert. Using it would effectively push VoxInsert toward acting like a media session instead of a passive dictation tool with an optional remote trigger.

## Transport And Device Model Notes

- Bluetooth headsets often use AVRCP semantics for play/pause and other remote media actions.
- Windows HID architecture documents consumer controls as shared HID clients.
- A single user-visible play/pause button can therefore surface through different Windows paths depending on device firmware, transport, and vendor driver behavior.

This is the main reason the feature cannot be designed around a universal promise like "all headset play/pause buttons can be intercepted globally."

## Interim Conclusion

The most defensible product framing is:

- keep the current keyboard toggle and cancel hotkeys
- add a separate option such as `Use play/pause media button to start and stop recording`
- treat headset/media-button input as an optional second input channel
- assume device variance and keep the keyboard hotkey as the fallback path

## External Case Studies And Practitioner Evidence

### 1. AutoHotkey: good bridge, explicit about device limits

- AutoHotkey exposes `Media_Play_Pause`, `Media_Next`, `Media_Prev`, and `Media_Stop` as first-class hotkey names when Windows surfaces the button as a multimedia key.
- Its docs also explicitly warn that some "mystery keys" do not generate detectable events because vendor hardware or drivers handle them below the level AutoHotkey can see.
- The documented fallback ladder is pragmatic: inspect scan codes first, remap in vendor software if possible, then fall back to HID-oriented tooling such as AHKHID or raw-input approaches for USB HID devices.
- mpv users actually use this in practice: a `Media_Play_Pause` AutoHotkey binding that forwards a normal key to mpv works as a user-side workaround, but the same threads show it is only a partial fix for broader media-control behavior.
- Takeaway for VoxInsert: keeping keyboard hotkeys as the stable contract is valuable because it preserves compatibility with tools like AutoHotkey, but that should not be confused with native universal headset-button support.

### 2. Mumble: Raw Input gets closer to the hardware, but AppKeys and HID still split the world

- Mumble moved shortcut capture to a Raw Input backend, and maintainers expected special keys to appear there.
- In practice, users reported that media and app keys collapsed to one value (`K0`) or disappeared entirely, while some extra keyboard buttons appeared on a separate `RIM_TYPEHID` interface.
- A Mumble maintainer noted that `RIDEV_APPKEYS` can expose AppKeys, but pairing it with `RIDEV_NOLEGACY` created problems for Qt input handling, so `WM_APPCOMMAND` was considered as a workaround.
- The thread is useful because it shows two distinct failure modes: higher-level media/app keys that do not map cleanly, and vendor-specific extra buttons that live on separate HID collections.
- Takeaway: Raw Input is still the strongest low-level foundation, but VoxInsert would need explicit handling for HID consumer-control devices and should not assume every interesting button looks like a normal keyboard event.

### 3. mpv, mpv.net, and ImPlay: `WM_APPCOMMAND` works, then hits a ceiling, then projects move to SMTC

- mpv.net had a concrete `WM_APPCOMMAND` bug. Its maintainer reproduced multimedia-key crashes with a virtual keyboard capable of sending `WM_APPCOMMAND`, fixed the handler, and shipped a release where play/pause, previous, and next worked correctly.
- The same maintainer also described Windows global media keys as complicated: `RegisterHotkey` has limitations, hook APIs are risky, and `WM_APPCOMMAND` is special-case plumbing that AutoHotkey hides from end users.
- mpv users then reported the bigger limitation: media buttons only worked while mpv was focused or on top, not as true background/global controls.
- In 2024 mpv addressed that class of problem by adding native Windows Media Control / `SystemMediaTransportControls` support. The feature request explicitly called out Bluetooth earphones and the limitations of script-based IPC bridges, and PR `#14338` closed the background-media-key issues.
- That integration was not free. The mpv work added a C++/WinRT dependency and raised the minimum Windows version to 10, which is a good reminder that the SMTC route is a real platform integration project, not a small message-loop tweak.
- Downstream projects followed that direction. ImPlay later resolved a Windows `MediaPlayPause` report by telling users to enable mpv's new `media-controls=yes` path and explicitly referenced the upstream SMTC work.
- Takeaway: if an app wants true system-media-session behavior, the modern Windows path is SMTC, not just `WM_APPCOMMAND`. But that is a product-level choice because it makes the app participate in Windows media controls rather than merely listening for a spare button.

### 4. spcplay: `WM_APPCOMMAND` is an easy prototype, but not enough for dependable inactive-window capture

- A spcplay user tried adding `WM_APPCOMMAND` support for wireless-headset gestures and reported that basic play/pause and navigation did work.
- The same experiment also showed the limits quickly: delivery was unreliable when the window was inactive or not foreground, and device behavior varied across headset gestures.
- The project maintainer responded that receiving these messages while inactive would likely require a more global interception approach and suggested external command-line control as a fallback.
- Takeaway: `WM_APPCOMMAND` is useful as a diagnostic and prototyping path, but it is not strong evidence that a tray utility can depend on it for background headset control.

### 5. GDeskTunes: shell-hook and AppCommand forwarding can work, but it gets brittle fast

- GDeskTunes first fixed a missing play/pause toggle mapping, then chased background capture problems through AppCommand registration and shell hooks.
- Their issue history shows partial success followed by fragile behavior: a separate 64-bit helper process, forwarded AppCommands, interference with Spotify, failures after sleep/wake, and minimize/focus quirks.
- This is valuable negative evidence. A project can make background media-button handling mostly work through hooks and forwarders and still end up with a maintenance-heavy subsystem.
- Takeaway: VoxInsert should avoid architectures that depend on shell-wide hooks or helper forwarders unless no lower-risk path exists.

### 6. Electron: even high-level frameworks do not guarantee ownership of media keys

- Electron exposes `Media Play/Pause` and related accelerators through `globalShortcut`.
- Its docs state that registration can fail silently when the OS or another application already owns the shortcut.
- That matters because it shows that even a mature cross-platform shortcut layer does not offer a magic solution. The operating system still arbitrates these keys.
- Takeaway: VoxInsert should treat media-button ownership as opportunistic, not guaranteed.

## Diagnostics And Device Identity Evidence

- NirSoft's KeyboardStateView reports key state even when its window is not focused, which makes it a practical way to confirm whether a device surfaces its button as a normal virtual key at all.
- A Microsoft Answers thread about problematic USB audio adapters identifies a separate `HID-compliant consumer control device` underneath the headset path in Device Manager.
- This supports the same model suggested by the Win32 and HID documentation: some headset buttons are not "the audio device" from the app's perspective. They are separate consumer-control HID interfaces that may need their own capture logic.

## What This Means For VoxInsert

### Recommended product framing

- Keep the existing keyboard hotkeys as the primary supported control surface.
- If VoxInsert gains media-button support, expose it as an opt-in secondary trigger, not as the only way to start or stop dictation.
- Do not promise universal support across all headsets, transports, or vendor drivers.

### Recommended technical framing

- Treat `RegisterHotKey` support for media virtual keys as opportunistic compatibility only.
- Treat `WM_APPCOMMAND` as a useful probe and fallback path, not the primary architecture.
- Treat Raw Input over HID consumer-control devices as the strongest Win32-native foundation for a tray app that wants device-level capture without pretending to be a media player.
- Consider SMTC only if VoxInsert intentionally wants to register as a Windows media session. That would be a different product posture, closer to a media app, and should be decided explicitly rather than smuggled in as "just another hotkey backend."

### Borrowable patterns from other projects

- Make the feature opt-in.
- Keep a known-good fallback input path.
- Log which path actually fired: keyboard virtual key, `WM_APPCOMMAND`, raw HID consumer control, or media-session callback.
- Preserve device identity when possible, because a headset button and a keyboard media key may be different devices.
- Expect sleep/resume and competing media apps to be test scenarios, not edge cases.

### Evidence-backed short-term user story

- Even before VoxInsert adds native support, users whose devices expose `Media_Play_Pause` through AutoHotkey can already bridge that button to VoxInsert's existing keyboard hotkey.
- That is not a product solution, but it is a low-risk way to validate whether a specific headset surfaces a usable event on a given Windows machine.

## External Sources Consulted

- AutoHotkey key list: https://www.autohotkey.com/docs/v2/KeyList.htm
- AutoHotkey hotkey docs: https://www.autohotkey.com/docs/v2/lib/Hotkey.htm
- Mumble issue `Special keys do not work anymore #5020`: https://github.com/mumble-voip/mumble/issues/5020
- mpv issue `Media buttons not working when mpv isn't focused #9336`: https://github.com/mpv-player/mpv/issues/9336
- mpv issue `Global Media Keys #13813`: https://github.com/mpv-player/mpv/issues/13813
- mpv issue `Windows: add native support for SystemMediaTransportControls #14007`: https://github.com/mpv-player/mpv/issues/14007
- mpv pull request `win32: add Media Control support #14338`: https://github.com/mpv-player/mpv/pull/14338
- mpv.net issue `Can't get the multimedia keys to work... any clue? #8`: https://github.com/mpvnet-player/mpv.net/issues/8
- ImPlay issue `MediaPlayPause not working on Windows #82`: https://github.com/tsl0922/ImPlay/issues/82
- spcplay issue `Support headset media controls #75`: https://github.com/dgrfactory/spcplay/issues/75
- GDeskTunes issue `Play/Pause toggle media key doesn't work #33`: https://github.com/Gearlux/GDeskTunes/issues/33
- Electron globalShortcut docs: https://www.electronjs.org/docs/latest/api/global-shortcut
- NirSoft KeyboardStateView: https://www.nirsoft.net/utils/keyboard_state_view.html
- Microsoft Answers thread on USB adapter / HID consumer control device: https://learn.microsoft.com/en-us/answers/questions/4038567/somethings-not-right-with-my-usb-to-headphone-adap

## Source Categories Already Consulted

- Microsoft Win32 input documentation
- Microsoft HID and Raw Input documentation
- Microsoft System Media Transport Controls documentation
- Bluetooth AVRCP specification overview
- Open source issue threads and framework docs from AutoHotkey, Mumble, mpv, mpv.net, ImPlay, spcplay, GDeskTunes, Electron, NirSoft, and Microsoft Answers