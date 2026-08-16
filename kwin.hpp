/** @fileoverview Declares KDE Plasma (KWin) discovery and capture support.
 *
 * The backend is compiled only when CMake is configured with
 * -DOMASNAP_KDE=ON; otherwise the inline stubs below keep call sites free
 * of preprocessor branching and the compiler drops the dead paths.
 */
#pragma once

#include "capture.hpp"

#ifdef OMASNAP_KDE
/** Returns true when running inside a KDE Plasma Wayland session. */
[[nodiscard]] bool kwinSession();
[[nodiscard]] bool kwinCaptureFocusedMonitor(CaptureData &capture,
                                             QString &error);
[[nodiscard]] bool kwinCaptureWindowSurface(const WindowTarget &window,
                                            QImage &image, QString &error);
/** Maps KWin script window JSON into monitor-relative window targets. */
[[nodiscard]] QVector<WindowTarget> kwinParseWindows(const QByteArray &json,
                                                     const MonitorInfo &monitor);
#else
[[nodiscard]] inline bool kwinSession() { return false; }
[[nodiscard]] inline bool kwinCaptureFocusedMonitor(CaptureData &,
                                                    QString &error) {
  error = QStringLiteral("KDE support is not compiled in");
  return false;
}
[[nodiscard]] inline bool kwinCaptureWindowSurface(const WindowTarget &,
                                                   QImage &, QString &error) {
  error = QStringLiteral("KDE support is not compiled in");
  return false;
}
#endif
