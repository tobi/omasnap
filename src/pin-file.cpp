/** @fileoverview Implements pinned snapshot and slot locking. */
#include "pin-file.hpp"

#include "capture.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

PinSnapshotFile::PinSnapshotFile(QString path)
    : path_(QFileInfo(path).absoluteFilePath()) {
  fd_ = ::open(QFile::encodeName(path_).constData(),
               O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd_ < 0 || ::flock(fd_, LOCK_SH | LOCK_NB) != 0) {
    if (fd_ >= 0)
      ::close(fd_);
    fd_ = -1;
  }
}

PinSnapshotFile::~PinSnapshotFile() {
  if (fd_ < 0)
    return;
  // Only the last holder may unlink. Upgrade SH -> EX; if another pin still
  // holds SH, leave the file for that process.
  const bool lastOwner = !preserve_ && ::flock(fd_, LOCK_EX | LOCK_NB) == 0;
  static const QRegularExpression internalName(
      QStringLiteral("^pin-[1-9][0-9]*-[1-9][0-9]*-[0-9a-f]{16}\\.png$"));
  const QFileInfo fileInfo(path_);
  const QString runtime = secureRuntimeDirectory();
  if (lastOwner && !runtime.isEmpty() && fileInfo.absolutePath() == runtime &&
      internalName.match(fileInfo.fileName()).hasMatch()) {
    QFile::remove(path_);
    QFile::remove(operationLogPath(path_));
  }
  ::close(fd_);
}

bool PinSnapshotFile::isLocked() const { return fd_ >= 0; }

void PinSnapshotFile::preserveForEditor() { preserve_ = true; }
