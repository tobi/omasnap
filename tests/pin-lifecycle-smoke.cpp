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

bool runPinLifecycleSmoke(QString &error) {
  const QString firstPath = pinnedSnapshotPath(987654);
  const QString secondPath = pinnedSnapshotPath(987654);
  if (firstPath.isEmpty() || secondPath.isEmpty() || firstPath == secondPath) {
    error = QStringLiteral("Pinned snapshot paths are not unique");
    return false;
  }

  QImage image(8, 8, QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::white);

  // Editing a pinned snapshot reopens at the captured scale: the pin save
  // records the logical size in a sidecar the file editor reads back.
  const QString scaledPath = pinnedSnapshotPath(987660);
  if (!savePinnedSnapshot(image, scaledPath, QSize(4, 4), error))
    return false;
  OperationLog sidecar;
  if (!loadOperationLog(operationLogPath(scaledPath), sidecar, error)) {
    QFile::remove(scaledPath);
    return false;
  }
  CaptureData reopened;
  describeFileCapture(reopened, QImage(scaledPath), sidecar);
  const bool scaleRestored = qFuzzyCompare(reopened.monitor.scale, 2.0) &&
                             reopened.previewSize == QSize(4, 4);
  QFile::remove(scaledPath);
  QFile::remove(operationLogPath(scaledPath));
  if (!scaleRestored) {
    error = QStringLiteral(
        "Pinned snapshot sidecar did not restore the captured scale");
    return false;
  }

  const QString closePath = pinnedSnapshotPath(987656);
  if (!savePinnedSnapshot(image, closePath, QSize(4, 4), error))
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
  if (QFileInfo::exists(operationLogPath(closePath))) {
    error = QStringLiteral("Pin close left its scale sidecar behind");
    QFile::remove(operationLogPath(closePath));
    return false;
  }

  const QString sharedPath = pinnedSnapshotPath(987659);
  if (!saveTemporarySnapshot(image, sharedPath, error))
    return false;
  {
    PinSnapshotFile first(sharedPath);
    if (!first.isLocked()) {
      error = QStringLiteral("Shared pin snapshot locks were not acquired");
      QFile::remove(sharedPath);
      return false;
    }
    {
      PinSnapshotFile second(sharedPath);
      if (!second.isLocked()) {
        error = QStringLiteral("Shared pin snapshot locks were not acquired");
        QFile::remove(sharedPath);
        return false;
      }
    }
    if (!QFileInfo::exists(sharedPath)) {
      error = QStringLiteral("Shared pin snapshot vanished before last close");
      return false;
    }
  }
  if (QFileInfo::exists(sharedPath)) {
    error = QStringLiteral("Last pin close did not remove the shared snapshot");
    QFile::remove(sharedPath);
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
  if (!savePinnedSnapshot(image, reopenPath, QSize(4, 4), error))
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
  if (!QFileInfo::exists(reopenPath) ||
      !QFileInfo::exists(operationLogPath(reopenPath))) {
    error = QStringLiteral("Reopened pin snapshot was not fully preserved");
    QFile::remove(reopenPath);
    QFile::remove(operationLogPath(reopenPath));
    return false;
  }
  QFile::remove(reopenPath);
  QFile::remove(operationLogPath(reopenPath));

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
