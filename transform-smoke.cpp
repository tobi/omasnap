/** @fileoverview Tests Wayland capture transforms and object cleanup. */
#include "transform-smoke.hpp"

#include "capture.hpp"
#include "surface-capture-smoke.hpp"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QVector>

#include <wayland-client-protocol.h>

namespace {
/** Writes an executable helper used to replace an external command. */
bool writeExecutable(const QString &path, const QByteArray &contents) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly) ||
      file.write(contents) != contents.size())
    return false;
  file.close();
  return QFile::setPermissions(path, QFileDevice::ReadOwner |
                                         QFileDevice::WriteOwner |
                                         QFileDevice::ExeOwner);
}

/** Creates a small image whose red channel stores easy-to-check values. */
QImage indexedImage(const QVector<QVector<int>> &rows) {
  QImage image(rows.constFirst().size(), rows.size(), QImage::Format_RGB32);
  for (int y = 0; y < rows.size(); ++y) {
    for (int x = 0; x < rows.at(y).size(); ++x)
      image.setPixelColor(x, y, QColor(rows.at(y).at(x), 0, 0));
  }
  return image;
}
} // namespace

bool runTransformSmoke(QString &error) {
  if (!runWaylandCleanupChecks()) {
    error =
        QStringLiteral("Wayland capture objects were not released in order");
    return false;
  }
  QTemporaryDir fakeCommands;
  if (!fakeCommands.isValid()) {
    error = QStringLiteral("Could not create transform-test directory");
    return false;
  }
  const QString fakeHyprctl =
      QDir(fakeCommands.path()).filePath(QStringLiteral("hyprctl"));
  const QString fakeGrim =
      QDir(fakeCommands.path()).filePath(QStringLiteral("grim"));
  const QByteArray hyprctlScript = QByteArrayLiteral(
      "#!/usr/bin/env bash\n"
      "set -euo pipefail\n"
      "if [[ \"${1:-}\" == \"monitors\" ]]; then\n"
      "  printf '[{\"focused\":true,\"scale\":1.0,\"width\":300,"
      "\"height\":200,\"transform\":%s,\"name\":\"TEST-ROTATED\","
      "\"x\":0,\"y\":0,\"activeWorkspace\":{\"id\":7}}]\\n' "
      "\"$OMASNAP_TEST_TRANSFORM\"\n"
      "else\n"
      "  printf '[]\\n'\n"
      "fi\n");
  const QByteArray grimScript =
      QByteArrayLiteral("#!/usr/bin/env bash\n"
                        "set -euo pipefail\n"
                        "cp -- \"$OMASNAP_TEST_PPM\" \"${@: -1}\"\n");
  if (!writeExecutable(fakeHyprctl, hyprctlScript) ||
      !writeExecutable(fakeGrim, grimScript)) {
    error = QStringLiteral("Could not create transform-test commands");
    return false;
  }
  QImage rotatedSource(300, 200, QImage::Format_RGB32);
  rotatedSource.fill(QColor(QStringLiteral("#123456")));
  const QString ppmPath =
      QDir(fakeCommands.path()).filePath(QStringLiteral("source.ppm"));
  if (!rotatedSource.save(ppmPath, "PPM")) {
    error = QStringLiteral("Could not create transform-test capture");
    return false;
  }

  const QByteArray originalPath = qgetenv("PATH");
  const QByteArray originalSignature = qgetenv("HYPRLAND_INSTANCE_SIGNATURE");
  // Force the hyprctl code path even when the smoke test runs inside KDE.
  qputenv("HYPRLAND_INSTANCE_SIGNATURE", "omasnap-smoke");
  qputenv("PATH", fakeCommands.path().toUtf8() + ':' + originalPath);
  qputenv("OMASNAP_TEST_PPM", ppmPath.toUtf8());
  for (const int transform : {1, 3, 5, 7}) {
    qputenv("OMASNAP_TEST_TRANSFORM", QByteArray::number(transform));
    CaptureData rotatedCapture;
    if (!captureFocusedMonitor(rotatedCapture, error) ||
        rotatedCapture.monitor.geometry.size() != QSize(200, 300) ||
        rotatedCapture.preview.size() != QSize(200, 300)) {
      if (error.isEmpty())
        error = QStringLiteral("Quarter-turn monitor geometry was not swapped");
      qputenv("HYPRLAND_INSTANCE_SIGNATURE", originalSignature);
      qputenv("PATH", originalPath);
      qunsetenv("OMASNAP_TEST_PPM");
      qunsetenv("OMASNAP_TEST_TRANSFORM");
      return false;
    }
  }
  qputenv("HYPRLAND_INSTANCE_SIGNATURE", originalSignature);
  qputenv("PATH", originalPath);
  qunsetenv("OMASNAP_TEST_PPM");
  qunsetenv("OMASNAP_TEST_TRANSFORM");

  const QImage upright = indexedImage({{1, 2}, {3, 4}, {5, 6}});
  const QVector<QPair<std::uint32_t, QImage>> transformedImages{
      {WL_OUTPUT_TRANSFORM_NORMAL, upright},
      {WL_OUTPUT_TRANSFORM_90, indexedImage({{2, 4, 6}, {1, 3, 5}})},
      {WL_OUTPUT_TRANSFORM_180, indexedImage({{6, 5}, {4, 3}, {2, 1}})},
      {WL_OUTPUT_TRANSFORM_270, indexedImage({{5, 3, 1}, {6, 4, 2}})},
      {WL_OUTPUT_TRANSFORM_FLIPPED, indexedImage({{2, 1}, {4, 3}, {6, 5}})},
      {WL_OUTPUT_TRANSFORM_FLIPPED_90, indexedImage({{1, 3, 5}, {2, 4, 6}})},
      {WL_OUTPUT_TRANSFORM_FLIPPED_180, indexedImage({{5, 6}, {3, 4}, {1, 2}})},
      {WL_OUTPUT_TRANSFORM_FLIPPED_270, indexedImage({{6, 4, 2}, {5, 3, 1}})},
  };
  for (const auto &[transform, transformed] : transformedImages) {
    if (normalizeWaylandCapture(transformed, transform) != upright) {
      error = QStringLiteral("Captured Wayland buffer was not upright");
      return false;
    }
  }
  return true;
}
