# Dependencies: learn the current set before adding to it

The dependency list is small on purpose and should stay that way. Before
adding anything — a library, a build tool, an external process — read this
file, then ask whether the thing you need is genuinely absent from it.

## Link-time (build and runtime)

From `CMakeLists.txt`, this is the entire list:

| Dependency | What it's for |
|---|---|
| **Qt6** (Concurrent, Core, Gui, Test, Widgets) 6.8+ | Everything: windowing, painting, the editor UI, the worker-pool threading model ([threading.md](threading.md)), the test harness |
| **LayerShellQt** | Layer-shell surfaces (the capture overlay, the editor, pinned captures) |
| **wayland-client** (pkg-config) | Raw protocol client code (`ext-image-copy-capture`, `zwlr_virtual_pointer_v1`) that LayerShellQt/QtWayland don't expose |
| **wayland-scanner** + protocol XML | Generates the C bindings for the above at build time; not a runtime dependency |

That's it. No JSON library (Qt's `QJsonDocument` handles `hyprctl -j`
output), no image codec beyond what Qt's own PNG support provides, no HTTP,
no logging framework, no CLI-parsing library beyond `QCommandLineParser`,
no config-file parser beyond `QSettings` (used for the one optional INI
file — see below).

## Runtime: external processes, not libraries

Omasnap shells out to a small number of existing Omarchy/Arch tools instead
of linking their libraries in-process. This is intentional: a `QProcess`
call to a well-maintained CLI tool that's already on every Omarchy install
is a dependency Omasnap doesn't have to build, version, or debug — the
alternative (vendoring an OCR engine, a clipboard protocol implementation,
or a compositor IPC client) would be strictly more code and more risk for
no user-visible benefit.

| Process | Used for | Required? |
|---|---|---|
| `hyprctl` | Monitor/window discovery (`-j` JSON), natural-scroll policy query | Yes — see [platform-scope.md](platform-scope.md) |
| `wl-copy` / `wl-paste` | Writing PNG/text to the Wayland clipboard, and verifying the write | Yes |
| `tesseract` | OCR text recognition | Only if OCR is used; missing tesseract fails just that action |
| `omarchy-notification-send` | Capture-finished notifications | No — falls back silently if absent (checked with `command -v` semantics via failed `QProcess::startDetached`) |
| `uwsm-app` / `nautilus` | Reveal a saved screenshot selected in Files | No — missing/failed reveal never invalidates a successful save |

Each of these is invoked through the same small `runProcess`/
`QProcess::startDetached` helpers in `src/capture.cpp`, from a background
worker (see [threading.md](threading.md)) — never inline on the UI thread,
and always with a timeout.

## Not a dependency: external Qt platform themes

Omarchy exports `QT_QPA_PLATFORMTHEME=gtk3` for the whole session so Qt apps
match GTK apps. Omasnap overrides it to `generic` (Qt's built-in theme) for
its own process in `main()` before `QApplication` is constructed: honouring
the session value loads the `qgtk3` plugin,
which initialises GTK3, GLib/GIO and dconf inside the process — measured at
81–112 ms of `QApplication` construction and ~20–24 MiB of RSS on an Omarchy
laptop — for an overlay that is hand-painted, opens no dialogs and reads no
palette. The only relevant values the external theme supplied were its
general and fixed fonts; their chrome and application-default replacements
are pinned by `chromeFont()`, `chromeMonoFont()`, and `chromeDefaultFont()`
(`src/overlay-chrome.cpp`) instead. Do not make startup or chrome rendering
depend on an external desktop theme, `QStyle`- or palette-derived chrome, or
icon-theme lookup: each is a startup cost with nothing in this codebase to
spend it on.

## The one config file

`~/.config/omasnap/omasnap.conf` is optional INI, read with `QSettings`.
It exists for exactly two things people legitimately need to override
(screenshot destination/filename pattern, and preset colors) — not as a
general settings mechanism. See the "minimally configurable" principle in
[AGENTS.md](../AGENTS.md) before adding a new key: the bar is "this is a
real escape hatch for a real divergent need," the same bar the existing
keys cleared, not "this would be nice to expose."

## Evaluating a new dependency

Ask, in order:

1. **Can Qt already do this?** Qt6's modules are broad (networking,
   concurrency, text layout, SVG, image I/O). Check before reaching
   further.
2. **Is this an OS-integration concern better solved by shelling out to an
   existing Omarchy tool**, the way clipboard, OCR, and notifications are?
   A new external-process dependency is far cheaper than a new linked
   library: it doesn't grow the binary, doesn't add a build-time
   dependency, and fails gracefully (a missing/failed process is just an
   error message).
3. **Does it exist only to support a non-Hyprland compositor?** Then it's
   out of scope — see [platform-scope.md](platform-scope.md).
4. **Is it justified anyway?** Then it needs to earn its place in the table
   above, and this file needs to be updated in the same PR. A dependency
   that isn't documented here is a dependency someone will add a second,
   redundant way to do the same thing next to, because they didn't know it
   existed.

## Binary size

Single, statically-linked-where-practical binary, installed to
`~/.local/bin/omasnap` (see the repository's `install-omarchy`). Every
dependency added here is weight every user carries on every install and
every update. If a feature can be built with what's already linked, that's
the implementation to ship.
