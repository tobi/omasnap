/** @fileoverview Exercises delay parsing, animation state, painting, and timing. */
#include "capture-delay-smoke.hpp"

#include "capture-delay.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QThread>

namespace {
bool hasVisiblePixels(const QImage &image) {
  for (int y = 0; y < image.height(); ++y) {
    const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
    for (int x = 0; x < image.width(); ++x) {
      if (qAlpha(line[x]) > 20)
        return true;
    }
  }
  return false;
}
} // namespace

bool runCaptureDelaySmoke(QString &error) {
  int seconds = -1;
  QString parseError;
  if (!parseCaptureDelay(QStringLiteral("0"), seconds, parseError) ||
      seconds != 0 ||
      !parseCaptureDelay(QStringLiteral("3600"), seconds, parseError) ||
      seconds != 3600) {
    error = QStringLiteral("Valid capture delays were not parsed");
    return false;
  }
  for (const QString &invalid : {QStringLiteral("-1"), QStringLiteral("+1"),
                                 QStringLiteral(" 1"), QStringLiteral("1.5"),
                                 QStringLiteral("soon"),
                                 QStringLiteral("3601")}) {
    parseError.clear();
    if (parseCaptureDelay(invalid, seconds, parseError) || parseError.isEmpty()) {
      error = QStringLiteral("Invalid capture delay was accepted: %1")
                  .arg(invalid);
      return false;
    }
  }

  for (const auto &[name, expected] :
       {std::pair{QStringLiteral("top-left"),
                  CaptureDelayPosition::TopLeft},
        std::pair{QStringLiteral("top-right"),
                  CaptureDelayPosition::TopRight},
        std::pair{QStringLiteral("bottom-left"),
                  CaptureDelayPosition::BottomLeft},
        std::pair{QStringLiteral("bottom-right"),
                  CaptureDelayPosition::BottomRight}}) {
    CaptureDelayPosition parsed = CaptureDelayPosition::TopLeft;
    if (!parseCaptureDelayPosition(name, parsed, parseError) ||
        parsed != expected) {
      error = QStringLiteral("Delay position was not parsed: %1").arg(name);
      return false;
    }
  }
  CaptureDelayPosition invalidPosition = CaptureDelayPosition::TopLeft;
  if (parseCaptureDelayPosition(QStringLiteral("center"), invalidPosition,
                                parseError)) {
    error = QStringLiteral("Invalid delay position was accepted");
    return false;
  }

  if (captureDelayText(59, true) != QStringLiteral("59") ||
      captureDelayText(60, true) != QStringLiteral("1:00") ||
      captureDelayText(61, true) != QStringLiteral("1:01") ||
      captureDelayText(3600, true) != QStringLiteral("60:00") ||
      captureDelayText(60, false) != QStringLiteral("60")) {
    error = QStringLiteral("Capture delay text formatting was incorrect");
    return false;
  }

  const CaptureDelayVisualState zero = captureDelayVisualState(0, 0);
  const CaptureDelayVisualState ten = captureDelayVisualState(10, 0);
  const CaptureDelayVisualState eleven = captureDelayVisualState(11, 0);
  const CaptureDelayVisualState quiet = captureDelayVisualState(60, 1200);
  const CaptureDelayVisualState morphing = captureDelayVisualState(60, 55200);
  const CaptureDelayVisualState finalPhase =
      captureDelayVisualState(60, 55900);
  const CaptureDelayVisualState exiting = captureDelayVisualState(60, 59910);
  const CaptureDelayVisualState complete = captureDelayVisualState(60, 60000);
  if (!zero.complete || zero.exitProgress != 1.0 || ten.quiet ||
      ten.circleProgress != 1.0 || !eleven.quiet ||
      eleven.circleProgress != 0.0 || quiet.remainingSeconds != 59 ||
      !quiet.quiet || quiet.circleProgress != 0.0 || morphing.remainingSeconds != 5 ||
      morphing.quiet || morphing.circleProgress <= 0.0 ||
      morphing.circleProgress >= 1.0 || finalPhase.circleProgress != 1.0 ||
      exiting.exitProgress < 0.49 || exiting.exitProgress > 0.51 ||
      !complete.complete) {
    error = QStringLiteral("Capture delay animation state was inconsistent");
    return false;
  }

  const CaptureDelayWidget painted(5, CaptureDelayPosition::TopLeft);
  const CaptureDelayWidget sevenSeconds(7, CaptureDelayPosition::TopRight);
  const CaptureDelayWidget morphPainted(60,
                                        CaptureDelayPosition::BottomRight);
  if (!hasVisiblePixels(painted.renderFrameForTest(500)) ||
      hasVisiblePixels(painted.renderFrameForTest(5000)) ||
      !hasVisiblePixels(sevenSeconds.renderFrameForTest(500)) ||
      !hasVisiblePixels(morphPainted.renderFrameForTest(55200))) {
    error = QStringLiteral("Capture delay token did not paint or clear cleanly");
    return false;
  }

  CaptureDelayWidget timed(1, CaptureDelayPosition::TopLeft);
  int completions = 0;
  QObject::connect(&timed, &CaptureDelayWidget::countdownFinished,
                   [&] { ++completions; });
  timed.startCountdown();
  QElapsedTimer wait;
  wait.start();
  while (completions == 0 && wait.elapsed() < 5000) {
    QApplication::processEvents();
    QThread::msleep(5);
  }
  if (completions != 1 || wait.elapsed() < 850) {
    error = QStringLiteral("Capture delay deadline did not fire once on time");
    return false;
  }
  QElapsedTimer duplicateWindow;
  duplicateWindow.start();
  while (duplicateWindow.elapsed() < 150) {
    QApplication::processEvents();
    QThread::msleep(5);
  }
  if (completions != 1) {
    error = QStringLiteral("Capture delay deadline fired more than once");
    return false;
  }
  return true;
}
