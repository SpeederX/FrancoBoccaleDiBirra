# FrancoBoccaleDiBirra

A minimal terminal console for managing local LLM backends (llama.cpp today,
vLLM/SGLang planned), with mic transcription, direct-URL GGUF downloads, and
a small structured-output "intent brain" for natural-language commands.

## Quickstart

For development checks, run `./scripts/verify.ps1` from PowerShell. It builds
and tests Debug and Release; see [CONTRIBUTING.md](CONTRIBUTING.md) for prerequisites.
The Ninja commands below need an x64 Visual Studio developer shell on Windows.

```
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
.\build\windows-msvc-debug\fbdb.exe
```

Everything below is optional — the console runs fine with none of it set and
just disables whatever it can't find.

### Config file

`<data_dir>/config.json` (`%LOCALAPPDATA%\FrancoBoccaleDiBirra\config.json`
on Windows) is written on first run and is meant to be hand-edited. It's the
single source of truth for every setting below — no environment variables
are read for any of it, so there's exactly one place to look, not two.

### Resolution order

For each setting: its config.json field, then an auto-detected default
where one is safe:

| Setting            | config.json key      | Auto-detected default                                  |
|---------------------|-----------------------|----------------------------------------------------------|
| llama-server binary | `llama_server_path`  | `llama-server.exe` on PATH                               |
| sherpa-onnx binary   | `sherpa_exe`          | searched under `<data_dir>/external/`                    |
| Orukeet model dir    | `orukeet_dir`         | searched under `<data_dir>/external/`                    |
| Qwen brain model     | `qwen_model`          | none — must be set explicitly                            |
| Qwen brain port      | `qwen_port`           | built-in default (8090)                                  |
| calibr root          | `calibr_root`         | none — must be set explicitly                            |

Diary's page/memory thresholds are also config.json-only, tuning knobs
rather than machine-specific paths: `diary_page_close_chars` (default
8000), `diary_checkpoint_chars` (default 5000), `diary_idle_close_minutes`
(default 10). 0 or unset means "use the default"; each is clamped to a hard
buffer cap regardless of what's set.

The sherpa-onnx exe and Orukeet's 4 model files just need to exist
*somewhere* under `<data_dir>/external/` (any subfolder depth) — that's
where a first-run setup would drop the downloaded SDK/model archives, so a
fresh checkout with nothing configured at all still finds them. Qwen has no
auto-detect on purpose: a models folder can hold GGUFs from under 1B to 30B+
parameters, and guessing one for the always-resident "lightweight" brain
would be exactly the wrong kind of surprise. calibr's root is likewise
always explicit — there's no reliable way to guess an arbitrary checkout
location on disk.

Orukeet auto-detects the spoken language on its own, so mixing languages
(e.g. Italian + English in the same recording) needs no setup — `/lang` is
just an informational label, not a switch.

## Input modes

This checkout has three selectable modes: command, scratchpad, and diary.
The `feature/project-mode` worktree contains a fourth-mode UI scaffold;
the coding agent is not implemented yet. Each current mode has its own color,
centerpiece effect, and memory behavior. A line with a
leading `/` is always a command, in every mode. What happens to free text
(no leading `/`) depends on the mode, shown by the color of the header/`>`
prompt/cursor/F-key row and, split across the two rows just above the
command line, the `[Tab] ...` mode/navigation tag (top) and that mode's live
stats (bottom):

- **command (cyan, default)** — free text goes to the Qwen intent brain,
  which tries to match it to one of the commands below. Sphere effect.
- **scratchpad (amber)** — free text goes straight to whatever model was
  loaded most recently (not the brain), as a real back-and-forth
  conversation with memory of earlier turns in the same session (a bounded
  20-turn window — older turns quietly drop off rather than growing
  forever). Loading a different model resets that memory, since a new
  model has no business inheriting another one's conversation. Pencil-doodle
  effect.
- **diary (violet)** — same live conversation mechanics as scratchpad
  (most-recently-loaded model, background thread, spinner while a reply is
  in flight), but backed by a real memory pipeline instead of scratchpad's
  throwaway 20-turn window:
  - Everything said in diary mode accumulates into an open **page** (raw
    text) that closes — starting background "sleep" consolidation — on
    whichever comes first: ~10 minutes idle, ~8000 characters, or you
    switching away from diary and back (a manual close). Closed pages are
    archived to disk, never discarded.
  - Sleep sends the closed page to the Qwen brain (always the brain,
    regardless of what you're chatting with — consolidation stays cheap
    either way) with a distillation prompt, and folds the result into a
    single **long-term memory** file.
  - Once long-term memory passes ~5000 characters, a **checkpoint** pass
    re-summarizes it against itself to keep it compact.
  - Long-term memory persists across restarts (`<data_dir>/diary/`) and
    gets excerpted into the first message of a fresh diary conversation, so
    the model actually carries the distilled past forward. Every save (both
    sleep and checkpoint) shows "diary: autosaved (N pages, M chars memory)"
    in the status bar, and the mode tag persistently shows "page #N",
    "archived: N pages", "memory: M chars", and "saved Ns ago" so you're
    never left guessing what's been written where.
  - **Alt+Left/Alt+Right** browse the archived pages read-only (shown in
    place of the live history panel). Pressing **Enter** on an empty command
    line while browsing asks to reopen that page — "this page was from a
    previous session, are you sure you want to reopen it?" — which makes it
    the live, editable page again (new turns extend that page's own file
    directly) after safely archiving whatever was open before, untouched.
  - **F7**/`/new_page` manually closes the current page and starts a fresh
    one — same safe archive-and-consolidate path as an automatic close, just
    triggered on demand. Hypercube effect.
- **project (planned)** — a coding mode under development in a separate worktree.

Scratchpad also has `/new_page`/F7, but since scratchpad keeps no durable
record at all by design, it asks first: "start a new scratchpad page?
everything here will be lost unless archived — discard, archive, or
cancel? (y/N/a)" — `a` writes the current transcript to
`<data_dir>/scratchpad/<timestamp>.txt` before clearing.

Both scratchpad's and diary's live chat requests can take a while for a
large model; the console stays responsive while waiting (background
thread), and once a reply lands the mode tag shows the last request's
context usage and prefill/decode speed (`147/4096 ctx 86/97 t/s`), read
straight from llama-server's own `/props` and `/v1/chat/completions`
response.

**Tab** switches modes. Inside `/load `/`/unload `, Tab still autocompletes
the model name instead — mode-switching only kicks in outside that context.

## Commands

Type at the `>` prompt.

| Command                    | What it does                                                        |
|-----------------------------|----------------------------------------------------------------------|
| `/add [name] [path]`       | Register a local `.gguf`. No path opens an inline file picker.       |
| `/load [name]`              | Start a registered model via llama-server. Name optional if only one is registered. |
| `/unload [name]`            | Stop a running model. Name optional if only one is loaded.           |
| `/remove [name]`            | Unregister a model (unloads it first if running). Name optional if only one is registered. |
| `/lang [code]`              | Show/set a language label (informational only — ASR auto-detects).   |
| `/leaderboard`               | Show calibr's benchmark results (needs calibr root configured).      |
| `/logs`                      | Tail calibr's action log (needs calibr root configured).             |
| `/dashboard`                 | Back to the main view from leaderboard/logs.                         |
| `/list` or `/models`         | Show registered models and which are currently loaded.               |
| `/new_page`                   | Scratchpad/diary only. Manually rotate the current page (see Input modes). |
| `/download <URL>`            | Download a direct HTTP(S) `.gguf`, then register it automatically.   |
| `/download cancel`             | Stop the active transfer; its partial file remains resumable.        |
| `/help`                      | Print this command list in the status bar.                           |
| `/quit`                      | Exit.                                                                 |

## Model downloads

`/download <URL>` accepts a direct HTTP(S) URL whose path ends in `.gguf`.
Hugging Face `/blob/` links are converted to their `/resolve/` download form.
The transfer runs through `curl` in the background, so the console stays
responsive. Files land under `<data_dir>/models/`; an in-progress transfer
uses a `.part` suffix and the same command resumes it after interruption.
On completion the filename becomes the model's registry name (with a numeric
suffix if that name is already taken), ready for `/load`. `/download cancel`
stops the process without deleting the resumable partial file. The stderr log
for the most recent attempt is `<data_dir>/logs/download/last.stderr.log`.

This first version expects `curl` on PATH (`curl.exe` is included with current
Windows releases). It deliberately starts from an explicit file URL; catalog
search and gated-model authentication can be layered on later.

## Keys

- **F1** — `/help`. **F2** — `/load` (nothing registered yet? opens the file
  picker, then adds *and loads* whatever you pick in one step). **F3** —
  `/unload`. **F4** — `/add` (opens the file picker; doesn't auto-load).
  **F5** — `/remove`. **F6** — `/list`. **F7** — `/new_page` (scratchpad/
  diary only; hidden from the legend in command and project mode, where it
  doesn't apply). Each acts immediately when there's exactly one model to
  act on; otherwise it falls back to the same "usage" hint typing the
  command would give you (tab-completes from there).
- **Ctrl+Space** — start/stop mic recording; on stop, transcribes and drops the
  result into the command line for you to review/edit before pressing Enter.
- **Left/Right** — move the cursor; **Backspace** deletes before it.
- **Tab** — switches input mode (see above), or autocompletes a model name
  after `/load `/`/unload `.
- **Up/Down** — scroll `/leaderboard` and `/logs`.
- **PageUp/PageDown** — scroll the history panel (dashboard only). Reading
  back through older exchanges doesn't get interrupted by new ones arriving
  — auto-follow only resumes once you've scrolled back to the bottom.
- **Alt+Left/Alt+Right** — diary mode only: browse archived pages read-only.
  **Enter** on an empty line while browsing asks to reopen that page as the
  live one (see Input modes).
- **Esc** — back to the dashboard from any other view.
- **Ctrl+C** — quit (same as F10 or `/quit`).

## History panel

The dashboard's right-hand column logs every exchange, labeled by mode:
"You: ..." / "Qwen: ..." in command mode (the model's raw JSON response,
unparsed, so you can see exactly what it decided and why), or "You: ..." /
"`<model name>`: ..." in scratchpad and diary mode. **Each mode has its own
separate panel** — command, scratchpad, and diary's live view are three
distinct logs, not one shared feed, because they really are three separate
conversations underneath (separate turn histories sent to the model).
Switching modes with Tab switches which panel you're looking at; nothing
from one mode's chat leaks into another's. Long replies wrap to the panel's
width and respect the model's own line breaks rather than running on as one
line. Each panel auto-follows its own latest exchange (unless you've
scrolled back with PageUp — see Keys); the column disappears on narrow
terminals rather than squeezing everything else.

The model's reply is shown verbatim, raw markdown syntax and all — an
earlier pass stripped `**`/`##`/etc., but it visibly mangled real replies
(cutting text, corrupting `snake_case`/`__dunder__` names), so this is
deliberately plain passthrough now rather than a half-working renderer.

## Dev tools: `--sweep-summarize`

A separate CLI mode on the same binary, for iterating on diary's
consolidation instructions without touching production data or the live
pipeline:

```
fbdb.exe --sweep-summarize --source=<page:N|ltm|all|test:N|test:all|file:PATH>
         [--models=<name1,name2,...>|all] [--load-timeout=<seconds>]
         [--experimental] [--out=DIR] [--no-open]
```

Loads each named model in turn (reused across every source before being
unloaded, not reloaded per source), sends it the instruction the live
pipeline actually uses, and writes a local HTML report — one tab per model,
original text next to that model's summary, opened in the default browser
when done. `--source=test:N`/`test:all` runs a handful of fixed synthetic
scenarios (`FB_SWEEP_TEST_SCENARIOS` in `sweep.c`) built to catch specific
failure modes (e.g. treating an assistant's unconfirmed guess as fact) with
a known-correct answer, independent of whatever's actually in your diary
data. `--experimental` swaps in an alternate instruction pair under active
development, never wired into the live pipeline — see `sweep.c` for what's
currently different and why. `--load-timeout` (default 180s) matters for
large models that need heavy CPU/mmap offload on a small GPU; a timeout is
reported distinctly from an actual crash.
