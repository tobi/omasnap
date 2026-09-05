/** @fileoverview Owns locks for pinned snapshot files and layout slots. */
#pragma once

#include <QString>

/** Tracks a pinned snapshot while its window is open. */
class PinSnapshotFile {
public:
  explicit PinSnapshotFile(QString path);
  ~PinSnapshotFile();

  PinSnapshotFile(const PinSnapshotFile &) = delete;
  PinSnapshotFile &operator=(const PinSnapshotFile &) = delete;

  [[nodiscard]] bool isLocked() const;
  void preserveForEditor();

private:
  QString path_;
  int fd_ = -1;
  bool preserve_ = false;
};
