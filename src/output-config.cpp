/** @fileoverview Screenshot destination and filename pattern from the user's
 *  INI config. */
#include "output-config.hpp"

#include <QDir>
#include <QSettings>
#include <QStandardPaths>

OutputConfig loadOutputConfig(const QString &filePath) {
  OutputConfig config;
  QSettings settings(filePath, QSettings::IniFormat);
  QString directory =
      settings.value(QStringLiteral("output/directory")).toString().trimmed();
  if (directory == QStringLiteral("~"))
    directory = QDir::homePath();
  else if (directory.startsWith(QStringLiteral("~/")))
    directory = QDir::homePath() + directory.mid(1);
  if (!directory.isEmpty())
    config.directory = directory;
  const QString filename =
      settings.value(QStringLiteral("output/filename")).toString().trimmed();
  if (!filename.isEmpty())
    config.filename = filename;
  return config;
}

QString formatScreenshotFilename(const QString &pattern, const QDateTime &when,
                                 const QString &appSlug) {
  QString name = pattern;
  if (appSlug.isEmpty()) {
    // Drop the token and the separator that introduced it, so the default
    // pattern does not leave a dangling dash on a capture with no app.
    for (const char *joined : {"-{app}", "_{app}", " {app}", "{app}-",
                               "{app}_", "{app} ", "{app}"})
      name.replace(QLatin1String(joined), QString());
  } else {
    name.replace(QStringLiteral("{app}"), appSlug);
  }
  name.replace(QStringLiteral("{date}"),
               when.toString(QStringLiteral("yyyy-MM-dd")));
  name.replace(QStringLiteral("{time}"),
               when.toString(QStringLiteral("HH-mm-ss")));
  // Filenames only: a slash would silently change the destination, and a
  // leading dot hides the file.
  name.replace(QLatin1Char('/'), QLatin1Char('-'));
  while (!name.isEmpty() &&
         QStringLiteral(". -_").contains(name.front()))
    name.remove(0, 1);
  name = name.trimmed();
  if (name.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive))
    name.chop(4);
  if (name.isEmpty())
    name = QStringLiteral("screenshot-") +
           when.toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
  return name + QStringLiteral(".png");
}

bool loadEditorWindowMode(const QString &filePath) {
  QSettings settings(filePath, QSettings::IniFormat);
  return settings.value(QStringLiteral("editor/mode"))
             .toString()
             .trimmed()
             .toLower() == QStringLiteral("window");
}

QString editorFloatRuleScript(bool floating) {
  // Registered before the editor window maps: floated after the fact, the
  // window tiles for a frame and visibly pops out. The named rule
  // coalesces across editors, and a tiled config re-registers it disabled
  // so a floating session's rule cannot leak into tiled use. The title
  // pattern skips the pins, whose titles have no space after the name.
  return floating ? QStringLiteral(
                        "hl.window_rule({ name = \"omasnap-editor-float\", "
                        "match = { title = \"^omasnap( .+)?$\" }, "
                        "float = true, center = true })")
                  : QStringLiteral(
                        "hl.window_rule({ name = \"omasnap-editor-float\", "
                        "match = { title = \"^omasnap( .+)?$\" }, "
                        "enabled = false })");
}

bool loadEditorWindowFloating(const QString &filePath) {
  QSettings settings(filePath, QSettings::IniFormat);
  return settings.value(QStringLiteral("editor/window"))
             .toString()
             .trimmed()
             .toLower() != QStringLiteral("tiled");
}

bool loadEditorWindowBackdropOpaque(const QString &filePath) {
  QSettings settings(filePath, QSettings::IniFormat);
  return settings.value(QStringLiteral("editor/backdrop"))
             .toString()
             .trimmed()
             .toLower() != QStringLiteral("translucent");
}

QString defaultConfigPath() {
  return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) +
         QStringLiteral("/omasnap/omasnap.conf");
}
