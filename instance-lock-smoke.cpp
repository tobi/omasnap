/** @fileoverview Tests single-instance handover decisions and live SIGTERM. */
#include "instance-lock-smoke.hpp"

#include "instance-lock.hpp"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QThread>

#include <csignal>
#include <unistd.h>

namespace {
volatile sig_atomic_t holderTerminated = 0;

struct DecisionCase {
  const char *name;
  InstanceLockProbe probe;
  InstanceHolder holder;
  InstanceMode mode;
  InstanceAction expected;
};

/** The full decision table; every row doubles as a named check. */
constexpr DecisionCase kDecisionCases[] = {
    {"free lock proceeds for capture", InstanceLockProbe::Acquired,
     InstanceHolder::Unknown, InstanceMode::Capture, InstanceAction::Proceed},
    {"free lock proceeds for file edit", InstanceLockProbe::Acquired,
     InstanceHolder::Unknown, InstanceMode::EditFile, InstanceAction::Proceed},
    {"live holder cancels a capture invocation", InstanceLockProbe::HeldByOther,
     InstanceHolder::Alive, InstanceMode::Capture,
     InstanceAction::CancelRunning},
    {"live holder is replaced for a file edit", InstanceLockProbe::HeldByOther,
     InstanceHolder::Alive, InstanceMode::EditFile,
     InstanceAction::ReplaceRunning},
    {"dead holder clears the stale lock for capture",
     InstanceLockProbe::HeldByOther, InstanceHolder::Dead, InstanceMode::Capture,
     InstanceAction::ClearStaleLock},
    {"dead holder clears the stale lock for a file edit",
     InstanceLockProbe::HeldByOther, InstanceHolder::Dead,
     InstanceMode::EditFile, InstanceAction::ClearStaleLock},
    {"unreadable lock info clears the stale lock",
     InstanceLockProbe::HeldByOther, InstanceHolder::Unknown,
     InstanceMode::Capture, InstanceAction::ClearStaleLock},
    {"permission error fails instead of pretending to be a second instance",
     InstanceLockProbe::PermissionError, InstanceHolder::Unknown,
     InstanceMode::Capture, InstanceAction::Fail},
    {"permission error fails for a file edit too",
     InstanceLockProbe::PermissionError, InstanceHolder::Unknown,
     InstanceMode::EditFile, InstanceAction::Fail},
    {"unknown lock error fails", InstanceLockProbe::UnknownError,
     InstanceHolder::Unknown, InstanceMode::Capture, InstanceAction::Fail},
};

bool runDecisionTableChecks(QString &error) {
  for (const DecisionCase &check : kDecisionCases) {
    if (decideInstanceAction(check.probe, check.holder, check.mode) !=
        check.expected) {
      error = QStringLiteral("Instance decision failed: %1")
                  .arg(QString::fromLatin1(check.name));
      return false;
    }
  }
  return true;
}

/** Starts this executable in lock-holder mode and waits until it owns it. */
bool startLockHolder(QProcess &holder, const QString &lockPath, QString &error) {
  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  environment.insert(QString::fromLatin1(kInstanceLockHolderVariable),
                     lockPath);
  holder.setProcessEnvironment(environment);
  holder.setProgram(QCoreApplication::applicationFilePath());
  holder.start();
  if (!holder.waitForStarted(5000)) {
    error = QStringLiteral("Could not start the lock-holder process");
    return false;
  }
  QDeadlineTimer deadline(5000);
  for (;;) {
    qint64 pid = 0;
    QString hostname;
    QString application;
    QLockFile probe(lockPath);
    if (QFile::exists(lockPath) &&
        probe.getLockInfo(&pid, &hostname, &application) &&
        pid == holder.processId())
      return true;
    if (deadline.hasExpired()) {
      error = QStringLiteral("Lock-holder process never took the lock");
      return false;
    }
    QThread::msleep(10);
  }
}

/** A file edit must terminate the overlay, wait for it, and still open. */
bool runFileEditHandoverCheck(const QString &lockPath, QString &error) {
  QProcess holder;
  if (!startLockHolder(holder, lockPath, error))
    return false;
  const qint64 holderPid = holder.processId();

  QLockFile lock(lockPath);
  const InstanceLockResult result =
      acquireInstanceLock(lock, InstanceMode::EditFile);
  if (!result.proceed || !result.error.isEmpty() || !lock.isLocked() ||
      result.signalledPid != holderPid) {
    error = QStringLiteral("File edit did not take over the lock: %1")
                .arg(result.error.isEmpty()
                         ? QStringLiteral("no editor would have opened")
                         : result.error);
    holder.kill();
    holder.waitForFinished(2000);
    return false;
  }
  if (!holder.waitForFinished(2000) || holder.exitCode() != 0) {
    error = QStringLiteral("Lock holder did not exit cleanly on SIGTERM");
    return false;
  }
  lock.unlock();
  return true;
}

/** A second capture invocation cancels the overlay without starting one. */
bool runCaptureCancelCheck(const QString &lockPath, QString &error) {
  QProcess holder;
  if (!startLockHolder(holder, lockPath, error))
    return false;
  const qint64 holderPid = holder.processId();

  QLockFile lock(lockPath);
  const InstanceLockResult result =
      acquireInstanceLock(lock, InstanceMode::Capture);
  if (result.proceed || result.exitCode != kInstanceCancelledExitCode ||
      !result.error.isEmpty() || result.signalledPid != holderPid) {
    error = QStringLiteral("Second capture invocation did not cancel the "
                           "running overlay");
    holder.kill();
    holder.waitForFinished(2000);
    return false;
  }
  if (!holder.waitForFinished(2000) || holder.exitCode() != 0) {
    error = QStringLiteral("Cancelled overlay did not shut down on SIGTERM");
    return false;
  }
  QLockFile released(lockPath);
  released.setStaleLockTime(0);
  if (!released.tryLock(0)) {
    error = QStringLiteral("SIGTERM did not release the single-instance lock");
    return false;
  }
  released.unlock();
  return true;
}

/** A killed instance leaves a lock file that the next launch must reclaim. */
bool runStaleLockCheck(const QString &lockPath, QString &error) {
  QProcess holder;
  if (!startLockHolder(holder, lockPath, error))
    return false;
  holder.kill();
  if (!holder.waitForFinished(2000)) {
    error = QStringLiteral("Could not kill the lock-holder process");
    return false;
  }
  if (!QFile::exists(lockPath)) {
    error = QStringLiteral("Killed lock holder unexpectedly cleaned up");
    return false;
  }
  QLockFile lock(lockPath);
  const InstanceLockResult result =
      acquireInstanceLock(lock, InstanceMode::Capture);
  if (!result.proceed || !lock.isLocked() || result.signalledPid != 0) {
    error = QStringLiteral("Stale lock was not reclaimed: %1").arg(result.error);
    return false;
  }
  lock.unlock();
  return true;
}

/** An unusable lock path must fail loudly, not look like a second instance. */
bool runUnwritableLockCheck(const QString &root, QString &error) {
  if (::geteuid() == 0)
    return true; // Root ignores the permission bits this check relies on.
  const QString locked = QDir(root).filePath(QStringLiteral("no-write"));
  if (!QDir().mkdir(locked) ||
      !QFile::setPermissions(locked, QFileDevice::ReadOwner |
                                         QFileDevice::ExeOwner)) {
    error = QStringLiteral("Could not create the read-only lock directory");
    return false;
  }
  QLockFile lock(QDir(locked).filePath(QStringLiteral("omasnap.instance")));
  const InstanceLockResult result =
      acquireInstanceLock(lock, InstanceMode::Capture);
  const bool ok = !result.proceed &&
                  result.exitCode == kInstanceLockErrorExitCode &&
                  !result.error.isEmpty();
  static_cast<void>(QFile::setPermissions(locked, QFileDevice::ReadOwner |
                                                      QFileDevice::WriteOwner |
                                                      QFileDevice::ExeOwner));
  if (!ok) {
    error = QStringLiteral("Unwritable lock path did not fail loudly");
    return false;
  }
  return true;
}
} // namespace

bool runInstanceLockSmoke(QString &error) {
  if (!runDecisionTableChecks(error))
    return false;
  QTemporaryDir lockRoot;
  if (!lockRoot.isValid()) {
    error = QStringLiteral("Could not create the instance-lock test directory");
    return false;
  }
  const QString lockPath =
      QDir(lockRoot.path()).filePath(QStringLiteral("omasnap.instance"));
  return runFileEditHandoverCheck(lockPath, error) &&
         runCaptureCancelCheck(lockPath, error) &&
         runStaleLockCheck(lockPath, error) &&
         runUnwritableLockCheck(lockRoot.path(), error);
}

int runInstanceLockHolder(const QString &lockPath) {
  struct sigaction action{};
  action.sa_handler = [](int) { holderTerminated = 1; };
  sigemptyset(&action.sa_mask);
  if (::sigaction(SIGTERM, &action, nullptr) != 0)
    return 95;
  QLockFile lock(lockPath);
  lock.setStaleLockTime(0);
  if (!lock.tryLock(0))
    return 96;
  QDeadlineTimer guard(30000);
  while (holderTerminated == 0 && !guard.hasExpired())
    QThread::msleep(5);
  if (holderTerminated == 0)
    return 97;
  lock.unlock(); // Mirrors the overlay's clean Qt teardown.
  return 0;
}
