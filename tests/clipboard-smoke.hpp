/** @fileoverview Declares clipboard image loading smoke checks. */
#pragma once

#include <QString>

/** Runs clipboard image loading checks with a fake wl-paste command. */
[[nodiscard]] bool runClipboardSmoke(QString &error);
