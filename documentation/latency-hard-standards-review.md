# VoxInsert latency hard-standards review

Scope: local Windows desktop app latency from keybind/SMTC toggle to microphone capture, then from captured audio to final transcript/insert readiness. Review lens: the `cpp-hard-standards-reviewer` skill, adapted to this app's practical hot paths. "Hot path" below means: `WM_HOTKEY`/SMTC toggle handling, recording startup, capture-loop frame forwarding, streaming backend append/finalization, fallback transcription, and final text insertion.

## Findings

- [Blocker] `src/runtime/streaming_recording_controller.cpp:97`, `src/runtime/streaming_recording_controller.cpp:108`, `src/runtime/streaming_recording_controller.cpp:117`, `src/transcription/openai_realtime_streaming_service.cpp:152`, `src/transcription/mistral_realtime_streaming_service.cpp:107` - Recording start does backend/session work before microphone capture starts.
  - Status 2026-06-03: fixed in `src/runtime/streaming_recording_controller.cpp`; see `documentation/latency-first-blocker-capture-first-results.md` for before/after evidence.
  - Breaks: zero hot-path blocking, bypass cold-path work, deterministic keybind-to-capture latency.
  - Why it matters: the toggle path creates a backend session, reads credentials, initializes WebSocket state, and starts backend machinery before `AudioRecorder::Start` is called. Even though `webSocket_.start()` is asynchronous, credential access and session setup still sit before capture. A slow Credential Manager call or backend setup failure delays the first audio frame.
  - Fix: start/prewarm audio first, or keep a warmed capture/session coordinator alive. Cache validated credential handles/strings outside the keypress path. Connect the streaming backend in parallel with capture and buffer raw PCM in a preallocated ring until the session is ready.

- [Blocker] `src/audio/audio_recorder.cpp:102`, `src/audio/audio_recorder.cpp:105`, `src/audio/audio_recorder.cpp:202`, `src/audio/audio_recorder.cpp:221` - The keybind path creates the recording thread and then synchronously waits for PortAudio stream open/start.
  - Breaks: zero hot-path allocation/blocking and keybind-to-capture latency.
  - Why it matters: `Start` blocks on `startupCondition_` until the worker opens and starts the PortAudio stream. On Windows, default-device lookup/open/start can be variable, especially after device changes or cold PortAudio initialization.
  - Fix: initialize PortAudio and open the default input stream at app startup or when settings change. Prefer a long-lived callback stream that is armed/disarmed by atomics, not a thread and stream creation per recording.

- [Blocker] `src/runtime/bounded_queue.h:29`, `src/runtime/bounded_queue.h:31`, `src/runtime/bounded_queue.h:35`, `src/runtime/bounded_queue.h:55`, `src/runtime/bounded_queue.h:105` - The capture-to-streaming queue is mutex/condition-variable based and backed by `std::queue`.
  - Breaks: lock-free algorithms, contiguous memory only, zero hot-path allocation.
  - Why it matters: the audio producer calls `TryPush` from the capture callback path, which takes a mutex and pushes into a default `std::queue`/`std::deque` storage structure. Capacity is logical, not preallocated contiguous storage, so queue growth can allocate and cache locality is poor.
  - Fix: replace it with a fixed-capacity SPSC ring buffer with preallocated slots. Use acquire/release atomics with explicit memory orders and no OS blocking in the producer.

- [Blocker] `src/runtime/streaming_recording_controller.cpp:121`, `src/runtime/streaming_recording_controller.cpp:126`, `src/transcription/streaming_transcription_service.h:33`, `src/transcription/streaming_transcription_service.h:42` - Every captured buffer allocates/copies into an owned `std::vector<int16_t>` chunk.
  - Breaks: zero hot-path allocation and data-oriented capture.
  - Why it matters: for the default 16 kHz/256-frame config, this runs roughly every 16 ms. Each callback constructs a chunk containing strings and a vector, assigns PCM into the vector, then enqueues it. This adds allocator jitter and extra copies exactly where the app needs stable capture timing.
  - Fix: preallocate fixed-size audio slots in a ring. Store session/utterance IDs once per session, not per audio chunk. Pass slot indices/spans through the queue.

- [Blocker] `src/runtime/streaming_recording_controller.cpp:127`, `src/runtime/streaming_recording_controller.cpp:128`, `src/runtime/streaming_recording_controller.cpp:218`, `src/runtime/streaming_recording_controller.cpp:219`, `src/runtime/streaming_recording_controller.cpp:303`, `src/runtime/streaming_recording_controller.cpp:347` - Streaming backpressure can drop audio/events without invalidating the streaming transcript.
  - Breaks: correctness, deterministic failure handling, and hot-path reliability.
  - Why it matters: if `audioQueue_` is full, the chunk is dropped and `backpressureObserved_` is set. If `eventQueue_` is full, backend events are dropped. `Finish` only treats `failed_ || !finalized_` as a streaming failure, so a finalized transcript produced from incomplete audio can still be inserted.
  - Fix: make any dropped audio chunk mark the streaming transcript untrusted and force retained-PCM file fallback, or block only outside the audio callback on a larger preallocated ring. Treat dropped final/events as a hard streaming failure.

- [Major] `src/runtime/streaming_recording_controller.cpp:176`, `src/runtime/streaming_recording_controller.cpp:177`, `config.example.json:24` - `append_batch_ms` defaults to 80 ms and is enforced as the streaming send threshold.
  - Breaks: minimal audio-captured-to-backend latency.
  - Why it matters: even with a ready WebSocket, audio is intentionally held until the batch reaches 80 ms. That hides some network overhead, but it adds a fixed latency floor before the backend can see early speech.
  - Fix: tune this down, make it provider-specific, and measure. For lowest latency, use 10 to 20 ms frames if the backend accepts them, or adaptive batching during connection warm-up only.

- [Major] `src/audio/audio_recorder.cpp:23`, `src/audio/audio_recorder.cpp:263`, `src/runtime/app_host_recording.cpp:69`, `src/runtime/app_host_recording.cpp:70`, `config.example.json:44` - RMS amplitude is computed on every audio buffer even when the status pill is disabled.
  - Breaks: keep the capture path minimal.
  - Why it matters: `StartRecording` always passes an amplitude callback, so `AudioRecorder` always computes RMS. `StatusPill::PostAmplitudeSample` returns early when disabled, but the capture thread already paid for the scan and `sqrt`.
  - Fix: pass an empty amplitude callback when the status pill is disabled, or compute/downsample meter data on a non-capture thread.

- [Major] `src/audio/audio_recorder.cpp:256`, `src/runtime/streaming_recording_controller.cpp:126`, `src/runtime/streaming_recording_controller.cpp:182` - Captured PCM is copied multiple times before it reaches the backend.
  - Breaks: zero hot-path allocation/copies.
  - Why it matters: each buffer is appended to retained PCM, copied into a streaming chunk, copied again into the append batch, then copied/encoded by the provider adapter. This increases CPU and memory bandwidth pressure during speech.
  - Fix: retain PCM in preallocated slabs and queue references to the same slabs. Batch by span list or ring cursors rather than appending vectors.

- [Major] `src/audio/audio_recorder.cpp:132`, `src/audio/audio_recorder.cpp:279` - Stopping a recording copies the whole retained sample vector.
  - Breaks: audio-captured-to-ready latency after stop.
  - Why it matters: the worker moves `capturedSamples` into `samples_`, then `Stop` copies `samples_` into the caller output. Longer recordings can copy megabytes on the finalization path.
  - Fix: move out with `samples = std::move(samples_)` after checking failure state, or return a retained audio object that owns the buffer without another copy.

- [Major] `src/transcription/openai_realtime_streaming_service.cpp:87`, `src/transcription/openai_realtime_streaming_service.cpp:92`, `src/transcription/openai_realtime_streaming_service.cpp:101`, `src/transcription/openai_realtime_streaming_service.cpp:202` - OpenAI resampling allocates a new vector per append.
  - Breaks: zero hot-path allocation.
  - Why it matters: every append allocates output storage before base64/JSON creation. The resampler is streaming in state, but not in storage.
  - Fix: keep a preallocated resample buffer in the session and reuse it. Better, capture at the provider-required sample rate when OpenAI realtime is selected.

- [Major] `src/transcription/openai_realtime_streaming_service.cpp:207`, `src/transcription/openai_realtime_streaming_service.cpp:214`, `src/transcription/mistral_realtime_streaming_service.cpp:156`, `src/transcription/mistral_realtime_streaming_service.cpp:163` - Every append builds base64 and JSON strings synchronously in the streaming pump.
  - Breaks: zero hot-path allocation and minimal backend-append latency.
  - Why it matters: base64 expands PCM by roughly 33 percent, `nlohmann::json` allocates, and `payload.dump()` allocates a serialized string per append. This is not the capture thread, but it is on the path from captured audio to backend availability.
  - Fix: reuse serialization buffers, reserve to the exact upper bound, avoid JSON DOM construction for fixed message shapes, or use a binary WebSocket path if the provider supports one.

- [Major] `src/transcription/openai_realtime_streaming_service.cpp:228`, `src/transcription/openai_realtime_streaming_service.cpp:480`, `src/transcription/mistral_realtime_streaming_service.cpp:178`, `src/transcription/mistral_realtime_streaming_service.cpp:429` - Pre-connect audio buffering is unbounded and stores encoded JSON payloads.
  - Breaks: bounded memory, zero hot-path allocation, predictable latency under slow network.
  - Why it matters: while the WebSocket is connecting, audio is converted to JSON strings and pushed into `pendingAudioPayloads_` with no capacity cap. A slow or stuck connection can consume memory and then flush a large backlog.
  - Fix: bound the backlog by time/bytes, store raw PCM in fixed slots, and fail streaming/fallback when the pre-connect window exceeds a small threshold.

- [Major] `src/runtime/streaming_recording_controller.cpp:259`, `src/runtime/streaming_recording_controller.cpp:261`, `src/runtime/streaming_recording_controller.cpp:272`, `src/runtime/streaming_recording_controller.cpp:285` - Stop/finalize can block on worker joins and up to the configured finalization timeout.
  - Breaks: audio-captured-to-ready deterministic latency.
  - Why it matters: after stop, the worker thread closes the queue, joins the audio pump, commits the utterance, then waits for `finalized_` or `failed_`. The default timeout is 8000 ms (`config.example.json:25`). This is a large tail-latency cliff.
  - Fix: make stop post a finalize request and return immediately to the UI. Enforce smaller provider-specific finalization budgets. If finalization is not already in progress from live partials, fallback quickly.

- [Major] `src/runtime/streaming_recording_controller.cpp:310`, `src/runtime/streaming_recording_controller.cpp:324`, `src/runtime/streaming_recording_controller.cpp:402` - Even after the streaming transcript is assembled, the code writes the WAV before inserting text.
  - Breaks: fastest final-transcript-to-user-visible result.
  - Why it matters: `transcriptUtf8` is available before WAV persistence, but `WritePcm16Mono` runs before `InsertText`. Disk path creation and writing happen on the finalization worker before the user sees text.
  - Fix: if streaming produced a trusted transcript, insert first and persist WAV/archive afterward on a separate worker. Keep the retained PCM in memory for fallback and archive.

- [Major] `src/audio/wav_writer.cpp:28`, `src/audio/wav_writer.cpp:36`, `src/audio/wav_writer.cpp:82`, `src/audio/wav_writer.cpp:108`, `src/runtime/streaming_recording_controller.cpp:357` - Fallback transcription writes the WAV to disk, then reads it back into memory for upload.
  - Breaks: avoid unnecessary I/O and copies on the recovery path.
  - Why it matters: when streaming fails, fallback depends on a just-written WAV file. The OpenAI/Mistral one-shot clients then read the entire file into `std::vector<char>` before posting it.
  - Fix: build an in-memory WAV buffer from retained PCM for fallback upload, or let the fallback client accept PCM plus format and encode only once.

- [Major] `src/transcription/openai_transcription_service.cpp:21`, `src/transcription/openai_transcription_service.cpp:22`, `src/transcription/openai_transcription_service.cpp:23`, `src/transcription/openai_transcription_service.cpp:177`, `src/transcription/openai_transcription_service.cpp:186`, `src/transcription/openai_transcription_service.cpp:222`, `src/transcription/mistral_transcription_service.cpp:20`, `src/transcription/mistral_transcription_service.cpp:21`, `src/transcription/mistral_transcription_service.cpp:22`, `src/transcription/mistral_transcription_service.cpp:197`, `src/transcription/mistral_transcription_service.cpp:211`, `src/transcription/mistral_transcription_service.cpp:247` - Fallback can take multiple network attempts with long timeouts and sleeps.
  - Breaks: bounded audio-captured-to-ready latency.
  - Why it matters: fallback uses 3 attempts, 15 s connect timeout, 120 s request timeout, and retry sleeps. Correct for reliability, bad for interactive stop-to-text latency if streaming is unhealthy.
  - Fix: use a separate interactive fallback budget, surface a clear failure quickly, and optionally continue a slow retry in the background as a recovery affordance.

- [Major] `src/insertion/text_injector.cpp:15`, `src/insertion/text_injector.cpp:16`, `src/insertion/text_injector.cpp:48`, `src/insertion/text_injector.cpp:221`, `src/insertion/text_injector.cpp:247`, `src/insertion/text_injector.cpp:255`, `src/config/app_config.h:65`, `config.example.json:31` - Clipboard insertion adds fixed sleeps and optional clipboard restore work.
  - Breaks: transcript-ready-to-insert latency.
  - Why it matters: default `restoreClipboard = true` reads the existing clipboard, writes the transcript, sends Ctrl+V, sleeps 125 ms, and may write the previous clipboard back. Clipboard contention can add up to 5 x 50 ms before failure.
  - Fix: make restore optional/off by default for low-latency mode, shorten or target the settle delay, and add direct text injection paths for controls that support it.

- [Major] `src/transcription/openai_realtime_streaming_service.cpp:144`, `src/runtime/streaming_recording_controller.cpp:70`, `src/config/app_config.cpp:667`, `src/transcription/streaming_transcription_service.h:175`, `src/transcription/openai_realtime_streaming_service.cpp:498`, `src/transcription/mistral_realtime_streaming_service.cpp:447` - Streaming sample-rate requirements are not enforced correctly.
  - Breaks: correctness and provider-specific hardware/data assumptions.
  - Why it matters: config accepts any positive `audio.sample_rate`, but OpenAI's resampler is hard-coded from 16000 to 24000 and Mistral sends captured samples as if they are 16000. `requiredSampleRate` exists in capabilities, but the controller does not consult it.
  - Fix: reject incompatible capture settings per backend, configure capture to the provider-required rate, or construct resamplers from the actual `captureFormat_.sampleRate`.

- [Major] `config.example.json:38`, `config.example.json:39`, `config.example.json:40`, `src/config/app_config.h:25`, `src/config/app_config.h:27`, `src/audio/audio_recorder.cpp:256`, `src/audio/audio_recorder.cpp:259` - Silence trimming settings exist in config but are not represented in `AudioConfig` or applied in capture/streaming.
  - Breaks: avoid sending useless audio and reduce backend work.
  - Why it matters: the app streams every captured buffer, including silence. The config advertises silence trimming, but the runtime audio config only contains sample rate/channels/frames/max duration.
  - Fix: either remove the dead settings or implement pre-roll/post-roll VAD with explicit provider policy. For streaming, suppress silence-only chunks when safe or send explicit continuity silence only when required.

- [Major] `src/transcription/streaming_transcription_service.h:180`, `src/transcription/streaming_transcription_service.h:184`, `src/transcription/streaming_transcription_service.h:192`, `src/transcription/streaming_transcription_service.h:199`, `src/transcription/transcription_service.h:13`, `src/transcription/transcription_service.h:16` - Critical transcription paths use virtual interfaces.
  - Breaks: static polymorphism/no vtables on the hot path.
  - Why it matters: every append/commit/fallback call goes through virtual dispatch. For this app the network dominates, but the hard standard bans this from latency-critical code.
  - Fix: move provider dispatch to startup and use concrete session types behind `std::variant`, CRTP, or a function table allocated once outside the hot path.

- [Major] `src/runtime/streaming_recording_controller.cpp:124`, `src/runtime/streaming_recording_controller.cpp:168`, `src/runtime/streaming_recording_controller.cpp:219`, `src/runtime/streaming_recording_controller.h:85`, `src/audio/audio_recorder.h:40`, `src/runtime/app_host_internal.h:93` - Atomics rely on default sequential consistency.
  - Breaks: strict memory ordering.
  - Why it matters: `load`, `store`, `fetch_add`, and `exchange` omit memory-order arguments. The default is the strongest ordering and can introduce unnecessary fences.
  - Fix: define the ownership model and use the weakest correct order, usually relaxed for counters/diagnostics, release on publication, acquire on consumption, and acq_rel on state transitions.

- [Major] `src/runtime/streaming_recording_controller.h:112`, `src/runtime/streaming_recording_controller.h:113`, `src/runtime/streaming_recording_controller.h:114`, `src/runtime/streaming_recording_controller.h:115`, `src/audio/audio_recorder.h:40`, `src/runtime/app_host_internal.h:93` - Shared atomics and concurrently touched structs are not cache-line aligned.
  - Breaks: cache line alignment and false-sharing prevention.
  - Why it matters: multiple atomic flags live adjacent to other mutable controller fields. There are no `alignas(64)` boundaries or `sizeof` assertions for critical structs.
  - Fix: group hot atomics by ownership, align cross-thread state to cache lines, and add `static_assert` size/layout checks for queue slots and shared state.

- [Major] `src/transcription/transcript_assembler.h:62`, `src/transcription/transcript_assembler.h:63`, `src/transcription/transcript_assembler.cpp:54`, `src/transcription/transcript_assembler.cpp:62` - The transcript assembler uses node maps and growing vectors on backend event processing.
  - Breaks: contiguous memory only and zero hot-path allocation.
  - Why it matters: streaming deltas/finals flow through `TranscriptAssembler`. `std::map` is node-based and `trace_.patches.push_back` can allocate up to its cap. This is not the capture thread, but it is in the capture-to-ready transcript path.
  - Fix: for a single utterance, use direct state without maps. For multiple utterances, use a flat vector or flat map with reserved capacity. Reserve the patch trace at reset/start.

- [Major] `src/runtime/app_host_recording.cpp:169`, `src/runtime/app_host_recording.cpp:171`, `src/runtime/app_host_recording.cpp:176`, `src/runtime/app_host_runtime.cpp:204`, `src/runtime/app_host_recording.cpp:218`, `src/runtime/app_host_recording.cpp:223` - The UI thread can join post-recording workers.
  - Breaks: avoid blocking the control/event thread.
  - Why it matters: hotkeys arrive on the hidden window message loop. `StopRecordingAndWriteWav` joins any prior worker before starting a new one, and completion handling joins before resetting state. If a worker is slow to exit, the control surface stalls.
  - Fix: make worker ownership one-way: completion messages should carry enough state to retire workers without blocking the UI, or use `std::jthread` with nonblocking state handoff and a cleanup thread.

- [Major] `src/ui/status_pill.cpp:21`, `src/ui/status_pill.cpp:477`, `src/ui/status_pill.cpp:526`, `src/ui/status_pill.cpp:564`, `src/ui/status_pill.cpp:577` - Status pill animation redraws every 16 ms with per-frame GDI allocation.
  - Breaks: keep UI/control path lightweight.
  - Why it matters: this is not on the capture thread, but it runs on the same message loop that receives hotkeys and completion messages. `Render` creates a compatible DC and DIB section each frame before `UpdateLayeredWindow`.
  - Fix: reuse DIB/DC resources while size/DPI are stable, throttle when idle, and keep the hidden control window's hotkey dispatch isolated from animation work.

- [Major] `src/input/smtc_controller.cpp:67`, `src/input/smtc_controller.cpp:74`, `src/input/smtc_controller.cpp:82`, `src/input/smtc_controller.cpp:88` - SMTC toggle depends on WinRT/media transport plumbing and posts into the same UI message path.
  - Breaks: deterministic toggle latency for media-button control.
  - Why it matters: SMTC is useful for headset buttons, but it is not a low-latency input primitive. The event handler filters the button then posts a message to the hidden window, adding an OS mediation layer compared with `WM_HOTKEY`.
  - Fix: keep SMTC as convenience input, not the low-latency path. Measure it separately from the global hotkey path.

- [Major] `src/runtime/streaming_recording_controller.cpp:384`, `src/runtime/streaming_recording_controller.cpp:388`, `src/runtime/post_recording_workflow.cpp:173`, `src/runtime/post_recording_workflow.cpp:177`, `src/archive/archive_service.cpp:278`, `src/archive/archive_service.cpp:289`, `src/archive/archive_service.cpp:290` - Archive enqueue copies the whole sample buffer when archive is enabled.
  - Breaks: avoid large post-insert copies.
  - Why it matters: the archive request stores `.samples = samples`, which copies retained PCM before enqueueing. This is after insertion in the streaming path, but still before final success reporting and can make the app feel slower with archive enabled.
  - Fix: move an owned retained-audio buffer to archive after all foreground use is done, or write archive from the already persisted WAV/Opus path asynchronously.

- [Major] `src/config/app_config.cpp:962`, `src/config/app_config.cpp:991`, `src/runtime/app_host_settings.cpp:227`, `src/runtime/app_host_settings.cpp:228` - `transcription.streaming.provider` is loaded and saved but then overwritten from the file-transcription provider.
  - Breaks: configuration reliability and provider-specific latency tuning.
  - Why it matters: the config accepts a streaming provider value, then forces it to `mistral_realtime` or `openai_realtime` from `transcription.provider`. That makes it impossible to tune low-latency streaming independently from fallback/file transcription.
  - Fix: either remove the separate config key or honor it. For latency work, allow streaming and fallback providers to be selected independently.

- [Major] `src/transcription/openai_realtime_streaming_service.cpp:419`, `src/transcription/openai_realtime_streaming_service.cpp:439`, `src/transcription/mistral_realtime_streaming_service.cpp:365`, `src/transcription/mistral_realtime_streaming_service.cpp:386`, `src/runtime/streaming_recording_controller.cpp:193`, `src/runtime/streaming_recording_controller.cpp:310` - Live deltas are assembled but not exposed to any consumer before stop finalization.
  - Breaks: "audio captured -> ready transcription" latency potential.
  - Why it matters: if a backend emits useful partials during recording, the app applies them internally but only reads the final transcript after stop. The user gets none of the "mostly ready before stop" benefit.
  - Fix: add a trusted preview/finalization model: keep partials internal for UI or staged insertion, then only do final replacement when the backend final arrives.

- [Major] `src/transcription/openai_realtime_streaming_service.cpp:407`, `src/transcription/openai_realtime_streaming_service.cpp:409`, `src/transcription/mistral_realtime_streaming_service.cpp:350`, `src/transcription/mistral_realtime_streaming_service.cpp:352`, `src/runtime/streaming_recording_controller.cpp:430`, `src/runtime/streaming_recording_controller.cpp:435`, `src/archive/archive_service.cpp:86`, `src/archive/archive_service.cpp:125` - Exceptions are used in production code and not banned by the build.
  - Breaks: ban exceptions.
  - Why it matters: JSON parsing, streaming finalization, and archive code all rely on exceptions/catches/throws. The build does not enforce exceptions-off, and tests explicitly compile with `/EHsc`.
  - Fix: convert hot-path and background workflow errors to expected/status-return style. Add MSVC equivalents of exceptions-off where feasible, or isolate exception-using third-party boundaries outside latency-critical code.

- [Major] `CMakeLists.txt:85`, `CMakeLists.txt:86`, `CMakeLists.txt:117`, `CMakeLists.txt:118`, `CMakeLists.txt:121` - Compilation flags do not enforce the hard standards.
  - Breaks: strict compilation, exception ban, RTTI ban, optimized release policy.
  - Why it matters: `/W4` is present, but `/WX` is not. There is no `/GR-`, no explicit exceptions-off policy, no LTO/interprocedural optimization setting, no architecture-specific release tuning, and the test target enables `/EHsc`.
  - Fix: add project-wide warning-as-error, RTTI/exception policy, release IPO/LTO, and explicit release optimization settings. For MSVC, consider `/WX`, `/permissive-`, `/GR-`, `/O2` or `/Ox`, `/GL`, `/LTCG`, and a deliberate exception boundary policy.

- [Major] `.github/workflows/ci.yml:77`, `.github/workflows/ci.yml:78`, `scripts/package-release.ps1:178`, `scripts/package-release.ps1:183`, `CMakeLists.txt:125` - CI packages and smoke-tests, but does not run the unit test target or sanitizers.
  - Breaks: sanitizer CI and reliable regression gates.
  - Why it matters: `add_test(NAME streaming_transcription ...)` exists, but the GitHub workflow only runs `package-release.ps1`; that script runs staged smoke tests, not `ctest`. There is no ASan/TSan/UBSan or MSVC sanitizer equivalent job.
  - Fix: add a CI job that configures/builds tests and runs `ctest --output-on-failure`. Add sanitizer jobs where supported, or separate clang-cl sanitizer builds for Windows.

- [Minor] `src/audio/audio_recorder.cpp:244`, `src/audio/audio_recorder.cpp:259` - Capture uses PortAudio blocking reads instead of a callback stream.
  - Breaks: strict OS/blocking-call hot-path standard.
  - Why it matters: the blocking read loop is simpler, but the capture cadence depends on the worker's blocking read and scheduling. A callback stream can reduce capture handoff jitter if the callback is allocation-free.
  - Fix: use a PortAudio callback that writes into a preallocated lock-free ring, with all backend work outside the callback.

- [Minor] `src/audio/audio_recorder.cpp:102`, `src/runtime/streaming_recording_controller.cpp:114`, `src/runtime/streaming_recording_controller.cpp:115`, `src/runtime/app_host_recording.cpp:176` - Latency-sensitive threads are not pinned or priority-boosted.
  - Breaks: thread affinity and OS scheduling sympathy.
  - Why it matters: there is no evidence of `SetThreadAffinityMask`, `SetThreadIdealProcessor`, MMCSS (`AvSetMmThreadCharacteristics`), or thread-priority policy. Windows can move capture/pump threads across cores or schedule them behind unrelated work.
  - Fix: at least set MMCSS/pro-audio characteristics for capture and keep audio pump priority appropriate. Consider affinity only after measuring, since desktop apps can hurt overall system behavior if they pin aggressively.

- [Minor] `src/runtime/streaming_recording_controller.h:33`, `src/runtime/streaming_recording_controller.h:48`, `src/audio/audio_recorder.h:17`, `src/audio/wav_writer.h:13` - Output parameters are non-const references instead of explicit mutation pointers.
  - Breaks: explicit mutation API rule.
  - Why it matters: APIs like `Stop(std::vector<int16_t>& samples, std::wstring& failureReason)` hide mutation at the call site. This is more brittleness/readability than raw latency.
  - Fix: use pointer outputs for mutation or return structured result objects.

- [Minor] `src/audio/audio_recorder.h:15`, `src/runtime/streaming_recording_controller.h:66`, `src/archive/archive_service.h:31` - Resource-owning classes do not explicitly declare the full Rule of 5 or 0 surface.
  - Breaks: Rule of 5 or 0.
  - Why it matters: these classes own threads, queues, handles, or external subsystems. Some operations are implicitly deleted by members, but the hard standard requires explicit define/delete decisions.
  - Fix: explicitly delete copy and move operations, or implement safe moves marked `noexcept`.

- [Minor] `src/transcription/openai_realtime_streaming_service.cpp:422`, `src/transcription/mistral_realtime_streaming_service.cpp:367`, `src/runtime/streaming_recording_controller.cpp:282`, `src/runtime/streaming_recording_controller.cpp:347` - No branch prediction annotations on hot success/failure paths.
  - Breaks: branch prediction guidance.
  - Why it matters: this is a smaller issue than allocation/locking, but the hard standard expects `[[likely]]`/`[[unlikely]]` for biased paths.
  - Fix: only add annotations after measuring; prefer clear fast-path shape first.

## Expected ROI By Finding

ROI here means expected user-visible return divided by implementation cost. "Latency return" is an estimate from code structure, not a benchmark result. Use it to order work, then confirm with instrumentation.

| Finding | ROI | Latency / reliability return | Effort | Notes |
| --- | --- | --- | --- | --- |
| Backend/session work before microphone capture | Very high | Can remove cold credential/backend setup from keybind-to-capture. Tail savings can be tens/hundreds of ms, occasionally seconds if credential or backend setup stalls. | Medium/Large | Best first move if the app sometimes misses the first spoken word. |
| Per-recording thread creation and PortAudio open/start wait | Very high | Can make keybind-to-capture mostly an atomic state flip instead of stream setup. Tail savings can be tens/hundreds of ms on Windows audio devices. | Large | Highest ROI for "instant push-to-talk" feel. |
| Mutex/`std::queue` capture-to-streaming queue | Very high | Removes lock contention and allocation risk from every captured frame. Improves glitch resistance more than raw average latency. | Medium | Pairs naturally with fixed audio slots. |
| Per-buffer `std::vector<int16_t>` chunk allocation/copy | Very high | Removes allocator jitter every ~16 ms at the default buffer size. Improves capture stability and CPU usage. | Medium | Best fixed with the SPSC ring, not as a standalone micro-patch. |
| Dropped audio/events do not invalidate transcript | Very high | Prevents inserting silently corrupted transcripts. Reliability ROI is higher than latency ROI. | Small/Medium | Marking streaming untrusted on drop is cheap and important. |
| 80 ms append batch floor | High | Can reduce captured-audio-to-backend latency by roughly 60-70 ms if reduced to 10-20 ms and accepted by provider. | Small/Medium | Easy to experiment with; benchmark provider behavior before shipping. |
| RMS computed when status pill disabled | Medium | Saves a per-buffer scan and `sqrt` only when the UI meter is off. | Small | Cheap cleanup, but not a top latency bottleneck if the pill is normally on. |
| Multiple PCM copies before backend | High | Cuts CPU and memory bandwidth during recording; improves stability under load. | Medium | Fold into slab/ring redesign. |
| Stop copies retained sample vector | Medium | Saves a whole-recording copy on stop; impact grows with recording length. | Small | Likely quick win: move instead of copy after failure checks. |
| OpenAI resampler allocates per append | Medium/High | Removes per-append allocation for OpenAI realtime. | Small/Medium | Capture at 24 kHz for OpenAI may outperform resampling if device/provider path behaves well. |
| Base64/JSON allocation per append | Medium | Reduces streaming pump CPU and allocator churn; smaller effect than audio-thread fixes. | Medium | Use reusable buffers or fixed-shape serialization. |
| Unbounded pre-connect encoded backlog | High | Prevents memory growth and large delayed flushes under slow network. | Medium | More reliability/tail-latency ROI than happy-path speed. |
| Stop/finalize joins and 8 s timeout | Very high | Can remove the worst stop-to-ready tail. Savings can be seconds on backend stalls. | Medium | Set an interactive budget and fallback early. |
| WAV write before insertion | High | Lets text appear before disk persistence. Savings depend on recording length and disk state. | Medium | Good user-perceived latency win. |
| Fallback writes WAV then rereads it | Medium | Saves disk I/O and a full-file memory read on fallback only. | Medium | Important if streaming fallback happens often. |
| Long fallback retries/timeouts | High | Bounds pathological stop-to-ready delays from minutes to an intentional interactive budget. | Small/Medium | Product decision: fast fail vs background recovery. |
| Clipboard restore/retry/sleeps | High | Can save 125-500+ ms after transcript readiness, especially with clipboard contention. | Small/Medium | Low-latency mode should default restore off or use a direct insertion path. |
| Streaming sample-rate requirements not enforced | High | Prevents provider-mismatched audio and unnecessary/incorrect resampling. | Small/Medium | Correctness win with latency side benefits. |
| Silence trimming config not implemented | Medium | Can reduce backend load and streaming bytes during silence; may improve cost and final latency. | Medium | Needs careful VAD/pre-roll to avoid clipping speech. |
| Virtual interfaces in transcription path | Low/Medium | Minimal raw latency impact versus network/JSON/audio work; standards compliance and predictability win. | Large | Not worth doing before the allocation/blocking fixes. |
| Default sequentially consistent atomics | Low/Medium | Small CPU/fence savings; improves standards compliance. | Small/Medium | Requires documenting ownership/memory-order reasoning. |
| No cache-line alignment for shared state | Low/Medium | Helps under cross-thread contention; likely small until other hot-path issues are fixed. | Medium | Do after defining ring/shared-state layout. |
| Transcript assembler uses node maps/growing vectors | Low/Medium | Reduces backend-event allocation and pointer chasing. | Small/Medium | Higher ROI if live partials become UI-visible. |
| UI thread joins post-recording workers | Medium | Prevents message-loop stalls around stop/completion. | Medium | Important for perceived responsiveness and repeated dictation. |
| Status pill per-frame GDI allocation | Low/Medium | Improves UI loop headroom and animation smoothness. | Medium | Secondary unless profiling shows hotkey dispatch delay during animation. |
| SMTC toggle path uses WinRT/media layer | Low | Measurement/expectation fix more than code fix; global hotkey remains the low-latency input. | Small | Keep SMTC as convenience, not the latency benchmark. |
| Archive enqueue copies whole sample buffer | Low/Medium | Only matters when archive is enabled; can reduce post-insert tail and memory spikes. | Small/Medium | Move ownership after foreground use or archive from persisted audio. |
| Streaming provider config overwritten | Medium | Enables choosing fastest streaming backend independently from fallback backend. | Small | Configuration ROI is high if one provider streams faster but another is better fallback. |
| Live deltas not exposed before stop | Very high | Biggest potential "ready transcription" win: text can be mostly assembled before the user releases the key. | Large | Requires careful final correction/insertion semantics. |
| Exceptions used and not banned | Medium | Improves deterministic failure handling and policy compliance; small direct latency return. | Large | Isolate third-party exception boundaries before attempting global exceptions-off. |
| Compile flags do not enforce standards | High | Prevents performance/policy regressions before they ship. No direct runtime latency unless it enables LTO/release tuning. | Medium | Add `/WX`, release IPO/LTO, RTTI/exception policy deliberately. |
| CI does not run tests/sanitizers | High | Reliability ROI; catches broken streaming/fallback changes earlier. | Medium | Essential before invasive latency rewrites. |
| PortAudio blocking reads instead of callback stream | Medium/High | Can reduce capture jitter and enable true callback-to-ring design. | Large | Strong ROI after prewarmed stream/ring architecture is chosen. |
| Threads not priority-boosted or affinity-aware | Medium | Better under CPU load; average latency may not change much. | Small/Medium | Start with MMCSS for audio capture before hard affinity. |
| Non-const reference output parameters | Low | Readability/API explicitness, little direct latency. | Medium | Do opportunistically with result-object refactors. |
| Rule of 5/0 not explicit | Low/Medium | Prevents lifecycle bugs around threads/resources; little direct latency. | Small | Cheap safety cleanup. |
| No branch prediction annotations | Low | Tiny micro-optimization at best. | Small | Only after profiling confirms biased hot branches. |

## Additional Gaps Against The Hard Standards

- Kernel bypass is not present and is not realistic for third-party TLS WebSocket transcription. The app uses ixwebsocket and cpr over the normal Windows network stack (`src/transcription/openai_realtime_streaming_service.cpp:193`, `src/transcription/mistral_realtime_streaming_service.cpp:141`, `src/transcription/openai_transcription_service.cpp:186`). For this product, the practical equivalent is to avoid connecting on the keypress path, keep sessions warm where provider policy allows, and aggressively bound fallback latency.
- Data-oriented design is partial. Raw PCM samples are contiguous, but queued work items are metadata-heavy structs with strings and vectors (`src/transcription/streaming_transcription_service.h:33`, `src/transcription/streaming_transcription_service.h:63`). Queue slots should be fixed-layout and small.
- `constexpr`/compile-time execution is used for constants, but provider dispatch, JSON message building, and configuration-driven branching stay runtime (`src/runtime/streaming_recording_controller.cpp:27`, `src/transcription/openai_realtime_streaming_service.cpp:212`). That is acceptable for product flexibility, but not compliant with the hard standard.
- Const-by-default is not followed consistently. Many APIs use mutable locals and mutable output references. The practical priority is to fix capture/streaming allocation and blocking first, then tighten constness as part of API cleanup.

## Highest-Impact Fix Order

1. Make microphone capture prewarmed and started before any backend/credential/network work.
2. Replace `BlockingBoundedQueue` and per-chunk `std::vector` ownership with a preallocated SPSC audio ring.
3. Treat any streaming audio/event drop as untrusted streaming output and fallback immediately.
4. Move insertion before WAV persistence when streaming has a trusted final transcript.
5. Remove or lower the 80 ms append batch floor after provider-specific measurement.
6. Enforce streaming sample-rate compatibility and construct resamplers from the actual capture format.
7. Add CI gates: `ctest`, warning-as-error, RTTI/exception policy, and sanitizer/clang-cl coverage.
