# Meeting mode

Meeting is a local workspace for editable notes, recorded system and microphone audio, a
speaker-attributed transcript, and a separate meeting report. The report contains
a summary, decisions, and action items. Audio, notes, and outputs are retained
locally, as requested. Meeting data is not added to diary memory.

## Start and use Meeting

Build and prepare the local transcription tools once on Windows:

```powershell
cmake -S . -B build\native -G "Visual Studio 17 2022" -A x64
cmake --build build\native --config Release --parallel 8
.\scripts\prepare-meeting.ps1
.\scripts\start-meeting.ps1
```

Preparation pins Parakeet v0.6.1 and the model repository revision, verifies
SHA256 hashes, and keeps model credits/licences beside the downloaded files.
It downloads about 1.28 GB. The models stay under
`%LOCALAPPDATA%\FrancoBoccaleDiBirra\external\meeting` by default; use
`-AssetsDirectory <folder>` on both scripts to choose another location.
`-CacheDirectory <folder>` lets preparation reuse previously downloaded files.
The launcher selects multilingual **Parakeet TDT 0.6B v3 Q8_0** for transcription
and the diarization component from `parakeet-bundle-small.gguf`. Parakeet.cpp is
the local runtime; its models are downloaded once by preparation, rather than
embedded in Franco. A separate configured LLM handles names, transcript cleanup
and the report; it receives text, not audio. Qwen is not required for ASR.

1. Meeting opens with help on the right. Enter `/meeting new` to begin.
2. Type notes on the left; Enter adds a line. Notes autosave locally.
3. Press Ctrl+Space to start recording. The default is **system + microphone**;
   transcription appears on the right in audio blocks of at most 20 seconds.
   Press Ctrl+Space to pause, then again to resume the **same meeting and speaker
   session**. F8 switches between notes and commands.
4. Use `/meeting speakers` to inspect the numbered voices and name evidence.
   Assign or correct a name with `/meeting name 1 Alice Smith`, including while
   recording or processing. User names take priority over inferred names.
5. Select the local LLM with `/meeting model <port> <exact-model-id>` and run
   `/meeting process`. A model loaded through the app's `/load` command can also
   be selected automatically for processing. With a selected LLM, new transcript
   text also triggers name identification from introductions or clear addressed
   names and replies. `/meeting identify` retries that step. Use F9 to cycle outputs.
   For a complete final report, run `/meeting finish`, wait for transcription to
   finish, then `/meeting process`.
6. Use `/meeting list`, then `/meeting open <full folder path>` to reopen a meeting.

An already-running llama.cpp endpoint can be selected at launch:

```powershell
.\scripts\start-meeting.ps1 -LlmPort 8080 -LlmModel "your-loaded-model-id"
```

The script does not start or download an LLM. Plain `fbdb --meeting` uses saved
settings. CLI overrides `--parakeet`, `--library`, `--asr`, `--diar`, `--ffmpeg`,
`--meeting-folder`, `--llm-port` and `--llm-model` apply to that session;
`/meeting setup`, `/meeting folder`, `/meeting source` and `/meeting model`
save settings in the app's config file.

Use `/meeting sources` to list the available devices, then select:

```text
/meeting source both
/meeting source system
/meeting source microphone
```

The selected source uses Windows' default output and/or microphone. To choose a
different physical device, change the default in Windows audio settings. Pause
before changing sources; the selection applies on the next start or resume.
Both sources are mixed into one mono recording; individual tracks are not saved.
With speakers instead of headphones, microphone pickup can duplicate remote audio.

`/meeting stop` pauses, like Ctrl+Space. `/meeting finish` closes the session and
flushes the last speech. Parakeet keeps some context pending, so a short tail can
remain until resuming or finishing; 20 seconds is the maximum input block size,
not a guaranteed display latency. Finalized sessions need a new meeting for
another recording. Paused sessions can resume while the app remains open.

For a local audio/video file, start a fresh meeting, press F8, and enter
`/meeting import <file path>`. Paths can contain spaces. FFmpeg must be on PATH
or configured using `/meeting setup ffmpeg <executable path>` (or the launcher's
`-FFmpeg` parameter). The original file is retained unchanged. Each meeting has
one recording/import; create another meeting for another source.

## Retained data and processing

By default, meetings are stored beside **the executable**, in
`meetings\<timestamp>`. For this worktree's Release build that is
`build\native\Release\meetings`. Show or change the default for new meetings:

```text
/meeting folder
/meeting folder D:\Meetings
```

The setting is persisted. Existing meetings keep their paths. Choose a writable
folder whose parent exists. The launcher also accepts `-MeetingFolder <path>` as
a session override. Transcript models remain in their separate assets directory.

| File | Contents |
| --- | --- |
| `audio.wav` | Selected sources mixed into one recording, or converted import; mono 16 kHz PCM |
| `notes.txt` | Editable notes, autosaved and saved on exit |
| `parakeet.scene.jsonl` | Original Parakeet output |
| `transcript.json` | Timings, utterance IDs, raw text, speaker labels and name evidence |
| `transcript.txt` | Readable original ASR transcript with speaker labels |
| `transcript-clean.txt` | LLM cleanup; unfinished utterances visibly marked as pending |
| `report.md` | Separate summary, decisions, action items and attributed user notes |
| `notes-used.txt` | Notes snapshot used by the latest LLM processing attempt |
| `report-parts.txt` | Partial reports retained for an incomplete processing attempt |
| `transcript-previous.json` | Previous transcript before an ASR retry |
| `speaker-name-proposal.json` | Latest LLM name proposal, including supporting quotes |
| `*.stderr.log` | Local tool diagnostics |

Audio and ASR remain local. Processing sends text and the notes snapshot only to
the selected **localhost** LLM endpoint. That endpoint controls any further
handling of the requests. There is no automatic deletion or diary ingestion.
Notes remain editable while recording/transcription/LLM jobs run. Later edits
require `/meeting process` again to enter the report.

Speaker 1, Speaker 2, etc. are recording-local labels in order of first appearance.
Unknown slot -1 becomes Unattributed. Introduction-derived names are marked
`introduction_inferred` with an exact supporting quote and utterance ID. The LLM
is asked to identify self introductions; mentions of another person's name do
not establish identity. Clear direct addresses followed immediately by the
addressed voice's reply can yield `dialogue_inferred` names, with the quote and
both utterance IDs. For example, voice 1 asks "Mario, puoi inviarlo?" and voice 2
answers: Mario is proposed for voice 2. A mention that Giulia is absent should
not name either voice. These are LLM inferences, available for review and correction.
User anchors take priority. There is no persistent voice
enrollment or identity recognition between meetings.

`/meeting process` uses a snapshot of the transcript and notes available when it
starts. It corrects punctuation/obvious transcription errors into
`transcript-clean.txt`, then writes a separate summary, decisions and action items
to `report.md`. The original transcript is retained. During live recording, it
processes completed utterances only; later speech or edited names/notes require
processing again. Identification alone changes names, without cleaning text or
creating a report.

ASR retries preserve names when raw text, timings and slot attribution match.
Changed ASR output requires reviewing the new numbers; previous anchors remain
in `transcript-previous.json`. A prior report is marked out of date after a retry
or a speaker-name change, and after an unfinished processing attempt.

`/meeting cancel` ends capture and cancels transcription/LLM jobs. Streaming ASR
finishes its current DLL call before returning; legacy CLI ASR is terminated.
An in-flight LLM
HTTP request must return or reach its 120-second timeout before cancellation ends.
The app remains responsive during that wait; quitting also waits for it. Failed
processing retains raw inputs and completed cleanup parts. The output pane shows
up to 2000 lines; the complete exported files remain available on disk.

## Implementation scope

- Windows system-output and microphone capture through miniaudio. System capture
  records what plays on the default output device, including other applications.
  Source selection defaults to both; microphone capture is now included directly.
- A multiline notes editor remains usable during capture and processing.
- Record and transcribe concurrently. Imported audio/video goes through
  the same pipeline, with FFmpeg converting it to mono 16 kHz PCM WAV.
- Parakeet's v0.6.1 C library keeps one scene session and loaded models across
  blocks and pauses. Its scene output supplies timings and recording-local slots.
  Headless transcription can still use the CLI if no library is configured.
- Speaker labels are numbered in order of first appearance. Explicit user names
  override names inferred from introductions or addressed-name/reply pairs. Unattributed speech stays marked
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
- [Streaming C API](https://github.com/mudler/parakeet.cpp/blob/v0.6.1/docs/capi.md)
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

## Local validation, 2026-10-08

The user-supplied AVI was read without modification. Two mono PCM clips were
extracted at 00:05:00 and 00:20:00, each 60 seconds long. Parakeet's multilingual
`tdt-0.6b-v3-q8_0.gguf` provided Italian ASR; the small bundle supplied diarization.

| Input | Measured time | Output |
| --- | --- | --- |
| 60 s movie clip at 00:05:00 | 28.083 s, CPU scene CLI including model load | 14 utterances, 3 numbered voices, 2 unattributed utterances |
| 60 s movie clip at 00:20:00 | 25.759 s, native import/transcribe pipeline | 8 utterances, 4 numbered voices |
| WASAPI recording of upstream speech fixture | 24.12 s retained audio, 0 dropped frames | Native transcription: 6 utterances, 2 numbered voices |
| Quiet system output after output-clock fix | 5.000 s / 80000 frames | 0 dropped frames; quiet periods preserved |
| Same 60 s Italian clip through native C API, blocks up to 20 s | 21.389 s including conversion/model load | 14 utterances; progressive output |
| Native UI: system + microphone, Ctrl+Space pause/resume, then finish | 48.2 s / 771200 retained frames | 0 dropped frames; 11 utterances; same directory and Parakeet session |

The first loopback experiment showed missing elapsed time during quiet output;
the recording now keeps the output clock active with zero-valued playback on
the same endpoint. The quiet-period check above verifies that correction.

Multilingual model SHA256:
`4d69a4a6683f4f2d952bad794c1357ca6eb628027695b4699c5a9ad4cd07d757`.
Parakeet Windows CPU archive SHA256:
`d74b4e857f75f91ae215c5e91ec42bf15ec84ed3a369423c134dbd6422243ed8`.
Parakeet Windows CPU library archive SHA256:
`95b41515efeab246094d1793a1c0a12d727bc1cc01e4c28fe37a16a994b26774`.

An isolated local Gemma 4 E4B Q4_K_XL instance on the RTX 2070 completed the LLM
pipeline on the first movie clip. This checks transport, schema validation and
separate exports; model-generated cleanup/report quality still needs user review.
It also inferred Alice from a synthetic transcript's explicit self introduction,
kept the voice mentioning Bob numbered, and reported Bob's stated task/deadline.
That check used fixture ASR text with the real LLM, not a speech recognition test.
The same local Gemma instance also linked Mario to the replying voice in a
synthetic Italian direct-address/reply pair, while leaving the mention of absent
Giulia unassigned. A first proposal used text instead of a numeric utterance ID
and was rejected; the prompt now includes the required numeric-ID example.
The movie ASR contains audible-dialogue uncertainties and transcription errors;
no word error rate or diarization accuracy score is claimed. The full film was
not processed. Overlapping speech, speaker-count limits and self introductions
need review with actual meeting recordings.

The native UI smoke test types multiline notes, saves/reopens a meeting, assigns
a name, cycles modes/outputs and verifies notes remain editable during a
cancelled background job. The rendered cell grid was also inspected. This does
not replace trying the actual terminal controls and hearing the recording.
Additional tests cover mixing, WAV append/resume, incremental output, stable
voice numbering across blocks, retained anchors on retry, dynamic name updates,
manual overrides during jobs, and preservation of cleanup during identification.
All 12 CTest suites passed in Debug and Release.

On 2026-10-09, native startup diagnostics reproduced a blank initial terminal
while the existing microphone initialized on the UI thread. In that run, the
first frame took 3938 ms from entering main, including 3922 ms in microphone
initialization. Microphone startup now runs in a background worker; the next
diagnostic run rendered the first frame after approximately 32 ms. Reading the
real console buffer confirmed the Meeting heading and commands at 120 x 30.
These two startup checks are diagnostics, not a general benchmark. Tests cover
a blocked initialization, device-open/start failures, shutdown while opening,
and successful metering/recording after readiness. The ordinary modes show
`mic opening` until ready; Meeting retains its independent source capture.

Run the complete suite (Python 3 enables the pipeline test):

```powershell
ctest --test-dir build\native -C Debug --output-on-failure
ctest --test-dir build\native -C Release --output-on-failure
```

For acceptance, make a short recording with two people introducing themselves,
type a note while recording, pause, resume, then finish and check speaker boundaries
against playback. Process it, correct one name, regenerate the report, and reopen the
folder to check all data persisted. Confirm the report uses the note and only
assigns owners/deadlines that were stated.
