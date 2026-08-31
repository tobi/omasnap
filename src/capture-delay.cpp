/** @fileoverview Paints and times the delayed-capture shutter token. */
#include "capture-delay.hpp"

#include "capture.hpp"

#include <QColor>
#include <QEasingCurve>
#include <QFont>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kMaximumDelaySeconds = 3600;
constexpr int kSurfaceSize = 116;
constexpr qreal kTokenDiameter = 86.0;
constexpr qreal kQuietWidth = 90.0;
constexpr qreal kQuietHeight = 46.0;
constexpr int kEntranceMs = 240;
constexpr int kDigitTransitionMs = 180;
constexpr int kMorphMs = 260;
constexpr int kExitMs = 180;
constexpr int kFrameMs = 16;
constexpr int kFinalSeconds = 5;

qreal clampedProgress(qreal value) { return std::clamp(value, 0.0, 1.0); }

qreal eased(qreal value, QEasingCurve::Type curve) {
  return QEasingCurve(curve).valueForProgress(clampedProgress(value));
}

QPointF entranceOffset(CaptureDelayPosition position) {
  constexpr qreal offset = 9.0;
  switch (position) {
  case CaptureDelayPosition::TopLeft:
    return {-offset, -offset};
  case CaptureDelayPosition::TopRight:
    return {offset, -offset};
  case CaptureDelayPosition::BottomLeft:
    return {-offset, offset};
  case CaptureDelayPosition::BottomRight:
    return {offset, offset};
  }
  return {};
}

void drawCenteredText(QPainter &painter, const QString &text,
                      const QRectF &bounds, const QFont &font,
                      const QColor &color, qreal opacity, qreal scale,
                      qreal verticalOffset = 0.0) {
  painter.save();
  painter.setOpacity(painter.opacity() * clampedProgress(opacity));
  const QPointF center = bounds.center() + QPointF(0, verticalOffset);
  painter.translate(center);
  painter.scale(scale, scale);
  painter.translate(-center);
  painter.setFont(font);
  painter.setPen(color);
  painter.drawText(bounds.translated(0, verticalOffset), Qt::AlignCenter, text);
  painter.restore();
}

void paintAperture(QPainter &painter, const QPointF &center, qreal amount) {
  amount = clampedProgress(amount);
  if (amount <= 0.001)
    return;
  painter.save();
  painter.translate(center);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(7, 7, 10, qRound(115 * amount)));
  const qreal inner = 5.0 + 15.0 * (1.0 - amount);
  for (int blade = 0; blade < 6; ++blade) {
    painter.save();
    painter.rotate(blade * 60.0 + 12.0 * (1.0 - amount));
    QPainterPath path;
    path.moveTo(0, -34);
    path.cubicTo(13, -31, 22, -22, 25, -11);
    path.lineTo(inner, -2);
    path.cubicTo(10, -11, 4, -17, 0, -23);
    path.closeSubpath();
    painter.drawPath(path);
    painter.restore();
  }
  painter.restore();
}
} // namespace

QString captureDelayText(int seconds, bool quiet) {
  if (!quiet || seconds < 60)
    return QString::number(seconds);
  return QStringLiteral("%1:%2")
      .arg(seconds / 60)
      .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

bool parseCaptureDelay(const QString &value, int &seconds, QString &error) {
  // The expression rejects signs/whitespace/fractions; conversion still checks
  // numeric overflow before the bounded cast below.
  static const QRegularExpression wholeSeconds(
      QStringLiteral("^[0-9]+$"));
  bool ok = false;
  const qlonglong parsed = value.toLongLong(&ok, 10);
  if (!wholeSeconds.match(value).hasMatch() || !ok || parsed < 0 || parsed > kMaximumDelaySeconds) {
    error = QStringLiteral(
        "Delay must be a whole number of seconds between 0 and %1")
                .arg(kMaximumDelaySeconds);
    return false;
  }
  seconds = static_cast<int>(parsed);
  return true;
}

bool parseCaptureDelayPosition(const QString &value,
                               CaptureDelayPosition &position,
                               QString &error) {
  if (value == QStringLiteral("top-left"))
    position = CaptureDelayPosition::TopLeft;
  else if (value == QStringLiteral("top-right"))
    position = CaptureDelayPosition::TopRight;
  else if (value == QStringLiteral("bottom-left"))
    position = CaptureDelayPosition::BottomLeft;
  else if (value == QStringLiteral("bottom-right"))
    position = CaptureDelayPosition::BottomRight;
  else {
    error = QStringLiteral(
        "Delay position must be top-left, top-right, bottom-left, or "
        "bottom-right");
    return false;
  }
  return true;
}

CaptureDelayVisualState captureDelayVisualState(int totalSeconds,
                                                qint64 elapsedMs) {
  CaptureDelayVisualState state;
  const qint64 totalMs = std::max(0, totalSeconds) * qint64(1000);
  const qint64 elapsed = std::clamp<qint64>(elapsedMs, 0, totalMs);
  state.complete = elapsed >= totalMs;
  state.remainingSeconds =
      std::max(1, static_cast<int>((totalMs - elapsed + 999) / 1000));
  state.secondProgress = static_cast<qreal>(elapsed % 1000) / 1000.0;
  state.entranceProgress = clampedProgress(
      static_cast<qreal>(elapsed) / static_cast<qreal>(kEntranceMs));
  const qint64 secondOffset = elapsed % 1000;
  state.digitTransitionProgress =
      elapsed < 1000
          ? 1.0
          : clampedProgress(static_cast<qreal>(secondOffset) /
                            static_cast<qreal>(kDigitTransitionMs));
  state.quiet = totalSeconds > 10 && state.remainingSeconds > kFinalSeconds;
  if (totalSeconds > 10 && state.remainingSeconds == kFinalSeconds)
    state.circleProgress =
        clampedProgress(static_cast<qreal>(secondOffset) /
                        static_cast<qreal>(kMorphMs));
  else
    state.circleProgress = state.quiet ? 0.0 : 1.0;
  state.exitProgress =
      totalMs > 0
          ? clampedProgress(
                static_cast<qreal>(elapsed - (totalMs - kExitMs)) /
                static_cast<qreal>(kExitMs))
          : 1.0;
  return state;
}

CaptureDelayWidget::CaptureDelayWidget(int seconds,
                                       CaptureDelayPosition position,
                                       QWidget *parent)
    : QWidget(parent), seconds_(seconds), position_(position) {
  setWindowTitle(QStringLiteral("omasnap-delay"));
  setWindowFlags(Qt::Window | Qt::FramelessWindowHint |
                 Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput);
  setAttribute(Qt::WA_TranslucentBackground);
  setAttribute(Qt::WA_TransparentForMouseEvents);
  setFocusPolicy(Qt::NoFocus);
  setFixedSize(kSurfaceSize, kSurfaceSize);

  frameTimer_.setInterval(kFrameMs);
  frameTimer_.setTimerType(Qt::PreciseTimer);
  connect(&frameTimer_, &QTimer::timeout, this,
          &CaptureDelayWidget::updateAnimation);
  deadlineTimer_.setSingleShot(true);
  deadlineTimer_.setTimerType(Qt::PreciseTimer);
  connect(&deadlineTimer_, &QTimer::timeout, this,
          &CaptureDelayWidget::finishCountdown);
}

void CaptureDelayWidget::startCountdown() {
  if (finished_) {
    emit countdownFinished();
    return;
  }
  if (seconds_ <= 0) {
    finishCountdown();
    return;
  }
  clock_.start();
  frameTimer_.start();
  deadlineTimer_.start(seconds_ * 1000);
  update();
}

void CaptureDelayWidget::destroySurface() { destroy(); }

void CaptureDelayWidget::updateAnimation() {
  if (!clock_.isValid())
    return;
  update();
  const qint64 elapsed = clock_.elapsed();
  const CaptureDelayVisualState state =
      captureDelayVisualState(seconds_, elapsed);
  const bool animating = state.entranceProgress < 1.0 ||
                         state.digitTransitionProgress < 1.0 || !state.quiet ||
                         state.exitProgress > 0.0;
  const int interval =
      animating ? kFrameMs : std::max(1, 1000 - int(elapsed % 1000));
  if (frameTimer_.interval() != interval)
    frameTimer_.setInterval(interval);
}

void CaptureDelayWidget::finishCountdown() {
  if (finished_)
    return;
  finished_ = true;
  frameTimer_.stop();
  deadlineTimer_.stop();
  emit countdownFinished();
}

QImage CaptureDelayWidget::renderFrameForTest(qint64 elapsedMs) const {
  QImage image(size(), QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  QPainter painter(&image);
  paintFrame(painter, elapsedMs);
  return image;
}

void CaptureDelayWidget::paintEvent(QPaintEvent *) {
  QPainter painter(this);
  paintFrame(painter, clock_.isValid() ? clock_.elapsed() : 0);
}

void CaptureDelayWidget::paintFrame(QPainter &painter, qint64 elapsedMs) const {
  painter.setRenderHint(QPainter::Antialiasing, true);
  const CaptureDelayVisualState state =
      captureDelayVisualState(seconds_, elapsedMs);
  if (state.complete)
    return;
  const qreal entrance = eased(state.entranceProgress, QEasingCurve::OutBack);
  const qreal exit = eased(state.exitProgress, QEasingCurve::InCubic);
  const qreal circle = eased(state.circleProgress, QEasingCurve::InOutCubic);
  const QPointF center(width() / 2.0, height() / 2.0);

  painter.save();
  painter.setOpacity(1.0 - exit);
  painter.translate(entranceOffset(position_) * (1.0 - state.entranceProgress));
  painter.translate(center);
  const qreal scale = std::max(0.01, (0.72 + 0.28 * entrance) *
                                         (1.0 - exit));
  painter.scale(scale, scale);
  painter.translate(-center);

  const qreal tokenWidth = kQuietWidth + (kTokenDiameter - kQuietWidth) * circle;
  const qreal tokenHeight = kQuietHeight + (kTokenDiameter - kQuietHeight) * circle;
  const QRectF token(center.x() - tokenWidth / 2.0,
                     center.y() - tokenHeight / 2.0, tokenWidth, tokenHeight);
  const qreal radius = tokenHeight / 2.0;

  for (int layer = 6; layer > 0; --layer) {
    const qreal spread = layer * 1.4;
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 8 + (6 - layer) * 5));
    painter.drawRoundedRect(token.adjusted(-spread, -spread, spread, spread),
                            radius + spread, radius + spread);
  }
  QRadialGradient glass(token.center(), tokenHeight * 0.62,
                        token.center() - QPointF(tokenWidth * 0.12,
                                                 tokenHeight * 0.18));
  glass.setColorAt(0, QColor(31, 30, 34, 248));
  glass.setColorAt(0.72, QColor(17, 17, 21, 247));
  glass.setColorAt(1, QColor(10, 10, 14, 250));
  painter.setPen(QPen(QColor(255, 255, 255, 36), 1.0));
  painter.setBrush(glass);
  painter.drawRoundedRect(token, radius, radius);

  const QRectF ring = QRectF(center.x() - 39, center.y() - 39, 78, 78);
  if (circle > 0.02) {
    painter.save();
    painter.setOpacity(painter.opacity() * circle);
    const int segmentCount =
        seconds_ <= 10 ? seconds_ : kFinalSeconds;
    const int activeIndex = segmentCount - state.remainingSeconds;
    const qreal span = 360.0 / std::max(1, segmentCount);
    constexpr qreal gap = 7.0;
    painter.setPen(QPen(QColor(255, 255, 255, 27), 3.0,
                        Qt::SolidLine, Qt::RoundCap));
    for (int index = 0; index < segmentCount; ++index) {
      const int start = qRound((90.0 - index * span) * 16.0);
      painter.drawArc(ring, start, qRound(-(span - gap) * 16.0));
    }
    painter.setPen(QPen(QColor(242, 198, 109), 3.2, Qt::SolidLine,
                        Qt::RoundCap));
    for (int index = 0; index < std::max(0, activeIndex); ++index) {
      const int start = qRound((90.0 - index * span) * 16.0);
      painter.drawArc(ring, start, qRound(-(span - gap) * 16.0));
    }
    if (activeIndex >= 0 && activeIndex < segmentCount) {
      const qreal fill = state.exitProgress > 0 ? 1.0 : state.secondProgress;
      const int start = qRound((90.0 - activeIndex * span) * 16.0);
      painter.drawArc(ring, start,
                      qRound(-(span - gap) * fill * 16.0));
    }
    painter.restore();
  }

  const qreal apertureAmount =
      std::max(1.0 - state.entranceProgress, state.exitProgress) * circle;
  paintAperture(painter, center, apertureAmount);

  // Stable timer strings use a compact UI face; the expressive Neucha digit
  // takes over for the camera-style final countdown.
  const bool quiet = state.quiet && circle < 0.5;
  const bool previousQuiet =
      quiet || (seconds_ > 10 && state.remainingSeconds == kFinalSeconds);
  QFont font = quiet ? QFont(QStringLiteral("Sans Serif"))
                     : annotationTextFont(9.6);
  const QString currentText =
      captureDelayText(state.remainingSeconds, quiet);
  const QString previousText =
      captureDelayText(state.remainingSeconds + 1, previousQuiet);
  font.setPixelSize(quiet ? (std::max(currentText.size(), previousText.size()) > 4
                                 ? 18
                                 : 23)
                          : 48);
  if (quiet)
    font.setWeight(QFont::DemiBold);
  QFont previousFont = font;
  if (previousQuiet != quiet) {
    previousFont = QFont(QStringLiteral("Sans Serif"));
    previousFont.setPixelSize(previousText.size() > 4 ? 18 : 23);
    previousFont.setWeight(QFont::DemiBold);
  }
  const QRectF textBounds = token.adjusted(7, 2, -7, -2);
  const QColor textColor(247, 242, 229);
  QPainterPath textClip;
  textClip.addRoundedRect(token.adjusted(4, 4, -4, -4),
                          std::max(0.0, radius - 4),
                          std::max(0.0, radius - 4));
  painter.save();
  painter.setClipPath(textClip);
  const qreal transition =
      eased(state.digitTransitionProgress, QEasingCurve::OutCubic);
  if (state.digitTransitionProgress < 1.0) {
    drawCenteredText(painter, previousText, textBounds, previousFont, textColor,
                     std::pow(1.0 - transition, 2.0),
                     1.0 + transition * 0.14, -transition * 30.0);
    drawCenteredText(painter, currentText, textBounds, font, textColor,
                     std::pow(transition, 2.0),
                     0.86 + transition * 0.14,
                     (1.0 - transition) * 30.0);
  } else {
    drawCenteredText(painter, currentText, textBounds, font, textColor, 1.0,
                     1.0);
  }
  painter.restore();

  const qreal quietChrome = seconds_ > 10 ? 1.0 - circle : 0.0;
  if (quietChrome > 0.0) {
    painter.save();
    painter.setOpacity(painter.opacity() * quietChrome);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(242, 198, 109));
    painter.drawEllipse(QPointF(token.left() + 10, token.center().y()), 2.3,
                        2.3);
    painter.restore();
  }
  painter.restore();
}
