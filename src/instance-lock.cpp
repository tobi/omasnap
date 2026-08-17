/** @fileoverview Hands the single-instance lock over to a starting process. */
#include "instance-lock.hpp"

#include <QDeadlineTimer>
#include <QLockFile>
#include <QThread>

#include <cerrno>
#include <csignal>
#include <cstring>

namespace {
/** How long a starting editor waits for the running instance to let go. */
constexpr int kHandoverTimeoutMs = 2000;
/** Poll interval while waiting for the lock file to disappear. */
constexpr int kHandoverPollMs = 20;

InstanceLockProbe probeLock(QLockFile &lock) {
  if (lock.tryLock(0))
    return InstanceLockProbe::Acquired;
  switch (lock.error()) {
  case QLockFile::LockFailedError:
    return InstanceLockProbe::HeldByOther;
  case QLockFile::PermissionError:
    return InstanceLockProbe::PermissionError;
  case QLockFile::NoError:
  case QLockFile::UnknownError:
    break;
  }
  return InstanceLockProbe::UnknownError;
}

/** Reads the lock holder's pid and whether that process still exists. */
InstanceHolder readHolder(QLockFile &lock, qint64 &pid) {
  qint64 lockPid = 0;
  QString hostname;
  QString application;
  if (!lock.getLockInfo(&lockPid, &hostname, &application) || lockPid <= 0)
    return InstanceHolder::Unknown;
  pid = lockPid;
  if (::kill(static_cast<pid_t>(lockPid), 0) != 0 && errno == ESRCH)
    return InstanceHolder::Dead;
  return InstanceHolder::Alive;
}

enum class SignalOutcome { Sent, AlreadyGone, Failed };

SignalOutcome terminateHolder(qint64 pid, QString &error) {
  if (::kill(static_cast<pid_t>(pid), SIGTERM) == 0)
    return SignalOutcome::Sent;
  if (errno == ESRCH)
    return SignalOutcome::AlreadyGone;
  error = QStringLiteral("Could not stop the running omasnap instance "
                         "(pid %1): %2")
              .arg(pid)
              .arg(QString::fromLocal8Bit(std::strerror(errno)));
  return SignalOutcome::Failed;
}

/** Claims a lock file whose owner is gone. */
InstanceLockResult claimAbandonedLock(QLockFile &lock) {
  lock.removeStaleLockFile();
  if (lock.tryLock(0))
    return {true, 0, {}, 0};
  return {false, kInstanceLockErrorExitCode,
          QStringLiteral("Could not claim the abandoned single-instance lock "
                         "file %1")
              .arg(lock.fileName()),
          0};
}

/** Waits for the terminated instance to release the lock, then takes it. */
bool waitForHandover(QLockFile &lock) {
  QDeadlineTimer deadline(kHandoverTimeoutMs);
  for (;;) {
    if (lock.tryLock(0))
      return true;
    qint64 pid = 0;
    if (lock.error() == QLockFile::LockFailedError &&
        readHolder(lock, pid) != InstanceHolder::Alive) {
      // The old process died without unlinking its lock file.
      lock.removeStaleLockFile();
      if (lock.tryLock(0))
        return true;
    }
    if (deadline.hasExpired())
      return false;
    QThread::msleep(kHandoverPollMs);
  }
}
} // namespace

InstanceAction decideInstanceAction(InstanceLockProbe probe,
                                    InstanceHolder holder, InstanceMode mode) {
  switch (probe) {
  case InstanceLockProbe::Acquired:
    return InstanceAction::Proceed;
  case InstanceLockProbe::PermissionError:
  case InstanceLockProbe::UnknownError:
    return InstanceAction::Fail;
  case InstanceLockProbe::HeldByOther:
    break;
  }
  if (holder != InstanceHolder::Alive)
    return InstanceAction::ClearStaleLock;
  return mode == InstanceMode::EditFile ? InstanceAction::ReplaceRunning
                                        : InstanceAction::CancelRunning;
}

InstanceLockResult acquireInstanceLock(QLockFile &lock, InstanceMode mode) {
  // Never expire a lock by age: a long annotation session is not stale.
  lock.setStaleLockTime(0);
  const InstanceLockProbe probe = probeLock(lock);
  qint64 holderPid = 0;
  const InstanceHolder holder = probe == InstanceLockProbe::HeldByOther
                                    ? readHolder(lock, holderPid)
                                    : InstanceHolder::Unknown;

  QString error;
  switch (decideInstanceAction(probe, holder, mode)) {
  case InstanceAction::Proceed:
    return {true, 0, {}, 0};
  case InstanceAction::ClearStaleLock:
    return claimAbandonedLock(lock);
  case InstanceAction::CancelRunning:
    switch (terminateHolder(holderPid, error)) {
    case SignalOutcome::Sent:
      return {false, kInstanceCancelledExitCode, {}, holderPid};
    case SignalOutcome::AlreadyGone:
      return claimAbandonedLock(lock); // Nothing to cancel; act as the first.
    case SignalOutcome::Failed:
      return {false, kInstanceLockErrorExitCode, error, 0};
    }
    break;
  case InstanceAction::ReplaceRunning:
    switch (terminateHolder(holderPid, error)) {
    case SignalOutcome::Sent:
      break;
    case SignalOutcome::AlreadyGone:
      return claimAbandonedLock(lock);
    case SignalOutcome::Failed:
      return {false, kInstanceLockErrorExitCode, error, 0};
    }
    if (waitForHandover(lock))
      return {true, 0, {}, holderPid};
    return {false, kInstanceLockErrorExitCode,
            QStringLiteral("Timed out waiting for omasnap (pid %1) to release "
                           "the single-instance lock")
                .arg(holderPid),
            holderPid};
  case InstanceAction::Fail:
    return {false, kInstanceLockErrorExitCode,
            (probe == InstanceLockProbe::PermissionError
                 ? QStringLiteral("No permission to use the single-instance "
                                  "lock file %1")
                 : QStringLiteral("Could not use the single-instance lock "
                                  "file %1"))
                .arg(lock.fileName()),
            0};
  }
  return {false, kInstanceLockErrorExitCode,
          QStringLiteral("Could not use the single-instance lock file %1")
              .arg(lock.fileName()),
          0};
}
