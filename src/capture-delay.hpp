/** @fileoverview Declares the delayed-capture countdown and its visual model. */
#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QString>
#include <QTimer>
#include <QWidget>

class QPainter;

/** Corners accepted by --delay-position. */
enum class CaptureDelayPosition { TopLeft, TopRight, BottomLeft, BottomRight };

struct CaptureDelayVisualState {
  int remainingSeconds = 1;
  qreal secondProgress = 0.0;
  qreal entranceProgress = 0.0;
  qreal digitTransitionProgress = 1.0;
  qreal circleProgress = 1.0;
  qreal exitProgress = 0.0;
  bool quiet = false;
  bool complete = false;
};

/** Strictly parses a whole-second delay in the inclusive range 0..3600. */
[[nodiscard]] bool parseCaptureDelay(const QString &value, int &seconds,
                                     QString &error);
/** Parses one of the four documented corner names. */
[[nodiscard]] bool parseCaptureDelayPosition(const QString &value,
                                             CaptureDelayPosition &position,
                                             QString &error);
/** Pure animation state used by both painting and headless smoke coverage. */
[[nodiscard]] CaptureDelayVisualState captureDelayVisualState(
    int totalSeconds, qint64 elapsedMs);
/** Formats quiet-state delays as seconds or m:ss. */
[[nodiscard]] QString captureDelayText(int seconds, bool quiet);

/** Custom-painted, focusless countdown surface. Layer-shell setup is external. */
class CaptureDelayWidget final : public QWidget {
  Q_OBJECT
public:
  CaptureDelayWidget(int seconds, CaptureDelayPosition position,
                     QWidget *parent = nullptr);

  void startCountdown();
  /** Immediately releases the native layer surface after it is hidden. */
  void destroySurface();
  [[nodiscard]] QImage renderFrameForTest(qint64 elapsedMs) const;

signals:
  void countdownFinished();

protected:
  void paintEvent(QPaintEvent *event) override;

private:
  void paintFrame(QPainter &painter, qint64 elapsedMs) const;
  void updateAnimation();
  void finishCountdown();

  int seconds_ = 0;
  CaptureDelayPosition position_ = CaptureDelayPosition::TopRight;
  QElapsedTimer clock_;
  QTimer frameTimer_;
  QTimer deadlineTimer_;
  bool finished_ = false;
};
