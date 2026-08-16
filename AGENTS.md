# Omasnap — Agent Guide

Omasnap is a super fast, native Wayland screenshot and annotation overlay,
built primarily for [Omarchy](https://omarchy.org) on Hyprland, with KDE
Plasma 6 as a second supported desktop. It captures
region, window, or full monitor, then opens an annotation editor with vector
layers (arrows, lines, freehand, highlighter, rectangles, numbered markers,
text, OCR). Finished captures go to clipboard, `~/Pictures/Screenshots`, or a
pinned always-on-top layer surface.

## Project principles

- **Speed first.** The tool must feel instant: capture, annotate, copy. No
  startup bloat, no settings UI, no wizards.
- **Wayland only.** Requires Wayland + Hyprland (monitor/window discovery via
  `hyprctl`) or Wayland + KDE Plasma 6 (KWin `ScreenShot2` DBus and scripting,
  see `kwin.cpp`). No X11, no macOS/Windows ports, no generic-compositor
  support.
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
| `main.cpp` | CLI parsing, single-instance lock, mode dispatch (capture / edit file / pin) |
| `capture.cpp/.hpp` | Capture selection overlay, snapshot lifecycle under `/run/user/<UID>/omasnap/` |
| `kwin.cpp/.hpp` | KDE Plasma backend: KWin `ScreenShot2` DBus capture and scripted window discovery; compiled only with `-DOMASNAP_KDE=ON` (`kwin.hpp` stubs it out otherwise) |
| `editor.cpp/.hpp` | Annotation editor: tools, layers, undo/redo, rendering, export |
| `pin.cpp/.hpp` | Pinned-capture layer-shell surfaces (bottom-right, all workspaces) |
| `surface-capture.cpp` | Clean window capture via `ext-image-copy-capture` Wayland protocol |
| `icons.cpp/.hpp` | Vector icon renderer for toolbar and pin controls |
| `editor-smoke.cpp`, `transform-smoke.cpp` | Headless smoke tests (Qt Test, offscreen platform) |
| `install-omarchy` | Omarchy installer (deps via `omarchy-pkg-add`, installs to `~/.local`) |
| `install-kde` | KDE Plasma installer (installs to `~/.local`, refreshes the KDE service cache) |
| `CMakeLists.txt` | Build definition; **the version lives here** (`project(omasnap VERSION ...)`) |

## Build and verify

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
QT_QPA_PLATFORM=offscreen ./build/omasnap-smoke /tmp/omasnap-smoke
```

Always run the smoke test after behavioral changes — it exercises startup
modes, tools, undo/redo, OCR, and native-DPI output headlessly. CI
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
