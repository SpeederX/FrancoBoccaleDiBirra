# Meeting mode

Meeting is a local workspace for editable notes, recorded system audio, a
speaker-attributed transcript, and a separate meeting report. The report contains
a summary, decisions, and action items. Audio, notes, and outputs are retained
locally, as requested. Meeting data is not added to diary memory.

## Implementation scope

- Windows system-output capture through the existing miniaudio WASAPI backend.
  This records what plays on the output device, including other applications.
  It does not include the user's microphone unless that audio is played there.
- A multiline notes editor remains usable during capture and processing.
- First record, then transcribe and process. Imported audio/video goes through
  the same pipeline, with FFmpeg converting it to mono 16 kHz PCM WAV.
- Parakeet runs as a local child process. Its `scene --json` output provides
  timed utterances and stable speaker slots across the recording.
- Speaker labels are numbered in order of first appearance. Explicit user names
  override names inferred from introductions. Unattributed speech stays marked
  as unattributed; it is not treated as a newly identified person.
- The LLM runs against a selected local llama.cpp backend. It produces a cleaned
  transcript and report separately. The original ASR output is preserved.
- Notes and long transcript processing use background jobs. Long inputs are
  processed in bounded parts rather than cut off to fit a chat turn.

## Upstream qualification

Checked 2026-10-08 against parakeet.cpp v0.6.1, commit
`5c9b0fbbd8e611d88e47d7e1ebae9bca9b447e8f`:

- [Repository](https://github.com/mudler/parakeet.cpp)
- [Diarization](https://github.com/mudler/parakeet.cpp/blob/v0.6.1/docs/diarization.md)
- [CLI](https://github.com/mudler/parakeet.cpp/blob/v0.6.1/docs/cli.md)
- [Speaker identification limits](https://github.com/mudler/parakeet.cpp/blob/v0.6.1/docs/speaker.md)

The Windows CPU release successfully processed upstream's 23.605 s two-speaker
fixture in 4.521 s including process startup and model load. This is one
qualification clip, not a general performance or accuracy claim. The output
contains six utterances with recurring slots 0 and 1. Names mentioned in the
fixture are literary references, not introductions.

Qualification model: `parakeet-bundle-small.gguf`, SHA256
`5f2c6697eb08d4d67e858692d4223a9d911aacc51ae16cf2aee9e760f8a2269a`.
This bundle's ASR is English only. Multilingual meetings need a compatible
multilingual ASR model such as Parakeet TDT 0.6B v3 plus the diarization component.
The diarization checkpoint supports up to eight speakers; overlapping voices
and movie soundtracks need direct sample evaluation.

Before feature changes, the main checkout baseline built in this worktree with
Visual Studio 2022 x64 and all three existing CTest tests passed in Debug.
The user's main checkout and its uncommitted files were preserved.
