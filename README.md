# FrancoBoccaleDiBirra

A minimal terminal console for managing local LLM backends (llama.cpp today,
vLLM later), with mic transcription and a small structured-output "intent
brain" for natural-language commands.

## Quickstart

```
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
.\build\windows-msvc-debug\fbdb.exe
```

Everything below is optional — the console runs fine with none of it set and
just disables whatever it can't find.

### Config file

`<data_dir>/config.json` (`%LOCALAPPDATA%\FrancoBoccaleDiBirra\config.json`
on Windows) is written on first run and is meant to be hand-edited. Each
setting below has a matching key there (`llama_server_path`, `sherpa_exe`,
`orukeet_dir`, `qwen_model`, `qwen_port`, `calibr_root`) — set it once and
forget it, instead of exporting an env var every session.

### Resolution order

For each setting: its env var (if set) wins, then its config.json field,
then an auto-detected default where one is safe:

| Setting            | Env var              | Auto-detected default                                  |
|---------------------|-----------------------|----------------------------------------------------------|
| llama-server binary | `FB_LLAMA_SERVER`     | `llama-server.exe` on PATH                               |
| sherpa-onnx binary   | `FB_SHERPA_ONNX_EXE`  | searched under `<data_dir>/external/`                    |
| Orukeet model dir    | `FB_ORUKEET_DIR`      | searched under `<data_dir>/external/`                    |
| Qwen brain model     | `FB_QWEN_MODEL`       | none — must be set explicitly                            |
| Qwen brain port      | `FB_QWEN_PORT`        | built-in default (8090)                                  |
| calibr root          | `FB_CALIBR_ROOT`      | none — must be set explicitly                            |

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

The plan is four modes (command, scratchpad, diary, project), each with its
own color, its own centerpiece effect where the sphere normally sits, and
its own idea of "memory." The first three are built and selectable — project
(folder-scoped context) is still being designed. A line with a leading `/`
is always a command, in every mode. What happens to free text (no leading
`/`) depends on the mode, shown by the color of the header/`>`
prompt/cursor/F-key row and the `[Tab] ...` tag on the right of that row:

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
    sleep and checkpoint) shows "diary: autosaved (N pages, M ch memory)" in
    the status bar, and the mode tag persistently shows "saved Ns ago" so
    you're never left guessing whether it actually wrote to disk.
  - **Alt+Left/Alt+Right** browse the archived pages read-only (shown in
    place of the live history panel); switching modes away from diary
    resets you back to "live" for next time. Hypercube effect.

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
| `/download`                  | Reserved, not implemented yet.                                       |
| `/help`                      | Print this command list in the status bar.                           |
| `/quit`                      | Exit.                                                                 |

## Keys

- **F1** — `/help`. **F2** — `/load` (nothing registered yet? opens the file
  picker, then adds *and loads* whatever you pick in one step). **F3** —
  `/unload`. **F4** — `/add` (opens the file picker; doesn't auto-load).
  **F5** — `/remove`. Each acts immediately when there's exactly one
  model to act on; otherwise it falls back to the same "usage" hint typing
  the command would give you (tab-completes from there).
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

Basic markdown gets cleaned up rather than shown as literal syntax: `#`/`##`
headings render bold in accent color with the `#`s stripped, and `**bold**`/
`` `code` `` delimiters are stripped with the inner text kept plain. This is
not a real markdown renderer — no nested styling, no italics, and
underscore emphasis (`__bold__`) is deliberately left alone rather than
risking corrupting `snake_case` names or `__dunder__` methods, which a
naive "strip all underscore pairs" pass does in practice.
