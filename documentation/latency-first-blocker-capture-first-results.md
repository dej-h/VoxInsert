# First latency blocker fix: capture before backend startup

Date: 2026-06-03

Scope: first blocker from `documentation/latency-hard-standards-review.md`: recording startup did backend/session work before microphone capture started.

## Change

`StreamingRecordingController::Start` now starts `AudioRecorder::Start` before allocating/creating/starting the streaming backend session.

If capture starts but backend startup fails, `Start` now returns success because recording is active. The controller disables streaming forwarding, records the backend failure, and `Finish` falls back to one-shot file transcription using retained PCM. This avoids stopping/restarting capture after the user has already started speaking.

## Before artifact

The original review captured the bad order in the pre-fix source:

```text
documentation/latency-hard-standards-review.md:7
backend CreateSession: src/runtime/streaming_recording_controller.cpp:97
backend session Start: src/runtime/streaming_recording_controller.cpp:108
audio recorder Start: src/runtime/streaming_recording_controller.cpp:117
```

This means backend session setup happened before capture startup.

## After artifact

Current source-order check:

```text
rg -n "request_\.audioRecorder->Start|backend_ = CreateBackend|backend_->CreateSession|session_->Start|streaming startup latency order=capture_first" src\runtime\streaming_recording_controller.cpp

105:    const bool recorderStarted = request_.audioRecorder->Start(
148:        backend_ = CreateBackend(streaming.provider);
157:        session_ = backend_->CreateSession(
171:                    "streaming startup latency order=capture_first backend_ready=false capture_start={}ms backend_setup_after_capture={}ms total={}ms provider={} queued_audio_chunks={}",
181:        if (!session_->Start(failureReason)) {
191:                    "streaming startup latency order=capture_first backend_ready=false capture_start={}ms backend_setup_after_capture={}ms total={}ms provider={} queued_audio_chunks={}",
214:                "streaming startup latency order=capture_first backend_ready=false capture_start={}ms backend_setup_after_capture={}ms total={}ms provider={} queued_audio_chunks={}",
235:                "streaming startup latency order=capture_first backend_ready=false capture_start={}ms backend_setup_after_capture={}ms total={}ms provider={} queued_audio_chunks={}",
267:                "streaming startup latency order=capture_first backend_ready=false capture_start={}ms backend_setup_after_capture={}ms total={}ms provider={} queued_audio_chunks={}",
289:                "streaming startup latency order=capture_first backend_ready=false capture_start={}ms backend_setup_after_capture={}ms total={}ms provider={} queued_audio_chunks={}",
303:            "streaming startup latency order=capture_first backend_ready=true capture_start={}ms backend_setup_after_capture={}ms pumps_start_after_capture={}ms total={}ms provider={} queued_audio_chunks={}",
```

Result: `AudioRecorder::Start` is now line 105, before backend allocation at line 148, `CreateSession` at line 157, and `session_->Start` at line 181.

## Runtime log artifact

The fix adds a debug log that records real startup timings when a streaming recording starts:

```text
streaming startup latency order=capture_first backend_ready=true capture_start={}ms backend_setup_after_capture={}ms pumps_start_after_capture={}ms total={}ms provider={} queued_audio_chunks={}
```

Failure path log:

```text
streaming startup latency order=capture_first backend_ready=false capture_start={}ms backend_setup_after_capture={}ms total={}ms provider={} queued_audio_chunks={}
```

The key verifiable field is `order=capture_first`. The timing fields separate capture readiness from backend setup, so a local run can show whether capture was ready before backend setup finished.

## Verification commands

Debug build:

```text
.\scripts\build-debug.ps1
```

Result:

```text
[2/2] Linking CXX executable VoxInsert.exe
```

Observed build warnings:

```text
warning C4530: C++ exception handler used, but unwind semantics are not enabled. Specify /EHsc
```

These warnings are pre-existing compile-policy debt already captured in the hard-standards review.

Unit tests:

```text
ctest --test-dir out\build\windows-msvc-debug --output-on-failure
```

Result:

```text
1/1 Test #1: streaming_transcription ..........   Passed    0.02 sec
100% tests passed, 0 tests failed out of 1
Total Test time (real) =   0.05 sec
```

## Expected measured difference

Before the fix, capture could not begin until backend session setup completed.

After the fix, capture readiness is independent of backend setup. The new log exposes this directly:

```text
capture_start=<microphone startup cost>ms
backend_setup_after_capture=<credential/backend setup cost after capture is already active>ms
```

The practical win is not that backend setup is faster; it is that backend setup no longer blocks the first captured audio frame.
