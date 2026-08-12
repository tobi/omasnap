/** @fileoverview Tests the lifetime rules for pinned screenshot files. */
#include "pin-lifecycle-smoke.hpp"

#include "capture.hpp"
#include "pin-file.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

/** Verifies pin names, ownership, cleanup, preservation, and pruning. */
bool runPinLifecycleSmoke(QString &error) {
  const QString firstPath = pinnedSnapshotPath(987654);
  const QString secondPath = pinnedSnapshotPath(987654);
  if (firstPath.isEmpty() || secondPath.isEmpty() || firstPath == secondPath) {
    error = QStringLiteral("Pinned snapshot paths are not unique");
    return false;
  }

  QImage image(8, 8, QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::white);
  const QString closePath = pinnedSnapshotPath(987656);
  if (!saveTemporarySnapshot(image, closePath, error))
    return false;
  {
    PinSnapshotFile file(closePath);
    if (!file.isLocked()) {
      error = QStringLiteral("Normal pin snapshot was not locked");
      QFile::remove(closePath);
      return false;
    }
  }
  if (QFileInfo::exists(closePath)) {
    error = QStringLiteral("Normal pin close did not remove its snapshot");
    QFile::remove(closePath);
    return false;
  }

  const QString lockedPath = pinnedSnapshotPath(987658);
  if (!saveTemporarySnapshot(image, lockedPath, error))
    return false;
  const int lockedFd =
      ::open(QFile::encodeName(lockedPath).constData(), O_RDONLY | O_CLOEXEC);
  if (lockedFd < 0 || ::flock(lockedFd, LOCK_EX | LOCK_NB) != 0) {
    error = QStringLiteral("Could not hold exclusive pin snapshot lock");
    if (lockedFd >= 0)
      ::close(lockedFd);
    QFile::remove(lockedPath);
    return false;
  }
  {
    PinSnapshotFile file(lockedPath);
    if (file.isLocked()) {
      error = QStringLiteral("Contended pin snapshot unexpectedly locked");
      ::flock(lockedFd, LOCK_UN);
      ::close(lockedFd);
      QFile::remove(lockedPath);
      return false;
    }
  }
  ::flock(lockedFd, LOCK_UN);
  ::close(lockedFd);
  if (!QFileInfo::exists(lockedPath)) {
    error = QStringLiteral("Unowned pin snapshot was removed");
    return false;
  }
  QFile::remove(lockedPath);

  const QString reopenPath = pinnedSnapshotPath(987657);
  if (!saveTemporarySnapshot(image, reopenPath, error))
    return false;
  {
    PinSnapshotFile file(reopenPath);
    if (!file.isLocked()) {
      error = QStringLiteral("Reopened pin snapshot was not locked");
      QFile::remove(reopenPath);
      return false;
    }
    file.preserveForEditor();
  }
  if (!QFileInfo::exists(reopenPath)) {
    error = QStringLiteral("Reopened pin snapshot was not preserved");
    return false;
  }
  QFile::remove(reopenPath);

  const QString unrelatedPath =
      QDir(secureRuntimeDirectory()).filePath(QStringLiteral("manual.png"));
  if (!saveTemporarySnapshot(image, unrelatedPath, error))
    return false;
  {
    PinSnapshotFile file(unrelatedPath);
    if (!file.isLocked()) {
      error = QStringLiteral("Unrelated runtime file was not locked");
      QFile::remove(unrelatedPath);
      return false;
    }
  }
  if (!QFileInfo::exists(unrelatedPath)) {
    error = QStringLiteral("Unrelated runtime file was removed");
    return false;
  }
  QFile::remove(unrelatedPath);

  const QString path = pinnedSnapshotPath(987654);
  if (!saveTemporarySnapshot(image, path, error))
    return false;
  QFile agedFile(path);
  if (!agedFile.open(QIODevice::ReadOnly) ||
      !agedFile.setFileTime(QDateTime::currentDateTime().addDays(-2),
                            QFileDevice::FileModificationTime)) {
    error = QStringLiteral("Could not age pin snapshot");
    QFile::remove(path);
    return false;
  }
  agedFile.close();

  bool survived = false;
  {
    PinSnapshotFile file(path);
    if (!file.isLocked()) {
      error = QStringLiteral("Could not hold pin snapshot lock");
      QFile::remove(path);
      return false;
    }
    prunePinnedSnapshots();
    survived = QFileInfo::exists(path);
  }
  if (!survived) {
    error = QStringLiteral("Active pin snapshot was pruned");
    return false;
  }

  const QString abandonedPath = pinnedSnapshotPath(987655);
  if (!saveTemporarySnapshot(image, abandonedPath, error))
    return false;
  QFile abandonedFile(abandonedPath);
  if (!abandonedFile.open(QIODevice::ReadOnly) ||
      !abandonedFile.setFileTime(QDateTime::currentDateTime().addDays(-2),
                                 QFileDevice::FileModificationTime)) {
    error = QStringLiteral("Could not age abandoned pin snapshot");
    QFile::remove(abandonedPath);
    return false;
  }
  abandonedFile.close();
  prunePinnedSnapshots();
  if (QFileInfo::exists(abandonedPath)) {
    error = QStringLiteral("Abandoned pin snapshot was not pruned");
    QFile::remove(abandonedPath);
    return false;
  }
  return true;
}
