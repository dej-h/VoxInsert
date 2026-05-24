# Mistral Transcription APIs

## Overview

Mistral's audio story is split across three related but different surfaces:

1. Chat completions with audio-capable Voxtral models
2. Offline transcription via `/v1/audio/transcriptions`
3. Realtime transcription for live streams

This split matters because the best-known Voxtral model names are not interchangeable across those surfaces.

The most important implementation fact for VoxInsert is this:

- `voxtral-small-latest` is an audio-capable model for chat use cases
- `voxtral-mini-latest` is the model currently supported on Mistral's dedicated offline transcription endpoint

That is why `voxtral-small-latest` fails with `invalid_model` on `/v1/audio/transcriptions`.

## Relevant Endpoints And Surfaces

### Chat completions with audio input

Mistral's audio docs describe Voxtral Small and Voxtral Mini as audio-capable models for chat use cases.

This is the path to use if you want to send audio and ask the model to do something broader than plain transcription, for example:

- transcribe with instructions
- summarize spoken content
- answer questions about audio
- mix audio understanding with normal chat behavior

Practical shape:

- use the chat completions API
- provide audio as base64, URL, or uploaded input depending on the integration path
- get back an assistant response rather than a dedicated STT payload

This is the path where Voxtral Small is relevant.

### `POST /v1/audio/transcriptions`

This is Mistral's dedicated offline speech-to-text endpoint.

The official offline transcription docs currently state that it supports:

- `voxtral-mini-latest`

This endpoint is optimized for straight transcription and supports specialized parameters such as:

- `context_bias`
- `language`
- `diarize`
- `timestamp_granularities`

For VoxInsert's current upload-a-finished-WAV architecture, this is the correct first-party Mistral endpoint.

### Realtime transcription

Mistral also exposes a live transcription path for streaming audio.

The current realtime docs call out:

- model: `voxtral-mini-transcribe-realtime-2602`
- audio format examples use PCM `s16le`
- `target_streaming_delay_ms` can trade latency for more context
- realtime is currently not compatible with `diarize`

This is the path to use if VoxInsert later grows from push-to-talk file upload into live partial transcript streaming.

## Voxtral Small vs Voxtral Mini

This is the distinction that caused the integration error.

| Model | Current official Mistral role | Best way to think about it |
| --- | --- | --- |
| `voxtral-small-latest` | Audio-capable chat model | Better general audio understanding path for chat-like use cases |
| `voxtral-mini-latest` | Audio-capable chat model and current offline transcription model | The first-party model Mistral currently supports for `/v1/audio/transcriptions` |
| `voxtral-mini-transcribe-realtime-2602` | Realtime transcription model | Low-latency live stream path |

The short version:

- Voxtral Small is not the current drop-in model for Mistral's transcription endpoint.
- Voxtral Mini is the current first-party transcription endpoint model.

## Can We Access Voxtral Small If We Want?

Yes, but not through the same endpoint VoxInsert currently uses.

If the goal is to use Voxtral Small through Mistral's official API, the practical route is:

1. use Mistral chat completions with audio input
2. pass the recorded audio as an audio message payload
3. ask the model to produce a clean transcript
4. parse the assistant text output

That means Voxtral Small is accessible, but it is a separate integration path.

What would change in VoxInsert if we wanted that:

- we would keep the current finished-audio workflow
- but we would add a second Mistral transport implementation for chat-with-audio
- the request payload would be chat-style rather than transcription-style multipart
- the response would be assistant content rather than the dedicated `text` field returned by the transcription endpoint

So the right mental model is:

- yes, Voxtral Small is available
- no, it is not a valid model id for Mistral's dedicated offline transcription endpoint today

## Useful Transcription Parameters

Mistral's offline transcription docs call out several parameters worth tracking.

### `context_bias`

This is Mistral's equivalent to a vocabulary biasing hint.

The docs describe it as useful for:

- names
- technical terms
- proper nouns
- domain-specific vocabulary

The docs also note:

- it is optimized for English
- support for other languages is experimental
- up to 100 words or phrases can be provided

For VoxInsert, this is the parameter that should carry product names, coding terms, acronyms, and technical vocabulary.

### `language`

Useful when the app already knows the input language. Mistral notes that the service can perform language detection, but explicitly setting language can improve accuracy.

### `diarize`

Useful for multi-speaker audio when the output needs speaker separation.

### `timestamp_granularities`

Useful when you need segment- or word-level timing.

Important note from the docs:

- timestamps are currently not compatible with `language`; choose one or the other for a request when that limitation applies

## Benchmark Snapshot

Benchmark source: Artificial Analysis Speech-to-Text leaderboard, snapshot read on 2026-05-24.

Relevant snapshot for Mistral and nearby competitors:

| Model | Provider | AA-WER | Speed Factor | Price / 1000 min | Readout |
| --- | --- | ---: | ---: | ---: | --- |
| `voxtral-small-latest` | Mistral | 2.9% | 65.0 | $4.00 | Best-known Voxtral accuracy story |
| `voxtral-mini-latest` via transcription | Mistral | 3.7% | 51.9 | $1.00 | Strong value option |
| Voxtral Mini Transcribe 2 | Mistral | 3.7% | 73.2 | $3.00 | Faster follow-on benchmark entry |
| `gpt-4o-transcribe` | OpenAI | 4.1% | 31.1 | $6.00 | More expensive and slower in this snapshot |
| `gpt-4o-mini-transcribe` | OpenAI | 4.6% | 47.3 | $3.00 | Cheaper OpenAI option, still weaker on AA-WER |

What that benchmark implies:

- Voxtral Small is the more exciting accuracy story.
- Voxtral Mini Transcribe is the more practical first-party endpoint choice today.
- The official endpoint support and the benchmark winner are not the same decision.

That is the core trap with Mistral audio right now.

## Practical VoxInsert Notes

For the current app architecture, the best first-party Mistral path is:

1. record a bounded file
2. upload it to `/v1/audio/transcriptions`
3. use `voxtral-mini-latest`
4. pass technical vocabulary through `context_bias`

Good default guidance for VoxInsert:

- use `voxtral-mini-latest` for the current dedicated transcription endpoint
- use `context_bias`, not an OpenAI-style prompt mental model
- consider Voxtral Small only if we intentionally add a chat-with-audio implementation path
- treat realtime as a separate future feature, not a toggle on the existing request path

## Recommended Product Framing

For practical decisions inside this repo:

- If the goal is the simplest Mistral transcription integration, choose Voxtral Mini Transcribe.
- If the goal is the best Voxtral accuracy and we are willing to build a second transport path, investigate Voxtral Small through chat completions with audio.
- If the goal is live low-latency captioning, investigate Mistral realtime transcription separately.

## Official References

- Mistral offline transcription: https://docs.mistral.ai/studio-api/audio/speech_to_text/offline_transcription
- Mistral realtime transcription: https://docs.mistral.ai/studio-api/audio/speech_to_text/realtime_transcription
- Mistral chat completions: https://docs.mistral.ai/studio-api/conversations/chat-completion
- Artificial Analysis STT leaderboard: https://artificialanalysis.ai/speech-to-text