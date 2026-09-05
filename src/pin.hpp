#pragma once

#include <QString>

/** Runs a detached pinned-image window using the current Omasnap process. */
[[nodiscard]] int runPinnedCapture(const QString &path);
