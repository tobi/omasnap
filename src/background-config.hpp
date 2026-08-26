/** @fileoverview Loads the custom backdrop image path and the default
 *  backdrop style from the user's INI config. */
#pragma once

#include "capture.hpp"

#include <QString>

/** Custom backdrop settings, loaded from the user's INI config or defaults. */
struct BackgroundConfig {
  /** Absolute path to an image used as the "Custom" backdrop, or empty when
   *  none is configured. */
  QString imagePath;
  /** Style a freshly opened capture starts with. `None` (today's behavior)
   *  unless the config asks for something else. */
  BackgroundStyle defaultStyle = BackgroundStyle::None;
};

/** Reads `[background] image` (a path, `~` expands to $HOME) and
 *  `[background] default` (a style name: none, aurora, sunset, lagoon,
 *  violet, custom) from an INI file. A missing file or key leaves defaults
 *  untouched; an unrecognized style name is ignored. Whether `default =
 *  custom` actually takes effect depends on `image` loading successfully,
 *  which the caller checks. */
[[nodiscard]] BackgroundConfig loadBackgroundConfig(const QString &filePath);
