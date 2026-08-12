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

/** Holds the first free layout slot while a pin is active. */
class PinSlotLock {
public:
  /** Claims the first available layout slot. */
  PinSlotLock();
  /** Releases the claimed layout slot. */
  ~PinSlotLock();

  PinSlotLock(const PinSlotLock &) = delete;
  PinSlotLock &operator=(const PinSlotLock &) = delete;

  /** Returns whether a slot was claimed. */
  [[nodiscard]] bool isLocked() const;
  /** Returns the claimed zero-based slot. */
  [[nodiscard]] int index() const;

private:
  int fd_ = -1;
  int index_ = -1;
};
