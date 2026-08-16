/** @fileoverview Tests KWin window-list JSON mapping without a compositor. */
#include "kwin-smoke.hpp"

#include "kwin.hpp"

bool runKWinParseSmoke(QString &error) {
  MonitorInfo monitor;
  monitor.name = QStringLiteral("DP-1");
  monitor.geometry = {2560, 100, 2560, 1440};
  monitor.pixelSize = {3840, 2160};
  monitor.scale = 1.5;

  // Windows arrive in KWin stacking order (bottom to top) with global
  // logical coordinates: one fully inside, one straddling the monitor edge,
  // one on another monitor, and one untitled.
  const QByteArray json = QByteArrayLiteral(
      "[{\"title\":\"editor\",\"id\":\"{aa}\",\"x\":2660,\"y\":200,"
      "\"width\":800,\"height\":600},"
      "{\"title\":\"straddler\",\"id\":\"{bb}\",\"x\":2360,\"y\":150,"
      "\"width\":400,\"height\":300},"
      "{\"title\":\"elsewhere\",\"id\":\"{cc}\",\"x\":0,\"y\":0,"
      "\"width\":640,\"height\":480},"
      "{\"title\":\"\",\"id\":\"{dd}\",\"x\":2560,\"y\":100,"
      "\"width\":2560,\"height\":1440}]");
  const QVector<WindowTarget> windows = kwinParseWindows(json, monitor);
  if (windows.size() != 3) {
    error = QStringLiteral("KWin window parsing kept %1 of 3 windows")
                .arg(windows.size());
    return false;
  }
  if (windows.at(0).rect != QRect(100, 100, 800, 600) ||
      windows.at(0).stableId != QStringLiteral("{aa}") ||
      windows.at(0).title != QStringLiteral("editor")) {
    error = QStringLiteral("KWin window parsing mangled an inside window");
    return false;
  }
  if (windows.at(1).rect != QRect(0, 50, 200, 300)) {
    error = QStringLiteral("KWin window parsing did not clip to the monitor");
    return false;
  }
  if (windows.at(2).rect != QRect(0, 0, 2560, 1440) ||
      windows.at(2).title != QStringLiteral("window")) {
    error = QStringLiteral("KWin window parsing lost the untitled fallback");
    return false;
  }
  if (kwinParseWindows(QByteArrayLiteral("not json"), monitor).size() != 0) {
    error = QStringLiteral("KWin window parsing accepted invalid JSON");
    return false;
  }
  return true;
}
