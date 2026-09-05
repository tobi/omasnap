/** @fileoverview Screenshot destination and filename pattern from the user's
 *  INI config. Every key is optional; defaults match the built-in behavior. */
#pragma once

#include <QDateTime>
#include <QString>

struct OutputConfig {
  /** Screenshot directory; empty means `~/Pictures/Screenshots`. */
  QString directory;
  /** Filename pattern without extension. Tokens: `{date}` (yyyy-MM-dd),
   *  `{time}` (HH-mm-ss), `{app}` (slug of the app under the selection). */
  QString filename = QStringLiteral("screenshot-{date}_{time}-{app}");
};

/** Reads [output] directory and [output] filename. A missing file or key
 *  leaves the default untouched; `~` in directory expands to $HOME. */
[[nodiscard]] OutputConfig loadOutputConfig(const QString &filePath);

/** Expands `pattern` for `when` and `appSlug` into a safe `.png` filename.
 *  An empty `appSlug` removes `{app}` together with one separator before it
 *  so `screenshot-{date}-{app}` yields `screenshot-<date>.png`, not a
 *  trailing dash. Never returns an empty or path-like name. */
[[nodiscard]] QString formatScreenshotFilename(const QString &pattern,
                                               const QDateTime &when,
                                               const QString &appSlug);

/** True when [editor] mode = window: the annotation editor opens as a
 *  normal compositor window instead of the fullscreen overlay. Overlay is
 *  the default and any other value. */
[[nodiscard]] bool loadEditorWindowMode(const QString &filePath);

/** True unless [editor] window = tiled: a windowed editor asks the
 *  compositor to float it at its natural size; tiled leaves it to the
 *  layout. */
[[nodiscard]] bool loadEditorWindowFloating(const QString &filePath);
/** Lua chunk registering (or, tiled, disabling) the compositor rule that
 *  floats and centers the editor window as it maps. */
[[nodiscard]] QString editorFloatRuleScript(bool floating);

/** True unless [editor] backdrop = translucent: a windowed editor paints a
 *  solid backdrop instead of the overlay's see-through dim. */
[[nodiscard]] bool loadEditorWindowBackdropOpaque(const QString &filePath);

/** ~/.config/omasnap/omasnap.conf (XDG config location). */
[[nodiscard]] QString defaultConfigPath();
