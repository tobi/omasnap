/** @fileoverview Declares native Wayland capture smoke checks. */
#pragma once

#include <QString>

/** Checks Wayland cleanup, monitor geometry, and buffer orientation. */
[[nodiscard]] bool runTransformSmoke(QString &error);
