# Development

Read the human-maintained [AGENTS.md](AGENTS.md) before changing this project.
This guide supplies the repository commands; AGENTS.md supplies project policy.

## Verify a change

From the checkout you changed, run:

```powershell
npm ci --prefix tools/agent-service --no-audit --no-fund
./scripts/verify.ps1
```

The command checks patch whitespace, builds the application, and runs CTest in
both Debug and Release, then checks and tests the Pi adapter. A failed build or
test returns a failing command. CI runs
the same command on Windows and Linux for pull requests and pushes to main.

Windows needs CMake and Visual Studio with **Desktop development with C++**.
The script discovers Visual Studio and selects x64 without depending on an
inherited compiler environment. Linux needs PowerShell, CMake, Ninja, and a C11
compiler. Full verification also needs Node.js 22.19 or newer and npm; the
native CMake build itself remains independent of Node. For a Ninja build on
Windows, use a matching x64 developer shell and
pass `-Generator Ninja`. `-Configuration Debug` is useful during iteration;
run both configurations before committing.

CTest includes `check_failure`, an intentional failing check whose nonzero exit
is required for success. It verifies that test checks remain active with
`NDEBUG`, rather than letting Release skip checks and setup operations.

## Repository map

| Path | Responsibility |
| --- | --- |
| `src/app/` | Mode selection, command dispatch, UI updates |
| `src/backend/` | Model process lifecycle and health |
| `src/net/` | HTTP and llama.cpp requests |
| `src/chat/` | Background chat jobs |
| `src/diary/` | Pages, archives, consolidation, memory |
| `src/config/` | Application configuration and model registration |
| `src/platform/` | Windows/POSIX process, thread, filesystem adapters |
| `src/tui/` | Terminal controls and rendering |
| `tests/` | Native regression tests |
| `tools/agent-service/` | Shared Pi SDK adapter and JSON-lines worker |
| `third_party/VERSIONS.md` | Vendored source identities |

Use a feature worktree for substantial changes. Run verification from that
worktree so its source and build artifacts match. Keep commits small enough to
review and revert independently. Do not edit the human-maintained AGENTS.md.
