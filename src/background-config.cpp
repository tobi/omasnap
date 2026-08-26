/** @fileoverview Loads the custom backdrop image path and the default
 *  backdrop style from the user's INI config. */
#include "background-config.hpp"

#include <QDir>
#include <QSettings>

BackgroundConfig loadBackgroundConfig(const QString &filePath) {
  BackgroundConfig config;
  QSettings settings(filePath, QSettings::IniFormat);
  QString image =
      settings.value(QStringLiteral("background/image")).toString().trimmed();
  if (image == QStringLiteral("~"))
    image = QDir::homePath();
  else if (image.startsWith(QStringLiteral("~/")))
    image = QDir::homePath() + image.mid(1);
  config.imagePath = image;
  BackgroundStyle style;
  if (backgroundStyleFromName(
          settings.value(QStringLiteral("background/default"))
              .toString()
              .trimmed()
              .toLower(),
          style))
    config.defaultStyle = style;
  return config;
}
