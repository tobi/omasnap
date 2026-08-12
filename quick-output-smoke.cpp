/** @fileoverview Verifies direct copy/save output for quick capture mode. */
#include "capture.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>

#include <cstdio>

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  QTemporaryDir output;
  if (!output.isValid())
    return 1;

  qputenv("OMASNAP_SCREENSHOT_DIR",
          QDir(output.path()).filePath(QStringLiteral("saved")).toUtf8());
  QImage image(32, 24, QImage::Format_ARGB32_Premultiplied);
  image.fill(QColor(QStringLiteral("#123456")));
  QString error;
  if (!quickOutput(image, QuickOutputMode::Save, error)) {
    std::fprintf(stderr, "%s\n", qPrintable(error));
    return 2;
  }

  const QStringList saved =
      QDir(QDir(output.path()).filePath(QStringLiteral("saved")))
          .entryList({QStringLiteral("*.png")}, QDir::Files);
  if (saved.size() != 1)
    return 3;
  const QImage savedImage(QDir(output.path()).filePath(
      QStringLiteral("saved/%1").arg(saved.constFirst())));
  if (savedImage.isNull() ||
      savedImage.convertToFormat(image.format()) != image)
    return 3;
  return 0;
}
