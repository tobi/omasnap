/** @fileoverview Declares pin file lifecycle smoke checks. */
#pragma once

class QString;

/** Verifies pin naming, ownership, cleanup, and stale-file pruning. */
[[nodiscard]] bool runPinLifecycleSmoke(QString &error);
