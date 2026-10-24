# Vendored: switch_ESP32

Everything in this directory is third-party code, copied verbatim. **Do not
modify it.** If a change is ever genuinely required, record it here with a diff
and a reason, so the copy stays auditable against upstream.

| | |
| --- | --- |
| Upstream | https://github.com/esp32beans/switch_ESP32 |
| Commit | `0adba99d9c2b32c86aed21cb74558cc35841530e` (branch `main`) |
| Retrieved | 2026-09-25 |
| License | MIT — Copyright (c) 2023 esp32beans@gmail.com (see `LICENSE`) |
| Modifications | **none** |

## Files and verified sizes

Byte counts checked against the upstream git tree at the commit above; all match.

| File | Bytes |
| --- | --- |
| `switch_ESP32.h` | 3419 |
| `switch_ESP32.cpp` | 6708 |
| `LICENSE` | 1077 |
| `README.md` | 322 |
| `examples/GamepadDemo/GamepadDemo.ino` | 1828 |

Upstream's `.gitignore` was not copied (it applies to the upstream repo, not to
this subdirectory).

## Why it is here rather than installed

Arduino IDE compiles the contents of a sketch's `src/` subfolder recursively, so
placing the library here makes the repo self-contained: no "Add .ZIP Library"
step for whoever clones it, and no chance of silently building against a
different version of the library than the one this project was tested with.

Two notes on that arrangement:

- `examples/GamepadDemo/GamepadDemo.ino` sits inside `src/`. Arduino only
  compiles `.ino` files at the sketch root, so this one is ignored. If a future
  IDE version ever tries to build it, move `examples/` out of `src/` rather than
  deleting it.
- Our own code never includes or subclasses `NSGamepad` directly — it goes
  through the `TimedPad` wrapper at the sketch root. That seam is what keeps this
  directory untouched.

## Line endings

These files are stored with LF endings exactly as upstream serves them. The repo
currently has `core.autocrlf=false` and no `.gitattributes`, so nothing rewrites
them. If CRLF normalisation is ever switched on repo-wide, add

```
firmware/shiny_pad/src/switch_ESP32/** -text
```

to `.gitattributes` to keep this directory byte-identical to upstream.
