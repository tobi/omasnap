/** @fileoverview Declares single-instance lock handover decisions. */
#pragma once

#include <QString>

class QLockFile;

/** Exit code for a capture invocation that cancelled a running overlay. */
inline constexpr int kInstanceCancelledExitCode = 0;
/** Exit code for a lock that could neither be taken nor handed over. */
inline constexpr int kInstanceLockErrorExitCode = 1;

/** What the starting process wants to do, which decides how it treats a
 * running instance. */
enum class InstanceMode {
  Capture,  // Screen-capture overlay: a second launch dismisses the first.
  EditFile, // Editor for an existing image: it must always appear.
};

/** Result of a single non-blocking attempt to take the lock. */
enum class InstanceLockProbe {
  Acquired,        // The lock is ours; no other instance is running.
  HeldByOther,     // Another process holds the lock.
  PermissionError, // The lock path is not writable.
  UnknownError,    // Anything else; never treat it as "already running".
};

/** Liveness of the process recorded in the lock file. */
enum class InstanceHolder {
  Alive,
  Dead,
  Unknown, // The lock info could not be read.
};

/** What the starting process should do about the lock. */
enum class InstanceAction {
  Proceed,        // Start normally.
  ClearStaleLock, // Remove the abandoned lock file, then start normally.
  CancelRunning,  // Terminate the running instance and exit without starting.
  ReplaceRunning, // Terminate the running instance, wait, then start.
  Fail,           // Report the lock error and exit non-zero.
};

/** Maps the observed lock state and the requested mode onto an action. */
[[nodiscard]] InstanceAction decideInstanceAction(InstanceLockProbe probe,
                                                  InstanceHolder holder,
                                                  InstanceMode mode);

/** Outcome of acting on the single-instance lock. */
struct InstanceLockResult {
  bool proceed = false; // Start the overlay or editor.
  int exitCode = 0;     // Used when proceed is false.
  QString error;        // Non-empty only for failures.
  qint64 signalledPid = 0; // Instance that was asked to quit, if any.
};

/** Takes the lock, terminating and waiting for a running instance as the mode
 * requires. */
[[nodiscard]] InstanceLockResult acquireInstanceLock(QLockFile &lock,
                                                     InstanceMode mode);
