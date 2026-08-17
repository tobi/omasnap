/** @fileoverview Handles screenshot selection, annotation, and editor drawing.
 */
#include "editor.hpp"
#include "icons.hpp"
#include "eyedropper.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDebug>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QRandomGenerator>
#include <QScreen>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <numbers>

namespace {
constexpr std::array<const char *, 6> kColorNames{
    "#ff375f", "#ff9f0a", "#ffd60a", "#30d158", "#0a84ff", "#bf5af2"};
constexpr std::array<qreal, 3> kTextSizes{2.0, 5.0, 9.0};
constexpr std::array<const char *, 3> kTextSizeNames{"S", "M", "L"};
constexpr qreal kToolbarWidth = 760;
constexpr qreal kMinimumRedactionExtent = 5.0;

qreal toolbarScale(qreal availableWidth) {
  constexpr qreal sideMargins = 16.0;
  return std::min<qreal>(
      1.0,
      std::max<qreal>(0.1, (availableWidth - sideMargins) / kToolbarWidth));
}
bool hasEndpointHandles(Annotation::Kind kind) {
  return kind == Annotation::Kind::Arrow || kind == Annotation::Kind::Line ||
         kind == Annotation::Kind::Rectangle ||
         kind == Annotation::Kind::Redaction ||
         kind == Annotation::Kind::Spotlight;
}

bool showsSelectionBounds(Annotation::Kind kind) {
  return kind != Annotation::Kind::Arrow && kind != Annotation::Kind::Line;
}

bool isStrokeKind(Annotation::Kind kind) {
  return kind == Annotation::Kind::Freehand ||
         kind == Annotation::Kind::Highlighter;
}

qreal strokeHitTolerance(const Annotation &annotation) {
  if (annotation.kind == Annotation::Kind::Highlighter)
    return std::max<qreal>(8.0, annotation.size * 3.0 + 4.0);
  return std::max<qreal>(8.0, annotation.size + 4.0);
}
bool supportsCreationConstraint(CaptureEditor::Tool tool) {
  return tool == CaptureEditor::Tool::Arrow ||
         tool == CaptureEditor::Tool::Line ||
         tool == CaptureEditor::Tool::Rectangle ||
         tool == CaptureEditor::Tool::Spotlight;
}


QString toolAction(CaptureEditor::Tool tool) {
  switch (tool) {
  case CaptureEditor::Tool::Select:
    return QStringLiteral("tool-select");
  case CaptureEditor::Tool::Arrow:
    return QStringLiteral("tool-arrow");
  case CaptureEditor::Tool::Line:
    return QStringLiteral("tool-line");
  case CaptureEditor::Tool::Spotlight:
    return QStringLiteral("tool-spotlight");
  case CaptureEditor::Tool::Freehand:
    return QStringLiteral("tool-freehand");
  case CaptureEditor::Tool::Highlighter:
    return QStringLiteral("tool-highlighter");
  case CaptureEditor::Tool::Marker:
    return QStringLiteral("tool-marker");
  case CaptureEditor::Tool::Rectangle:
    return QStringLiteral("tool-rectangle");
  case CaptureEditor::Tool::Redact:
    return QStringLiteral("tool-redact");
  case CaptureEditor::Tool::Text:
    return QStringLiteral("tool-text");
  case CaptureEditor::Tool::Ocr:
    return QStringLiteral("tool-ocr");
  case CaptureEditor::Tool::Eyedropper:
    return QStringLiteral("tool-eyedropper");
  }
  return {};
}

QString redactionStyleName(RedactionStyle style) {
  return style == RedactionStyle::Solid ? QStringLiteral("Solid")
                                        : QStringLiteral("Pixelate");
}

qreal constrainedRedactionCoordinate(qreal candidate, qreal fixed,
                                     qreal original) {
  return original <= fixed
             ? std::min(candidate, fixed - kMinimumRedactionExtent)
             : std::max(candidate, fixed + kMinimumRedactionExtent);
}

QPointF constrainedRedactionEndpoint(const QPointF &candidate,
                                     const QPointF &fixed,
                                     const QPointF &original) {
  return {
      constrainedRedactionCoordinate(candidate.x(), fixed.x(), original.x()),
      constrainedRedactionCoordinate(candidate.y(), fixed.y(), original.y())};
}

void drawStatusPill(QPainter &painter, const QRect &bounds,
                    const QString &text) {
  QFont font(QStringLiteral("Noto Sans"));
  font.setPixelSize(13);
  painter.setFont(font);
  const int width = painter.fontMetrics().horizontalAdvance(text) + 28;
  const QRectF pill((bounds.width() - width) / 2.0, bounds.height() - 42.0,
                    width, 30);
  painter.setPen(QPen(QColor(255, 255, 255, 32), 1));
  painter.setBrush(QColor(18, 18, 22, 232));
  painter.drawRoundedRect(pill, 10, 10);
  painter.setPen(Qt::white);
  painter.drawText(pill, Qt::AlignCenter, text);
}

void drawInstantTooltip(QPainter &painter, const QRect &bounds,
                        const QRectF &anchor, const QString &text) {
  if (text.isEmpty())
    return;
  QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
  font.setPixelSize(12);
  painter.setFont(font);
  const qreal width = painter.fontMetrics().horizontalAdvance(text) + 20;
  const qreal height = 28;
  qreal x = std::clamp(anchor.center().x() - width / 2.0, 8.0,
                       std::max(8.0, bounds.width() - width - 8.0));
  qreal y = anchor.top() - height - 7;
  if (y < 6)
    y = anchor.bottom() + 7;
  const QRectF pill(x, y, width, height);
  painter.setPen(QPen(QColor(255, 255, 255, 42), 1));
  painter.setBrush(QColor(12, 12, 15, 248));
  painter.drawRoundedRect(pill, 7, 7);
  painter.setPen(Qt::white);
  painter.drawText(pill, Qt::AlignCenter, text);
}

void drawHotkeyLegend(QPainter &painter, const QRect &bounds,
                      const QPointF &cursor,
                      const QVector<QPair<QString, QString>> &entries) {
  if (entries.isEmpty())
    return;
  constexpr int columns = 2;
  const int rows = (entries.size() + columns - 1) / columns;
  const qreal width = 414;
  const qreal height = rows * 19 + 24;
  QRectF panel(bounds.width() - width - 14, 14, width, height);
  if (panel.adjusted(-28, -28, 28, 28).contains(cursor))
    panel.moveLeft(14);

  painter.setPen(QPen(QColor(255, 255, 255, 34), 1));
  painter.setBrush(QColor(13, 15, 20, 224));
  painter.drawRoundedRect(panel, 11, 11);
  QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
  font.setPixelSize(11);
  painter.setFont(font);
  const qreal columnWidth = (panel.width() - 24) / columns;
  for (int index = 0; index < entries.size(); ++index) {
    const int column = index / rows;
    const int row = index % rows;
    const qreal x = panel.left() + 12 + column * columnWidth;
    const qreal y = panel.top() + 12 + row * 19;
    painter.setPen(QColor(QStringLiteral("#a9b6cb")));
    painter.drawText(QRectF(x, y, 70, 18), Qt::AlignLeft | Qt::AlignVCenter,
                     entries.at(index).first);
    painter.setPen(QColor(QStringLiteral("#f5f5f7")));
    painter.drawText(QRectF(x + 72, y, columnWidth - 76, 18),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     entries.at(index).second);
  }
}

QString backgroundName(BackgroundStyle style) {
  switch (style) {
  case BackgroundStyle::None:
    return QStringLiteral("None");
  case BackgroundStyle::Aurora:
    return QStringLiteral("Aurora");
  case BackgroundStyle::Sunset:
    return QStringLiteral("Sunset");
  case BackgroundStyle::Lagoon:
    return QStringLiteral("Lagoon");
  case BackgroundStyle::Violet:
    return QStringLiteral("Violet");
  }
  return {};
}
} // namespace

QPointF constrainedCreationEndpoint(CaptureEditor::Tool tool,
                                    const QPointF &start, const QPointF &end) {
  const QPointF delta = end - start;
  if (tool == CaptureEditor::Tool::Rectangle ||
      tool == CaptureEditor::Tool::Spotlight) {
    const qreal extent = std::max(std::abs(delta.x()), std::abs(delta.y()));
    if (qFuzzyIsNull(extent))
      return end;
    const qreal xDirection = delta.x() < 0 ? -1.0 : 1.0;
    const qreal yDirection = delta.y() < 0 ? -1.0 : 1.0;
    return start + QPointF(xDirection * extent, yDirection * extent);
  }
  if (tool == CaptureEditor::Tool::Arrow || tool == CaptureEditor::Tool::Line) {
    const qreal length = QLineF(start, end).length();
    if (qFuzzyIsNull(length))
      return end;
    constexpr qreal angleStep = std::numbers::pi_v<qreal> / 4.0;
    const qreal angle =
        std::round(std::atan2(delta.y(), delta.x()) / angleStep) * angleStep;
    return start + QPointF(std::cos(angle) * length, std::sin(angle) * length);
  }
  return end;
}

CaptureEditor::CaptureEditor(CaptureData capture, CaptureMode mode,
                             QuickOutputMode quickOutput, QWidget *parent)
    : QWidget(parent), capture_(std::move(capture)),
      quickOutputMode_(quickOutput) {
  setWindowTitle(QStringLiteral("Omasnap"));
  setWindowFlags(Qt::Window | Qt::FramelessWindowHint |
                 Qt::WindowStaysOnTopHint);
  setAttribute(Qt::WA_TranslucentBackground);
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);

  for (QScreen *screen : QGuiApplication::screens()) {
    if (screen->name() == capture_.monitor.name) {
      setGeometry(screen->geometry());
      break;
    }
  }
  if (geometry().isEmpty())
    setGeometry(QGuiApplication::primaryScreen()->geometry());
  cursor_ = mapFromGlobal(QCursor::pos());

  textEditor_ = new QLineEdit(this);
  textEditor_->hide();
  textEditor_->setFrame(false);
  textEditor_->setTextMargins(0, 0, 0, 0);
  textEditor_->setStyleSheet(
      QStringLiteral("QLineEdit { color: #ff375f; background: transparent; "
                     "border: none; padding: 0;"
                     " selection-background-color: #0a84ff; }"));
  textEditor_->installEventFilter(this);
  connect(
      textEditor_, &QLineEdit::textChanged, this, [this](const QString &text) {
        const QFontMetrics metrics(textEditor_->font());
        const int desiredWidth = std::max(
            48, metrics.horizontalAdvance(text + QStringLiteral("  ")));
        const int availableWidth =
            std::max(48, qRound(editImageRect().right() - textEditor_->x()));
        textEditor_->resize(std::min(desiredWidth, availableWidth),
                            textEditor_->height());
      });

  connect(&ocrWatcher_, &QFutureWatcher<OcrResult>::finished, this, [this] {
    const OcrResult result = ocrWatcher_.result();
    busy_ = false;
    if (!result.error.isEmpty()) {
      setStatus(result.error);
      return;
    }
    QString clipboardError;
    if (!copyTextToClipboard(result.text, clipboardError)) {
      setStatus(clipboardError);
      return;
    }
    setStatus(QStringLiteral("OCR copied to clipboard"));
    sendCaptureNotification(QStringLiteral("Copied text from screenshot"));
  });

  if (mode == CaptureMode::Fullscreen || mode == CaptureMode::File) {
    selection_ = QRectF(QPointF(), capture_.preview.size());
    enterEdit(
        mode == CaptureMode::File
            ? QStringLiteral("Editing image from file · Copy/Save to output")
            : QStringLiteral("Full screen selected · native resolution · "
                             "outer handles crop"));
  } else if (mode == CaptureMode::Window) {
    windowMode_ = true;
    hoveredWindow_ = windowAt(cursor_);
    setStatus(QStringLiteral("Window mode · click or Super+Arrows then Enter · "
                             "Space returns to area"));
  }
  updatePointerCursor();
}

CaptureEditor::~CaptureEditor() {
  if (snapshotPath_.isEmpty() || !QFile::exists(snapshotPath_))
    return;
  const QString runtime = secureRuntimeDirectory();
  if (!runtime.isEmpty() && QFileInfo(snapshotPath_).absolutePath() == runtime)
    QFile::remove(snapshotPath_);
}

bool CaptureEditor::eventFilter(QObject *watched, QEvent *event) {
  if (watched == textEditor_ && event->type() == QEvent::KeyPress) {
    auto *key = static_cast<QKeyEvent *>(event);
    if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
      acceptText();
      return true;
    }
    if (key->key() == Qt::Key_Escape) {
      handleEscape();
      return true;
    }
  }
  return QWidget::eventFilter(watched, event);
}

QColor CaptureEditor::annotationColor() const {
  if (usingCustomColor_)
    return customColor_;
  return QColor(QString::fromLatin1(
      kColorNames.at(static_cast<std::size_t>(colorIndex_))));
}
QRectF CaptureEditor::annotationBounds(const Annotation &annotation) const {
  if (annotation.kind == Annotation::Kind::Marker) {
    const qreal diameter = std::max<qreal>(24.0, annotation.size * 6.0);
    return {annotation.start.x() - diameter / 2.0,
            annotation.start.y() - diameter / 2.0, diameter, diameter};
  }
  if (annotation.kind == Annotation::Kind::Text) {
    const QFontMetricsF metrics(annotationTextFont(annotation.size));
    return {annotation.start.x(), annotation.start.y() - metrics.ascent(),
            metrics.horizontalAdvance(annotation.text), metrics.height()};
  }
  if (isStrokeKind(annotation.kind)) {
    if (annotation.points.isEmpty())
      return {};
    qreal left = annotation.points.first().x();
    qreal right = left;
    qreal top = annotation.points.first().y();
    qreal bottom = top;
    for (const QPointF &point : annotation.points) {
      left = std::min(left, point.x());
      right = std::max(right, point.x());
      top = std::min(top, point.y());
      bottom = std::max(bottom, point.y());
    }
    return {QPointF(left, top), QPointF(right, bottom)};
  }
  return QRectF(annotation.start, annotation.end).normalized();
}

int CaptureEditor::annotationAt(const QPointF &point) const {
  const auto containsPoint = [this, &point](const Annotation &annotation) {
    if (annotation.kind == Annotation::Kind::Arrow ||
        annotation.kind == Annotation::Kind::Line) {
      const QPointF delta = annotation.end - annotation.start;
      const qreal lengthSquared = delta.x() * delta.x() + delta.y() * delta.y();
      if (lengthSquared <= 0)
        return false;
      const QPointF fromStart = point - annotation.start;
      const qreal t =
          std::clamp((fromStart.x() * delta.x() + fromStart.y() * delta.y()) /
                         lengthSquared,
                     0.0, 1.0);
      const QPointF closest = annotation.start + delta * t;
      if (QLineF(point, closest).length() <=
          std::max<qreal>(8.0, annotation.size + 4.0))
        return true;
    } else if (isStrokeKind(annotation.kind)) {
      const qreal tolerance = strokeHitTolerance(annotation);
      for (int pointIndex = 1; pointIndex < annotation.points.size();
           ++pointIndex) {
        const QLineF segment(annotation.points.at(pointIndex - 1),
                             annotation.points.at(pointIndex));
        if (segment.length() <= 0)
          continue;
        const QPointF delta = segment.p2() - segment.p1();
        const QPointF fromStart = point - segment.p1();
        const qreal lengthSquared =
            delta.x() * delta.x() + delta.y() * delta.y();
        const qreal t =
            std::clamp((fromStart.x() * delta.x() + fromStart.y() * delta.y()) /
                           lengthSquared,
                       0.0, 1.0);
        const QPointF closest = segment.p1() + delta * t;
        if (QLineF(point, closest).length() <= tolerance)
          return true;
      }
    } else {
      const QRectF bounds = annotationBounds(annotation);
      if (annotation.kind == Annotation::Kind::Rectangle) {
        const qreal tolerance = std::max<qreal>(7.0, annotation.size + 3.0);
        const QRectF outer =
            bounds.adjusted(-tolerance, -tolerance, tolerance, tolerance);
        const QRectF inner =
            bounds.adjusted(tolerance, tolerance, -tolerance, -tolerance);
        return outer.contains(point) &&
               (inner.isEmpty() || !inner.contains(point));
      }
      if (bounds.adjusted(-7, -7, 7, 7).contains(point))
        return true;
    }
    return false;
  };

  for (const int layer : {0, 1, 2}) {
    for (int index = annotations_.size() - 1; index >= 0; --index) {
      const Annotation &annotation = annotations_.at(index);
      const int annotationLayer =
          annotation.kind == Annotation::Kind::Spotlight
              ? 1
              : annotation.kind == Annotation::Kind::Redaction ? 2 : 0;
      if (annotationLayer != layer)
        continue;
      if (layer == 1) {
        if (spotlightPath(annotation).contains(point))
          return index;
        continue;
      }
      if (containsPoint(annotation))
        return index;
    }
  }
  return -1;
}

void CaptureEditor::scaleSelectedAnnotation(qreal factor) {
  if (selectedAnnotation_ < 0 || selectedAnnotation_ >= annotations_.size())
    return;
  recordEdit();
  Annotation &annotation = annotations_[selectedAnnotation_];
  if (annotation.kind == Annotation::Kind::Spotlight) {
    annotation.magnification =
        std::clamp(annotation.magnification * factor, 1.0, 4.0);
    setStatus(QStringLiteral("Spotlight magnification · %1× · wheel adjusts")
                  .arg(annotation.magnification, 0, 'f', 1));
    persistSnapshot();
    return;
  }
  const QPointF center = annotationBounds(annotation).center();
  qreal geometryFactor = factor;
  if (annotation.kind == Annotation::Kind::Redaction) {
    const QRectF bounds = annotationBounds(annotation);
    if (bounds.width() > 0 && bounds.height() > 0) {
      geometryFactor =
          std::max({factor, kMinimumRedactionExtent / bounds.width(),
                    kMinimumRedactionExtent / bounds.height()});
    }
  }
  const auto scaledPoint = [center, geometryFactor](const QPointF &point) {
    return center + (point - center) * geometryFactor;
  };
  if (hasEndpointHandles(annotation.kind)) {
    annotation.start = scaledPoint(annotation.start);
    annotation.end = scaledPoint(annotation.end);
    annotation.size = std::clamp(annotation.size * geometryFactor, 2.0, 30.0);
  } else if (isStrokeKind(annotation.kind)) {
    for (QPointF &point : annotation.points)
      point = scaledPoint(point);
    if (!annotation.points.isEmpty()) {
      annotation.start = annotation.points.first();
      annotation.end = annotation.points.last();
    }
    annotation.size = std::clamp(annotation.size * factor, 2.0, 30.0);
  } else if (annotation.kind == Annotation::Kind::Marker) {
    annotation.size = std::clamp(annotation.size * factor, 2.0, 30.0);
  } else if (annotation.kind == Annotation::Kind::Text) {
    annotation.size = std::clamp(annotation.size * factor, 1.0, 24.0);
    annotation.start += center - annotationBounds(annotation).center();
  }
  setStatus(QStringLiteral("Selected layer · wheel zoom %1%")
                .arg(qRound(factor * 100)));
  persistSnapshot();
}

QRectF CaptureEditor::colorPaletteRect() const {
  const qreal scale = toolbarScale(width());
  const qreal toolbarWidth = kToolbarWidth * scale;
  const qreal buttonHeight = 36 * scale;
  const qreal toolbarX = (width() - toolbarWidth) / 2.0;
  const qreal toolbarY =
      std::max<qreal>(10, editImageRect().top() - buttonHeight - 10);
  const QRectF anchor(toolbarX + 360 * scale, toolbarY, 36 * scale,
                      buttonHeight);
  const qreal paletteWidth = 232;
  const qreal x = std::clamp(anchor.center().x() - paletteWidth / 2.0, 8.0,
                             std::max(8.0, width() - paletteWidth - 8.0));
  return {x, anchor.bottom() + 4, paletteWidth, 36};
}

QRectF CaptureEditor::customColorPanelRect() const {
  QRectF panel(colorPaletteRect().left(), colorPaletteRect().bottom() + 6, 220,
               150);
  if (panel.right() > width() - 8)
    panel.moveRight(width() - 8);
  if (panel.bottom() > height() - 8)
    panel.moveBottom(height() - 8);
  return panel;
}

QRectF CaptureEditor::textSizePanelRect() const {
  const qreal scale = toolbarScale(width());
  const qreal toolbarWidth = kToolbarWidth * scale;
  const qreal buttonHeight = 36 * scale;
  const qreal toolbarX = (width() - toolbarWidth) / 2.0;
  const qreal toolbarY =
      std::max<qreal>(10, editImageRect().top() - buttonHeight - 10);
  const QRectF anchor(toolbarX + 320 * scale, toolbarY, 36 * scale,
                      buttonHeight);
  return {anchor.center().x() - 51, anchor.bottom() + 6, 102, 34};
}

void CaptureEditor::applyCustomColor(const QPointF &position) {
  const QRectF panel = customColorPanelRect();
  const QRectF field = panel.adjusted(12, 12, -36, -12);
  const QRectF hue(panel.right() - 26, panel.top() + 12, 14,
                   panel.height() - 24);
  qreal saturation = customColor_.hsvSaturationF();
  qreal value = customColor_.valueF();
  if (hue.contains(position)) {
    customHue_ =
        std::clamp((position.y() - hue.top()) / hue.height(), 0.0, 1.0);
  } else if (field.contains(position)) {
    saturation =
        std::clamp((position.x() - field.left()) / field.width(), 0.0, 1.0);
    value = 1.0 -
            std::clamp((position.y() - field.top()) / field.height(), 0.0, 1.0);
  } else {
    return;
  }
  customColor_ = QColor::fromHsvF(customHue_, saturation, value);
  usingCustomColor_ = true;
  if (selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations_.size() &&
      annotations_.at(selectedAnnotation_).kind !=
          Annotation::Kind::Redaction) {
    recordEdit();
    annotations_[selectedAnnotation_].color = customColor_;
    persistSnapshot();
  }
  update();
}

QRectF CaptureEditor::normalizedSelection(const QPointF &first,
                                          const QPointF &second) const {
  const QRectF bounds(QPointF(), QSizeF(width(), height()));
  const QPointF a(std::clamp(first.x(), bounds.left(), bounds.right()),
                  std::clamp(first.y(), bounds.top(), bounds.bottom()));
  const QPointF b(std::clamp(second.x(), bounds.left(), bounds.right()),
                  std::clamp(second.y(), bounds.top(), bounds.bottom()));
  return QRectF(a, b).normalized();
}

QRectF CaptureEditor::editImageRect() const {
  if (selection_.isEmpty())
    return {};
  const QRectF available(30, 68, std::max(1, width() - 60),
                         std::max(1, height() - 126));
  const qreal scale =
      std::min<qreal>({1.0, available.width() / selection_.width(),
                       available.height() / selection_.height()});
  const QSizeF shown = selection_.size() * scale;
  return {available.center().x() - shown.width() / 2.0,
          available.center().y() - shown.height() / 2.0, shown.width(),
          shown.height()};
}

QVector<QRectF> CaptureEditor::cropHandleRects() const {
  const QRectF image = editImageRect();
  if (image.isEmpty())
    return {};
  constexpr qreal outside = 7;
  constexpr qreal size = 12;
  const qreal half = size / 2.0;
  const std::array<QPointF, 8> centers{
      image.topLeft() + QPointF(-outside, -outside),
      QPointF(image.center().x(), image.top() - outside),
      image.topRight() + QPointF(outside, -outside),
      QPointF(image.right() + outside, image.center().y()),
      image.bottomRight() + QPointF(outside, outside),
      QPointF(image.center().x(), image.bottom() + outside),
      image.bottomLeft() + QPointF(-outside, outside),
      QPointF(image.left() - outside, image.center().y())};
  QVector<QRectF> handles;
  handles.reserve(static_cast<qsizetype>(centers.size()));
  for (const QPointF &center : centers)
    handles.push_back({center.x() - half, center.y() - half, size, size});
  return handles;
}

int CaptureEditor::cropHandleAt(const QPointF &point) const {
  const QVector<QRectF> handles = cropHandleRects();
  for (int index = 0; index < handles.size(); ++index) {
    if (handles.at(index).contains(point))
      return index;
  }
  return -1;
}

qreal CaptureEditor::editScale() const {
  return selection_.width() > 0 ? editImageRect().width() / selection_.width()
                                : 1.0;
}

QPointF CaptureEditor::toAnnotationPoint(const QPointF &position) const {
  const QRectF image = editImageRect();
  const qreal scale = std::max<qreal>(editScale(), 0.001);
  return {std::clamp((position.x() - image.left()) / scale, 0.0,
                     selection_.width()),
          std::clamp((position.y() - image.top()) / scale, 0.0,
                     selection_.height())};
}

QRectF CaptureEditor::sourceRect(const QRectF &logicalRect) const {
  if (capture_.preview.isNull() || capture_.source.isNull() ||
      capture_.preview.width() <= 0 || capture_.preview.height() <= 0)
    return {};
  const qreal scaleX =
      capture_.source.width() / static_cast<qreal>(capture_.preview.width());
  const qreal scaleY =
      capture_.source.height() / static_cast<qreal>(capture_.preview.height());
  return {logicalRect.x() * scaleX, logicalRect.y() * scaleY,
          logicalRect.width() * scaleX, logicalRect.height() * scaleY};
}

int CaptureEditor::windowAt(const QPointF &position) const {
  for (int index = capture_.windows.size() - 1; index >= 0; --index) {
    if (capture_.windows.at(index).rect.contains(position.toPoint()))
      return index;
  }
  return -1;
}

int CaptureEditor::windowInDirection(int current, int key) const {
  if (capture_.windows.isEmpty())
    return -1;

  const QPointF origin = current >= 0 && current < capture_.windows.size()
                             ? capture_.windows.at(current).rect.center()
                             : cursor_;
  int best = -1;
  qreal bestScore = std::numeric_limits<qreal>::max();
  for (int index = 0; index < capture_.windows.size(); ++index) {
    if (index == current)
      continue;
    const QPointF delta = capture_.windows.at(index).rect.center() - origin;
    qreal along = 0;
    qreal across = 0;
    if (key == Qt::Key_Left && delta.x() < 0) {
      along = -delta.x();
      across = std::abs(delta.y());
    } else if (key == Qt::Key_Right && delta.x() > 0) {
      along = delta.x();
      across = std::abs(delta.y());
    } else if (key == Qt::Key_Up && delta.y() < 0) {
      along = -delta.y();
      across = std::abs(delta.x());
    } else if (key == Qt::Key_Down && delta.y() > 0) {
      along = delta.y();
      across = std::abs(delta.x());
    } else {
      continue;
    }
    const qreal score = along + across * 1.75;
    if (score < bestScore) {
      best = index;
      bestScore = score;
    }
  }
  return best;
}

QVector<CaptureEditor::ToolbarButton> CaptureEditor::toolbarButtons() const {
  QVector<ToolbarButton> buttons;
  const qreal scale = toolbarScale(width());
  const qreal height = 36 * scale;
  const qreal gap = 4 * scale;
  const qreal total = kToolbarWidth * scale;
  qreal x = (width() - total) / 2.0;
  const qreal y = std::max<qreal>(10, editImageRect().top() - height - 10);
  auto add = [&](qreal buttonWidth, QString action, QString label,
                 QString tooltip, QColor color = {}) {
    const qreal scaledWidth = buttonWidth * scale;
    buttons.push_back({QRectF(x, y, scaledWidth, height), std::move(action),
                       std::move(label), std::move(tooltip), color});
    x += scaledWidth + gap;
  };

  add(36, QStringLiteral("tool-select"), {},
      QStringLiteral("Select/move · V · Wheel zoom · outer handles crop"));
  add(36, QStringLiteral("tool-spotlight"), {},
      QStringLiteral("Spotlight · S · %1 · %2× · S cycles shape")
          .arg(spotlightShape_ == SpotlightShape::Ellipse
                   ? QStringLiteral("ellipse")
                   : spotlightShape_ == SpotlightShape::Rectangle
                         ? QStringLiteral("rectangle")
                         : QStringLiteral("rounded"))
          .arg(spotlightMagnification_, 0, 'f', 1));
  add(36, QStringLiteral("tool-arrow"), {},
      QStringLiteral("Arrow · A · Shift snaps 45° · Size %1 · Wheel")
          .arg(qRound(annotationSize_)));
  add(36, QStringLiteral("tool-line"), {},
      QStringLiteral("Line · L · Shift snaps 45° · Size %1 · Wheel")
          .arg(qRound(annotationSize_)));
  add(36, QStringLiteral("tool-freehand"), {},
      QStringLiteral("Freehand · F · Size %1 · Wheel")
          .arg(qRound(annotationSize_)));
  add(36, QStringLiteral("tool-highlighter"), {},
      QStringLiteral("Highlighter · H · Size %1 · Wheel")
          .arg(qRound(annotationSize_)));
  add(36, QStringLiteral("tool-marker"), {},
      QStringLiteral("Number marker · C · Size %1 · Wheel")
          .arg(qRound(annotationSize_)));
  add(36, QStringLiteral("tool-rectangle"), {},
      QStringLiteral("Rectangle · R · Shift makes square"));
  add(36, QStringLiteral("tool-redact"), {},
      QStringLiteral("Redact · D · %1 · D again toggles")
          .arg(redactionStyleName(redactionStyle_)));
  add(36, QStringLiteral("tool-text"), {},
      QStringLiteral("Neucha text · T · %1 · Wheel")
          .arg(QString::fromLatin1(
              kTextSizeNames.at(static_cast<std::size_t>(textSizeIndex_)))));
  add(36, QStringLiteral("palette"), {}, QStringLiteral("Annotation color"),
      annotationColor());
  add(36, QStringLiteral("tool-ocr"), {},
      QStringLiteral("Draw around text to copy · O"));
  add(36, QStringLiteral("background"), {},
      QStringLiteral("Cycle backdrop · B"));
  add(36, QStringLiteral("undo"), {}, QStringLiteral("Undo · Ctrl+Z"));
  add(36, QStringLiteral("redo"), {},
      QStringLiteral("Redo · Ctrl+Shift+Z / Ctrl+Y"));
  add(36, QStringLiteral("pin"), {},
      QStringLiteral("Pin on screen · P · Ctrl+C on the pin copies it"));
  add(36, QStringLiteral("copy"), {}, QStringLiteral("Copy only · Ctrl+C"));
  add(40, QStringLiteral("both"), {}, QStringLiteral("Copy and save · Enter"));
  add(36, QStringLiteral("save"), {}, QStringLiteral("Save only · Ctrl+S"));
  add(36, QStringLiteral("close"), {}, QStringLiteral("Close · Esc twice"));

  if (colorPaletteOpen_) {
    const QRectF palette = colorPaletteRect();
    for (int index = 0; index < 6; ++index) {
      buttons.push_back(
          {{palette.left() + 4 + index * 28, palette.top() + 4, 24, 28},
           QStringLiteral("color-%1").arg(index),
           {},
           QStringLiteral("Color · %1").arg(index + 1),
           QColor(QString::fromLatin1(
               kColorNames.at(static_cast<std::size_t>(index))))});
    }
    buttons.push_back({{palette.left() + 4 + 6 * 28, palette.top() + 4, 24, 28},
                       QStringLiteral("custom-color"),
                       {},
                       QStringLiteral("Custom color"),
                       {}});
    buttons.push_back({{palette.left() + 4 + 7 * 28, palette.top() + 4, 24, 28},
                       QStringLiteral("tool-eyedropper"),
                       {},
                       QStringLiteral("Sample from image · I"),
                       {}});
  }
  return buttons;
}

void CaptureEditor::setStatus(QString status) {
  status_ = std::move(status);
  update();
}

CaptureEditor::EditState CaptureEditor::editState() const {
  return {annotations_, backgroundStyle_, selection_, selectedAnnotation_,
          nextMarker_};
}

void CaptureEditor::applyEditState(const EditState &state) {
  annotations_ = state.annotations;
  backgroundStyle_ = state.backgroundStyle;
  selection_ = state.selection;
  selectedAnnotation_ = std::clamp(state.selectedAnnotation, -1,
                                   static_cast<int>(annotations_.size()) - 1);
  nextMarker_ = state.nextMarker;
  editingAnnotation_ = -1;
  interaction_ = Interaction::None;
  freehandPoints_.clear();
  textEditor_->clear();
  textEditor_->hide();
  setFocus(Qt::OtherFocusReason);
  updatePointerCursor();
  update();
}

void CaptureEditor::cancelActiveDragForHistory() {
  if (dragStartStateValid_)
    applyEditState(dragStartState_);
  dragging_ = false;
  creationConstraintActive_ = false;
  interaction_ = Interaction::None;
  dragStartStateValid_ = false;
  dragChanged_ = false;
  freehandPoints_.clear();
}

void CaptureEditor::pushUndoState(const EditState &state) {
  undoStack_.push_back(state);
  constexpr qsizetype maximumUndoStates = 100;
  while (undoStack_.size() > maximumUndoStates)
    undoStack_.removeFirst();
  redoStack_.clear();
}

void CaptureEditor::recordEdit() { pushUndoState(editState()); }

void CaptureEditor::undoEdit() {
  cancelActiveDragForHistory();
  if (undoStack_.isEmpty()) {
    setStatus(QStringLiteral("Nothing to undo"));
    return;
  }
  redoStack_.push_back(editState());
  applyEditState(undoStack_.takeLast());
  persistSnapshot();
  setStatus(QStringLiteral("Undo · Ctrl+Shift+Z or Ctrl+Y to redo"));
}

void CaptureEditor::redoEdit() {
  cancelActiveDragForHistory();
  if (redoStack_.isEmpty()) {
    setStatus(QStringLiteral("Nothing to redo"));
    return;
  }
  undoStack_.push_back(editState());
  applyEditState(redoStack_.takeLast());
  persistSnapshot();
  setStatus(QStringLiteral("Redo · Ctrl+Z to undo"));
}

void CaptureEditor::persistSnapshot() {
  if (selection_.isEmpty())
    return;
  if (snapshotPath_.isEmpty())
    snapshotPath_ = temporarySnapshotPath();
  const QImage image =
      renderCapture(capture_, selection_, annotations_, backgroundStyle_);
  QString error;
  if (!saveTemporarySnapshot(image, snapshotPath_, error))
    qWarning().noquote() << error;
}

void CaptureEditor::pinSnapshot() {
  if (busy_ || selection_.isEmpty())
    return;

  prunePinnedSnapshots();
  const QString path = pinnedSnapshotPath(++pinCount_);
  if (path.isEmpty()) {
    --pinCount_;
    setStatus(QStringLiteral("Could not create private runtime directory"));
    return;
  }
  const QImage image =
      renderCapture(capture_, selection_, annotations_, backgroundStyle_);
  QString error;
  if (!saveTemporarySnapshot(image, path, error)) {
    setStatus(error);
    --pinCount_;
    return;
  }

  if (!QProcess::startDetached(QCoreApplication::applicationFilePath(),
                               {QStringLiteral("--pin"), path})) {
    QFile::remove(path);
    --pinCount_;
    setStatus(QStringLiteral("Could not start pinned capture"));
    return;
  }

  close();
}

void CaptureEditor::enterEdit(QString status) {
  phase_ = Phase::Edit;
  tool_ = Tool::Select;
  undoStack_.clear();
  redoStack_.clear();
  setStatus(std::move(status));
  updatePointerCursor();
  if (quickOutputMode_ != QuickOutputMode::None) {
    const OutputMode output = quickOutputMode_ == QuickOutputMode::Copy
                                  ? OutputMode::Copy
                                  : quickOutputMode_ == QuickOutputMode::Save
                                        ? OutputMode::Save
                                        : OutputMode::Both;
    finish(output);
    return;
  }
  persistSnapshot();
}

void CaptureEditor::handleEscape() {
  const qint64 closeWindowMs =
      static_cast<qint64>(QApplication::doubleClickInterval()) * 2;
  if (escapeTimer_.isValid() && escapeTimer_.elapsed() <= closeWindowMs) {
    close();
    return;
  }
  escapeTimer_.restart();
  textEditor_->clear();
  textEditor_->hide();
  editingAnnotation_ = -1;
  if (dragStartStateValid_) {
    applyEditState(dragStartState_);
    persistSnapshot();
  }
  dragStartStateValid_ = false;
  dragChanged_ = false;
  creationConstraintActive_ = false;
  dragging_ = false;
  interaction_ = Interaction::None;
  colorPaletteOpen_ = false;
  customColorPickerOpen_ = false;
  freehandPoints_.clear();
  if (phase_ == Phase::Edit) {
    tool_ = Tool::Select;
    setStatus(QStringLiteral("Select/move · Esc again to close"));
  } else {
    windowMode_ = false;
    selection_ = {};
    setStatus(QStringLiteral("Area mode · Esc again to close"));
  }
  setFocus(Qt::OtherFocusReason);
  updatePointerCursor();
  update();
}

void CaptureEditor::chooseWindow(int index) {
  if (index < 0 || index >= capture_.windows.size())
    return;

  const WindowTarget target = capture_.windows.at(index);
  setStatus(QStringLiteral("Capturing clean window surface…"));
  QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
  QImage surface;
  QString surfaceError;
  if (!captureWindowSurface(target, surface, surfaceError)) {
    qWarning().noquote()
        << QStringLiteral("Window capture failed: %1").arg(surfaceError);
    setStatus(QStringLiteral("Window capture failed · %1 · choose another")
                  .arg(surfaceError));
    update();
    return;
  }

  capture_.source = surface;
  capture_.preview = surface.scaled(target.rect.size(), Qt::IgnoreAspectRatio,
                                    Qt::SmoothTransformation);
  selection_ = QRectF(QPointF(), target.rect.size());
  windowMode_ = false;
  enterEdit(QStringLiteral(
      "Window selected · Select moves layers · wheel zooms · outer handles "
      "crop"));
}

void CaptureEditor::beginText(const QPointF &point, int annotationIndex) {
  editingAnnotation_ = annotationIndex;
  QString existingText;
  if (annotationIndex >= 0 && annotationIndex < annotations_.size()) {
    const Annotation &annotation = annotations_.at(annotationIndex);
    textColor_ = annotation.color;
    textSize_ = annotation.size;
    textPoint_ =
        annotation.start -
        QPointF(0, QFontMetricsF(annotationTextFont(textSize_)).ascent());
    existingText = annotation.text;
  } else {
    textPoint_ = point;
    textColor_ = annotationColor();
    textSize_ = kTextSizes.at(static_cast<std::size_t>(textSizeIndex_));
  }

  const QRectF image = editImageRect();
  const qreal scale = editScale();
  const QPointF position = image.topLeft() + textPoint_ * scale;
  QFont displayFont = annotationTextFont(textSize_);
  displayFont.setPixelSize(
      std::max(12, qRound(displayFont.pixelSize() * scale)));
  const QFontMetrics metrics(displayFont);
  textEditor_->setFont(displayFont);
  textEditor_->setStyleSheet(
      QStringLiteral(
          "QLineEdit { color: %1; background: transparent; border: none;"
          " margin: 0; padding: 0; selection-background-color: #0a84ff; }")
          .arg(textColor_.name()));
  textEditor_->setGeometry(qRound(position.x()), qRound(position.y()), 72,
                           metrics.height());
  textEditor_->setText(existingText);
  textEditor_->show();
  textEditor_->raise();
  textEditor_->setFocus(Qt::MouseFocusReason);
  if (!existingText.isEmpty())
    textEditor_->selectAll();
}

void CaptureEditor::acceptText() {
  const QString text = textEditor_->text().trimmed();
  if (!text.isEmpty()) {
    recordEdit();
    Annotation annotation;
    annotation.kind = Annotation::Kind::Text;
    annotation.start =
        textPoint_ +
        QPointF(0, QFontMetricsF(annotationTextFont(textSize_)).ascent());
    annotation.text = text;
    annotation.color = textColor_;
    annotation.size = textSize_;
    if (editingAnnotation_ >= 0 && editingAnnotation_ < annotations_.size()) {
      annotations_[editingAnnotation_] = std::move(annotation);
      selectedAnnotation_ = editingAnnotation_;
      tool_ = Tool::Select;
      setStatus(QStringLiteral("Text updated · drag to move · handle resizes"));
    } else {
      annotations_.push_back(std::move(annotation));
      selectedAnnotation_ = -1;
      setStatus(QStringLiteral("Text added · Esc for select mode"));
    }
    persistSnapshot();
  } else if (editingAnnotation_ >= 0) {
    tool_ = Tool::Select;
  }
  editingAnnotation_ = -1;
  textEditor_->clear();
  textEditor_->hide();
  setFocus(Qt::OtherFocusReason);
  updatePointerCursor();
  update();
}

void CaptureEditor::selectWindowInDirection(int key) {
  int current = hoveredWindow_;
  if (current < 0)
    current = windowAt(cursor_);
  const int next = windowInDirection(current, key);
  if (next < 0)
    return;
  hoveredWindow_ = next;
  setStatus(QStringLiteral("%1 · Super+Arrows choose · Enter captures")
                .arg(capture_.windows.at(next).title));
}

void CaptureEditor::runOcr(const QRectF &localSelection) {
  if (busy_ || selection_.isEmpty())
    return;
  QRectF target = selection_;
  if (!localSelection.isEmpty()) {
    target = QRectF(selection_.topLeft() + localSelection.topLeft(),
                    localSelection.size())
                 .intersected(selection_);
  }
  if (target.width() < 2 || target.height() < 2)
    return;
  busy_ = true;
  setStatus(QStringLiteral("Reading selected text…"));

  QVector<Annotation> ocrAnnotations;
  const QPointF offset = selection_.topLeft() - target.topLeft();
  for (const Annotation &annotation : annotations_) {
    if (annotation.kind != Annotation::Kind::Redaction)
      continue;
    Annotation adjusted = annotation;
    adjusted.start += offset;
    adjusted.end += offset;
    for (QPointF &point : adjusted.points)
      point += offset;
    ocrAnnotations.push_back(std::move(adjusted));
  }

  const QImage image =
      renderCapture(capture_, target, ocrAnnotations, BackgroundStyle::None);
  ocrWatcher_.setFuture(QtConcurrent::run([image] {
    OcrResult result;
    result.text = recognizeText(image, result.error);
    return result;
  }));
}

void CaptureEditor::finish(OutputMode mode) {
  if (busy_ || selection_.isEmpty())
    return;
  busy_ = true;
  setStatus(QStringLiteral("Preparing screenshot…"));
  QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
  persistSnapshot();
  const QFileInfo snapshotFile(snapshotPath_);
  if (snapshotPath_.isEmpty() || !snapshotFile.exists() ||
      snapshotFile.size() <= 0) {
    busy_ = false;
    setStatus(QStringLiteral("Could not prepare screenshot snapshot"));
    return;
  }

  QString error;
  QString saved;
  if (mode == OutputMode::Copy || mode == OutputMode::Both) {
    if (!copyPngFileToClipboard(snapshotPath_, error)) {
      busy_ = false;
      setStatus(error);
      return;
    }
  }
  if (mode == OutputMode::Save || mode == OutputMode::Both) {
    saved = moveSnapshotToScreenshots(snapshotPath_, error);
    if (saved.isEmpty()) {
      busy_ = false;
      setStatus(error);
      return;
    }
    snapshotPath_ = saved;
  }

  if (mode == OutputMode::Copy)
    sendCaptureNotification(QStringLiteral("Screenshot copied to clipboard"));
  else if (mode == OutputMode::Save)
    sendCaptureNotification(QStringLiteral("Screenshot saved"), saved);
  else
    sendCaptureNotification(QStringLiteral("Screenshot saved and copied"),
                            saved);
  close();
}

void CaptureEditor::handleToolbar(const QString &action) {
  if (action == QStringLiteral("tool-select"))
    tool_ = Tool::Select;
  else if (action == QStringLiteral("tool-arrow"))
    tool_ = Tool::Arrow;
  else if (action == QStringLiteral("tool-line"))
    tool_ = Tool::Line;
  else if (action == QStringLiteral("tool-freehand"))
    tool_ = Tool::Freehand;
  else if (action == QStringLiteral("tool-highlighter"))
    tool_ = Tool::Highlighter;
  else if (action == QStringLiteral("tool-marker"))
    tool_ = Tool::Marker;
  else if (action == QStringLiteral("tool-rectangle"))
    tool_ = Tool::Rectangle;
  else if (action == QStringLiteral("tool-spotlight")) {
    if (tool_ == Tool::Spotlight) {
      spotlightShape_ = spotlightShape_ == SpotlightShape::Ellipse
                            ? SpotlightShape::Rectangle
                            : spotlightShape_ == SpotlightShape::Rectangle
                                  ? SpotlightShape::RoundedRectangle
                                  : SpotlightShape::Ellipse;
    } else {
      tool_ = Tool::Spotlight;
    }
    selectedAnnotation_ = -1;
  }
  else if (action == QStringLiteral("tool-redact")) {
    if (tool_ == Tool::Redact) {
      redactionStyle_ = redactionStyle_ == RedactionStyle::Solid
                            ? RedactionStyle::Pixelate
                            : RedactionStyle::Solid;
    } else {
      tool_ = Tool::Redact;
    }
    selectedAnnotation_ = -1;
    setStatus(QStringLiteral("Redact: %1 · drag sensitive content · D toggles")
                  .arg(redactionStyleName(redactionStyle_)));
  } else if (action == QStringLiteral("tool-text"))
    tool_ = Tool::Text;
  else if (action == QStringLiteral("tool-eyedropper")) {
    tool_ = Tool::Eyedropper;
    customColorPickerOpen_ = false;
  }
  else if (action == QStringLiteral("tool-ocr"))
    tool_ = Tool::Ocr;
  else if (action == QStringLiteral("palette"))
    colorPaletteOpen_ = true;
  else if (action.startsWith(QStringLiteral("color-"))) {
    colorIndex_ = std::clamp(action.sliced(6).toInt(), 0, 5);
    usingCustomColor_ = false;
    customColorPickerOpen_ = false;
    if (selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations_.size() &&
        annotations_.at(selectedAnnotation_).kind !=
            Annotation::Kind::Redaction) {
      recordEdit();
      annotations_[selectedAnnotation_].color = annotationColor();
      persistSnapshot();
    }
  } else if (action == QStringLiteral("custom-color")) {
    usingCustomColor_ = true;
    customColorPickerOpen_ = !customColorPickerOpen_;
  } else if (action == QStringLiteral("ocr"))
    runOcr();
  else if (action == QStringLiteral("background")) {
    recordEdit();
    backgroundStyle_ = static_cast<BackgroundStyle>(
        (static_cast<int>(backgroundStyle_) + 1) % 5);
    setStatus(QStringLiteral("Backdrop: %1 · B cycles")
                  .arg(backgroundName(backgroundStyle_)));
    persistSnapshot();
  } else if (action == QStringLiteral("undo")) {
    undoEdit();
  } else if (action == QStringLiteral("redo")) {
    redoEdit();
  } else if (action == QStringLiteral("pin"))
    pinSnapshot();
  else if (action == QStringLiteral("copy"))
    finish(OutputMode::Copy);
  else if (action == QStringLiteral("both"))
    finish(OutputMode::Both);
  else if (action == QStringLiteral("save"))
    finish(OutputMode::Save);
  else if (action == QStringLiteral("close"))
    close();
  updatePointerCursor();
  update();
}

void CaptureEditor::keyPressEvent(QKeyEvent *event) {
  if (event->key() == Qt::Key_Shift && phase_ == Phase::Edit && dragging_ &&
      supportsCreationConstraint(tool_)) {
    creationConstraintActive_ = true;
    event->accept();
    update();
    return;
  }
  if (event->key() == Qt::Key_Escape) {
    handleEscape();
    return;
  }
  if (phase_ == Phase::Select) {
    if (event->matches(QKeySequence::SelectAll)) {
      windowMode_ = false;
      dragging_ = false;
      hoveredWindow_ = -1;
      selection_ = QRectF(QPointF(), capture_.preview.size());
      enterEdit(QStringLiteral(
          "Full screen selected · native resolution · outer handles crop"));
      update();
      return;
    }
    const bool directionalKey =
        event->key() == Qt::Key_Left || event->key() == Qt::Key_Right ||
        event->key() == Qt::Key_Up || event->key() == Qt::Key_Down;
    if (windowMode_ && directionalKey &&
        event->modifiers().testFlag(Qt::MetaModifier)) {
      selectWindowInDirection(event->key());
      event->accept();
      update();
      return;
    }
    if (windowMode_ &&
        (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
      chooseWindow(hoveredWindow_);
      return;
    }
    if (event->key() == Qt::Key_Space) {
      windowMode_ = !windowMode_;
      dragging_ = false;
      selection_ = {};
      hoveredWindow_ = windowMode_ ? windowAt(cursor_) : -1;
      setStatus(windowMode_
                    ? QStringLiteral("Window mode · click or Super+Arrows then "
                                     "Enter · Space returns to area")
                    : QStringLiteral(
                          "Drag to select an area · Space selects a window"));
      updatePointerCursor();
      return;
    }
    QWidget::keyPressEvent(event);
    return;
  }

  const bool redoShortcut = event->matches(QKeySequence::Redo) ||
                            (event->key() == Qt::Key_Y &&
                             event->modifiers().testFlag(Qt::ControlModifier));
  if (redoShortcut) {
    redoEdit();
  } else if (event->matches(QKeySequence::Undo)) {
    undoEdit();
  } else if (event->matches(QKeySequence::Copy)) {
    finish(OutputMode::Copy);
    return;
  } else if (event->matches(QKeySequence::Save)) {
    finish(OutputMode::Save);
    return;
  } else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
    finish(OutputMode::Both);
    return;
  } else if ((event->key() == Qt::Key_Delete ||
              event->key() == Qt::Key_Backspace) &&
             selectedAnnotation_ >= 0 &&
             selectedAnnotation_ < annotations_.size()) {
    recordEdit();
    annotations_.removeAt(selectedAnnotation_);
    selectedAnnotation_ = -1;
    persistSnapshot();
  } else if (event->key() == Qt::Key_V) {
    tool_ = Tool::Select;
  } else if (event->key() == Qt::Key_A) {
    tool_ = Tool::Arrow;
  } else if (event->key() == Qt::Key_L) {
    tool_ = Tool::Line;
  } else if (event->key() == Qt::Key_F) {
    tool_ = Tool::Freehand;
  } else if (event->key() == Qt::Key_H) {
    tool_ = Tool::Highlighter;
  } else if (event->key() == Qt::Key_C || event->key() == Qt::Key_M) {
    tool_ = Tool::Marker;
  } else if (event->key() == Qt::Key_R) {
    tool_ = Tool::Rectangle;
  } else if (event->key() == Qt::Key_S) {
    if (tool_ == Tool::Spotlight) {
      spotlightShape_ = spotlightShape_ == SpotlightShape::Ellipse
                            ? SpotlightShape::Rectangle
                            : spotlightShape_ == SpotlightShape::Rectangle
                                  ? SpotlightShape::RoundedRectangle
                                  : SpotlightShape::Ellipse;
    } else {
      tool_ = Tool::Spotlight;
    }
    selectedAnnotation_ = -1;
  } else if (event->key() == Qt::Key_D) {
    if (selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations_.size() &&
        annotations_.at(selectedAnnotation_).kind ==
            Annotation::Kind::Redaction) {
      recordEdit();
      Annotation &redaction = annotations_[selectedAnnotation_];
      redaction.redactionStyle =
          redaction.redactionStyle == RedactionStyle::Solid
              ? RedactionStyle::Pixelate
              : RedactionStyle::Solid;
      setStatus(QStringLiteral("Selected redaction: %1 · D toggles")
                    .arg(redactionStyleName(redaction.redactionStyle)));
      persistSnapshot();
    } else {
      if (tool_ == Tool::Redact) {
        redactionStyle_ = redactionStyle_ == RedactionStyle::Solid
                              ? RedactionStyle::Pixelate
                              : RedactionStyle::Solid;
      } else {
        tool_ = Tool::Redact;
      }
      selectedAnnotation_ = -1;
      setStatus(
          QStringLiteral("Redact: %1 · drag sensitive content · D toggles")
              .arg(redactionStyleName(redactionStyle_)));
    }
  } else if (event->key() == Qt::Key_T) {
    tool_ = Tool::Text;
  } else if (event->key() == Qt::Key_I) {
    tool_ = Tool::Eyedropper;
  } else if (event->key() == Qt::Key_O) {
    tool_ = Tool::Ocr;
  } else if (event->key() == Qt::Key_P) {
    pinSnapshot();
    return;
  } else if (event->key() == Qt::Key_B) {
    recordEdit();
    backgroundStyle_ = static_cast<BackgroundStyle>(
        (static_cast<int>(backgroundStyle_) + 1) % 5);
    setStatus(QStringLiteral("Backdrop: %1 · B cycles")
                  .arg(backgroundName(backgroundStyle_)));
    persistSnapshot();
  } else if (event->key() >= Qt::Key_1 && event->key() <= Qt::Key_6) {
    colorIndex_ = event->key() - Qt::Key_1;
    usingCustomColor_ = false;
    if (selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations_.size() &&
        annotations_.at(selectedAnnotation_).kind !=
            Annotation::Kind::Redaction) {
      recordEdit();
      annotations_[selectedAnnotation_].color = annotationColor();
      persistSnapshot();
    }
  } else {
    QWidget::keyPressEvent(event);
    return;
  }
  updatePointerCursor();
  update();
}

void CaptureEditor::keyReleaseEvent(QKeyEvent *event) {
  if (event->key() == Qt::Key_Shift && creationConstraintActive_) {
    creationConstraintActive_ = false;
    event->accept();
    update();
    return;
  }
  QWidget::keyReleaseEvent(event);
}

void CaptureEditor::mouseMoveEvent(QMouseEvent *event) {
  cursor_ = event->position();
  if (phase_ == Phase::Select) {
    if (windowMode_)
      hoveredWindow_ = windowAt(cursor_);
    else if (dragging_)
      selection_ = normalizedSelection(dragStart_, cursor_);
  } else {
    if (tool_ == Tool::Select && dragging_ &&
        interaction_ >= Interaction::CropTopLeft) {
      if (cropDragImageRect_.width() <= 0.0 ||
          cropDragImageRect_.height() <= 0.0)
        return;
      const int handle = static_cast<int>(interaction_) -
                         static_cast<int>(Interaction::CropTopLeft);
      const QSizeF previewSize = capture_.preview.size();
      if (previewSize.width() <= 0.0 || previewSize.height() <= 0.0)
        return;
      const qreal sourceX = originalSelection_.left() +
                            (cursor_.x() - cropDragImageRect_.left()) *
                                originalSelection_.width() /
                                cropDragImageRect_.width();
      const qreal sourceY =
          originalSelection_.top() + (cursor_.y() - cropDragImageRect_.top()) *
                                         originalSelection_.height() /
                                         cropDragImageRect_.height();
      constexpr qreal minimumCrop = 16;
      QRectF updated = originalSelection_;
      if (handle == 0 || handle == 6 || handle == 7) {
        const qreal maxLeft = std::clamp(
            originalSelection_.right() - minimumCrop, 0.0, previewSize.width());
        updated.setLeft(std::clamp(sourceX, 0.0, maxLeft));
      }
      if (handle == 2 || handle == 3 || handle == 4) {
        const qreal minRight = std::clamp(
            originalSelection_.left() + minimumCrop, 0.0, previewSize.width());
        updated.setRight(std::clamp(sourceX, minRight, previewSize.width()));
      }
      if (handle == 0 || handle == 1 || handle == 2) {
        const qreal maxTop = std::clamp(
            originalSelection_.bottom() - minimumCrop, 0.0, previewSize.height());
        updated.setTop(std::clamp(sourceY, 0.0, maxTop));
      }
      if (handle == 4 || handle == 5 || handle == 6) {
        const qreal minBottom = std::clamp(
            originalSelection_.top() + minimumCrop, 0.0, previewSize.height());
        updated.setBottom(
            std::clamp(sourceY, minBottom, previewSize.height()));
      }
      const QPointF annotationDelta = selection_.topLeft() - updated.topLeft();
      if (!annotationDelta.isNull()) {
        for (Annotation &annotation : annotations_) {
          annotation.start += annotationDelta;
          if (hasEndpointHandles(annotation.kind))
            annotation.end += annotationDelta;
          if (isStrokeKind(annotation.kind)) {
            for (QPointF &point : annotation.points)
              point += annotationDelta;
            if (!annotation.points.isEmpty()) {
              annotation.start = annotation.points.first();
              annotation.end = annotation.points.last();
            }
          }
        }
      }
      selection_ = updated;
      if (updated != originalSelection_)
        dragChanged_ = true;
    } else if (tool_ == Tool::Select && dragging_ && selectedAnnotation_ >= 0 &&
               selectedAnnotation_ < annotations_.size()) {
      const QPointF point = toAnnotationPoint(cursor_);
      Annotation &annotation = annotations_[selectedAnnotation_];
      annotation = originalAnnotation_;
      if (interaction_ == Interaction::Move) {
        const QPointF delta = point - dragStart_;
        annotation.start += delta;
        if (hasEndpointHandles(annotation.kind))
          annotation.end += delta;
        if (isStrokeKind(annotation.kind)) {
          for (QPointF &strokePoint : annotation.points)
            strokePoint += delta;
        }
      } else if (interaction_ == Interaction::ResizeStart) {
        if (hasEndpointHandles(annotation.kind)) {
          annotation.start =
              annotation.kind == Annotation::Kind::Redaction
                  ? constrainedRedactionEndpoint(point, annotation.end,
                                                 originalAnnotation_.start)
                  : point;
        }
      } else if (interaction_ == Interaction::ResizeEnd) {
        if (hasEndpointHandles(annotation.kind)) {
          annotation.end =
              annotation.kind == Annotation::Kind::Redaction
                  ? constrainedRedactionEndpoint(point, annotation.start,
                                                 originalAnnotation_.end)
                  : point;
        } else if (isStrokeKind(annotation.kind)) {
          const QRectF originalBounds = annotationBounds(originalAnnotation_);
          const qreal scaleX =
              originalBounds.width() > 0
                  ? std::max<qreal>(0.05, (point.x() - originalBounds.left()) /
                                              originalBounds.width())
                  : 1.0;
          const qreal scaleY =
              originalBounds.height() > 0
                  ? std::max<qreal>(0.05, (point.y() - originalBounds.top()) /
                                              originalBounds.height())
                  : 1.0;
          for (int index = 0; index < annotation.points.size(); ++index) {
            const QPointF relative =
                originalAnnotation_.points.at(index) - originalBounds.topLeft();
            annotation.points[index] =
                originalBounds.topLeft() +
                QPointF(relative.x() * scaleX, relative.y() * scaleY);
          }
        } else if (annotation.kind == Annotation::Kind::Marker) {
          annotation.size = std::clamp(
              QLineF(annotation.start, point).length() / 3.0, 2.0, 30.0);
        } else if (annotation.kind == Annotation::Kind::Text) {
          const QRectF originalBounds = annotationBounds(originalAnnotation_);
          const qreal ratio =
              originalBounds.width() > 0
                  ? std::abs(point.x() - originalBounds.left()) /
                        originalBounds.width()
                  : 1.0;
          annotation.size =
              std::clamp(originalAnnotation_.size * ratio, 1.0, 24.0);
          annotation.start.setY(
              originalBounds.top() +
              QFontMetricsF(annotationTextFont(annotation.size)).ascent());
        }
      }
      dragChanged_ = true;
    }
    if ((tool_ == Tool::Freehand || tool_ == Tool::Highlighter) && dragging_) {
      const QPointF point = toAnnotationPoint(cursor_);
      if (freehandPoints_.isEmpty() ||
          QLineF(freehandPoints_.last(), point).length() >= 1.5)
        freehandPoints_.push_back(point);
    }
    bool overPaletteAnchor = false;
    for (const ToolbarButton &button : toolbarButtons()) {
      if (button.action == QStringLiteral("palette") &&
          button.rect.contains(cursor_)) {
        overPaletteAnchor = true;
        break;
      }
    }
    const bool overPalette =
        colorPaletteOpen_ &&
        colorPaletteRect().adjusted(0, -4, 0, 0).contains(cursor_);
    const bool overCustom =
        customColorPickerOpen_ && customColorPanelRect().contains(cursor_);
    colorPaletteOpen_ = overPaletteAnchor || overPalette || overCustom;
  }
  updatePointerCursor();
  update();
}

void CaptureEditor::mouseDoubleClickEvent(QMouseEvent *event) {
  if (phase_ != Phase::Edit || event->button() != Qt::LeftButton ||
      !editImageRect().contains(event->position()))
    return;
  const int index = annotationAt(toAnnotationPoint(event->position()));
  if (index < 0 || annotations_.at(index).kind != Annotation::Kind::Text)
    return;
  selectedAnnotation_ = index;
  tool_ = Tool::Select;
  beginText({}, index);
  event->accept();
  update();
}

void CaptureEditor::mousePressEvent(QMouseEvent *event) {
  if (busy_)
    return;
  if (event->button() == Qt::RightButton) {
    if (phase_ == Phase::Select) {
      if (dragging_) {
        dragging_ = false;
        selection_ = {};
        update();
      }
    } else if (phase_ == Phase::Edit) {
      if (textEditor_->isVisible())
        acceptText();
      if (dragging_) {
        handleEscape();
      } else if (tool_ != Tool::Select || selectedAnnotation_ >= 0) {
        tool_ = Tool::Select;
        selectedAnnotation_ = -1;
        setStatus(QStringLiteral("Select/move · Esc again to close"));
        updatePointerCursor();
        update();
      }
    }
    return;
  }
  if (event->button() != Qt::LeftButton)
    return;
  cursor_ = event->position();
  if (phase_ == Phase::Select) {
    if (windowMode_) {
      chooseWindow(windowAt(cursor_));
      return;
    }
    dragStart_ = cursor_;
    selection_ = {};
    dragging_ = true;
    return;
  }
  if (customColorPickerOpen_) {
    if (customColorPanelRect().contains(cursor_)) {
      applyCustomColor(cursor_);
      return;
    }
    customColorPickerOpen_ = false;
    if (!colorPaletteRect().contains(cursor_)) {
      update();
      return;
    }
  }
  if (tool_ == Tool::Text && !colorPaletteOpen_ && !customColorPickerOpen_ &&
      textSizePanelRect().contains(cursor_)) {
    const qreal localX = cursor_.x() - textSizePanelRect().left();
    textSizeIndex_ = std::clamp(static_cast<int>(localX / 34.0), 0, 2);
    setStatus(QStringLiteral("Neucha · size %1 · wheel changes size")
                  .arg(QString::fromLatin1(kTextSizeNames.at(
                      static_cast<std::size_t>(textSizeIndex_)))));
    update();
    return;
  }

  for (const ToolbarButton &button : toolbarButtons()) {
    if (button.rect.contains(cursor_)) {
      handleToolbar(button.action);
      return;
    }
  }
  if (tool_ == Tool::Select) {
    const int cropHandle = cropHandleAt(cursor_);
    if (cropHandle >= 0) {
      originalSelection_ = selection_;
      cropDragImageRect_ = editImageRect();
      interaction_ = static_cast<Interaction>(
          static_cast<int>(Interaction::CropTopLeft) + cropHandle);
      dragStartState_ = editState();
      selectedAnnotation_ = -1;
      dragStartStateValid_ = true;
      dragChanged_ = false;
      dragging_ = true;
      setStatus(QStringLiteral("Cropping screenshot · drag outer handle"));
      updatePointerCursor();
      update();
      return;
    }
  }
  if (!editImageRect().contains(cursor_))
    return;

  const QPointF point = toAnnotationPoint(cursor_);
  if (tool_ == Tool::Eyedropper) {
    customColor_ = sampleSourceColor(capture_.source, capture_.preview.size(),
                                     selection_, editImageRect(), cursor_);
    usingCustomColor_ = true;
    if (customColor_.hsvHueF() >= 0)
      customHue_ = customColor_.hsvHueF();
    if (selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations_.size() &&
        annotations_.at(selectedAnnotation_).kind !=
            Annotation::Kind::Redaction &&
        annotations_.at(selectedAnnotation_).kind !=
            Annotation::Kind::Spotlight) {
      recordEdit();
      annotations_[selectedAnnotation_].color = customColor_;
      persistSnapshot();
    }
    QString clipboardError;
    static_cast<void>(copyTextToClipboard(
        customColor_.name(QColor::HexRgb).toUpper(), clipboardError));
    tool_ = Tool::Select;
    selectedAnnotation_ = -1;
    setStatus(QStringLiteral("Sampled %1").arg(
        customColor_.name(QColor::HexRgb).toUpper()));
    updatePointerCursor();
    update();
    return;
  }
  if (tool_ == Tool::Select) {
    const qreal tolerance = 9.0 / std::max<qreal>(editScale(), 0.01);
    if (selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations_.size()) {
      const Annotation &selected = annotations_.at(selectedAnnotation_);
      const QRectF bounds = annotationBounds(selected);
      const QPointF first =
          hasEndpointHandles(selected.kind) ? selected.start : bounds.topLeft();
      const QPointF last = hasEndpointHandles(selected.kind)
                               ? selected.end
                               : bounds.bottomRight();
      if (QLineF(point, first).length() <= tolerance &&
          hasEndpointHandles(selected.kind)) {
        interaction_ = Interaction::ResizeStart;
      } else if (QLineF(point, last).length() <= tolerance) {
        interaction_ = Interaction::ResizeEnd;
      } else {
        selectedAnnotation_ = annotationAt(point);
        interaction_ =
            selectedAnnotation_ >= 0 ? Interaction::Move : Interaction::None;
      }
    } else {
      selectedAnnotation_ = annotationAt(point);
      interaction_ =
          selectedAnnotation_ >= 0 ? Interaction::Move : Interaction::None;
    }
    if (selectedAnnotation_ >= 0) {
      originalAnnotation_ = annotations_.at(selectedAnnotation_);
      dragStartState_ = editState();
      dragStartStateValid_ = true;
      dragChanged_ = false;
      dragStart_ = point;
      dragging_ = true;
      setStatus(QStringLiteral("Vector layer selected · drag to move · handles "
                               "resize · double-click text"));
    } else {
      dragStartStateValid_ = false;
      dragChanged_ = false;
      dragging_ = false;
      setStatus(QStringLiteral("No layer selected"));
    }
    updatePointerCursor();
    update();
    return;
  }
  if (tool_ == Tool::Marker) {
    recordEdit();
    Annotation annotation;
    annotation.kind = Annotation::Kind::Marker;
    annotation.start = point;
    annotation.number = nextMarker_++;
    annotation.color = annotationColor();
    annotation.size = annotationSize_;
    annotations_.push_back(std::move(annotation));
    selectedAnnotation_ = -1;
    setStatus(QStringLiteral("Marker %1 added · Esc for select mode")
                  .arg(annotation.number));
    persistSnapshot();
    updatePointerCursor();
  } else if (tool_ == Tool::Text) {
    beginText(point);
  } else {
    dragStart_ = point;
    dragging_ = true;
    creationConstraintActive_ = supportsCreationConstraint(tool_) &&
                                event->modifiers().testFlag(Qt::ShiftModifier);
    if (tool_ == Tool::Redact) {
      activeRedactionSeed_ = QRandomGenerator::system()->generate();
      if (activeRedactionSeed_ == 0)
        activeRedactionSeed_ = 1;
    }
    if (tool_ == Tool::Freehand || tool_ == Tool::Highlighter) {
      freehandPoints_.clear();
      freehandPoints_.reserve(256);
      freehandPoints_.push_back(point);
    }
  }
  update();
}

void CaptureEditor::mouseReleaseEvent(QMouseEvent *event) {
  if (event->button() != Qt::LeftButton || !dragging_)
    return;
  if (phase_ == Phase::Select) {
    selection_ = normalizedSelection(dragStart_, event->position());
    dragging_ = false;
    if (selection_.width() >= 2 && selection_.height() >= 2)
      enterEdit(QStringLiteral("Area selected · Select moves layers · wheel "
                               "zooms · outer handles crop"));
    updatePointerCursor();
    update();
    return;
  }
  if (tool_ == Tool::Select) {
    const bool cropped = interaction_ >= Interaction::CropTopLeft;
    const bool changed = dragStartStateValid_ && dragChanged_;
    if (changed)
      pushUndoState(dragStartState_);
    dragStartStateValid_ = false;
    dragChanged_ = false;
    dragging_ = false;
    interaction_ = Interaction::None;
    if (cropped)
      setStatus(QStringLiteral(
          "Crop updated · Select moves layers · wheel zooms selected layer"));
    if (changed)
      persistSnapshot();
    updatePointerCursor();
    update();
    return;
  }

  const QPointF rawEnd = toAnnotationPoint(event->position());
  const QPointF end = creationConstraintActive_
                          ? constrainedCreationEndpoint(tool_, dragStart_, rawEnd)
                          : rawEnd;
  creationConstraintActive_ = false;
  if (tool_ == Tool::Freehand || tool_ == Tool::Highlighter) {
    if (freehandPoints_.isEmpty() ||
        QLineF(freehandPoints_.last(), end).length() >= 1.0)
      freehandPoints_.push_back(end);
    qreal length = 0;
    for (int index = 1; index < freehandPoints_.size(); ++index)
      length += QLineF(freehandPoints_.at(index - 1), freehandPoints_.at(index))
                    .length();
    if (length > 4) {
      recordEdit();
      const bool highlighter = tool_ == Tool::Highlighter;
      Annotation annotation;
      annotation.kind = highlighter ? Annotation::Kind::Highlighter
                                    : Annotation::Kind::Freehand;
      annotation.start = freehandPoints_.first();
      annotation.end = freehandPoints_.last();
      annotation.color = annotationColor();
      annotation.size = annotationSize_;
      annotation.points = std::move(freehandPoints_);
      annotations_.push_back(std::move(annotation));
      selectedAnnotation_ = -1;
      setStatus(
          highlighter
              ? QStringLiteral("Highlight added · Esc for select mode")
              : QStringLiteral("Stroke added · Esc for select mode"));
      persistSnapshot();
    }
    freehandPoints_.clear();
    dragging_ = false;
    updatePointerCursor();
    update();
    return;
  }
  if (tool_ == Tool::Ocr) {
    const QRectF ocrRect(dragStart_, end);
    dragging_ = false;
    if (ocrRect.normalized().width() > 4 && ocrRect.normalized().height() > 4) {
      runOcr(ocrRect.normalized());
      tool_ = Tool::Select;
      updatePointerCursor();
    }
    update();
    return;
  }
  const QRectF draggedRect(dragStart_, end);
  const bool validRedaction =
      tool_ != Tool::Redact ||
      (draggedRect.normalized().width() >= kMinimumRedactionExtent &&
       draggedRect.normalized().height() >= kMinimumRedactionExtent);
  if (QLineF(dragStart_, end).length() > 4 && validRedaction) {
    recordEdit();
    Annotation annotation;
    if (tool_ == Tool::Redact) {
      annotation.kind = Annotation::Kind::Redaction;
      annotation.redactionStyle = redactionStyle_;
      annotation.redactionSeed = activeRedactionSeed_;
    } else if (tool_ == Tool::Spotlight) {
      annotation.kind = Annotation::Kind::Spotlight;
      annotation.magnification = spotlightMagnification_;
      annotation.spotlightShape = spotlightShape_;
    } else {
      annotation.kind = tool_ == Tool::Rectangle
                            ? Annotation::Kind::Rectangle
                            : (tool_ == Tool::Line ? Annotation::Kind::Line
                                                   : Annotation::Kind::Arrow);
    }
    annotation.start = dragStart_;
    annotation.end = end;
    annotation.color = annotationColor();
    annotation.size = annotationSize_;
    annotations_.push_back(std::move(annotation));
    selectedAnnotation_ = -1;
    const bool redacted = tool_ == Tool::Redact;
    setStatus(redacted
                  ? QStringLiteral("%1 redaction added · Esc for select mode")
                        .arg(redactionStyleName(redactionStyle_))
                  : QStringLiteral("Layer added · Esc for select mode"));
    persistSnapshot();
    updatePointerCursor();
  } else if (tool_ == Tool::Redact) {
    setStatus(QStringLiteral(
        "Redaction needs both width and height · drag a rectangle"));
  }
  dragging_ = false;
  update();
}

void CaptureEditor::wheelEvent(QWheelEvent *event) {
  if (phase_ != Phase::Edit) {
    QWidget::wheelEvent(event);
    return;
  }
  const int step = event->angleDelta().y() > 0 ? 1 : -1;
  if (tool_ == Tool::Select && selectedAnnotation_ >= 0 &&
      selectedAnnotation_ < annotations_.size()) {
    scaleSelectedAnnotation(step > 0 ? 1.1 : 1.0 / 1.1);
  } else if (tool_ == Tool::Text) {
    textSizeIndex_ = std::clamp(textSizeIndex_ + step, 0, 2);
    setStatus(QStringLiteral("Neucha · size %1 · wheel changes size")
                  .arg(QString::fromLatin1(kTextSizeNames.at(
                      static_cast<std::size_t>(textSizeIndex_)))));
  } else if (tool_ == Tool::Spotlight) {
    spotlightMagnification_ =
        std::clamp(spotlightMagnification_ + (step > 0 ? 0.25 : -0.25), 1.0,
                   4.0);
    setStatus(QStringLiteral("Spotlight magnification · %1× · wheel adjusts")
                  .arg(spotlightMagnification_, 0, 'f', 1));
  } else if (tool_ == Tool::Arrow || tool_ == Tool::Line ||
             tool_ == Tool::Freehand || tool_ == Tool::Highlighter ||
             tool_ == Tool::Marker) {
    annotationSize_ = std::clamp(annotationSize_ + step, 2.0, 12.0);
    setStatus(QStringLiteral("Size %1 · mouse wheel changes size")
                  .arg(qRound(annotationSize_)));
  } else {
    QWidget::wheelEvent(event);
    return;
  }
  event->accept();
  update();
}

void CaptureEditor::updatePointerCursor() {
  if (phase_ == Phase::Select) {
    setCursor(windowMode_ ? Qt::PointingHandCursor : Qt::CrossCursor);
    return;
  }
  if ((colorPaletteOpen_ && colorPaletteRect().contains(cursor_)) ||
      (customColorPickerOpen_ && customColorPanelRect().contains(cursor_))) {
    setCursor(Qt::PointingHandCursor);
    return;
  }
  if (tool_ == Tool::Text && !colorPaletteOpen_ && !customColorPickerOpen_ &&
      textSizePanelRect().contains(cursor_)) {
    setCursor(Qt::PointingHandCursor);
    return;
  }
  for (const ToolbarButton &button : toolbarButtons()) {
    if (button.rect.contains(cursor_)) {
      setCursor(Qt::PointingHandCursor);
      return;
    }
  }
  if (tool_ == Tool::Select) {
    int cropHandle = cropHandleAt(cursor_);
    if (dragging_ && interaction_ >= Interaction::CropTopLeft) {
      cropHandle = static_cast<int>(interaction_) -
                   static_cast<int>(Interaction::CropTopLeft);
    }
    if (cropHandle == 0 || cropHandle == 4)
      setCursor(Qt::SizeFDiagCursor);
    else if (cropHandle == 2 || cropHandle == 6)
      setCursor(Qt::SizeBDiagCursor);
    else if (cropHandle == 1 || cropHandle == 5)
      setCursor(Qt::SizeVerCursor);
    else if (cropHandle == 3 || cropHandle == 7)
      setCursor(Qt::SizeHorCursor);
    else
      setCursor(Qt::ArrowCursor);
  } else if (tool_ == Tool::Marker)
    setCursor(Qt::PointingHandCursor);
  else if (tool_ == Tool::Text)
    setCursor(Qt::IBeamCursor);
  else
    setCursor(Qt::CrossCursor);
}

void CaptureEditor::paintSelect(QPainter &painter) {
  painter.drawImage(rect(), capture_.source);
  painter.fillRect(rect(), QColor(0, 0, 0, 143));

  if (windowMode_) {
    if (hoveredWindow_ >= 0 && hoveredWindow_ < capture_.windows.size()) {
      const QRect window = capture_.windows.at(hoveredWindow_).rect;
      painter.drawImage(window, capture_.source, sourceRect(window));
    }
    for (int index = 0; index < capture_.windows.size(); ++index) {
      const WindowTarget &window = capture_.windows.at(index);
      painter.setPen(QPen(
          index == hoveredWindow_ ? Qt::white : QColor(255, 255, 255, 72), 2));
      painter.setBrush(Qt::NoBrush);
      painter.drawRect(window.rect);
    }
  } else if (!selection_.isEmpty()) {
    painter.drawImage(selection_, capture_.source, sourceRect(selection_));
    painter.setPen(QPen(Qt::white, 2));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(selection_);
  }

  if (!windowMode_ && !dragging_) {
    painter.setPen(QPen(QColor(255, 255, 255, 56), 1));
    painter.drawLine(QPointF(cursor_.x(), 0), QPointF(cursor_.x(), height()));
    painter.drawLine(QPointF(0, cursor_.y()), QPointF(width(), cursor_.y()));
  }

  QFont badgeFont(QStringLiteral("Noto Sans"));
  badgeFont.setBold(true);
  badgeFont.setPixelSize(11);
  painter.setFont(badgeFont);
  const QString badge =
      windowMode_ ? QStringLiteral("WINDOW  ×") : QStringLiteral("AREA  ×");
  const int badgeWidth = painter.fontMetrics().horizontalAdvance(badge) + 24;
  const QRectF badgeRect((width() - badgeWidth) / 2.0, 12, badgeWidth, 32);
  painter.setPen(QPen(QColor(255, 255, 255, 32), 1));
  painter.setBrush(QColor(18, 18, 22, 235));
  painter.drawRoundedRect(badgeRect, 10, 10);
  painter.setPen(windowMode_ ? QColor(QStringLiteral("#ffd60a"))
                             : QColor(QStringLiteral("#30d158")));
  painter.drawText(badgeRect, Qt::AlignCenter, badge);
  drawHotkeyLegend(painter, rect(), cursor_,
                   {{QStringLiteral("Drag"), QStringLiteral("Area")},
                    {QStringLiteral("Space"), QStringLiteral("Window")},
                    {QStringLiteral("Ctrl+A"), QStringLiteral("Fullscreen")},
                    {QStringLiteral("Esc ×2"), QStringLiteral("Close")}});
  drawStatusPill(painter, rect(), status_);
}

void CaptureEditor::paintEdit(QPainter &painter) {
  painter.setCompositionMode(QPainter::CompositionMode_Source);
  painter.fillRect(rect(), QColor(0, 0, 0, 160));
  painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
  const QRectF image = editImageRect();
  const bool hasBackground = backgroundStyle_ != BackgroundStyle::None;
  if (hasBackground) {
    const QRectF backing = image.adjusted(-28, -28, 28, 28);
    painter.setPen(Qt::NoPen);
    for (int layer = 20; layer > 0; --layer) {
      const qreal spread = layer * 0.7;
      painter.setBrush(QColor(0, 0, 0, 3 + (20 - layer) / 3));
      painter.drawRoundedRect(
          image.adjusted(-spread, -spread + 12, spread, spread + 12),
          15 + spread, 15 + spread);
    }
    for (int layer = 10; layer > 0; --layer) {
      const qreal spread = layer * 0.45;
      painter.setBrush(QColor(0, 0, 0, 5 + (10 - layer) * 2));
      painter.drawRoundedRect(
          image.adjusted(-spread, -spread + 7, spread, spread + 7), 12 + spread,
          12 + spread);
    }
    paintCaptureBackground(painter, backing, backgroundStyle_);
  }

  QPainterPath clip;
  clip.addRoundedRect(image, hasBackground ? 10 : 0, hasBackground ? 10 : 0);
  QVector<Annotation> redactions;
  for (const Annotation &annotation : annotations_) {
    if (annotation.kind == Annotation::Kind::Redaction)
      redactions.push_back(annotation);
  }
  if (dragging_ && tool_ == Tool::Redact) {
    Annotation preview;
    preview.kind = Annotation::Kind::Redaction;
    preview.start = dragStart_;
    preview.end = toAnnotationPoint(cursor_);
    preview.redactionStyle = redactionStyle_;
    preview.redactionSeed = activeRedactionSeed_;
    redactions.push_back(std::move(preview));
  }
  painter.save();
  painter.setClipPath(clip);
  if (redactions.isEmpty()) {
    painter.drawImage(image, capture_.source, sourceRect(selection_));
  } else {
    if (redactionPreviewCache_.isNull() ||
        cachedRedactionSelection_ != selection_ ||
        cachedPreviewRedactions_ != redactions) {
      cachedRedactionSelection_ = selection_;
      cachedPreviewRedactions_ = redactions;
      redactionPreviewCache_ = renderCapture(capture_, selection_, redactions,
                                             BackgroundStyle::None);
    }
    painter.drawImage(image, redactionPreviewCache_);
  }
  painter.restore();

  painter.save();
  painter.translate(image.topLeft());
  painter.scale(editScale(), editScale());
  painter.save();
  painter.setClipRect(QRectF(QPointF(), selection_.size()));
  QVector<Annotation> liveAnnotations = annotations_;
  if (dragging_ && tool_ == Tool::Spotlight) {
    Annotation preview;
    preview.kind = Annotation::Kind::Spotlight;
    preview.start = dragStart_;
    const QPointF end = toAnnotationPoint(cursor_);
    preview.end = creationConstraintActive_
                      ? constrainedCreationEndpoint(tool_, dragStart_, end)
                      : end;
    preview.magnification = spotlightMagnification_;
    preview.spotlightShape = spotlightShape_;
    preview.color = annotationColor();
    preview.size = annotationSize_;
    liveAnnotations.push_back(std::move(preview));
  }
  // Spotlights magnify what they sample, so a lens over a redaction must read
  // the already redacted preview instead of the untouched source pixels.
  const bool sampleRedacted =
      !redactions.isEmpty() && !redactionPreviewCache_.isNull();
  paintSpotlights(painter,
                  sampleRedacted ? redactionPreviewCache_ : capture_.source,
                  QRectF(QPointF(), selection_.size()),
                  sampleRedacted ? QRectF(redactionPreviewCache_.rect())
                                 : sourceRect(selection_),
                  liveAnnotations);
  for (int index = 0; index < annotations_.size(); ++index) {
    if (index != editingAnnotation_ &&
        annotations_.at(index).kind != Annotation::Kind::Redaction)
      paintAnnotation(painter, annotations_.at(index));
  }
  if (dragging_ && tool_ != Tool::Select && tool_ != Tool::Redact) {
    Annotation preview;
    if (tool_ == Tool::Freehand || tool_ == Tool::Highlighter) {
      preview.kind = tool_ == Tool::Highlighter ? Annotation::Kind::Highlighter
                                                : Annotation::Kind::Freehand;
      preview.points = freehandPoints_;
    } else if (tool_ == Tool::Spotlight) {
      preview.kind = Annotation::Kind::Spotlight;
      preview.start = dragStart_;
      const QPointF end = toAnnotationPoint(cursor_);
      preview.end = creationConstraintActive_
                        ? constrainedCreationEndpoint(tool_, dragStart_, end)
                        : end;
      preview.magnification = spotlightMagnification_;
      preview.spotlightShape = spotlightShape_;
    } else {
      preview.kind = (tool_ == Tool::Rectangle || tool_ == Tool::Ocr)
                         ? Annotation::Kind::Rectangle
                         : (tool_ == Tool::Line ? Annotation::Kind::Line
                                                : Annotation::Kind::Arrow);
      preview.start = dragStart_;
      const QPointF end = toAnnotationPoint(cursor_);
      preview.end = creationConstraintActive_
                        ? constrainedCreationEndpoint(tool_, dragStart_, end)
                        : end;
    }
    preview.color = tool_ == Tool::Ocr ? QColor(Qt::white) : annotationColor();
    preview.size = tool_ == Tool::Ocr ? 2.0 : annotationSize_;
    paintAnnotation(painter, preview);
  } else if (tool_ == Tool::Marker && image.contains(cursor_)) {
    Annotation preview;
    preview.kind = Annotation::Kind::Marker;
    preview.start = toAnnotationPoint(cursor_);
    preview.number = nextMarker_;
    preview.color = annotationColor();
    preview.color.setAlpha(185);
    preview.size = annotationSize_;
    paintAnnotation(painter, preview);
  }
  painter.restore();
  if (selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations_.size() &&
      selectedAnnotation_ != editingAnnotation_) {
    const Annotation &selected = annotations_.at(selectedAnnotation_);
    const QRectF bounds = annotationBounds(selected).adjusted(-4, -4, 4, 4);
    const qreal scale = std::max<qreal>(editScale(), 0.01);
    if (showsSelectionBounds(selected.kind)) {
      painter.setPen(
          QPen(QColor(255, 255, 255, 220), 1.0 / scale, Qt::DashLine));
      painter.setBrush(Qt::NoBrush);
      painter.drawRect(bounds);
    }
    const qreal radius = 5.0 / scale;
    painter.setPen(QPen(Qt::white, 1.0 / scale));
    painter.setBrush(QColor(QStringLiteral("#0a84ff")));
    if (hasEndpointHandles(selected.kind))
      painter.drawEllipse(selected.start, radius, radius);
    const QPointF last =
        hasEndpointHandles(selected.kind) ? selected.end : bounds.bottomRight();
    painter.drawEllipse(last, radius, radius);
  }
  painter.restore();

  if (tool_ == Tool::Select) {
    painter.setPen(QPen(QColor(QStringLiteral("#0a84ff")), 1, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(image.adjusted(-1, -1, 1, 1));
    painter.setPen(QPen(QColor(QStringLiteral("#0a84ff")), 2));
    painter.setBrush(QColor(QStringLiteral("#f5f5f7")));
    for (const QRectF &handle : cropHandleRects())
      painter.drawRoundedRect(handle, 3, 3);
  }

  if (colorPaletteOpen_) {
    painter.setPen(QPen(QColor(255, 255, 255, 34), 1));
    painter.setBrush(QColor(22, 22, 28, 248));
    painter.drawRoundedRect(colorPaletteRect(), 9, 9);
  }
  if (customColorPickerOpen_) {
    const QRectF panel = customColorPanelRect();
    const QRectF field = panel.adjusted(12, 12, -36, -12);
    const QRectF hue(panel.right() - 26, panel.top() + 12, 14,
                     panel.height() - 24);
    painter.setPen(QPen(QColor(255, 255, 255, 38), 1));
    painter.setBrush(QColor(20, 20, 25, 250));
    painter.drawRoundedRect(panel, 10, 10);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor::fromHsvF(customHue_, 1.0, 1.0));
    painter.drawRoundedRect(field, 5, 5);
    QLinearGradient white(field.topLeft(), field.topRight());
    white.setColorAt(0, Qt::white);
    white.setColorAt(1, QColor(255, 255, 255, 0));
    painter.setBrush(white);
    painter.drawRoundedRect(field, 5, 5);
    QLinearGradient black(field.topLeft(), field.bottomLeft());
    black.setColorAt(0, QColor(0, 0, 0, 0));
    black.setColorAt(1, Qt::black);
    painter.setBrush(black);
    painter.drawRoundedRect(field, 5, 5);

    QLinearGradient hues(hue.topLeft(), hue.bottomLeft());
    hues.setColorAt(0.0, QColor::fromHsvF(0.0, 1, 1));
    hues.setColorAt(1.0 / 6.0, QColor::fromHsvF(1.0 / 6.0, 1, 1));
    hues.setColorAt(2.0 / 6.0, QColor::fromHsvF(2.0 / 6.0, 1, 1));
    hues.setColorAt(3.0 / 6.0, QColor::fromHsvF(3.0 / 6.0, 1, 1));
    hues.setColorAt(4.0 / 6.0, QColor::fromHsvF(4.0 / 6.0, 1, 1));
    hues.setColorAt(5.0 / 6.0, QColor::fromHsvF(5.0 / 6.0, 1, 1));
    hues.setColorAt(1.0, QColor::fromHsvF(1.0, 1, 1));
    painter.setBrush(hues);
    painter.drawRoundedRect(hue, 4, 4);

    const QPointF fieldMarker(
        field.left() + customColor_.hsvSaturationF() * field.width(),
        field.bottom() - customColor_.valueF() * field.height());
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(Qt::white, 2));
    painter.drawEllipse(fieldMarker, 5, 5);
    const qreal hueY = hue.top() + customHue_ * hue.height();
    painter.drawLine(QPointF(hue.left() - 2, hueY),
                     QPointF(hue.right() + 2, hueY));
  }

  const QString currentTool = toolAction(tool_);
  QFont buttonFont = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
  buttonFont.setPixelSize(11);
  buttonFont.setBold(true);
  painter.setFont(buttonFont);
  const QVector<ToolbarButton> buttons = toolbarButtons();
  const ToolbarButton *hoveredButton = nullptr;
  for (const ToolbarButton &button : buttons) {
    const bool selected =
        button.action == currentTool ||
        (button.action == QStringLiteral("background") && hasBackground) ||
        (button.action == QStringLiteral("palette") && colorPaletteOpen_) ||
        (button.action == QStringLiteral("custom-color") &&
         usingCustomColor_) ||
        (!usingCustomColor_ &&
         button.action == QStringLiteral("color-%1").arg(colorIndex_));
    const bool hovered = button.rect.contains(cursor_);
    if (hovered)
      hoveredButton = &button;
    painter.setPen(QPen(QColor(255, 255, 255, selected ? 64 : 26), 1));
    painter.setBrush(selected ? QColor(66, 66, 75, 250)
                              : (hovered ? QColor(48, 48, 56, 248)
                                         : QColor(34, 34, 40, 244)));
    if (button.action == QStringLiteral("both"))
      painter.setBrush(QColor(QStringLiteral("#0a84ff")));
    painter.drawRoundedRect(button.rect, 8, 8);
    if (button.color.isValid()) {
      const QPointF center = button.rect.center();
      painter.setPen(QPen(selected ? Qt::white : QColor(255, 255, 255, 80),
                          selected ? 2 : 1));
      painter.setBrush(button.color);
      painter.drawEllipse(center, 7, 7);
    } else {
      drawToolbarIcon(painter, button.rect, button.action, button.label,
                      QColor(245, 245, 247));
    }
  }
  if (tool_ == Tool::Text && !colorPaletteOpen_ && !customColorPickerOpen_) {
    const QRectF panel = textSizePanelRect();
    painter.setPen(QPen(QColor(255, 255, 255, 34), 1));
    painter.setBrush(QColor(22, 22, 28, 248));
    painter.drawRoundedRect(panel, 9, 9);
    QFont sizeFont = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    sizeFont.setPixelSize(11);
    sizeFont.setBold(true);
    painter.setFont(sizeFont);
    for (int index = 0; index < 3; ++index) {
      const QRectF item(panel.left() + index * 34, panel.top(), 34,
                        panel.height());
      if (index == textSizeIndex_) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(QStringLiteral("#0a84ff")));
        painter.drawRoundedRect(item.adjusted(3, 3, -3, -3), 7, 7);
      }
      painter.setPen(QColor(QStringLiteral("#f5f5f7")));
      painter.drawText(item, Qt::AlignCenter,
                       QString::fromLatin1(
                           kTextSizeNames.at(static_cast<std::size_t>(index))));
    }
  }
  drawStatusPill(painter, rect(), status_);
  drawHotkeyLegend(
      painter, rect(), cursor_,
      {{QStringLiteral("V"), QStringLiteral("Select / move layer")},
       {QStringLiteral("A"), QStringLiteral("Arrow")},
       {QStringLiteral("L"), QStringLiteral("Line")},
       {QStringLiteral("F / H"), QStringLiteral("Freehand / Highlighter")},
       {QStringLiteral("C"), QStringLiteral("Marker")},
       {QStringLiteral("R / D"), QStringLiteral("Rectangle / Redact")},
       {QStringLiteral("T"), QStringLiteral("Text")},
       {QStringLiteral("Double click"), QStringLiteral("Edit text layer")},
       {QStringLiteral("1–6"), QStringLiteral("Color")},
       {QStringLiteral("Wheel"), QStringLiteral("Zoom selected / tool size")},
       {QStringLiteral("O"), QStringLiteral("Select OCR text")},
       {QStringLiteral("B / P"), QStringLiteral("Backdrop / Pin on screen")},
       {QStringLiteral("Ctrl+Z"), QStringLiteral("Undo")},
       {QStringLiteral("Ctrl+Shift+Z"), QStringLiteral("Redo")},
       {QStringLiteral("Enter"), QStringLiteral("Copy + save")},
       {QStringLiteral("Ctrl+C"), QStringLiteral("Copy only")},
       {QStringLiteral("Ctrl+S"), QStringLiteral("Save only")},
       {QStringLiteral("Esc"), QStringLiteral("Arrow / twice close")}});
  if (hoveredButton) {
    drawInstantTooltip(painter, rect(), hoveredButton->rect,
                       hoveredButton->tooltip);
  } else if (tool_ == Tool::Arrow || tool_ == Tool::Line ||
             tool_ == Tool::Freehand || tool_ == Tool::Highlighter ||
             tool_ == Tool::Marker || tool_ == Tool::Redact ||
             tool_ == Tool::Text) {
    const QString selectedAction = toolAction(tool_);
    for (const ToolbarButton &button : buttons) {
      if (button.action == selectedAction) {
        QString tooltip;
        if (tool_ == Tool::Text) {
          tooltip =
              QStringLiteral("Neucha · S  M  L · current %1 · Scroll wheel")
                  .arg(QString::fromLatin1(kTextSizeNames.at(
                      static_cast<std::size_t>(textSizeIndex_))));
        } else if (tool_ == Tool::Redact) {
          tooltip = QStringLiteral("Redact · %1 · D toggles style")
                        .arg(redactionStyleName(redactionStyle_));
        } else {
          tooltip = QStringLiteral("Size %1 · Scroll wheel")
                        .arg(qRound(annotationSize_));
        }
        drawInstantTooltip(painter, rect(), button.rect, tooltip);
        break;
      }
    }
  }
}

void CaptureEditor::paintEvent(QPaintEvent *event) {
  Q_UNUSED(event)
  QPainter painter(this);
  painter.setRenderHints(QPainter::Antialiasing |
                         QPainter::SmoothPixmapTransform |
                         QPainter::TextAntialiasing);
  if (phase_ == Phase::Select)
    paintSelect(painter);
  else
    paintEdit(painter);
}
