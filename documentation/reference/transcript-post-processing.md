# Transcript Post-Processing

## Goal

Capture a future VoxInsert feature where the inserted text is not always the raw speech-to-text output.

The idea is to let the user speak in an instruction-heavy dictation style, then run one optional language-model rewrite step that turns that raw transcript into the text the user actually wants to paste.

This is a product and architecture note, not an implementation plan.

## Problem Statement

Raw transcription is good at preserving what the user literally said. That is not always the same thing as what the user wants inserted.

Some users naturally dictate like this:

- "I want to talk about the launch timeline. Mention that the Windows build is working now. Add a short thank-you at the end."
- "Write this as a short Slack message. Use the thumbs-up emoji."
- "Say that Friday works for me, then ask whether 2 PM is still open."

If VoxInsert inserts the raw transcript directly, the destination app receives the meta-instructions instead of the intended final wording.

## Desired Behavior

When this future feature is enabled, VoxInsert should support a two-stage text flow:

1. Convert audio into a raw transcript.
2. Rewrite that transcript into the final user-intended text.

The inserted text should be the rewritten result, not the literal raw transcript.

### Example

Spoken dictation:

```text
Write a short follow-up. Thank them for the quick turnaround. Say that Friday works for me. Add the thumbs-up emoji.
```

Raw transcript:

```text
Write a short follow-up. Thank them for the quick turnaround. Say that Friday works for me. Add the thumbs-up emoji.
```

Desired inserted text:

```text
Thanks for the quick turnaround. Friday works for me. 👍
```

## Current VoxInsert Baseline

Today VoxInsert inserts the raw provider transcript.

In the current flow:

- `TranscriptionClient::Transcribe(...)` returns UTF-8 text.
- `RunPostRecordingWorkflow(...)` converts that transcript to `std::wstring`.
- The result is passed directly to `TextInjector::InsertText(...)`.

That means a future rewrite step belongs between successful transcription and text insertion.

The current archive path also stores one transcript field, so this feature will need a deliberate decision about whether VoxInsert saves the raw transcript, the rewritten text, or both.

## Product Shape

The strongest version of this feature is likely:

- opt-in, not the default behavior
- separate from the speech-to-text provider choice
- phrased as a rewrite or interpretation mode, not as "better transcription"
- able to fall back to raw transcription cleanly if the rewrite step fails

The default VoxInsert experience should remain literal speech-to-text insertion unless the user explicitly enables a rewrite mode.

## Likely Pipeline Shape

Future processing would look like this:

1. Record audio.
2. Upload audio to the configured transcription provider.
3. Receive raw transcript text.
4. If rewrite mode is enabled, call a text model with the raw transcript plus a rewrite instruction.
5. Insert the rewritten result.
6. Preserve raw transcript data separately when useful for logs, archives, or debugging.

## Requirements And Guardrails

- If post-processing fails, VoxInsert should insert the raw transcript instead of losing the user's text.
- Added latency should stay modest enough that the feature still feels like dictation, not a full writing workflow.
- The rewrite stage should follow user instructions about tone, formatting, structure, emoji, and examples without inventing unrelated content.
- Literal dictation still matters. There should eventually be a way to force verbatim behavior for cases like quoting, code, identifiers, or exact wording.
- The feature should work as a second stage on top of the existing provider abstraction instead of becoming entangled with one specific speech-to-text backend.
- Archive and recovery behavior should stay understandable: users should know whether "last transcript" means raw text, inserted text, or both.

## UX Implications

If this is implemented later, it should probably be exposed as a separate setting such as:

- `Rewrite dictation into final text`
- `Interpret instruction-style dictation`
- `Post-process transcript before insert`

That setting should not silently replace the base transcription behavior for everyone.

The future settings surface may also need:

- a model selection or provider choice for the rewrite step
- a short system prompt or style instruction
- an easy way to disable rewriting temporarily
- clear wording about added latency and token cost

## Open Design Questions

- Which provider should run the rewrite step: OpenAI, Mistral, the same provider as speech-to-text, or a separate configurable text model?
- Should the archive JSON keep both `raw_transcript` and `final_text`?
- Should the tray actions for copy and reinsert use the rewritten output by default?
- Do we need a "verbatim" escape phrase or toggle for exact dictation?
- Should rewrite prompts be global, provider-specific, or user-editable?
- How much cleanup should be allowed before the feature becomes too creative or too lossy?

## Suggested Incremental Rollout

1. Keep this note as the product anchor.
2. Add a no-op post-transcription processing abstraction so the pipeline has a clean insertion point.
3. Preserve raw transcript and final inserted text as separate concepts in runtime state and archives.
4. Add an opt-in provider-backed rewrite implementation.
5. Add a small amount of observability so failures, latency, and fallback-to-raw behavior are easy to diagnose.

## Summary

The key design distinction is simple:

- raw transcription answers "what did the user say?"
- post-processing answers "what text did the user want produced?"

That is a meaningful product feature, not just a prompt tweak, so it should be implemented later as an explicit second stage in the transcription pipeline.