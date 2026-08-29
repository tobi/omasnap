# Omasnap — Agent Guide

Omasnap is a super fast, native Wayland screenshot and annotation overlay,
built for [Omarchy](https://omarchy.org) on Hyprland. It captures region,
window, or full monitor (plus a scrolling-region mode that stitches a taller
page into one image), then opens an annotation editor with vector layers
(arrows, lines, freehand, highlighter, rectangles, ellipses, numbered
markers, text, OCR). Finished captures go to clipboard,
`~/Pictures/Screenshots`, or a pinned always-on-top layer surface.

## Project principles

Each of these has a longer writeup under `docs/` — read it before making a
change that touches the principle, not just this summary.

- **A specialized tool, not a general app.** Omasnap does one job — capture,
  annotate, output — and does it fast. It is not a drawing program, not a
  file manager, not a general Wayland utility. A feature that isn't in
  service of "screenshot, mark it up, send it somewhere" doesn't belong
  here, however useful it might be on its own.
- **The main thread never blocks.** Capture, paint, and input handling are
  the UI thread's whole job. Disk I/O on a full-resolution image, spawning
  a process, PNG encoding — all of it runs on a worker via
  `QtConcurrent`/`QFutureWatcher`, never inline. See
  [docs/threading.md](docs/threading.md).
- **Every operation is undoable.** The operation log is the source of
  truth; the visible image is rebuilt from it. Rendering for editing is a
  pure, repeatable function of that log — nothing is baked into the working
  image as you draw. Output is applied only on **Copy**, **Save**, or both,
  which is the one moment a flattened image is produced. Redaction is the
  deliberate, documented exception: it must actually destroy pixels at
  render time so nothing recoverable leaks into an export, while remaining
  a normal, undoable log entry until you export. See
  [docs/editing-model.md](docs/editing-model.md).
- **Minimally configurable — pre-configured to be right, like Omarchy.**
  No settings UI, no wizards, no onboarding. The defaults are the product;
  a config key is a narrow escape hatch for a real divergent need (where to
  save, what to name it, preset colors), never a general mechanism. Adding
  a new key needs the same justification the existing ones had, not "this
  would be nice to expose."
- **Speed first.** Instant capture, annotate, copy. No startup bloat.
- **Wayland only, Hyprland only.** Monitor/window discovery, input
  injection quirks, and notification conventions are all Hyprland-specific
  on purpose. Code that happens to also run on another wlroots compositor,
  because it stands on a real Wayland protocol rather than a Hyprland
  shortcut, is a fine accident — it is not a target, not tested, and not a
  bug magnet we chase. A PR that supports another compositor (niri, KDE,
  etc.) is welcome **only if it adds zero complexity to the Hyprland
  path** — no compositor branching, no backend abstraction, no new
  dependency pulled in just for it. Otherwise it's rejected; fork it
  instead. No X11, no macOS/Windows. See
  [docs/platform-scope.md](docs/platform-scope.md).
- **Lean, learned dependencies.** The dependency set is Qt6 + LayerShellQt +
  wayland-client, plus shelling out to a few existing Omarchy tools
  (`hyprctl`, `wl-copy`/`wl-paste`, `tesseract`, `omarchy-notification-send`)
  instead of linking their equivalents in-process. Know this list before
  proposing an addition to it. See [docs/dependencies.md](docs/dependencies.md).
- **Single small binary.** Everything (capture, editor, pin mode, scroll
  capture) runs from the one `omasnap` executable. Every new dependency or
  vendored asset is weight every install carries.
- **No backwards compatibility.** Break keybindings, CLI flags, file
  formats, or internals whenever it keeps the code simpler or the tool
  faster. Do not add compatibility shims, deprecation aliases, or migration
  code.
- **Omarchy aesthetics.** `omarchy-notification-send` when available,
  `OMASNAP_OCR_LANGS`/`OMARCHY_OCR_LANGS` fallback for OCR languages,
  minimal vector-drawn icons (no icon-theme dependency), the bundled Neucha
  font.

## Repository layout

| Path | Purpose |
|---|---|
| `src/main.cpp` | CLI parsing, single-instance lock, mode dispatch |
| `src/instance-lock.cpp/.hpp` | Single-instance handover: cancel a running overlay, or stop it and take over for `--file` |
| `src/capture.cpp/.hpp` | Capture, render pipeline, output (clipboard/save/notify), source+JSON operation-log persistence, config loading glue |
| `src/editor.cpp/.hpp` | Annotation editor: tools, vector layers, operation-log undo/redo, the select↔edit phase machine, export |
| `src/overlay-chrome.cpp/.hpp` | Shared chrome every overlay wears: the capture-kind tab strip, hotkey legend, status pill |
| `src/scroll-capture.cpp/.hpp` | The scroll-capture panel: region-live page, manual/auto mode, grips, stitched result |
| `src/scroll-inject.cpp/.hpp` | Auto-scroll wheel injection (uinput / `zwlr_virtual_pointer_v1`) |
| `src/auto-capture.cpp/.hpp`, `src/stitch.cpp/.hpp` | Pure, offline-testable frame classification and stitching |
| `src/stitch-replay.cpp` | Standalone tool: replay a dumped frame directory through the stitcher with no compositor |
| `src/surface-capture.cpp` | In-process output/window capture via `ext-image-copy-capture` |
| `src/cut.cpp/.hpp` | Cut-band tool: remove a strip and collapse the gap |
| `src/clip.cpp/.hpp` | Clip-out: copy a path (rect/ellipse/lasso), punch a hole, lift as a layer |
| `src/recent-snaps.cpp/.hpp` | The recents shelf: shelving/reopening working documents |
| `src/output-config.cpp/.hpp`, `src/palette-config.cpp/.hpp` | The optional `omasnap.conf` INI: output destination/filename, color presets |
| `src/pin.cpp/.hpp`, `src/pin-file.cpp/.hpp`, `src/pin-layout.cpp/.hpp` | Pinned-capture layer-shell surfaces (bottom-right, all workspaces) |
| `src/icons.cpp/.hpp` | Vector icon renderer for toolbar and pin controls |
| `src/cli-path.cpp/.hpp` | Command-line image target resolution |
| `src/eyedropper.cpp/.hpp` | Display-to-source color sampling |
| `tests/*-smoke.cpp/.hpp` | Headless Qt Test coverage: offscreen region clicks, async capture, single-instance handover, stitching fixtures |
| `docs/` | Longer writeups of the principles above — read before changing behavior they cover |
| `install-omarchy` | Omarchy installer (deps via `omarchy-pkg-add`, installs to `~/.local`) |
| `CMakeLists.txt` | Build definition; **the version lives here** (`project(omasnap VERSION ...)`) |

## Build and verify

```bash
make check
```

`make check` configures and builds the project, runs the complete headless
offscreen Qt smoke suite (including simulated region clicks and asynchronous
capture), runs `clang-tidy`, and runs `clazy-standalone`/`qmllint` when those
tools and source types are available. Use `make build` for a build-only pass,
`make smoke` for the behavioral smoke suite, and `make install` to install to
`~/.local`.

Always run `make check` after behavioral changes. CI
(`.github/workflows/build-linux.yml`) runs the same build and smoke on every
push and PR.

Dependencies (Arch): `base-devel cmake ninja pkgconf qt6-base layer-shell-qt
wayland wayland-protocols wl-clipboard tesseract tesseract-data-eng`. See
[docs/dependencies.md](docs/dependencies.md) before adding to this list.

## Release process

1. Bump `project(omasnap VERSION ...)` in `CMakeLists.txt`.
2. Build and run the smoke test (above).
3. Commit, tag `v<version>`, push main and the tag. The GitHub workflow
   attaches the build artifact to the release automatically.
4. **Update omarchy-pkgs on every new version release.** In the
   [omarchy-pkgs](https://github.com/omacom-io/omarchy-pkgs) fork
   (`pkgbuilds/omasnap/`):
   - Set `pkgver` in `PKGBUILD` to the new version.
   - Replace `sha256sums` with the hash of
     `https://github.com/tobi/omasnap/archive/refs/tags/v<version>.tar.gz`
     (`curl -sL <url> | sha256sum`).
   - Commit on a branch and open a PR to `omacom-io/omarchy-pkgs`.

See `README.md` for user-facing features, keybindings, and install
instructions — keep it in sync when behavior changes.
