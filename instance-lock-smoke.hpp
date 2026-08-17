/** @fileoverview Declares single-instance lock smoke checks. */
#pragma once

#include <QString>

/** Environment variable that turns this executable into a lock holder. */
inline constexpr char kInstanceLockHolderVariable[] =
    "OMASNAP_SMOKE_HOLD_INSTANCE_LOCK";

/** Checks the handover decision table and live SIGTERM handovers. */
[[nodiscard]] bool runInstanceLockSmoke(QString &error);

/** Holds the given lock file until SIGTERM, then releases it and exits. */
[[nodiscard]] int runInstanceLockHolder(const QString &lockPath);
