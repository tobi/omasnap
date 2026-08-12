# Omasnap

A native Wayland screenshot and annotation overlay designed for Omarchy and Hyprland.
It captures the focused monitor before mapping an exclusive layer-shell surface, so the
editor never appears in its own screenshot. The editor retains annotations as movable,
resizable vector layers and preserves the monitor's native pixels on scaled displays.

[![Looping Omasnap demonstration](assets/omasnap.gif)](assets/omasnap.mp4)

## Features

- Freeform region, window, and full-monitor capture modes.
- Clean window-surface capture through Wayland image-copy protocols. A failed native
  capture stays in the window picker; Omasnap never substitutes a crop of the desktop.
- Select/move/resize layers, mouse-wheel scaling, and eight external recropping handles.
- Arrows, straight lines, smoothed freehand strokes, translucent highlighter strokes,
  rectangles, numbered markers, and editable Neucha text.
- Per-layer preset or custom colors (including highlighter ink), undo/redo history,
  OCR-region capture,
  mesh-gradient backdrops, and rendered drop shadows.
- Pin a finished capture as a bottom-right always-on-top layer surface, launched
  from the same `omasnap` executable and visible on every workspace.
- Crash-resistant working snapshots under `/run/user/<UID>/omasnap/` (falling back to
  a private `/tmp/omasnap-<UID>/`), written immediately after selection and overwritten
  after every completed edit. Saving moves that file into `~/Pictures/Screenshots`;
  clipboard output streams the same PNG.
- Verified PNG clipboard output through `wl-copy`/`wl-paste`, plus timestamped files
  under `~/Pictures/Screenshots` by default.
- Correct native-pixel export on fractional or integer-scaled monitors.

## Platform scope

The supported target is **Wayland + Hyprland**, with Omarchy as the primary integration.
The renderer, layer surface, clipboard, and clean-window capture use Wayland protocols;
monitor/window discovery currently calls `hyprctl`. `grim` captures the monitor before
the layer maps. Selection displays that captured frame, while the annotation editor uses
a translucent layer scrim over the live desktop and draws only the selected capture.
Another Wayland compositor could support the application after supplying equivalent
monitor and window discovery; generic Wayland support is not claimed by 1.0.

Runtime commands used by the application:

- `hyprctl`
- `grim`
- `wl-copy` and `wl-paste`
- `tesseract`
- `omarchy-notification-send` when available; saved captures include a thumbnail and
  reopen in Omasnap when clicked. Notification failure does not invalidate output.

## Install on Omarchy

Clone the repository and run the Omarchy installer:

```bash
git clone https://github.com/tobi/omasnap.git
cd omasnap
./install-omarchy
```

The installer uses Omarchy's package helper for missing dependencies, builds in
`~/.cache/omasnap`, and installs under `~/.local`. It does not modify
Hyprland configuration.

### Hyprland binding

Paste this into a Lua config loaded after `require("default.hypr.omarchy")`:

```lua
hl.unbind("PRINT")
hl.unbind("F12")
hl.unbind("ALT + SHIFT + 4")

o.bind("PRINT", "Screenshot", "omasnap")
o.bind("F12", "Screenshot", "omasnap")
o.bind("ALT + SHIFT + 4", "Screenshot", "omasnap")

hl.layer_rule({
  match = { namespace = "^omasnap$" },
  no_anim = true,
  animation = "none",
})
```

Apply and verify:

```bash
hyprctl reload
hyprctl configerrors
hyprctl binds -j | jq -c \
  '[.[] | select(.description == "Screenshot") | {modmask,key,description}]'
```

`omarchy plugin add` is intentionally not used. Omarchy plugins are Quickshell QML
extensions; they do not install native executables or system packages.

Set `OMASNAP_PREFIX` before running `install-omarchy` to use a prefix other than
`~/.local`.

### Manual Arch Linux build

Install the complete build/runtime dependency set:

```bash
sudo pacman -S --needed \
  base-devel cmake ninja pkgconf qt6-base layer-shell-qt \
  wayland wayland-protocols hyprland grim wl-clipboard \
  tesseract tesseract-data-eng tesseract-data-tha
```

Build and install:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build --parallel
cmake --install build
```

The install step places:

- `~/.local/bin/omasnap`
- `~/.local/share/applications/omasnap.desktop`
- `~/.local/share/licenses/omasnap/Neucha-OFL.txt`

Ensure `~/.local/bin` is on `PATH`, then verify the installed CLI:

```bash
omasnap --version
omasnap --help
```

## CLI capture modes

Running without arguments opens freeform region selection:

```bash
omasnap
```

Explicit starting modes:

```bash
omasnap --capture-region
omasnap --capture-window
omasnap --capture-fullscreen
```

Compatibility positional names are also accepted:

```bash
omasnap region
omasnap windows
omasnap fullscreen
omasnap smart       # maps to region selection
```

These options choose what is initially selected; the editor still controls whether the
result is copied, saved, or both.

Quick output skips the annotation editor. Add `--copy` to copy only, `--save` to save
only, or both flags to copy and save. Region and window captures output after selection;
fullscreen captures output immediately. Quick output cannot be combined with `--file` or
`--pin`.

### Edit an existing image

Point omasnap at any readable image and it opens straight into the annotation editor
with the whole image selected, skipping the screen-capture step:

```bash
omasnap ~/Pictures/Screenshots/screenshot-2026-08-11_10-00-00.png
# or
omasnap --file /path/to/capture.png
```

File URLs are accepted too. A saved capture notification's "Click to edit" action launches
`omasnap` on the finished screenshot, so it can be reopened and re-annotated. Clipboard-only
captures are not retained on disk and therefore have no delayed edit action.

Environment overrides:

```bash
OMASNAP_SCREENSHOT_DIR="$HOME/Pictures/Captures" omasnap
OMASNAP_OCR_LANGS="eng+deu" omasnap
# Thai plus English:
OMASNAP_OCR_LANGS="tha+eng" omasnap
```

Install the corresponding Tesseract language data before adding a language to
`OMASNAP_OCR_LANGS`. When unset, omasnap falls back to Omarchy's
`OMARCHY_OCR_LANGS` (which commonly includes the user's script, e.g.
`tha+eng`), then to `eng`.

## Controls

### Capture selection

| Input | Action |
|---|---|
| Drag | Select a region |
| `Space` | Toggle region/window selection |
| `SUPER + Arrow` | Move among windows in window mode |
| `Enter` | Capture the highlighted window |
| `Ctrl+A` | Select the full focused monitor |
| `Esc`, `Esc` | Dismiss |

### Annotation editor

| Input | Action |
|---|---|
| `V` | Select/move/resize layers; wheel scales the selected layer |
| `A` | Arrow |
| `L` | Straight line |
| `F` | Freehand stroke |
| `C` | Numbered marker |
| `R` | Rectangle |
| `T` | Neucha text |
| `O` | Drag an OCR region and copy recognized text |
| `B` | Cycle backdrop |
| `1`–`6` | Set annotation color |
| Wheel | Scale selected layer or change active tool size |
| Double-click text | Reopen text editing |
| `Delete` | Delete selected layer |
| `Ctrl+Z` | Undo |
| `Ctrl+Shift+Z`, `Ctrl+Y` | Redo |
| `Ctrl+C` | Copy PNG only |
| `Ctrl+S` | Save PNG only |
| `Enter` | Copy and save |
| `P` | Pin the capture on screen and close the editor |
| `Esc` | Return to Select; press again to close |

### Pinned captures

`P` renders the current capture, writes it to a `pin-<pid>-<n>.png` under the runtime
snapshot directory, and launches the same `omasnap` executable in detached pin mode.
The layer-shell surface is anchored 14 logical pixels from the bottom-right corner and
stays visible on every workspace without compositor window rules. It preserves the image
aspect ratio, with a maximum width of one third of the screen and a maximum height of one
half.

Pinning neither touches the clipboard nor writes to the screenshot directory; it is a
fourth output alongside copy, save, and copy-and-save. `P` closes the editor and releases
the single-instance lock immediately. Pins from separate captures accumulate as independent
processes.

Hover the pin to reveal its controls:

| Input on a pin | Action |
|---|---|
| Edit button | Reopen the full-resolution PNG in Omasnap and replace the pin |
| Link button | Copy the source file path |
| Copy button, `Ctrl+C` | Copy the full-resolution PNG |
| Double-wide top-left drag handle | Drag the PNG into a file-capable drop target |
| Wheel | Resize within the screen caps, preserving aspect ratio |
| Close button, `Esc`, middle-click | Close |

Image and path copying use `wl-copy` rather than `QClipboard`, so clipboard data remains
available after the pin is closed. No font-based symbol set or compositor-specific window
rule is required; the controls use the same vector icon renderer as the annotation toolbar.

Creation tools return to Select after one placement without selecting the new layer. In
Select mode, arrows and lines show only their two endpoint handles; other layers show a
selection boundary. The eight blue/white handles outside the image recrop its corners or
edges.

## Development and verification

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
QT_QPA_PLATFORM=offscreen \
  ./build/omasnap-smoke /tmp/omasnap-smoke
```

The smoke executable exercises region/window/fullscreen startup modes, capture selection,
temporary snapshot updates, annotation tools, undo/redo, vector movement and scaling,
text editing, OCR, native-DPI output, endpoint-only line selection, and external crop
handles.

`.github/workflows/build-linux.yml` performs the same release build and interaction smoke
in an Arch Linux container, stages the CMake installation, and uploads a versioned Linux
artifact. A `v*` tag also attaches that artifact to the corresponding GitHub release.

## Acknowledgements

The capture and annotation workflow is inspired by three excellent screenshot tools:

- [Shottr](https://shottr.cc/) — fast region/window capture, OCR, and polished backdrops.
- [Satty](https://github.com/Satty-org/Satty) — a focused, Wayland-native annotation workflow.
- [Flameshot](https://github.com/flameshot-org/flameshot) — selection-first capture and an
  approachable annotation toolbar.

Thanks to their authors and contributors for establishing the interaction patterns that made
this project possible. Omasnap is an independent implementation and is not
affiliated with those projects.

## Project history

This standalone repository was extracted with `git filter-repo` from the original Omarchy
system-customization repository. The former `omasnap/` directory was promoted to
the repository root while retaining its relevant commit history.

The bundled Neucha font is distributed under the SIL Open Font License; its license is in
`assets/OFL.txt` and is installed with the application.
