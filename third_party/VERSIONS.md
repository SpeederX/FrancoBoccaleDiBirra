# Vendored dependencies

Single-header/small-source libraries, committed as plain files (no submodules,
no package manager) so the build works offline and stays reproducible.

| Library | Version/tag | License | SHA-256 | Source |
|---|---|---|---|---|
| cJSON | v1.7.18 | MIT | `0578cc29132912edbc88f83207a8fc76e5db3db0605497e909a9384ef3cc474` (cJSON.h)<br>`75c51de8fa40ac9d7a99319c6330719bd692eb81c0a869265f3d4c682533f9b` (cJSON.c) | https://github.com/DaveGamble/cJSON |
| termbox2 | v2.5.0 | MIT | `ef63b6bd6e3a1c8ad339b7aa0bf52dc21f3e92bd886264ebe588ab86ccd1e5d1` | https://github.com/termbox/termbox2 |
| miniaudio | 0.11.21 | Public domain / MIT-0 | `6b2029714f8634c4d7c70cc042f45074e0565766113fc064f20cd27c986be9c9` | https://github.com/mackron/miniaudio |

## Notes

- **termbox2 has no Windows backend** (verified: zero `_WIN32`/`windows.h`
  references in the vendored header; it hard-requires `termios.h`,
  `sys/ioctl.h`, `unistd.h`). It is only compiled on POSIX targets via
  `src/tui/backend_termbox2.c`. Windows uses a hand-written console backend at
  `src/platform/win/console_win.c` implementing the same `ScreenOps` interface
  — see `docs/ARCHITECTURE.md`.
- miniaudio is built with `MA_NO_DECODING`, `MA_NO_ENCODING`, `MA_NO_ENGINE`,
  `MA_NO_NODE_GRAPH`, `MA_NO_GENERATION`, `MA_NO_RESOURCE_MANAGER` defined
  (set in `CMakeLists.txt`) — we only ever use the raw capture device API.
- cJSON is compiled as a small static library target (`third_party/cjson/`).

## External, non-vendored runtime dependencies (not committed to this repo)

These are acquired separately by the user and referenced by path in the
console's own config file — never bundled, both for size and licensing
reasons:

- **llama-server** (llama.cpp) — whichever build the user already has
  configured; multiple candidates already exist on this machine.
- **sherpa-onnx** prebuilt CLI (e.g. `sherpa-onnx-non-streaming-asr-x64-*.exe`)
  — Apache-2.0, https://github.com/k2-fsa/sherpa-onnx/releases.
- **Orukeet** ONNX INT8 model (`encoder`/`decoder`/`joiner.int8.onnx` +
  `tokens.txt`) — code MIT, **weights CC BY-SA 4.0**:
  https://github.com/Oruk-AI/orukeet. Attribution is shown in the console's
  `/help`/about screen; weights are never bundled or redistributed.
