/** @fileoverview Declares display-transform smoke checks. */
#pragma once

#include <QString>

/** Checks monitor geometry and captured-buffer orientation. */
[[nodiscard]] bool runTransformSmoke(QString &error);
