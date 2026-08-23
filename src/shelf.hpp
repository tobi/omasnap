#pragma once

#include <QString>

/** Runs or notifies the single per-session capture Shelf process. */
[[nodiscard]] int runCaptureShelf(const QString &path,
                                  const QString &screenName = {});
