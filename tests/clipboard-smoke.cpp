/** @fileoverview Tests clipboard image loading without a live compositor. */
#include "clipboard-smoke.hpp"

#include "capture.hpp"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QScopeGuard>
#include <QTemporaryDir>

namespace {
/** Writes an executable fake command. */
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

/** Checks that an offered PNG is decoded. */
bool runImageCheck(QString &error) {
  QImage image;
  if (!loadClipboardImage(image, error))
    return false;
  if (image.size() != QSize(3, 2) ||
      image.pixelColor(1, 0) != QColor(18, 52, 86) ||
      image.pixelColor(2, 1) != QColor(171, 205, 239)) {
    error = QStringLiteral("Clipboard image pixels were not preserved");
    return false;
  }
  return true;
}

/** Checks that a text-only clipboard reports a clear failure. */
bool runTextOnlyCheck(QString &error) {
  qputenv("OMASNAP_TEST_CLIPBOARD_TEXT_ONLY", "1");
  QImage image(1, 1, QImage::Format_ARGB32);
  QString clipboardError;
  if (loadClipboardImage(image, clipboardError) || !image.isNull() ||
      !clipboardError.contains(QStringLiteral("image"),
                               Qt::CaseInsensitive)) {
    error = QStringLiteral("Text-only clipboard was not rejected clearly");
    return false;
  }
  return true;
}

/** Checks that a failed image transfer keeps the wl-paste error. */
bool runReadFailureCheck(QString &error) {
  qunsetenv("OMASNAP_TEST_CLIPBOARD_TEXT_ONLY");
  qputenv("OMASNAP_TEST_CLIPBOARD_READ_FAILURE", "1");
  QImage image;
  QString clipboardError;
  if (loadClipboardImage(image, clipboardError) || !image.isNull() ||
      !clipboardError.contains(QStringLiteral("clipboard changed"),
                               Qt::CaseInsensitive)) {
    error = QStringLiteral("Clipboard transfer failure lost its cause");
    return false;
  }
  return true;
}
} // namespace

bool runClipboardSmoke(QString &error) {
  QTemporaryDir directory;
  if (!directory.isValid()) {
    error = QStringLiteral("Could not create clipboard-test directory");
    return false;
  }

  const QString imagePath =
      QDir(directory.path()).filePath(QStringLiteral("clipboard.png"));
  QImage expected(3, 2, QImage::Format_ARGB32);
  expected.fill(Qt::transparent);
  expected.setPixelColor(1, 0, QColor(18, 52, 86));
  expected.setPixelColor(2, 1, QColor(171, 205, 239));
  if (!expected.save(imagePath, "PNG")) {
    error = QStringLiteral("Could not create clipboard-test image");
    return false;
  }

  const QString fakeWlPaste =
      QDir(directory.path()).filePath(QStringLiteral("wl-paste"));
  const QByteArray script = QByteArrayLiteral(
      "#!/usr/bin/env bash\n"
      "set -euo pipefail\n"
      "if [[ \"${1:-}\" == \"--list-types\" ]]; then\n"
      "  if [[ -n \"${OMASNAP_TEST_CLIPBOARD_TEXT_ONLY:-}\" ]]; then\n"
      "    printf 'text/plain;charset=utf-8\\n'\n"
      "  else\n"
      "    printf 'text/plain\\nimage/png\\n'\n"
      "  fi\n"
      "  exit 0\n"
      "fi\n"
      "if [[ \"${1:-}\" == \"--no-newline\" && \"${2:-}\" == \"--type\" "
      "&& \"${3:-}\" == \"image/png\" ]]; then\n"
      "  if [[ -n \"${OMASNAP_TEST_CLIPBOARD_READ_FAILURE:-}\" ]]; then\n"
      "    printf 'clipboard changed before image transfer\\n' >&2\n"
      "    exit 1\n"
      "  fi\n"
      "  cat -- \"$OMASNAP_TEST_CLIPBOARD_IMAGE\"\n"
      "  exit 0\n"
      "fi\n"
      "exit 1\n");
  if (!writeExecutable(fakeWlPaste, script)) {
    error = QStringLiteral("Could not create fake wl-paste command");
    return false;
  }
  const QString fakeWlCopy =
      QDir(directory.path()).filePath(QStringLiteral("wl-copy"));
  const QByteArray copyScript = QByteArrayLiteral(
      "#!/usr/bin/env bash\n"
      "set -euo pipefail\n"
      "[[ \"${1:-}\" == \"--type\" && \"${2:-}\" == \"image/png\" ]]\n"
      "cat > \"$OMASNAP_TEST_CLIPBOARD_COPY\"\n");
  if (!writeExecutable(fakeWlCopy, copyScript)) {
    error = QStringLiteral("Could not create fake wl-copy command");
    return false;
  }

  const bool pathWasSet = qEnvironmentVariableIsSet("PATH");
  const QByteArray oldPath = qgetenv("PATH");
  const bool imageWasSet =
      qEnvironmentVariableIsSet("OMASNAP_TEST_CLIPBOARD_IMAGE");
  const QByteArray oldImage = qgetenv("OMASNAP_TEST_CLIPBOARD_IMAGE");
  const bool textOnlyWasSet =
      qEnvironmentVariableIsSet("OMASNAP_TEST_CLIPBOARD_TEXT_ONLY");
  const QByteArray oldTextOnly = qgetenv("OMASNAP_TEST_CLIPBOARD_TEXT_ONLY");
  const bool readFailureWasSet =
      qEnvironmentVariableIsSet("OMASNAP_TEST_CLIPBOARD_READ_FAILURE");
  const QByteArray oldReadFailure =
      qgetenv("OMASNAP_TEST_CLIPBOARD_READ_FAILURE");
  const bool copyWasSet =
      qEnvironmentVariableIsSet("OMASNAP_TEST_CLIPBOARD_COPY");
  const QByteArray oldCopy = qgetenv("OMASNAP_TEST_CLIPBOARD_COPY");
  const auto restoreEnvironment = qScopeGuard([=] {
    pathWasSet ? qputenv("PATH", oldPath) : qunsetenv("PATH");
    imageWasSet ? qputenv("OMASNAP_TEST_CLIPBOARD_IMAGE", oldImage)
                : qunsetenv("OMASNAP_TEST_CLIPBOARD_IMAGE");
    textOnlyWasSet
        ? qputenv("OMASNAP_TEST_CLIPBOARD_TEXT_ONLY", oldTextOnly)
        : qunsetenv("OMASNAP_TEST_CLIPBOARD_TEXT_ONLY");
    readFailureWasSet
        ? qputenv("OMASNAP_TEST_CLIPBOARD_READ_FAILURE", oldReadFailure)
        : qunsetenv("OMASNAP_TEST_CLIPBOARD_READ_FAILURE");
    copyWasSet ? qputenv("OMASNAP_TEST_CLIPBOARD_COPY", oldCopy)
               : qunsetenv("OMASNAP_TEST_CLIPBOARD_COPY");
  });
  const QString copiedPath =
      QDir(directory.path()).filePath(QStringLiteral("copied.png"));
  qputenv("PATH", directory.path().toUtf8() + ':' + oldPath);
  qputenv("OMASNAP_TEST_CLIPBOARD_IMAGE", imagePath.toUtf8());
  qputenv("OMASNAP_TEST_CLIPBOARD_COPY", copiedPath.toUtf8());
  qunsetenv("OMASNAP_TEST_CLIPBOARD_TEXT_ONLY");
  qunsetenv("OMASNAP_TEST_CLIPBOARD_READ_FAILURE");

  if (!copyImageToClipboard(expected, error) ||
      QImage(copiedPath) != expected) {
    if (error.isEmpty())
      error = QStringLiteral("Clipboard image copy did not preserve pixels");
    return false;
  }
  return runImageCheck(error) && runTextOnlyCheck(error) &&
         runReadFailureCheck(error);
}
