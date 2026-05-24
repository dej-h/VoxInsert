# OpenAI Transcription APIs

## Overview

OpenAI exposes speech-to-text primarily through its Audio API. For VoxInsert, the most relevant path is a finished-file upload to the transcription endpoint.

OpenAI currently has three distinct transcription surfaces worth tracking:

1. File-based transcription via `/v1/audio/transcriptions`
2. File-based translation via `/v1/audio/translations`
3. Realtime transcription for ongoing audio streams

The main architectural distinction is simple:

- If you already have a finished WAV or MP3 file, use the file-based Audio API.
- If you want live partial transcript deltas while the user is still speaking, use the Realtime transcription flow instead.

For VoxInsert's current push-to-talk flow, the file-based transcription endpoint is the right default.

## Relevant Endpoints

### `POST /v1/audio/transcriptions`

Use this when the output should stay in the original language of the audio.

Supported model families called out in the current OpenAI speech-to-text guide:

- `gpt-4o-transcribe`
- `gpt-4o-mini-transcribe`
- `gpt-4o-transcribe-diarize`
- `whisper-1`

Useful notes:

- This is the core endpoint for VoxInsert's current implementation style.
- `gpt-4o-transcribe` and `gpt-4o-mini-transcribe` support `json` and `text` responses.
- `gpt-4o-transcribe-diarize` adds speaker-aware output.
- `whisper-1` has the broadest legacy feature support, but it is not the best fit if the goal is the newer GPT-4o transcription path.

### `POST /v1/audio/translations`

Use this when the input audio may be in another language but the output must be English.

Current OpenAI guide notes:

- Translation is separate from transcription.
- Translation output is English text.
- This endpoint currently supports only `whisper-1`.

This matters because `gpt-4o-transcribe` is not a drop-in replacement for translation use cases.

### Realtime transcription guide

OpenAI's speech-to-text guide explicitly separates realtime transcription from finished-file uploads.

Use this path when:

- audio is still being recorded
- you want partial deltas before the user stops speaking
- you want the service to participate in turn detection or ongoing stream handling

Do not confuse this with `stream=true` on a completed file upload. Those are different flows.

## Model Notes

### `gpt-4o-transcribe`

Best fit when you want OpenAI's stronger transcription model in the current file-upload architecture.

Useful characteristics from the current docs:

- supports prompts
- supports `json` and `text`
- supports streamed output for completed-file transcription
- benchmark position is respectable, but not class-leading on independent price/speed/accuracy trade-offs

### `gpt-4o-mini-transcribe`

This is the lighter and cheaper OpenAI transcription option.

Useful characteristics:

- same general file-based endpoint shape as `gpt-4o-transcribe`
- cheaper than `gpt-4o-transcribe`
- faster on benchmark snapshots
- somewhat weaker accuracy on benchmark snapshots

### `gpt-4o-transcribe-diarize`

Use this when speaker labels matter.

Important caveats from the current guide:

- available via `/v1/audio/transcriptions`
- supports `diarized_json`
- requires `chunking_strategy` for inputs longer than 30 seconds
- does not support prompts
- does not support `timestamp_granularities[]`
- not currently supported in the Realtime API

### `whisper-1`

Still relevant when you specifically need features OpenAI currently reserves for Whisper.

Important notes from the current guide:

- supports transcription and translation
- supports `json`, `text`, `srt`, `verbose_json`, and `vtt`
- supports timestamp granularities
- does not support streamed completed-file transcription

## File Limits, Formats, And Output Constraints

OpenAI's current speech-to-text guide calls out these input constraints for file uploads:

- file size limit: 25 MB
- supported input types: `mp3`, `mp4`, `mpeg`, `mpga`, `m4a`, `wav`, `webm`

That fits VoxInsert's current short WAV upload approach, but only as long as recordings stay bounded.

Practical implication:

- short dictation clips are fine as WAV
- long recordings will need chunking or a more compact format

## Prompting And Accuracy Controls

OpenAI's newer GPT-4o transcription models support the `prompt` parameter.

That is useful for VoxInsert because the app's dictation often contains:

- product names
- code terms
- acronyms
- mixed technical vocabulary

Prompting is especially useful for:

- preserving punctuation style
- biasing domain vocabulary
- carrying context across chunked audio
- preventing common technical terms from being misheard

OpenAI also exposes log probability output on the GPT-4o transcription models, which can be useful later if VoxInsert ever wants confidence-aware UX.

## Timestamp And Speaker Features

Feature support is not uniform across OpenAI models.

- timestamps: currently tied to `whisper-1`
- diarization: handled by `gpt-4o-transcribe-diarize`
- prompting: available for `gpt-4o-transcribe` and `gpt-4o-mini-transcribe`, not for the diarize model

This means model selection is partly a feature-selection decision, not just an accuracy decision.

## Benchmark Snapshot

Benchmark source: Artificial Analysis Speech-to-Text leaderboard, snapshot read on 2026-05-24.

Artificial Analysis reports three headline metrics that are useful for product decisions:

- AA-WER: lower is better
- Speed Factor: higher is better
- USD per 1,000 minutes: lower is better

OpenAI-relevant snapshot:

| Model | Provider | AA-WER | Speed Factor | Price / 1000 min | Readout |
| --- | --- | ---: | ---: | ---: | --- |
| `gpt-4o-transcribe` | OpenAI | 4.1% | 31.1 | $6.00 | Strong but not leading on value |
| `gpt-4o-mini-transcribe` | OpenAI | 4.6% | 47.3 | $3.00 | Cheaper and faster, less accurate |
| `voxtral-mini-latest` via transcription | Mistral | 3.7% | 51.9 | $1.00 | Better value reference point |
| `voxtral-small-latest` | Mistral | 2.9% | 65.0 | $4.00 | Better accuracy reference point |

How to interpret that snapshot:

- OpenAI's transcription models are viable, but they are not currently the strongest price/performance option in the public benchmark.
- `gpt-4o-transcribe` is more expensive and slower than the Mistral transcription path benchmarked there.
- `gpt-4o-mini-transcribe` narrows the cost gap, but also gives up some accuracy.

Benchmark caveat:

- These numbers change over time.
- Artificial Analysis uses its own AA-WER methodology across multiple datasets.
- Product fit should still consider reliability, endpoint shape, account setup, and feature availability.

## Practical VoxInsert Notes

For the current app architecture, OpenAI remains straightforward because:

1. the app already records a finished file before upload
2. the endpoint accepts direct file uploads
3. prompting is useful for technical dictation
4. the response shape is simple when using `json` or `text`

Good default guidance for VoxInsert:

- use `/v1/audio/transcriptions` for the current push-to-talk flow
- use `gpt-4o-transcribe` when accuracy matters more than cost
- use `gpt-4o-mini-transcribe` when cost or latency matters more
- keep translation as a separate feature decision because it is Whisper-only today
- treat realtime transcription as a separate architecture, not a toggle on the current file-upload flow

## Official References

- OpenAI speech-to-text guide: https://developers.openai.com/api/docs/guides/speech-to-text
- OpenAI realtime transcription guide: https://developers.openai.com/api/docs/guides/realtime-transcription
- OpenAI audio API overview: https://developers.openai.com/api/docs/guides/audio
- Artificial Analysis STT leaderboard: https://artificialanalysis.ai/speech-to-text