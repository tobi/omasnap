/** @fileoverview Declares the pinned screenshot window. */
#pragma once

#include <QString>

/** Displays a pinned screenshot until its window closes. */
[[nodiscard]] int runPinnedCapture(const QString &path);
