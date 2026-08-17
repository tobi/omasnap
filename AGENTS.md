# Omasnap — Agent Guide

Omasnap is a super fast, native Wayland screenshot and annotation overlay,
built primarily for [Omarchy](https://omarchy.org) on Hyprland. It captures
region, window, or full monitor, then opens an annotation editor with vector
layers (arrows, lines, freehand, highlighter, rectangles, numbered markers,
text, OCR). Finished captures go to clipboard, `~/Pictures/Screenshots`, or a
pinned always-on-top layer surface.

## Project principles

- **Speed first.** The tool must feel instant: capture, annotate, copy. No
  startup bloat, no settings UI, no wizards.
- **Wayland only.** Requires Wayland + Hyprland (monitor/window discovery via
  `hyprctl`). No X11, no macOS/Windows ports, no generic-compositor support.
- **No backwards compatibility.** Break keybindings, CLI flags, file formats,
  or internals whenever it keeps the code simpler or the tool faster. Do not
  add compatibility shims, deprecation aliases, or migration code.
- **Omarchy aesthetics.** Follow Omarchy conventions: uses
  `omarchy-notification-send` when available, falls back to
  `OMARCHY_OCR_LANGS` for OCR languages, keeps UI minimal with vector-drawn
  icons (no icon-theme dependency) and the bundled Neucha font.
- **Single binary.** Everything (capture, editor, pin mode) runs from the one
  `omasnap` executable.

## Repository layout

| Path | Purpose |
|---|---|
| `src/main.cpp` | CLI parsing, single-instance lock, mode dispatch (capture / edit file / pin) |
| `src/instance-lock.cpp/.hpp` | Single-instance handover: cancel a running overlay, or stop it and take over for `--file` |
| `src/capture.cpp/.hpp` | Capture selection overlay, rendering, output, and private runtime snapshots |
| `src/editor.cpp/.hpp` | Annotation editor: tools, vector layers, undo/redo, rendering, export |
| `src/pin.cpp/.hpp` | Pinned-capture layer-shell surfaces (bottom-right, all workspaces) |
| `src/surface-capture.cpp` | Clean window capture via `ext-image-copy-capture` Wayland protocol |
| `src/icons.cpp/.hpp` | Vector icon renderer for toolbar and pin controls |
| `src/cli-path.cpp/.hpp` | Command-line image target resolution |
| `src/eyedropper.cpp/.hpp` | Display-to-source color sampling |
| `src/pin-file.cpp/.hpp`, `src/pin-layout.cpp/.hpp` | Pin file lifecycle and layout helpers |
| `tests/*-smoke.cpp/.hpp` | Headless Qt Test coverage, including offscreen region-click, async-capture, and single-instance handover checks |
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
wayland wayland-protocols grim wl-clipboard tesseract tesseract-data-eng`.

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
