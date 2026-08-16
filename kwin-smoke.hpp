/** @fileoverview Declares the KWin window-list parsing smoke test. */
#pragma once

#include <QString>

#ifdef OMASNAP_KDE
[[nodiscard]] bool runKWinParseSmoke(QString &error);
#else
[[nodiscard]] inline bool runKWinParseSmoke(QString &) { return true; }
#endif
