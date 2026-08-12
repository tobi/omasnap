/** @fileoverview Owns a lock for a pinned snapshot file. */
#pragma once

#include <QString>

/** Tracks a pinned snapshot while its window is open. */
class PinSnapshotFile {
public:
  /** Opens and locks the supplied snapshot path. */
  explicit PinSnapshotFile(QString path);
  /** Releases the lock and removes an owned internal snapshot. */
  ~PinSnapshotFile();

  PinSnapshotFile(const PinSnapshotFile &) = delete;
  PinSnapshotFile &operator=(const PinSnapshotFile &) = delete;

  /** Returns whether the snapshot lock is held. */
  [[nodiscard]] bool isLocked() const;
  /** Keeps the snapshot for reopening in the editor. */
  void preserveForEditor();

private:
  QString path_;
  int fd_ = -1;
  bool preserve_ = false;
};
