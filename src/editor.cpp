/** @fileoverview Handles screenshot selection, annotation, and editor drawing.
 */
#include "editor.hpp"
#include "icons.hpp"
#include "eyedropper.hpp"
#include "palette-config.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDebug>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QProxyStyle>
#include <QRandomGenerator>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QTextDocument>
#include <QThread>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <numbers>

/// The inline text editor: a multiline editor whose native caret is hidden (the
/// editor paints a shorter one over Neucha's tall line box) and whose caret
/// rectangle is exposed for that.
class InlineTextEdit final : public QPlainTextEdit {
public:
  explicit InlineTextEdit(QWidget *parent) : QPlainTextEdit(parent) {
    setStyle(&caretlessStyle_);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setWordWrapMode(QTextOption::NoWrap);
    document()->setDocumentMargin(0);
  }
  using QPlainTextEdit::cursorRect;
  using QPlainTextEdit::setViewportMargins;

private:
  class CaretlessStyle final : public QProxyStyle {
  public:
    int pixelMetric(PixelMetric metric, const QStyleOption *option,
                    const QWidget *widget) const override {
      return metric == PM_TextCursorWidth
                 ? 0
                 : QProxyStyle::pixelMetric(metric, option, widget);
    }
  };
  CaretlessStyle caretlessStyle_;
};

namespace {
constexpr std::array<qreal, 3> kTextSizes{2.0, 5.0, 9.0};
constexpr std::array<const char *, 3> kTextSizeNames{"S", "M", "L"};
constexpr qreal kToolbarWidth = 840;
constexpr qreal kToolbarImageGap = 46;
constexpr qreal kMinimumRedactionExtent = 5.0;
constexpr int kBackdropDim = 143;
constexpr qreal kNudgeStep = 1.0;
constexpr qreal kNudgeStepShift = 10.0;

/** Keeps a downward submenu alive while the pointer travels toward it. */
bool inDownwardSubmenuTriangle(const QPointF &origin, const QRectF &submenu,
                               const QPointF &pointer) {
  if (origin.isNull() || pointer.y() < origin.y() ||
      pointer.y() > submenu.bottom() + 6.0)
    return false;
  QPolygonF triangle;
  triangle << origin << submenu.bottomLeft() + QPointF(-8, 6)
           << submenu.bottomRight() + QPointF(8, 6);
  return triangle.containsPoint(pointer, Qt::OddEvenFill);
}
/// Arrow presses closer together than this share one undo entry, so a held
/// key reads as one move.
constexpr qint64 kNudgeCoalesceMs = 100;
/// How long after the last wheel notch the selection chrome stays faint. Long
/// enough to cover a run of notches, short enough that it is back before the
/// hand has left the wheel.
constexpr int kAdjustSettleMs = 400;

qreal toolbarScale(qreal availableWidth) {
  constexpr qreal sideMargins = 16.0;
  return std::min<qreal>(
      1.0,
      std::max<qreal>(0.1, (availableWidth - sideMargins) / kToolbarWidth));
}
// A spotlight at 1x is not a failed zoom, it is a plain highlight: the dimming
// still isolates the region. Name that state so it reads as somewhere to stop
// rather than the bottom of a range.
/// The whole state of a spotlight in one line. Adjusting one of the three
/// settings still reports all three: they interact (a ring reads differently
/// at 3× than at 1×), and the two you are not touching should not have to be
/// remembered or re-discovered by pressing keys to find out.
QString spotlightStatus(SpotlightShape shape, qreal magnification,
                        qreal border) {
  const QString shapeName = shape == SpotlightShape::Ellipse
                                ? QStringLiteral("ellipse")
                            : shape == SpotlightShape::Rectangle
                                ? QStringLiteral("rectangle")
                                : QStringLiteral("rounded");
  const QString zoom =
      magnification <= 1.0
          ? QStringLiteral("no zoom")
          : QStringLiteral("%1×").arg(magnification, 0, 'f', 1);
  const QString ring = border <= 0.0
                           ? QStringLiteral("no border")
                           : QStringLiteral("border %1").arg(qRound(border));
  return QStringLiteral("Spotlight · %1 · %2 · %3 · S cycles shape · wheel "
                        "zooms · Alt+wheel border")
      .arg(shapeName, zoom, ring);
}

} // namespace

QString spotlightStatusForTest(SpotlightShape shape, qreal magnification,
                               qreal border) {
  return spotlightStatus(shape, magnification, border);
}

namespace {
bool hasEndpointHandles(Annotation::Kind kind) {
  return kind == Annotation::Kind::Arrow || kind == Annotation::Kind::Line ||
         kind == Annotation::Kind::Rectangle ||
         kind == Annotation::Kind::Ellipse ||
         kind == Annotation::Kind::Redaction ||
         kind == Annotation::Kind::Spotlight;
}

/// Any handle drag on a layer (as opposed to a move, or a capture crop).
bool isLayerResize(CaptureEditor::Interaction interaction) {
  return interaction >= CaptureEditor::Interaction::ResizeStart &&
         interaction <= CaptureEditor::Interaction::ResizeLeft;
}

/// The eight handles that reshape a box, as opposed to the two ends of a line.
bool isBoxResize(CaptureEditor::Interaction interaction) {
  return interaction >= CaptureEditor::Interaction::ResizeTopLeft &&
         interaction <= CaptureEditor::Interaction::ResizeLeft;
}

/// How far from an edge a press still counts as grabbing it: wide enough to
/// hit without aiming, since some layers are grabbable only by their border.
constexpr qreal kEdgeGrabTolerance = 12.0;

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
void translateAnnotation(Annotation &annotation, const QPointF &delta) {
  annotation.start += delta;
  if (hasEndpointHandles(annotation.kind))
    annotation.end += delta;
  if (isStrokeKind(annotation.kind)) {
    for (QPointF &point : annotation.points)
      point += delta;
  }
}

/// A redaction's mosaic seed; zero is reserved for "not yet seeded".
quint32 freshRedactionSeed() {
  const quint32 seed = QRandomGenerator::system()->generate();
  return seed == 0 ? 1 : seed;
}

/// Offset for a duplicated layer: down-left by default, flipped per axis
/// when that would push the copy off the canvas and the other way fits.
QPointF duplicateOffset(const QRectF &bounds, const QSizeF &canvas) {
  constexpr qreal step = 100.0;
  qreal dx = -step;
  qreal dy = step;
  if (bounds.left() + dx < 0 && bounds.right() - dx <= canvas.width())
    dx = -dx;
  if (bounds.bottom() + dy > canvas.height() && bounds.top() - dy >= 0)
    dy = -dy;
  return {dx, dy};
}

/// Endpoint that keeps the original |dx|:|dy| ratio around the fixed
/// endpoint while a bounding-box shape is resized with Shift; the axis the
/// user has scaled more wins and the other follows.
QPointF aspectLockedEndpoint(const QPointF &fixed,
                             const QPointF &originalMoving,
                             const QPointF &candidate) {
  const QPointF original = originalMoving - fixed;
  if (qFuzzyIsNull(original.x()) || qFuzzyIsNull(original.y()))
    return candidate;
  const QPointF offset = candidate - fixed;
  const qreal scale = std::max(std::abs(offset.x()) / std::abs(original.x()),
                               std::abs(offset.y()) / std::abs(original.y()));
  const qreal signX = offset.x() < 0 ? -1.0 : 1.0;
  const qreal signY = offset.y() < 0 ? -1.0 : 1.0;
  return fixed + QPointF(signX * scale * std::abs(original.x()),
                         signY * scale * std::abs(original.y()));
}

/// Unit move for an arrow key; null for any other key.
QPointF arrowKeyDelta(int key, qreal step) {
  switch (key) {
  case Qt::Key_Left:
    return {-step, 0};
  case Qt::Key_Right:
    return {step, 0};
  case Qt::Key_Up:
    return {0, -step};
  case Qt::Key_Down:
    return {0, step};
  default:
    return {};
  }
}

bool supportsCreationConstraint(CaptureEditor::Tool tool) {
  return tool == CaptureEditor::Tool::Arrow ||
         tool == CaptureEditor::Tool::Line ||
         tool == CaptureEditor::Tool::Rectangle ||
         tool == CaptureEditor::Tool::Ellipse ||
         tool == CaptureEditor::Tool::Spotlight;
}

bool supportsCenteredCreation(CaptureEditor::Tool tool) {
  return tool == CaptureEditor::Tool::Rectangle ||
         tool == CaptureEditor::Tool::Ellipse ||
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
  case CaptureEditor::Tool::Ellipse:
    return QStringLiteral("tool-ellipse");
  case CaptureEditor::Tool::Redact:
    return QStringLiteral("tool-redact");
  case CaptureEditor::Tool::Cut:
    return QStringLiteral("tool-cut");
  case CaptureEditor::Tool::Text:
    return QStringLiteral("tool-text");
  case CaptureEditor::Tool::Ocr:
    return QStringLiteral("tool-ocr");
  case CaptureEditor::Tool::Eyedropper:
    return QStringLiteral("tool-eyedropper");
  }
  return {};
}

// Annotation kind a drag-to-create tool previews and commits (OCR only
// previews its rectangle).
Annotation::Kind dragShapeKind(CaptureEditor::Tool tool) {
  switch (tool) {
  case CaptureEditor::Tool::Rectangle:
  case CaptureEditor::Tool::Ocr:
    return Annotation::Kind::Rectangle;
  case CaptureEditor::Tool::Ellipse:
    return Annotation::Kind::Ellipse;
  case CaptureEditor::Tool::Line:
    return Annotation::Kind::Line;
  case CaptureEditor::Tool::Arrow:
  default:
    return Annotation::Kind::Arrow;
  }
}

constexpr qreal kCornerRadiusStep = 2.0;
constexpr qreal kMaximumCornerRadius = 24.0;

QString fillName(bool filled) {
  return filled ? QStringLiteral("Filled") : QStringLiteral("Hollow");
}

QString cornerName(qreal radius) {
  return radius > 0.0 ? QStringLiteral("%1 px corners").arg(qRound(radius))
                      : QStringLiteral("square corners");
}

QString textBackgroundName(TextBackground background) {
  return background == TextBackground::Pill ? QStringLiteral("Pill")
                                            : QStringLiteral("Plain");
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

/**
 * Formats a native pixel size for the measurement readout. The number is the
 * pixel count the export actually carries, which on a scaled monitor is not
 * the logical size the pointer moves through.
 */
QString formatPixelSize(const QSizeF &size) {
  return QStringLiteral("%1 × %2")
      .arg(qRound(size.width()))
      .arg(qRound(size.height()));
}

QString formatPixelPoint(const QPointF &point) {
  return QStringLiteral("%1, %2").arg(qRound(point.x())).arg(qRound(point.y()));
}

/**
 * Draws the measurement readout below-right of the pointer, flipping to the
 * opposite side instead of clipping at an overlay edge. Digits use the fixed
 * font so a live drag does not jitter the pill width per frame.
 */
void drawMeasureBadge(QPainter &painter, const QRect &bounds,
                      const QPointF &cursor, const QString &text) {
  if (text.isEmpty())
    return;
  QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  font.setPixelSize(12);
  font.setBold(true);
  painter.setFont(font);
  const qreal width = painter.fontMetrics().horizontalAdvance(text) + 16;
  constexpr qreal height = 22;
  constexpr qreal gap = 15;
  constexpr qreal margin = 6;
  qreal x = cursor.x() + gap;
  if (x + width > bounds.width() - margin)
    x = cursor.x() - gap - width;
  qreal y = cursor.y() + gap;
  if (y + height > bounds.height() - margin)
    y = cursor.y() - gap - height;
  const QRectF pill(
      std::clamp(x, margin, std::max(margin, bounds.width() - width - margin)),
      std::clamp(y, margin,
                 std::max(margin, bounds.height() - height - margin)),
      width, height);
  painter.setPen(QPen(QColor(255, 255, 255, 42), 1));
  painter.setBrush(QColor(12, 12, 15, 235));
  painter.drawRoundedRect(pill, 6, 6);
  painter.setPen(Qt::white);
  painter.drawText(pill, Qt::AlignCenter, text);
}

void drawHotkeyLegend(QPainter &painter, const QRect &bounds,
                      const QPointF &cursor,
                      const QVector<QPair<QString, QString>> &entries,
                      const QVector<QPointF> &keepVisible = {}) {
  if (entries.isEmpty())
    return;
  constexpr int columns = 2;
  const int rows = (entries.size() + columns - 1) / columns;
  const qreal width = 414;
  const qreal height = rows * 19 + 24;
  const QRectF right(bounds.width() - width - 14, 14, width, height);
  const QRectF left(14, 14, width, height);
  auto hiddenCount = [&](const QRectF &candidate) {
    int count = 0;
    for (const QPointF &point : keepVisible) {
      if (candidate.contains(point))
        ++count;
    }
    return count;
  };
  // Flip away from the pointer, but not onto a selected handle. Adding Cut
  // grew the card far enough that a line-select click near mid-canvas moved
  // it over the start handle.
  const bool cursorWantsLeft =
      right.adjusted(-28, -28, 28, 28).contains(cursor);
  QRectF panel = cursorWantsLeft ? left : right;
  const QRectF other = cursorWantsLeft ? right : left;
  if (hiddenCount(panel) > hiddenCount(other))
    panel = other;

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
      tool == CaptureEditor::Tool::Ellipse ||
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

QPointF centeredCreationStart(CaptureEditor::Tool tool, const QPointF &center,
                              const QPointF &end) {
  if (!supportsCenteredCreation(tool))
    return center;
  return center * 2.0 - end;
}

CaptureEditor::CaptureEditor(CaptureData capture, CaptureMode mode,
                             QuickOutputMode quickOutput, OperationLog log,
                             QWidget *parent)
    : QWidget(parent), capture_(std::move(capture)),
      quickOutputMode_(quickOutput) {
  pristineSource_ = capture_.source;
  pristineLogicalSize_ = capture_.previewSize;
  paletteConfig_ = loadPaletteConfig(defaultPaletteConfigPath());
  customColor_ = paletteConfig_.custom;
  if (!log.ops.isEmpty()) {
    ops_ = std::move(log.ops);
    opIndex_ = std::clamp(log.index, 0, static_cast<int>(ops_.size()));
    nextAnnotationId_ = std::max<quint64>(log.nextId, 1);
    nextMarker_ = std::max(log.nextMarker, 1);
  }
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

  textEditor_ = new InlineTextEdit(this);
  textEditor_->hide();
  textEditor_->setViewportMargins(0, 0, 0, 0);
  textEditor_->setStyleSheet(
      QStringLiteral("QPlainTextEdit { color: #ff375f; background: transparent; "
                     "border: none; padding: 0;"
                     " selection-background-color: #0a84ff; }"));
  textEditor_->installEventFilter(this);
  // The editor paints its own, shorter caret (see paintEdit); blink it.
  textCaretTimer_.setInterval(530);
  connect(&textCaretTimer_, &QTimer::timeout, this, [this] {
    textCaretOn_ = !textCaretOn_;
    update(textEditor_->geometry().adjusted(-4, -4, 4, 4));
  });
  connect(textEditor_, &QPlainTextEdit::cursorPositionChanged, this, [this] {
    textCaretOn_ = true;
    textCaretTimer_.start();
    update();
  });
  // A run of nudges persists once, shortly after the last key, instead of
  // re-rendering the snapshot on every auto-repeat.
  nudgePersistTimer_.setSingleShot(true);
  nudgePersistTimer_.setInterval(kNudgeCoalesceMs);
  connect(&nudgePersistTimer_, &QTimer::timeout, this,
          [this] { endNudgeRun(); });
  connect(textEditor_, &QPlainTextEdit::textChanged, this, [this] {
        const QString text = textEditor_->toPlainText();
        const QFontMetrics metrics(textEditor_->font());
        int widestLine = 0;
        const QStringList lines = text.split('\n');
        for (const QString &line : lines)
          widestLine = std::max(widestLine,
                                metrics.horizontalAdvance(line + QStringLiteral("  ")));
        const int sidePadding = textEditPill_
                                    ? qRound(std::max(4.0, metrics.height() * 0.18))
                                    : 0;
        const int desiredWidth = std::max(48, widestLine + sidePadding * 2);
        const int availableWidth =
            std::max(48, qRound(editImageRect().right() - textEditor_->x()));
        // QPlainTextEdit needs a little more than QFontMetrics::height(): its
        // block layout keeps leading/descent outside the nominal line box.
        // Without that room the first Return scrolls the original line out of
        // the viewport, making it look as though the text was erased.
        const int lineCount = std::max(1, static_cast<int>(lines.size()));
        const int desiredHeight = lineCount * metrics.lineSpacing() +
                                  metrics.descent() + 4;
        textEditor_->resize(std::min(desiredWidth, availableWidth),
                            desiredHeight);
        textEditor_->verticalScrollBar()->setValue(0);
        QTimer::singleShot(0, textEditor_, [editor = textEditor_] {
          editor->verticalScrollBar()->setValue(0);
        });
        update();
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

  connect(&snapshotWatcher_, &QFutureWatcher<bool>::finished, this, [this] {
    snapshotBusy_ = false;
    snapshotWriteOk_ = snapshotWatcher_.result();
    if (snapshotWriteOk_ && QFile::exists(snapshotPath_))
      sourceWritten_ = true;
    // Let the cut finish before chaining another snapshot so persistence sees
    // its final committed operation rather than an intermediate interaction.
    if (snapshotDirty_ && !cutDragActive_)
      startSnapshotRender();
  });

  connect(&captureWatcher_, &QFutureWatcher<CaptureJob>::finished, this,
          [this] {
            capturePending_ = false;
            const CaptureJob job = captureWatcher_.result();
            if (!job.ok) {
              setStatus(QStringLiteral("Screen capture failed"));
              emit captureReady(false, job.error);
              update();
              return;
            }
            capture_ = job.capture;
            pristineSource_ = capture_.source;
            pristineLogicalSize_ = capture_.previewSize;
            cuts_.clear();
            redactionBaseStale_ = true;
            switch (pendingMode_) {
            case CaptureMode::Fullscreen:
              selection_ = QRectF(QPointF(), capture_.previewSize);
              enterEdit(QStringLiteral(
                  "Full screen selected · native resolution · outer handles "
                  "crop"));
              break;
            case CaptureMode::Window:
              windowMode_ = true;
              hoveredWindow_ = windowAt(cursor_);
              setStatus(QStringLiteral(
                  "Window mode · click or Super+Arrows then Enter · Space "
                  "returns to area"));
              break;
            case CaptureMode::Region:
              setStatus(QStringLiteral(
                  "Drag to select an area · Space selects a window"));
              break;
            case CaptureMode::File:
              break;
            }
            updatePointerCursor();
            update();
            emit captureReady(true, {});
          });

  connect(&pinWatcher_, &QFutureWatcher<QImage>::finished, this, [this] {
    pinPending_ = false;
    const QString path = pendingPinPath_;
    const QImage image = pinWatcher_.result();
    QString error;
    if (image.isNull() || !saveTemporarySnapshot(image, path, error)) {
      --pinCount_;
      setStatus(error.isEmpty()
                    ? QStringLiteral("Could not render pinned capture")
                    : error);
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
  });

  if (capture_.source.isNull()) {
    // The pixel capture has not landed yet; the overlay shows a Capturing…
    // state until startCapture() completes.
    capturePending_ = true;
    pendingMode_ = mode;
    setStatus(QStringLiteral("Capturing screen…"));
  } else if (mode == CaptureMode::Fullscreen || mode == CaptureMode::File) {
    if (ops_.isEmpty())
      selection_ = QRectF(QPointF(), capture_.previewSize);
    else
      replayLog();
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
  adjustSettleTimer_.setSingleShot(true);
  adjustSettleTimer_.setInterval(kAdjustSettleMs);
  connect(&adjustSettleTimer_, &QTimer::timeout, this, [this] {
    adjustingSelection_ = false;
    update();
  });
  updatePointerCursor();
}

CaptureEditor::~CaptureEditor() {
  // Never remove the working snapshot under an in-flight write; drain the
  // current render (dropping any coalesced follow-up) before cleanup.
  snapshotDirty_ = false;
  waitForSnapshot();
  const QString runtime = secureRuntimeDirectory();
  const auto removeWorking = [&](const QString &path) {
    if (path.isEmpty() || !QFile::exists(path))
      return;
    if (!runtime.isEmpty() && QFileInfo(path).absolutePath() == runtime)
      QFile::remove(path);
  };
  removeWorking(snapshotPath_);
  removeWorking(workingLogPath());
}

bool CaptureEditor::eventFilter(QObject *watched, QEvent *event) {
  if (watched == textEditor_ && event->type() == QEvent::KeyPress) {
    auto *key = static_cast<QKeyEvent *>(event);
    if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
        key->modifiers().testFlag(Qt::ControlModifier)) {
      acceptText();
      return true;
    }
    if (key->key() == Qt::Key_Escape) {
      handleEscape();
      return true;
    }
  } else if (watched == textEditor_ && event->type() == QEvent::FocusOut) {
    QTimer::singleShot(0, this, [this] {
      if (textEditor_->isVisible() && !textEditor_->hasFocus())
        acceptText();
    });
  }
  return QWidget::eventFilter(watched, event);
}

QColor CaptureEditor::annotationColor() const {
  if (usingCustomColor_)
    return customColor_;
  return paletteConfig_.palette.at(static_cast<std::size_t>(colorIndex_));
}
QRectF CaptureEditor::annotationBounds(const Annotation &annotation) const {
  if (annotation.kind == Annotation::Kind::Marker) {
    const qreal diameter = std::max<qreal>(24.0, annotation.size * 6.0);
    return {annotation.start.x() - diameter / 2.0,
            annotation.start.y() - diameter / 2.0, diameter, diameter};
  }
  if (annotation.kind == Annotation::Kind::Text)
    return annotationTextBounds(annotation);
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
QRectF CaptureEditor::selectedAnnotationsBounds() const {
  QRectF bounds;
  bool initialized = false;
  for (const int index : selectedAnnotations_) {
    if (index < 0 || index >= annotations_.size())
      continue;
    QRectF annotation = annotationBounds(annotations_.at(index));
    if (annotation.isNull())
      continue;
    // A horizontal arrow or line has zero height, which isEmpty() reports as
    // nothing at all; give it an extent so the group still covers it.
    annotation = annotation.normalized();
    if (annotation.width() <= 0.0)
      annotation.setWidth(1.0);
    if (annotation.height() <= 0.0)
      annotation.setHeight(1.0);
    bounds = initialized ? bounds.united(annotation) : annotation;
    initialized = true;
  }
  return initialized ? bounds : QRectF();
}

void CaptureEditor::selectAllAnnotations() {
  if (annotations_.isEmpty()) {
    setStatus(QStringLiteral("No layers to select"));
    return;
  }
  selectedAnnotations_.clear();
  for (int index = 0; index < annotations_.size(); ++index)
    selectedAnnotations_.push_back(index);
  selectedAnnotation_ = annotations_.size() - 1;
  tool_ = Tool::Select;
  setStatus(QStringLiteral("All %1 layers selected · Delete removes them")
                .arg(annotations_.size()));
}

bool CaptureEditor::annotationSelected(int index) const {
  return selectedAnnotations_.contains(index);
}

QVector<QPair<QPointF, CaptureEditor::Interaction>>
CaptureEditor::annotationHandles(const Annotation &annotation) const {
  // A line or an arrow is two points, so it has two handles and no box.
  if (annotation.kind == Annotation::Kind::Arrow ||
      annotation.kind == Annotation::Kind::Line) {
    return {{annotation.start, Interaction::ResizeStart},
            {annotation.end, Interaction::ResizeEnd}};
  }
  const QRectF bounds = annotationBounds(annotation);
  // A text's handle is its wrap width, not its size: one handle, on the edge
  // the wrapping actually moves.
  if (annotation.kind == Annotation::Kind::Text)
    return {{bounds.bottomRight(), Interaction::ResizeEnd}};
  // A counter is a disc: any corner sets its size, and sides would say
  // something about width and height that a disc cannot honour.
  if (annotation.kind == Annotation::Kind::Marker) {
    return {{bounds.topLeft(), Interaction::ResizeTopLeft},
            {bounds.topRight(), Interaction::ResizeTopRight},
            {bounds.bottomRight(), Interaction::ResizeBottomRight},
            {bounds.bottomLeft(), Interaction::ResizeBottomLeft}};
  }
  QVector<QPair<QPointF, Interaction>> handles{
      {bounds.topLeft(), Interaction::ResizeTopLeft},
      {bounds.topRight(), Interaction::ResizeTopRight},
      {bounds.bottomRight(), Interaction::ResizeBottomRight},
      {bounds.bottomLeft(), Interaction::ResizeBottomLeft}};
  // Side handles only where there is room for them: on a layer barely wider
  // than the handles themselves they would be a blob of overlapping dots.
  const qreal room = 34.0 / std::max<qreal>(editScale(), 0.01);
  if (bounds.width() >= room) {
    handles.push_back({QPointF(bounds.center().x(), bounds.top()),
                       Interaction::ResizeTop});
    handles.push_back({QPointF(bounds.center().x(), bounds.bottom()),
                       Interaction::ResizeBottom});
  }
  if (bounds.height() >= room) {
    handles.push_back({QPointF(bounds.right(), bounds.center().y()),
                       Interaction::ResizeRight});
    handles.push_back({QPointF(bounds.left(), bounds.center().y()),
                       Interaction::ResizeLeft});
  }
  return handles;
}

CaptureEditor::Interaction
CaptureEditor::selectedHandleAt(const QPointF &point) const {
  // The single answer to "is this press a resize?", shared by every tool. A
  // handle can sit well outside the layer it belongs to, a spotlight's corner
  // being out in the dimmed surround, so hit-testing the shape alone would
  // miss it and start a marquee or a new drawing instead.
  if (selectedAnnotation_ < 0 || selectedAnnotation_ >= annotations_.size())
    return Interaction::None;
  const qreal tolerance = 9.0 / std::max<qreal>(editScale(), 0.01);
  Interaction nearest = Interaction::None;
  qreal nearestDistance = tolerance;
  for (const auto &[position, handle] :
       annotationHandles(annotations_.at(selectedAnnotation_))) {
    const qreal distance = QLineF(point, position).length();
    if (distance <= nearestDistance) {
      nearestDistance = distance;
      nearest = handle;
    }
  }
  return nearest;
}

CaptureEditor::Interaction CaptureEditor::pointerHandle() const {
  if (!editImageRect().contains(cursor_))
    return Interaction::None;
  return selectedHandleAt(toAnnotationPoint(cursor_));
}

Qt::CursorShape CaptureEditor::handleCursorShape(Interaction handle) const {
  // Never the move cursor: the handle resizes, the body moves, and the pointer
  // should say which one, and which way, before the press.
  switch (handle) {
  case Interaction::ResizeStart:
  case Interaction::ResizeEnd:
    if (selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations_.size() &&
        annotations_.at(selectedAnnotation_).kind == Annotation::Kind::Text)
      return Qt::SizeHorCursor;
    return Qt::PointingHandCursor;
  case Interaction::ResizeTopLeft:
  case Interaction::ResizeBottomRight:
    return Qt::SizeFDiagCursor;
  case Interaction::ResizeTopRight:
  case Interaction::ResizeBottomLeft:
    return Qt::SizeBDiagCursor;
  case Interaction::ResizeTop:
  case Interaction::ResizeBottom:
    return Qt::SizeVerCursor;
  case Interaction::ResizeLeft:
  case Interaction::ResizeRight:
    return Qt::SizeHorCursor;
  default:
    return Qt::ArrowCursor;
  }
}

void CaptureEditor::applyBoxResize(Annotation &annotation, Interaction handle,
                                   const QPointF &point,
                                   const QRectF &original) {
  if (annotation.kind == Annotation::Kind::Marker) {
    annotation.size =
        std::clamp(QLineF(annotation.start, point).length() / 3.0, 2.0, 30.0);
    return;
  }
  const bool movesLeft = handle == Interaction::ResizeTopLeft ||
                         handle == Interaction::ResizeLeft ||
                         handle == Interaction::ResizeBottomLeft;
  const bool movesRight = handle == Interaction::ResizeTopRight ||
                          handle == Interaction::ResizeRight ||
                          handle == Interaction::ResizeBottomRight;
  const bool movesTop = handle == Interaction::ResizeTopLeft ||
                        handle == Interaction::ResizeTop ||
                        handle == Interaction::ResizeTopRight;
  const bool movesBottom = handle == Interaction::ResizeBottomLeft ||
                           handle == Interaction::ResizeBottom ||
                           handle == Interaction::ResizeBottomRight;
  QPointF target = point;
  if (resizeConstraintActive_ && (movesLeft || movesRight) &&
      (movesTop || movesBottom)) {
    // Shift keeps the proportions, measured against the corner that stays.
    const QPointF fixed(movesLeft ? original.right() : original.left(),
                        movesTop ? original.bottom() : original.top());
    const QPointF moving(movesLeft ? original.left() : original.right(),
                         movesTop ? original.top() : original.bottom());
    target = aspectLockedEndpoint(fixed, moving, point);
  }
  // An edge dragged past its opposite turns the layer inside out and keeps
  // following the pointer, the way every drawing tool behaves. Pulling the
  // right edge left across the shape gives a shape on the left, not a sliver
  // pinned at the crossing point.
  QRectF box = original;
  if (movesLeft)
    box.setLeft(target.x());
  if (movesRight)
    box.setRight(target.x());
  if (movesTop)
    box.setTop(target.y());
  if (movesBottom)
    box.setBottom(target.y());
  box = box.normalized();
  // A redaction still has a floor: it covers something, so it may not be
  // reduced to a line. The edge nearer where it started is the one that holds.
  const qreal minimum = annotation.kind == Annotation::Kind::Redaction
                            ? kMinimumRedactionExtent
                            : 0.0;
  if (minimum > 0.0) {
    const qreal fixedX = movesLeft ? original.right() : original.left();
    const qreal fixedY = movesTop ? original.bottom() : original.top();
    if (box.width() < minimum) {
      if (std::abs(box.left() - fixedX) <= std::abs(box.right() - fixedX))
        box.setRight(box.left() + minimum);
      else
        box.setLeft(box.right() - minimum);
    }
    if (box.height() < minimum) {
      if (std::abs(box.top() - fixedY) <= std::abs(box.bottom() - fixedY))
        box.setBottom(box.top() + minimum);
      else
        box.setTop(box.bottom() - minimum);
    }
  }

  if (isStrokeKind(annotation.kind)) {
    // Mirrored when the drag crossed over: the stroke flips with the box
    // rather than piling up against the edge it crossed.
    const bool flippedX = movesLeft ? target.x() > original.right()
                          : movesRight ? target.x() < original.left()
                                       : false;
    const bool flippedY = movesTop ? target.y() > original.bottom()
                          : movesBottom ? target.y() < original.top()
                                        : false;
    const qreal scaleX =
        (original.width() > 0 ? box.width() / original.width() : 1.0) *
        (flippedX ? -1.0 : 1.0);
    const qreal scaleY =
        (original.height() > 0 ? box.height() / original.height() : 1.0) *
        (flippedY ? -1.0 : 1.0);
    for (int index = 0; index < annotation.points.size(); ++index) {
      const QPointF relative =
          originalAnnotation_.points.at(index) - original.topLeft();
      const QPointF anchor(scaleX < 0 ? box.right() : box.left(),
                           scaleY < 0 ? box.bottom() : box.top());
      annotation.points[index] =
          anchor + QPointF(relative.x() * scaleX, relative.y() * scaleY);
    }
    if (!annotation.points.isEmpty()) {
      annotation.start = annotation.points.first();
      annotation.end = annotation.points.last();
    }
    return;
  }
  annotation.start = box.topLeft();
  annotation.end = box.bottomRight();
}

QString CaptureEditor::toolStatus() const {
  const int size = qRound(annotationSize_);
  switch (tool_) {
  case Tool::Select:
    return QStringLiteral("Select · drag moves layers · wheel zooms · outer "
                          "handles crop");
  case Tool::Spotlight: {
    const QString shape =
        spotlightShape_ == SpotlightShape::Ellipse ? QStringLiteral("ellipse")
        : spotlightShape_ == SpotlightShape::Rectangle
            ? QStringLiteral("rectangle")
            : QStringLiteral("rounded");
    const QString zoom =
        spotlightMagnification_ <= 1.0
            ? QStringLiteral("no zoom")
            : QStringLiteral("%1×").arg(spotlightMagnification_, 0, 'f', 1);
    const QString ring =
        spotlightBorder_ <= 0.0
            ? QStringLiteral("no border")
            : QStringLiteral("border %1").arg(qRound(spotlightBorder_));
    return QStringLiteral("Spotlight · %1 · %2 · %3 · S cycles shape · wheel "
                          "zooms · Alt+wheel border")
        .arg(shape, zoom, ring);
  }
  case Tool::Redact:
    return QStringLiteral("Redact · %1 · D toggles style")
        .arg(redactionStyleName(redactionStyle_).toLower());
  case Tool::Text:
    return QStringLiteral("Text · size %1 · click to type")
        .arg(QString::fromLatin1(
            kTextSizeNames.at(static_cast<std::size_t>(textSizeIndex_))));
  case Tool::Marker:
    return QStringLiteral("Marker · next %1 · size %2 · wheel resizes")
        .arg(nextMarker_)
        .arg(size);
  case Tool::Ocr:
    return QStringLiteral("Copy text · drag over the words to copy them");
  case Tool::Eyedropper:
    return QStringLiteral("Eyedropper · click to take a color from the "
                          "capture");
  case Tool::Rectangle:
    return QStringLiteral("Rectangle · size %1 · wheel resizes · Shift keeps "
                          "it square")
        .arg(size);
  case Tool::Ellipse:
    return QStringLiteral("Ellipse · size %1 · wheel resizes · Shift keeps "
                          "it circular")
        .arg(size);
  case Tool::Cut:
    return QStringLiteral("Cut · drag a band to remove it");
  case Tool::Arrow:
  case Tool::Line:
  case Tool::Freehand:
  case Tool::Highlighter:
    break;
  }
  const QString name = tool_ == Tool::Arrow      ? QStringLiteral("Arrow")
                       : tool_ == Tool::Line     ? QStringLiteral("Line")
                       : tool_ == Tool::Freehand ? QStringLiteral("Pen")
                                                 : QStringLiteral("Highlighter");
  return QStringLiteral("%1 · size %2 · wheel resizes · Shift constrains")
      .arg(name)
      .arg(size);
}

bool CaptureEditor::annotationContains(const Annotation &annotation,
                                       const QPointF &point,
                                       bool edgeOnly) const {
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
        // Filled rectangles hit anywhere inside unless this is an edge-only
        // grab; hollow ones only on the stroke band.
        const qreal tolerance =
            std::max<qreal>(kEdgeGrabTolerance, annotation.size + 3.0);
        const QRectF outer =
            bounds.adjusted(-tolerance, -tolerance, tolerance, tolerance);
        if (!edgeOnly && annotation.filled)
          return outer.contains(point);
        const QRectF inner =
            bounds.adjusted(tolerance, tolerance, -tolerance, -tolerance);
        return outer.contains(point) &&
               (inner.isEmpty() || !inner.contains(point));
      }
      if (annotation.kind == Annotation::Kind::Ellipse) {
        // Same rule for ellipses: filled hits the silhouette unless this is
        // an edge-only grab; hollow only a band around the outline.
        const qreal tolerance =
            std::max<qreal>(kEdgeGrabTolerance, annotation.size + 3.0);
        QPainterPath outline;
        outline.addEllipse(bounds);
        QPainterPathStroker band;
        band.setWidth(tolerance * 2.0);
        return band.createStroke(outline).contains(point) ||
               (!edgeOnly && annotation.filled && outline.contains(point));
      }
      if (edgeOnly && annotation.kind == Annotation::Kind::Redaction) {
        const QRectF outer =
            bounds.adjusted(kEdgeGrabTolerance, kEdgeGrabTolerance,
                            -kEdgeGrabTolerance, -kEdgeGrabTolerance);
        return bounds
                   .adjusted(-kEdgeGrabTolerance, -kEdgeGrabTolerance,
                             kEdgeGrabTolerance, kEdgeGrabTolerance)
                   .contains(point) &&
               (outer.isEmpty() || !outer.contains(point));
      }
      if (bounds.adjusted(-7, -7, 7, 7).contains(point))
        return true;
    }
    return false;
}

int CaptureEditor::annotationAt(const QPointF &point) const {
  for (const int layer : {0, 1, 2, 3, 4}) {
    for (int index = annotations_.size() - 1; index >= 0; --index) {
      const Annotation &annotation = annotations_.at(index);
      const int annotationLayer =
          annotation.kind == Annotation::Kind::Marker      ? 0
          : annotation.kind == Annotation::Kind::Text      ? 1
          : annotation.kind == Annotation::Kind::Spotlight ? 3
          : annotation.kind == Annotation::Kind::Redaction ? 4
                                                           : 2;
      if (annotationLayer != layer)
        continue;
      if (layer == 3) {
        if (spotlightPath(annotation).contains(point))
          return index;
        continue;
      }
      if (annotationContains(annotation, point, false))
        return index;
    }
  }
  return -1;
}

bool CaptureEditor::toolGrabsLayer(int index) const {
  if (index < 0 || index >= annotations_.size())
    return false;
  // A counter is stamped over anything that is not a counter. It still picks
  // up its own kind: two that must overlap are placed apart and dragged
  // together.
  if (tool_ == Tool::Marker)
    return annotations_.at(index).kind == Annotation::Kind::Marker;
  return true;
}

bool CaptureEditor::pointerGrabsLayer() const {
  return editImageRect().contains(cursor_) &&
         toolGrabsLayer(annotationEdgeAt(toAnnotationPoint(cursor_)));
}

int CaptureEditor::annotationEdgeAt(const QPointF &point) const {
  for (const int layer : {0, 1, 2, 3, 4}) {
    for (int index = annotations_.size() - 1; index >= 0; --index) {
      const Annotation &annotation = annotations_.at(index);
      const int annotationLayer =
          annotation.kind == Annotation::Kind::Marker      ? 0
          : annotation.kind == Annotation::Kind::Text      ? 1
          : annotation.kind == Annotation::Kind::Spotlight ? 3
          : annotation.kind == Annotation::Kind::Redaction ? 4
                                                           : 2;
      if (annotationLayer != layer)
        continue;
      if (layer == 3) {
        const QPainterPath opening = spotlightPath(annotation);
        if (!opening.contains(point))
          continue;
        QPainterPathStroker band;
        band.setWidth(kEdgeGrabTolerance * 2.0);
        if (band.createStroke(opening).contains(point))
          return index;
        continue;
      }
      if (annotationContains(annotation, point, true))
        return index;
    }
  }
  return -1;
}

int CaptureEditor::hoveredSpotlightAt(const QPointF &position) const {
  if (dragging_ || !editImageRect().contains(position))
    return -1;
  const int index = annotationAt(toAnnotationPoint(position));
  if (index < 0 || annotations_.at(index).kind != Annotation::Kind::Spotlight)
    return -1;
  return index;
}
void CaptureEditor::duplicateSelectedAnnotation() {
  if (dragging_ || textEditor_->isVisible() || selectedAnnotation_ < 0 ||
      selectedAnnotation_ >= annotations_.size())
    return;
  endNudgeRun();
  Annotation copy = annotations_.at(selectedAnnotation_);
  copy.id = 0;
  translateAnnotation(
      copy, duplicateOffset(annotationBounds(copy), selection_.size()));
  if (copy.kind == Annotation::Kind::Marker)
    copy.number = nextMarker_;
  if (copy.kind == Annotation::Kind::Redaction) {
    // A copy must not share the original's mosaic; give it its own seed.
    copy.redactionSeed = freshRedactionSeed();
  }
  commitAnnotate(std::move(copy));
  if (!annotations_.isEmpty()) {
    selectedAnnotation_ = annotations_.size() - 1;
    selectedAnnotations_ = {selectedAnnotation_};
  }
  setStatus(QStringLiteral("Duplicated · Alt+D again offsets further"));
}

bool CaptureEditor::adjustSelectedAnnotationRing(int step) {
  // Alt+wheel is the secondary control, and with a layer selected it belongs
  // to that layer rather than to what the next one will look like. Kinds with
  // no second setting say so, and the wheel falls through to the armed tool.
  beginSelectionAdjust();
  if (selectedAnnotation_ < 0 || selectedAnnotation_ >= annotations_.size())
    return false;
  Annotation &annotation = annotations_[selectedAnnotation_];
  if (annotation.kind != Annotation::Kind::Spotlight)
    return false;
  annotation.size = std::clamp(annotation.size + step * 2.0, 0.0, 12.0);
  setStatus(spotlightStatus(annotation.spotlightShape,
                            annotation.magnification, annotation.size));
  commitPatch({selectedAnnotation_});
  return true;
}

void CaptureEditor::beginSelectionAdjust() {
  adjustingSelection_ = true;
  adjustSettleTimer_.start();
}

void CaptureEditor::adjustSelectedAnnotation(int step) {
  // The wheel is the weight control: a layer keeps the shape and the place it
  // was drawn in, and only gets heavier or lighter. How big it is belongs to
  // the corner handle, where Shift keeps the proportions: one gesture each,
  // so neither has to be undone to reach the other.
  beginSelectionAdjust();
  if (selectedAnnotation_ < 0 || selectedAnnotation_ >= annotations_.size())
    return;
  Annotation &annotation = annotations_[selectedAnnotation_];
  const auto weigh = [step](qreal size, qreal lowest, qreal highest) {
    return std::clamp(size + step, lowest, highest);
  };
  switch (annotation.kind) {
  case Annotation::Kind::Spotlight:
    annotation.magnification =
        std::clamp(annotation.magnification + step * 0.25, 1.0, 4.0);
    setStatus(spotlightStatus(annotation.spotlightShape,
                              annotation.magnification, annotation.size));
    commitPatch({selectedAnnotation_});
    return;
  case Annotation::Kind::Marker:
    // A counter has no stroke to weigh: its size is the counter itself.
    annotation.size = weigh(annotation.size, 2.0, 30.0);
    setStatus(QStringLiteral("Counter %1 · size %2 · wheel resizes")
                  .arg(annotation.number)
                  .arg(qRound(annotation.size)));
    commitPatch({selectedAnnotation_});
    return;
  case Annotation::Kind::Text:
    // Type has no stroke either; its weight is its size.
    annotation.size = weigh(annotation.size, 1.0, 24.0);
    setStatus(QStringLiteral("Selected text · size %1 · wheel resizes")
                  .arg(qRound(annotation.size)));
    commitPatch({selectedAnnotation_});
    return;
  case Annotation::Kind::Redaction: {
    // A cover-up is all fill, so the only thing its wheel can mean is how
    // much it covers.
    const qreal factor = step > 0 ? 1.1 : 1.0 / 1.1;
    const QRectF bounds = annotationBounds(annotation);
    const QPointF center = bounds.center();
    const qreal scale =
        bounds.width() > 0 && bounds.height() > 0
            ? std::max({factor, kMinimumRedactionExtent / bounds.width(),
                        kMinimumRedactionExtent / bounds.height()})
            : factor;
    annotation.start = center + (annotation.start - center) * scale;
    annotation.end = center + (annotation.end - center) * scale;
    setStatus(QStringLiteral("Redaction · %1%").arg(qRound(scale * 100)));
    commitPatch({selectedAnnotation_});
    return;
  }
  case Annotation::Kind::Arrow:
  case Annotation::Kind::Line:
  case Annotation::Kind::Freehand:
  case Annotation::Kind::Highlighter:
  case Annotation::Kind::Rectangle:
  case Annotation::Kind::Ellipse:
    break;
  }
  annotation.size = weigh(annotation.size, 2.0, 30.0);
  setStatus(QStringLiteral("Selected layer · thickness %1 · handle resizes")
                .arg(qRound(annotation.size)));
  commitPatch({selectedAnnotation_});
}

QLineF CaptureEditor::creationSpan(const QPointF &rawEnd) const {
  const QPointF end =
      creationConstraintActive_
          ? constrainedCreationEndpoint(tool_, dragStart_, rawEnd)
          : rawEnd;
  const QPointF start = creationCenteredActive_
                            ? centeredCreationStart(tool_, dragStart_, end)
                            : dragStart_;
  return {start, end};
}

void CaptureEditor::toggleShapeFill() {
  fillShapes_ = !fillShapes_;
  selectedAnnotation_ = -1;
  setStatus(QStringLiteral("%1 shapes · %2 again toggles fill")
                .arg(fillName(fillShapes_))
                .arg(tool_ == Tool::Ellipse ? QStringLiteral("E")
                                            : QStringLiteral("R")));
}

void CaptureEditor::toggleTextBackground() {
  if (selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations_.size() &&
      annotations_.at(selectedAnnotation_).kind == Annotation::Kind::Text) {
    Annotation &text = annotations_[selectedAnnotation_];
    text.textBackground = text.textBackground == TextBackground::Pill
                              ? TextBackground::Plain
                              : TextBackground::Pill;
    setStatus(QStringLiteral("Selected text: %1 · T again toggles")
                  .arg(textBackgroundName(text.textBackground).toLower()));
    commitPatch({selectedAnnotation_});
    return;
  }
  textBackground_ = textBackground_ == TextBackground::Pill
                        ? TextBackground::Plain
                        : TextBackground::Pill;
  selectedAnnotation_ = -1;
  setStatus(QStringLiteral("Text: %1 · T again toggles")
                .arg(textBackgroundName(textBackground_).toLower()));
}

QPointF CaptureEditor::constrainedResizeEndpoint(
    const Annotation &annotation, const QPointF &candidate,
    const QPointF &fixed, const QPointF &originalMoving) const {
  QPointF point = candidate;
  if (resizeConstraintActive_) {
    if (annotation.kind == Annotation::Kind::Arrow ||
        annotation.kind == Annotation::Kind::Line) {
      point = constrainedCreationEndpoint(Tool::Line, fixed, candidate);
    } else {
      point = aspectLockedEndpoint(fixed, originalMoving, candidate);
    }
  }
  if (annotation.kind == Annotation::Kind::Redaction)
    return constrainedRedactionEndpoint(point, fixed, originalMoving);
  return point;
}

void CaptureEditor::nudgeSelectedAnnotation(const QPointF &delta) {
  if (selectedAnnotation_ < 0 || selectedAnnotation_ >= annotations_.size())
    return;
  // Presses in quick succession (a held key) share one undo entry and one
  // deferred snapshot.
  if (!nudgeTimer_.isValid() || nudgeTimer_.elapsed() > kNudgeCoalesceMs)
    endNudgeRun();
  nudgeTimer_.restart();
  translateAnnotation(annotations_[selectedAnnotation_], delta);
  setStatus(QStringLiteral("Nudged · arrows move 1 px · Shift 10 px"));
  nudgePersistTimer_.start();
}

void CaptureEditor::endNudgeRun() {
  const bool hadRun = nudgeTimer_.isValid();
  nudgeTimer_.invalidate();
  if (nudgePersistTimer_.isActive())
    nudgePersistTimer_.stop();
  if (hadRun && selectedAnnotation_ >= 0 &&
      selectedAnnotation_ < annotations_.size())
    commitPatch({selectedAnnotation_});
}

QRectF CaptureEditor::colorPaletteRect() const {
  const qreal scale = toolbarScale(width());
  const qreal toolbarWidth = kToolbarWidth * scale;
  const qreal buttonHeight = 36 * scale;
  const qreal toolbarX = (width() - toolbarWidth) / 2.0;
  const qreal toolbarY =
      std::max<qreal>(10, chromeAnchorTop() - buttonHeight - kToolbarImageGap);
  const QRectF anchor(toolbarX + 440 * scale, toolbarY, 36 * scale,
                      buttonHeight);
  const qreal paletteWidth =
      8.0 + (static_cast<qreal>(paletteConfig_.palette.size()) + 2.0) * 28.0;
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

QRectF CaptureEditor::shapeMenuRect() const {
  const qreal scale = toolbarScale(width());
  const qreal toolbarX = (width() - kToolbarWidth * scale) / 2.0;
  const qreal buttonHeight = 36 * scale;
  const qreal toolbarY =
      std::max<qreal>(10, chromeAnchorTop() - buttonHeight - kToolbarImageGap);
  const QRectF anchor(toolbarX + 240 * scale, toolbarY, buttonHeight,
                      buttonHeight);
  return {anchor.center().x() - 58, anchor.bottom() + 4, 116, 36};
}

QRectF CaptureEditor::textSizePanelRect() const {
  const qreal scale = toolbarScale(width());
  const qreal toolbarWidth = kToolbarWidth * scale;
  const qreal buttonHeight = 36 * scale;
  const qreal toolbarX = (width() - toolbarWidth) / 2.0;
  const qreal toolbarY =
      std::max<qreal>(10, chromeAnchorTop() - buttonHeight - kToolbarImageGap);
  const QRectF anchor(toolbarX + 400 * scale, toolbarY, 36 * scale,
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
    annotations_[selectedAnnotation_].color = customColor_;
    commitPatch({selectedAnnotation_});
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

QRectF CaptureEditor::baseImageRect() const {
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

qreal CaptureEditor::chromeAnchorTop() const {
  // Zoomed past fit the content fills the viewport band, so anchoring to the
  // centered fit rect would float the chrome over it. Anchor to the band.
  const qreal base = baseImageRect().top();
  return viewZoom_ > 1.0 ? std::min<qreal>(base, 68.0) : base;
}

QRectF CaptureEditor::editImageRect() const {
  const QRectF base = baseImageRect();
  if (base.isEmpty() || viewZoom_ <= 1.0)
    return base.translated(viewOffset_);
  const QSizeF shown = base.size() * viewZoom_;
  const QPointF center = base.center();
  return QRectF(center.x() - shown.width() / 2.0,
                center.y() - shown.height() / 2.0, shown.width(),
                shown.height())
      .translated(viewOffset_);
}

qreal CaptureEditor::maxViewZoom() const {
  const QRectF base = baseImageRect();
  if (base.isEmpty() || selection_.width() <= 0)
    return 1.0;
  const qreal baseScale = base.width() / selection_.width();
  return std::max<qreal>(1.0, 4.0 / std::max<qreal>(baseScale, 0.0001));
}

void CaptureEditor::clampViewOffset() {
  const QRectF base = baseImageRect();
  if (base.isEmpty())
    return;
  const QSizeF shown = base.size() * viewZoom_;
  const QRectF available(30, 68, std::max(1, width() - 60),
                         std::max(1, height() - 126));
  // Keep the image covering the viewport where it is larger, and centered
  // (no free pan) on any axis where it is smaller.
  const auto clampAxis = [](qreal shownLen, qreal availLen, qreal &offset) {
    if (shownLen <= availLen) {
      offset = 0.0;
      return;
    }
    const qreal slack = (shownLen - availLen) / 2.0;
    offset = std::clamp(offset, -slack, slack);
  };
  clampAxis(shown.width(), available.width(), viewOffset_.rx());
  clampAxis(shown.height(), available.height(), viewOffset_.ry());
}

void CaptureEditor::setViewZoom(qreal zoom, const QPointF &focus) {
  const qreal clamped = std::clamp(zoom, 1.0, maxViewZoom());
  if (qFuzzyCompare(clamped, viewZoom_))
    return;
  const QRectF before = editImageRect();
  const qreal beforeScale = before.width() > 0 ? before.width() / selection_.width() : 1.0;
  const QPointF imagePoint((focus.x() - before.left()) / std::max(beforeScale, 1e-6),
                           (focus.y() - before.top()) / std::max(beforeScale, 1e-6));
  viewZoom_ = clamped;
  clampViewOffset();
  const QRectF after = editImageRect();
  const qreal afterScale = after.width() > 0 ? after.width() / selection_.width() : 1.0;
  const QPointF mappedFocus(after.left() + imagePoint.x() * afterScale,
                            after.top() + imagePoint.y() * afterScale);
  viewOffset_ += focus - mappedFocus;
  clampViewOffset();
  updatePointerCursor();
  update();
}

void CaptureEditor::panView(const QPointF &delta) {
  if (viewZoom_ <= 1.0)
    return;
  viewOffset_ += delta;
  clampViewOffset();
  update();
}

void CaptureEditor::resetView() {
  if (viewZoom_ == 1.0 && viewOffset_.isNull())
    return;
  viewZoom_ = 1.0;
  viewOffset_ = {};
  updatePointerCursor();
  update();
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

QPointF
CaptureEditor::toUnclampedAnnotationPoint(const QPointF &position) const {
  const QRectF image = editImageRect();
  const qreal scale = std::max<qreal>(editScale(), 0.001);
  return {(position.x() - image.left()) / scale,
          (position.y() - image.top()) / scale};
}

bool CaptureEditor::selectedLayerAcceptsPoint(const QPointF &point) const {
  if (selectedAnnotation_ < 0 || selectedAnnotation_ >= annotations_.size())
    return false;
  const Annotation &selected = annotations_.at(selectedAnnotation_);
  const qreal tolerance = 9.0 / std::max<qreal>(editScale(), 0.01);
  const QRectF bounds = annotationBounds(selected);
  const bool endpoints = hasEndpointHandles(selected.kind);
  const QPointF first = endpoints ? selected.start : bounds.topLeft();
  const QPointF last = endpoints ? selected.end : bounds.bottomRight();
  if (endpoints && QLineF(point, first).length() <= tolerance)
    return true;
  if (QLineF(point, last).length() <= tolerance)
    return true;
  return annotationAt(point) == selectedAnnotation_;
}

QRectF CaptureEditor::sourceRect(const QRectF &logicalRect) const {
  if (capture_.source.isNull() || capture_.previewSize.isEmpty())
    return {};
  const qreal scaleX =
      capture_.source.width() / static_cast<qreal>(capture_.previewSize.width());
  const qreal scaleY =
      capture_.source.height() / static_cast<qreal>(capture_.previewSize.height());
  return {logicalRect.x() * scaleX, logicalRect.y() * scaleY,
          logicalRect.width() * scaleX, logicalRect.height() * scaleY};
}

QPointF CaptureEditor::sourcePoint(const QPointF &logicalPoint) const {
  return sourceRect(QRectF(logicalPoint, QSizeF())).topLeft();
}

QRectF CaptureEditor::mapWidgetToPreview(const QRectF &widgetRect) const {
  const QSize widget = size();
  const QSize preview = capture_.previewSize;
  if (widget.isEmpty() || preview.isEmpty() || widget == preview)
    return widgetRect;
  const qreal scaleX = preview.width() / static_cast<qreal>(widget.width());
  const qreal scaleY = preview.height() / static_cast<qreal>(widget.height());
  return {widgetRect.x() * scaleX, widgetRect.y() * scaleY,
          widgetRect.width() * scaleX, widgetRect.height() * scaleY};
}

QRectF CaptureEditor::mapPreviewToWidget(const QRectF &previewRect) const {
  const QSize widget = size();
  const QSize preview = capture_.previewSize;
  if (widget.isEmpty() || preview.isEmpty() || widget == preview)
    return previewRect;
  const qreal scaleX = widget.width() / static_cast<qreal>(preview.width());
  const qreal scaleY = widget.height() / static_cast<qreal>(preview.height());
  return {previewRect.x() * scaleX, previewRect.y() * scaleY,
          previewRect.width() * scaleX, previewRect.height() * scaleY};
}

QString CaptureEditor::measurementText() const {
  if (capture_.source.isNull())
    return {};
  if (phase_ == Phase::Select) {
    if (windowMode_) {
      if (hoveredWindow_ < 0 || hoveredWindow_ >= capture_.windows.size())
        return {};
      return formatPixelSize(
          sourceRect(capture_.windows.at(hoveredWindow_).rect).size());
    }
    // A fresh drag reads 0 × 0 rather than falling back to the pointer
    // position: the number must track the frame the moment it starts.
    if (dragging_ || !selection_.isEmpty())
      return formatPixelSize(sourceRect(selection_).size());
    return formatPixelPoint(sourcePoint(cursor_));
  }
  if (tool_ == Tool::Select && dragging_ &&
      interaction_ >= Interaction::CropTopLeft)
    return formatPixelSize(sourceRect(selection_).size());
  return {};
}

int CaptureEditor::windowAt(const QPointF &position) const {
  const QPointF previewPoint =
      mapWidgetToPreview(QRectF(position, QSizeF())).topLeft();
  for (int index = capture_.windows.size() - 1; index >= 0; --index) {
    if (QRectF(capture_.windows.at(index).rect).contains(previewPoint))
      return index;
  }
  return -1;
}

int CaptureEditor::windowInDirection(int current, int key) const {
  if (capture_.windows.isEmpty())
    return -1;

  const QPointF origin = current >= 0 && current < capture_.windows.size()
                             ? QPointF(capture_.windows.at(current).rect.center())
                             : mapWidgetToPreview(QRectF(cursor_, QSizeF())).topLeft();
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
  const qreal y =
      std::max<qreal>(10, chromeAnchorTop() - height - kToolbarImageGap);
  auto add = [&](qreal buttonWidth, QString action, QString label,
                 QString tooltip, QColor color = {}) {
    const qreal scaledWidth = buttonWidth * scale;
    buttons.push_back({QRectF(x, y, scaledWidth, height), std::move(action),
                       std::move(label), std::move(tooltip), color});
    x += scaledWidth + gap;
  };

  add(36, QStringLiteral("tool-select"), {},
      QStringLiteral("Select/move · V · Wheel zoom · outer handles crop"));
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
  const QString fillHint = fillShapes_ ? QStringLiteral("filled") : QString();
  const bool ellipseSelected = tool_ == Tool::Ellipse;
  add(36, ellipseSelected ? QStringLiteral("tool-ellipse")
                          : QStringLiteral("tool-rectangle"),
      fillHint,
      QStringLiteral("Shapes · R rectangle · E ellipse · hover for fill"));
  add(36, QStringLiteral("tool-spotlight"), {},
      QStringLiteral("Spotlight · S · %1 · %2× · S cycles shape")
          .arg(spotlightShape_ == SpotlightShape::Ellipse
                   ? QStringLiteral("ellipse")
                   : spotlightShape_ == SpotlightShape::Rectangle
                         ? QStringLiteral("rectangle")
                         : QStringLiteral("rounded"))
          .arg(spotlightMagnification_, 0, 'f', 1));
  add(36, QStringLiteral("tool-redact"), {},
      QStringLiteral("Redact · D · %1 · D again toggles")
          .arg(redactionStyleName(redactionStyle_)));
  add(36, QStringLiteral("tool-cut"), {},
      QStringLiteral("Cut out a band · X · drag across"));
  add(36, QStringLiteral("tool-text"), {},
      QStringLiteral("Neucha text · T · %1 · %2 · T again toggles pill · Wheel")
          .arg(QString::fromLatin1(
              kTextSizeNames.at(static_cast<std::size_t>(textSizeIndex_))))
          .arg(textBackgroundName(textBackground_)));
  add(36, QStringLiteral("palette"), {}, QStringLiteral("Annotation color"),
      annotationColor());
  add(36, QStringLiteral("tool-ocr"), {},
      QStringLiteral("Copy all text in the image · O"));
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

  if (shapeMenuOpen_) {
    const QRectF menu = shapeMenuRect();
    buttons.push_back({{menu.left() + 4, menu.top() + 4, 32, 28},
                       QStringLiteral("shape-rectangle"), {},
                       QStringLiteral("Rectangle · R"), {}});
    buttons.push_back({{menu.left() + 40, menu.top() + 4, 32, 28},
                       QStringLiteral("shape-ellipse"), {},
                       QStringLiteral("Ellipse · E"), {}});
    buttons.push_back({{menu.left() + 76, menu.top() + 4, 32, 28},
                       QStringLiteral("shape-fill"),
                       fillShapes_ ? QStringLiteral("filled") : QString(),
                       QStringLiteral("Toggle filled or outlined shapes"), {}});
  }
  if (colorPaletteOpen_) {
    const QRectF palette = colorPaletteRect();
    const int presetCount = static_cast<int>(paletteConfig_.palette.size());
    for (int index = 0; index < presetCount; ++index) {
      buttons.push_back(
          {{palette.left() + 4 + index * 28, palette.top() + 4, 24, 28},
           QStringLiteral("color-%1").arg(index),
           {},
           QStringLiteral("Color · %1").arg(index + 1),
           paletteConfig_.palette.at(static_cast<std::size_t>(index))});
    }
    buttons.push_back({{palette.left() + 4 + presetCount * 28, palette.top() + 4,
                        24, 28},
                       QStringLiteral("custom-color"), {},
                       QStringLiteral("Custom color"), {}});
    buttons.push_back({{palette.left() + 4 + (presetCount + 1) * 28,
                        palette.top() + 4, 24, 28},
                       QStringLiteral("tool-eyedropper"), {},
                       QStringLiteral("Sample from image · I"), {}});
  }
  return buttons;
}

void CaptureEditor::setStatus(QString status) {
  status_ = std::move(status);
  update();
}

CaptureEditor::EditState CaptureEditor::editState() const {
  return {annotations_,        backgroundStyle_,     selection_,
          selectedAnnotation_, selectedAnnotations_, nextMarker_,
          cuts_};
}

void CaptureEditor::applyEditState(const EditState &state) {
  annotations_ = state.annotations;
  backgroundStyle_ = state.backgroundStyle;
  selection_ = state.selection;
  selectedAnnotation_ = std::clamp(state.selectedAnnotation, -1,
                                   static_cast<int>(annotations_.size()) - 1);
  selectedAnnotations_.clear();
  for (const int index : state.selectedAnnotations) {
    if (index >= 0 && index < annotations_.size() &&
        !selectedAnnotations_.contains(index))
      selectedAnnotations_.push_back(index);
  }
  if (selectedAnnotation_ >= 0 &&
      !selectedAnnotations_.contains(selectedAnnotation_))
    selectedAnnotations_.push_back(selectedAnnotation_);
  nextMarker_ = state.nextMarker;
  if (state.cuts != cuts_) {
    cuts_ = state.cuts;
    refreshComposedCapture();
  }
  editingAnnotation_ = -1;
  interaction_ = Interaction::None;
  freehandPoints_.clear();
  textEditor_->clear();
  textEditor_->hide();
  textCaretTimer_.stop();
  setFocus(Qt::OtherFocusReason);
  updatePointerCursor();
  update();
}

void CaptureEditor::cancelActiveDragForHistory() {
  if (dragStartStateValid_)
    replayLog();
  dragging_ = false;
  creationConstraintActive_ = false;
  creationCenteredActive_ = false;
  resizeConstraintActive_ = false;
  interaction_ = Interaction::None;
  dragStartStateValid_ = false;
  dragChanged_ = false;
  freehandPoints_.clear();
  if (cutDragActive_) {
    cutDragActive_ = false;
    refreshComposedCapture();
  }
}

QString CaptureEditor::workingLogPath() const {
  return snapshotPath_.isEmpty() ? QString() : operationLogPath(snapshotPath_);
}

bool CaptureEditor::restoreOperationLog(const QString &path, QString &error) {
  OperationLog log;
  if (!loadOperationLog(path, log, error))
    return false;
  ops_ = std::move(log.ops);
  opIndex_ = std::clamp(log.index, 0, static_cast<int>(ops_.size()));
  nextAnnotationId_ = std::max<quint64>(log.nextId, 1);
  nextMarker_ = std::max(log.nextMarker, 1);
  replayLog();
  phase_ = Phase::Edit;
  scheduleSnapshot();
  return true;
}

void CaptureEditor::commitOp(Operation op) {
  if (opIndex_ < ops_.size())
    ops_.resize(opIndex_);
  ops_.push_back(std::move(op));
  constexpr qsizetype maximumOps = 100;
  while (ops_.size() > maximumOps) {
    // Replay starts at the full monitor; the first Crop is the selected
    // region/window. Dropping it leaves annotations in cropped space.
    if (ops_.constFirst().type == Operation::Type::Crop)
      ops_.removeAt(1);
    else
      ops_.removeFirst();
    if (opIndex_ > 0)
      --opIndex_;
  }
  opIndex_ = ops_.size();
  replayLog();
  scheduleSnapshot();
}

void CaptureEditor::commitAnnotate(Annotation annotation) {
  if (annotation.id == 0)
    annotation.id = nextAnnotationId_++;
  Operation op;
  op.type = Operation::Type::Annotate;
  op.annotations = {std::move(annotation)};
  commitOp(std::move(op));
}

void CaptureEditor::commitPatch(const QVector<int> &indices) {
  Operation op;
  op.type = Operation::Type::Patch;
  for (const int index : indices) {
    if (index < 0 || index >= annotations_.size())
      continue;
    Annotation annotation = annotations_.at(index);
    if (annotation.id == 0)
      annotation.id = nextAnnotationId_++;
    op.annotations.push_back(std::move(annotation));
  }
  if (op.annotations.isEmpty())
    return;
  commitOp(std::move(op));
}

void CaptureEditor::commitDelete(const QVector<int> &indices) {
  Operation op;
  op.type = Operation::Type::Delete;
  for (const int index : indices) {
    if (index >= 0 && index < annotations_.size() &&
        annotations_.at(index).id != 0)
      op.ids.push_back(annotations_.at(index).id);
  }
  if (op.ids.isEmpty())
    return;
  commitOp(std::move(op));
}

void CaptureEditor::commitCrop(const QRectF &crop) {
  Operation op;
  op.type = Operation::Type::Crop;
  op.crop = crop;
  commitOp(std::move(op));
}

void CaptureEditor::commitCut(CutOp cut) {
  Operation op;
  op.type = Operation::Type::Cut;
  op.cut = std::move(cut);
  commitOp(std::move(op));
}

void CaptureEditor::commitBackground(BackgroundStyle style) {
  Operation op;
  op.type = Operation::Type::Background;
  op.background = style;
  commitOp(std::move(op));
}

void CaptureEditor::replayLog() {
  QVector<quint64> selectedIds;
  for (const int index : selectedAnnotations_) {
    if (index >= 0 && index < annotations_.size() &&
        annotations_.at(index).id != 0)
      selectedIds.push_back(annotations_.at(index).id);
  }
  const quint64 selectedId =
      selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations_.size()
          ? annotations_.at(selectedAnnotation_).id
          : 0;

  const QSize startSize =
      pristineLogicalSize_.isEmpty() ? capture_.previewSize
                                     : pristineLogicalSize_;
  QRectF selection{QPointF(), QSizeF(startSize)};
  BackgroundStyle background = BackgroundStyle::None;
  QVector<Annotation> annotations;
  QVector<CutOp> cuts;
  int nextMarker = 1;
  for (int index = 0; index < opIndex_ && index < ops_.size(); ++index) {
    const Operation &op = ops_.at(index);
    switch (op.type) {
    case Operation::Type::Crop:
      selection = op.crop;
      break;
    case Operation::Type::Background:
      background = op.background;
      break;
    case Operation::Type::Annotate:
      for (const Annotation &annotation : op.annotations)
        annotations.push_back(annotation);
      break;
    case Operation::Type::Patch:
      // A patch is a pick-up: the layer you just changed comes to the top,
      // same as raiseAnnotation(), so a click-to-raise survives replay.
      for (const Annotation &annotation : op.annotations) {
        const auto match = std::ranges::find_if(
            annotations, [&](const Annotation &current) {
              return current.id != 0 && current.id == annotation.id;
            });
        if (match != annotations.end()) {
          Annotation updated = annotation;
          annotations.erase(match);
          annotations.push_back(std::move(updated));
        }
      }
      break;
    case Operation::Type::Delete:
      annotations.erase(std::remove_if(annotations.begin(), annotations.end(),
                                       [&](const Annotation &annotation) {
                                         return op.ids.contains(annotation.id);
                                       }),
                        annotations.end());
      break;
    case Operation::Type::Cut: {
      const bool horizontal = op.cut.orientation == Qt::Horizontal;
      const qreal lo = op.cut.logicalStart;
      const qreal hi = op.cut.logicalEnd;
      const qreal band = hi - lo;
      for (Annotation &annotation : annotations) {
        auto shift = [&](QPointF &point) {
          if (horizontal)
            point.setY(shiftForCut(point.y(), lo, hi));
          else
            point.setX(shiftForCut(point.x(), lo, hi));
        };
        shift(annotation.start);
        shift(annotation.end);
        for (QPointF &point : annotation.points)
          shift(point);
      }
      if (band > 0.0) {
        if (horizontal)
          selection.setHeight(std::max<qreal>(1.0, selection.height() - band));
        else
          selection.setWidth(std::max<qreal>(1.0, selection.width() - band));
      }
      cuts.push_back(op.cut);
      break;
    }
    }
  }

  annotations_ = std::move(annotations);
  backgroundStyle_ = background;
  if (cuts != cuts_) {
    cuts_ = std::move(cuts);
    refreshComposedCapture();
  }
  if (!selection.isEmpty())
    selection_ = selection;
  nextMarker = 1;
  for (const Annotation &annotation : annotations_) {
    if (annotation.kind == Annotation::Kind::Marker)
      nextMarker = std::max(nextMarker, annotation.number + 1);
  }
  nextMarker_ = nextMarker;
  selectedAnnotations_.clear();
  selectedAnnotation_ = -1;
  for (int index = 0; index < annotations_.size(); ++index) {
    if (selectedIds.contains(annotations_.at(index).id))
      selectedAnnotations_.push_back(index);
    if (selectedId != 0 && annotations_.at(index).id == selectedId)
      selectedAnnotation_ = index;
  }
  if (selectedAnnotation_ < 0 && !selectedAnnotations_.isEmpty())
    selectedAnnotation_ = selectedAnnotations_.constLast();
  editingAnnotation_ = -1;
  redactionBaseStale_ = true;
  updatePointerCursor();
  update();
}

int CaptureEditor::raiseAnnotation(int index) {
  // Picking a layer up puts it on top of the ones it overlaps: the last thing
  // you touched is the thing you are working on. Text and counters are painted
  // above everything regardless, so this never buries a label or a callout.
  if (index < 0 || index >= annotations_.size() - 1)
    return index;
  Annotation raised = annotations_.takeAt(index);
  annotations_.push_back(std::move(raised));
  const int top = static_cast<int>(annotations_.size()) - 1;
  const auto remap = [index, top](int position) {
    if (position < 0)
      return position;
    if (position == index)
      return top;
    return position > index ? position - 1 : position;
  };
  selectedAnnotation_ = remap(selectedAnnotation_);
  for (int &position : selectedAnnotations_)
    position = remap(position);
  editingAnnotation_ = remap(editingAnnotation_);
  return top;
}

void CaptureEditor::undoEdit() {
  endNudgeRun();
  cancelActiveDragForHistory();
  if (opIndex_ <= 0) {
    setStatus(QStringLiteral("Nothing to undo"));
    return;
  }
  --opIndex_;
  replayLog();
  scheduleSnapshot();
  setStatus(QStringLiteral("Undo · Ctrl+Shift+Z or Ctrl+Y to redo"));
}

void CaptureEditor::redoEdit() {
  endNudgeRun();
  cancelActiveDragForHistory();
  if (opIndex_ >= ops_.size()) {
    setStatus(QStringLiteral("Nothing to redo"));
    return;
  }
  ++opIndex_;
  replayLog();
  scheduleSnapshot();
  setStatus(QStringLiteral("Redo · Ctrl+Z to undo"));
}

void CaptureEditor::scheduleSnapshot() {
  if (suppressSnapshots_ || capture_.source.isNull())
    return;
  if (snapshotPath_.isEmpty())
    snapshotPath_ = temporarySnapshotPath();
  if (snapshotBusy_) {
    snapshotDirty_ = true;
    return;
  }
  startSnapshotRender();
}

QImage CaptureEditor::renderCurrentOutput() const {
  return renderCapture(capture_, selection_, annotations_, backgroundStyle_);
}

void CaptureEditor::startSnapshotRender() {
  snapshotBusy_ = true;
  snapshotDirty_ = false;
  const QImage source = capture_.source;
  const QString path = snapshotPath_;
  const QString logPath = operationLogPath(path);
  const OperationLog log{ops_, opIndex_, nextAnnotationId_, nextMarker_};
  const bool writeSource = !sourceWritten_ || !QFile::exists(path);
  snapshotWatcher_.setFuture(QtConcurrent::run(
      [source, path, logPath, log, writeSource] {
        QString error;
        if (writeSource && !saveTemporarySnapshot(source, path, error, -1))
          return false;
        return saveOperationLog(logPath, log, error);
      }));
}

bool CaptureEditor::waitForSnapshot() {
  // Drain in-flight and coalesced snapshot renders, letting the event loop
  // run so the watcher signals can chain the next render. Used before
  // exporting so the working snapshot on disk reflects the newest state.
  while (snapshotBusy_ || snapshotDirty_) {
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    QThread::yieldCurrentThread();
  }
  return snapshotWriteOk_;
}

void CaptureEditor::pinSnapshot() {
  if (busy_ || pinPending_ || selection_.isEmpty())
    return;

  prunePinnedSnapshots();
  const QString path = pinnedSnapshotPath(++pinCount_);
  if (path.isEmpty()) {
    --pinCount_;
    setStatus(QStringLiteral("Could not create private runtime directory"));
    return;
  }
  pendingPinPath_ = path;
  pinPending_ = true;
  setStatus(QStringLiteral("Preparing pinned capture…"));
  const CaptureData captureCopy = capture_;
  const QVector<Annotation> annotations = annotations_;
  const QRectF selection = selection_;
  const BackgroundStyle background = backgroundStyle_;
  pinWatcher_.setFuture(QtConcurrent::run(
      [captureCopy, annotations, selection, background] {
        return renderCapture(captureCopy, selection, annotations, background);
      }));
}

void CaptureEditor::startCapture(CaptureMode mode, bool includeWindows) {
  if (captureStarted_ || !capture_.source.isNull())
    return;
  captureStarted_ = true;
  capturePending_ = true;
  pendingMode_ = mode;
  const MonitorInfo monitor = capture_.monitor;
  captureWatcher_.setFuture(QtConcurrent::run([monitor, includeWindows] {
    CaptureJob job;
    job.capture.monitor = monitor;
    job.ok =
        captureMonitorPixels(monitor, job.capture, includeWindows, job.error);
    return job;
  }));
}

void CaptureEditor::enterEdit(QString status) {
  phase_ = Phase::Edit;
  tool_ = Tool::Select;
  viewZoom_ = 1.0;
  viewOffset_ = {};
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
  const QRectF full(QPointF(), capture_.previewSize);
  if (ops_.isEmpty() && !selection_.isEmpty() && selection_ != full)
    commitCrop(selection_);
  else
    scheduleSnapshot();
}

void CaptureEditor::handleEscape() {
  if (cutDragActive_) {
    cutDragActive_ = false;
    dragging_ = false;
    refreshComposedCapture();
    setStatus(QStringLiteral("Cut cancelled"));
    updatePointerCursor();
    update();
    return;
  }
  const qint64 closeWindowMs =
      static_cast<qint64>(QApplication::doubleClickInterval()) * 2;
  if (escapeTimer_.isValid() && escapeTimer_.elapsed() <= closeWindowMs) {
    close();
    return;
  }
  escapeTimer_.restart();
  textEditor_->clear();
  textEditor_->hide();
  textCaretTimer_.stop();
  editingAnnotation_ = -1;
  if (dragStartStateValid_) {
    replayLog();
    scheduleSnapshot();
  }
  dragStartStateValid_ = false;
  dragChanged_ = false;
  creationConstraintActive_ = false;
  creationCenteredActive_ = false;
  resizeConstraintActive_ = false;
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
  selection_ = QRectF(capture_.windows.at(index).rect);
  redactionBaseStale_ = true;
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
  // While typing, show the same cream pill the committed text will have.
  const TextBackground background =
      annotationIndex >= 0 && annotationIndex < annotations_.size()
          ? annotations_.at(annotationIndex).textBackground
          : textBackground_;
  const bool pill = background == TextBackground::Pill;
  textEditPill_ = pill;
  const int pillPad = pill ? qRound(std::max(4.0, metrics.height() * 0.18)) : 0;
  textEditor_->setStyleSheet(
      QStringLiteral("QPlainTextEdit { color: %1; background: transparent; "
                     "border: none; margin: 0; padding: 0;"
                     " selection-background-color: #0a84ff; }")
          .arg(textColor_.name()));
  textEditor_->setViewportMargins(pillPad, 0, pillPad, 0);
  textEditor_->setGeometry(qRound(position.x()) - pillPad, qRound(position.y()),
                           72 + 2 * pillPad,
                           metrics.lineSpacing() + metrics.descent() + 4);
  textEditor_->setPlainText(existingText);
  textEditor_->show();
  textEditor_->raise();
  textEditor_->setFocus(Qt::MouseFocusReason);
  if (!existingText.isEmpty())
    textEditor_->selectAll();
  textCaretOn_ = true;
  textCaretTimer_.start();
}

void CaptureEditor::acceptText() {
  const QString text = textEditor_->toPlainText().trimmed();
  if (!text.isEmpty()) {
    Annotation annotation;
    annotation.kind = Annotation::Kind::Text;
    annotation.start =
        textPoint_ +
        QPointF(0, QFontMetricsF(annotationTextFont(textSize_)).ascent());
    annotation.text = text;
    annotation.color = textColor_;
    annotation.size = textSize_;
    annotation.textBackground =
        editingAnnotation_ >= 0 && editingAnnotation_ < annotations_.size()
            ? annotations_.at(editingAnnotation_).textBackground
            : textBackground_;
    if (editingAnnotation_ >= 0 && editingAnnotation_ < annotations_.size()) {
      annotation.id = annotations_.at(editingAnnotation_).id;
      annotations_[editingAnnotation_] = annotation;
      selectedAnnotation_ = editingAnnotation_;
      tool_ = Tool::Select;
      setStatus(QStringLiteral("Text updated · drag to move · handle resizes"));
      commitPatch({editingAnnotation_});
    } else {
      selectedAnnotation_ = -1;
      setStatus(QStringLiteral("Text added · Esc for select mode"));
      commitAnnotate(std::move(annotation));
    }
  } else if (editingAnnotation_ >= 0) {
    tool_ = Tool::Select;
  }
  editingAnnotation_ = -1;
  textEditor_->clear();
  textEditor_->hide();
  textCaretTimer_.stop();
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

  // Render the OCR crop and recognize on the worker pool; a full-resolution
  // selection would otherwise stall the overlay while tesseract runs.
  const CaptureData captureCopy = capture_;
  ocrWatcher_.setFuture(QtConcurrent::run([captureCopy, target,
                                           ocrAnnotations]() mutable {
    OcrResult result;
    const QImage image = renderCapture(captureCopy, target, ocrAnnotations,
                                       BackgroundStyle::None);
    if (image.isNull())
      result.error = QStringLiteral("Could not prepare image for OCR");
    else
      result.text = recognizeText(image, result.error);
    return result;
  }));
}

void CaptureEditor::finish(OutputMode mode) {
  if (busy_ || selection_.isEmpty())
    return;
  busy_ = true;
  setStatus(QStringLiteral("Preparing screenshot…"));
  const QImage image = renderCurrentOutput();
  QString error;
  const QString exportPath = temporaryExportPath();
  if (image.isNull() || exportPath.isEmpty() ||
      !saveTemporarySnapshot(image, exportPath, error, -1)) {
    busy_ = false;
    setStatus(error.isEmpty()
                  ? QStringLiteral("Could not prepare screenshot snapshot")
                  : error);
    return;
  }

  QString saved;
  if (mode == OutputMode::Copy || mode == OutputMode::Both) {
    if (!copyPngFileToClipboard(exportPath, error)) {
      QFile::remove(exportPath);
      busy_ = false;
      setStatus(error);
      return;
    }
  }
  if (mode == OutputMode::Save || mode == OutputMode::Both) {
    saved = moveSnapshotToScreenshots(exportPath, error);
    if (saved.isEmpty()) {
      QFile::remove(exportPath);
      busy_ = false;
      setStatus(error);
      return;
    }
  } else {
    QFile::remove(exportPath);
  }
  if (!snapshotPath_.isEmpty()) {
    QFile::remove(workingLogPath());
    QFile::remove(snapshotPath_);
    snapshotPath_.clear();
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

void CaptureEditor::saveAs() {
  // Ctrl+Shift+S: pick a destination, write a copy there, and keep the
  // editor open so Enter/Ctrl+C/Ctrl+S still produce the default outputs.
  if (busy_ || selection_.isEmpty())
    return;
  busy_ = true;
  setStatus(QStringLiteral("Preparing screenshot…"));
  snapshotOutputRequested_ = true;
  scheduleSnapshot();
  const bool snapshotOk = waitForSnapshot();
  // Later mid-edit writes go back to the fast crash-recovery compression.
  snapshotOutputRequested_ = false;
  const QFileInfo snapshotFile(snapshotPath_);
  if (!snapshotOk || snapshotPath_.isEmpty() || !snapshotFile.exists() ||
      snapshotFile.size() <= 0) {
    busy_ = false;
    setStatus(QStringLiteral("Could not prepare screenshot snapshot"));
    return;
  }

  QSettings settings(defaultPaletteConfigPath(), QSettings::IniFormat);
  QString startDir =
      settings.value(QStringLiteral("output/save_as_dir")).toString();
  if (startDir.isEmpty() || !QDir(startDir).exists())
    startDir = screenshotRootDir();
  const QString suggested =
      QDir(startDir).filePath(defaultScreenshotFileName());
  // The editor is a fullscreen stays-on-top surface, so a dialog opened on
  // top of it ends up buried behind it with no way to reach either window.
  // Hide the overlay for the dialog's lifetime (parent nullptr keeps the
  // dialog mapped while the editor is hidden), then restore it.
  hide();
  QString target = QFileDialog::getSaveFileName(
      nullptr, QStringLiteral("Save screenshot as"), suggested,
      QStringLiteral("PNG image (*.png)"));
  show();
  raise();
  activateWindow();
  if (target.isEmpty()) {
    busy_ = false;
    setStatus(QStringLiteral("Save As cancelled"));
    return;
  }
  if (!target.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive))
    target += QStringLiteral(".png");
  if (QFile::exists(target) && !QFile::remove(target)) {
    busy_ = false;
    setStatus(QStringLiteral("Could not overwrite: %1").arg(target));
    return;
  }
  if (!QFile::copy(snapshotPath_, target)) {
    busy_ = false;
    setStatus(QStringLiteral("Could not save to: %1").arg(target));
    return;
  }
  settings.setValue(QStringLiteral("output/save_as_dir"),
                    QFileInfo(target).absolutePath());
  busy_ = false;
  setStatus(QStringLiteral("Saved to %1").arg(target));
  sendCaptureNotification(QStringLiteral("Screenshot saved"), target);
}

void CaptureEditor::handleToolbar(const QString &action) {
  const Tool toolBefore = tool_;
  const QString statusBefore = status_;
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
  else if (action == QStringLiteral("tool-rectangle") ||
           action == QStringLiteral("tool-ellipse") ||
           action == QStringLiteral("shape-rectangle") ||
           action == QStringLiteral("shape-ellipse")) {
    const Tool shape = action.endsWith(QStringLiteral("rectangle"))
                           ? Tool::Rectangle
                           : Tool::Ellipse;
    if (tool_ == shape && action.startsWith(QStringLiteral("tool-")))
      toggleShapeFill();
    else
      tool_ = shape;
  } else if (action == QStringLiteral("shape-fill")) {
    if (tool_ != Tool::Rectangle && tool_ != Tool::Ellipse)
      tool_ = Tool::Rectangle;
    toggleShapeFill();
  } else if (action == QStringLiteral("tool-spotlight")) {
    if (tool_ == Tool::Spotlight) {
      spotlightShape_ = spotlightShape_ == SpotlightShape::Ellipse
                            ? SpotlightShape::Rectangle
                            : spotlightShape_ == SpotlightShape::Rectangle
                                  ? SpotlightShape::RoundedRectangle
                                  : SpotlightShape::Ellipse;
      setStatus(toolStatus());
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
  } else if (action == QStringLiteral("tool-cut")) {
    tool_ = Tool::Cut;
    selectedAnnotation_ = -1;
    setStatus(QStringLiteral("Cut: drag across a band to remove it"));
  } else if (action == QStringLiteral("tool-text")) {
    if (tool_ == Tool::Text)
      toggleTextBackground();
    else
      tool_ = Tool::Text;
  } else if (action == QStringLiteral("tool-eyedropper")) {
    if (tool_ != Tool::Eyedropper)
      toolBeforeEyedropper_ = tool_;
    tool_ = Tool::Eyedropper;
    customColorPickerOpen_ = false;
  }
  else if (action == QStringLiteral("tool-ocr")) {
    runOcr();
    return;
  }
  else if (action == QStringLiteral("palette"))
    colorPaletteOpen_ = true;
  else if (action.startsWith(QStringLiteral("color-"))) {
    colorIndex_ = std::clamp(action.sliced(6).toInt(), 0,
                             static_cast<int>(paletteConfig_.palette.size()) - 1);
    usingCustomColor_ = false;
    customColorPickerOpen_ = false;
    if (selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations_.size() &&
        annotations_.at(selectedAnnotation_).kind !=
            Annotation::Kind::Redaction) {
      annotations_[selectedAnnotation_].color = annotationColor();
      commitPatch({selectedAnnotation_});
    }
  } else if (action == QStringLiteral("custom-color")) {
    usingCustomColor_ = true;
    customColorPickerOpen_ = !customColorPickerOpen_;
  } else if (action == QStringLiteral("ocr"))
    runOcr();
  else if (action == QStringLiteral("background")) {
    const auto next = static_cast<BackgroundStyle>(
        (static_cast<int>(backgroundStyle_) + 1) % 5);
    setStatus(QStringLiteral("Backdrop: %1 · B cycles")
                  .arg(backgroundName(next)));
    commitBackground(next);
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
  if (tool_ != toolBefore && status_ == statusBefore)
    setStatus(toolStatus());
  updatePointerCursor();
  update();
}

void CaptureEditor::keyPressEvent(QKeyEvent *event) {
  modifiersSeen_ = true;
  const Tool toolBefore = tool_;
  const QString statusBefore = status_;
  if (capturePending_) {
    if (event->key() == Qt::Key_Escape)
      handleEscape();
    event->accept();
    return;
  }
  if (event->key() == Qt::Key_Shift && phase_ == Phase::Edit && dragging_) {
    // Shift pressed mid-drag constrains the drag: creation for drawing
    // tools, handle resizing for the Select tool.
    const bool resizing =
        isLayerResize(interaction_);
    if (resizing)
      resizeConstraintActive_ = true;
    else if (supportsCreationConstraint(tool_))
      creationConstraintActive_ = true;
    if (resizing || supportsCreationConstraint(tool_)) {
      event->accept();
      update();
      return;
    }
  }
  if (event->key() == Qt::Key_Alt && phase_ == Phase::Edit && dragging_ &&
      supportsCenteredCreation(tool_)) {
    creationCenteredActive_ = true;
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
      selection_ = QRectF(QPointF(), capture_.previewSize);
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

  if (cutDragActive_) {
    // Any key here (Esc's own cancel already returned above) leaves the
    // A tool-switch key would otherwise leave the preview active because the
    // release no longer reaches the Cut branch. Cancel first, then handle
    // the key against the unchanged committed capture.
    cutDragActive_ = false;
    dragging_ = false;
    refreshComposedCapture();
    setStatus(QStringLiteral("Cut cancelled"));
  }

  const bool redoShortcut = event->matches(QKeySequence::Redo) ||
                            (event->key() == Qt::Key_Y &&
                             event->modifiers().testFlag(Qt::ControlModifier));
  // Zoom keys. The bare keys work in the edit phase too, so zoom never
  // depends on a modifier reaching us: a remote keyboard bridge may inject
  // the modifier in a way the compositor never publishes as xkb state.
  const bool zoomModifier =
      event->modifiers().testFlag(Qt::ControlModifier) || phase_ == Phase::Edit;
  if (event->matches(QKeySequence::ZoomIn) ||
      (zoomModifier &&
       (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal))) {
    setViewZoom(viewZoom_ * 1.25, editImageRect().center());
    setStatus(QStringLiteral("Zoom %1% · + / - zoom · 0 fits · wheel scrolls")
                  .arg(qRound(viewZoom_ *
                              (baseImageRect().width() /
                               std::max<qreal>(selection_.width(), 1)) *
                              100)));
    return;
  } else if (event->matches(QKeySequence::ZoomOut) ||
             (zoomModifier && (event->key() == Qt::Key_Minus ||
                               event->key() == Qt::Key_Underscore))) {
    setViewZoom(viewZoom_ / 1.25, editImageRect().center());
    setStatus(QStringLiteral("Zoom %1% · + / - zoom · 0 fits · wheel scrolls")
                  .arg(qRound(viewZoom_ *
                              (baseImageRect().width() /
                               std::max<qreal>(selection_.width(), 1)) *
                              100)));
    return;
  } else if (zoomModifier && event->key() == Qt::Key_0) {
    resetView();
    return;
  }
  if (redoShortcut) {
    redoEdit();
  } else if (event->matches(QKeySequence::Undo)) {
    undoEdit();
  } else if (event->matches(QKeySequence::Copy)) {
    finish(OutputMode::Copy);
    return;
  } else if (event->matches(QKeySequence::SaveAs) ||
             (event->key() == Qt::Key_S &&
              event->modifiers().testFlag(Qt::ControlModifier) &&
              event->modifiers().testFlag(Qt::ShiftModifier))) {
    saveAs();
    return;
  } else if (event->matches(QKeySequence::Save)) {
    finish(OutputMode::Save);
    return;
  } else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
    finish(OutputMode::Both);
    return;
  } else if (event->key() == Qt::Key_D &&
             event->modifiers() == Qt::AltModifier) {
    duplicateSelectedAnnotation();
  } else if (const QPointF nudge = arrowKeyDelta(
                 event->key(), heldModifiers(event->modifiers())
                                       .testFlag(Qt::ShiftModifier)
                                   ? kNudgeStepShift
                                   : kNudgeStep);
             !nudge.isNull() &&
             !heldModifiers(event->modifiers())
                  .testAnyFlags(Qt::ControlModifier | Qt::AltModifier |
                                Qt::MetaModifier) &&
             selectedAnnotation_ >= 0 &&
             selectedAnnotation_ < annotations_.size() && !dragging_ &&
             !textEditor_->isVisible()) {
    nudgeSelectedAnnotation(nudge);
  } else if (viewZoom_ > 1.0 && selectedAnnotation_ < 0 && !dragging_ &&
             !textEditor_->isVisible() &&
             !event->modifiers().testAnyFlags(Qt::ControlModifier |
                                              Qt::AltModifier |
                                              Qt::MetaModifier) &&
             (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right ||
              event->key() == Qt::Key_Up || event->key() == Qt::Key_Down)) {
    // With nothing selected the arrows have nothing else to do, so they walk
    // the zoomed capture. It is the one way to reach the middle of a long
    // capture without a middle mouse button. Shift takes a page at a time.
    const qreal step =
        event->modifiers().testFlag(Qt::ShiftModifier) ? 240.0 : 60.0;
    const QPointF delta =
        event->key() == Qt::Key_Left    ? QPointF(step, 0)
        : event->key() == Qt::Key_Right ? QPointF(-step, 0)
        : event->key() == Qt::Key_Up    ? QPointF(0, step)
                                        : QPointF(0, -step);
    panView(delta);
    setStatus(QStringLiteral("Panning · arrows move, Shift jumps · middle-drag "
                             "pans · Ctrl+0 fits"));
  } else if ((event->key() == Qt::Key_Delete ||
              event->key() == Qt::Key_Backspace) &&
             !selectedAnnotations_.isEmpty()) {
    commitDelete(selectedAnnotations_);
    selectedAnnotations_.clear();
    selectedAnnotation_ = -1;
  } else if (event->key() == Qt::Key_V) {
    tool_ = Tool::Select;
  } else if (event->matches(QKeySequence::SelectAll)) {
    selectAllAnnotations();
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
  } else if (event->key() == Qt::Key_R || event->key() == Qt::Key_E) {
    const bool rectangle = event->key() == Qt::Key_R;
    const Tool shape = rectangle ? Tool::Rectangle : Tool::Ellipse;
    if (!dragging_ && selectedAnnotation_ >= 0 &&
        selectedAnnotation_ < annotations_.size() &&
        annotations_.at(selectedAnnotation_).kind == dragShapeKind(shape)) {
      Annotation &selected = annotations_[selectedAnnotation_];
      selected.filled = !selected.filled;
      setStatus(
          QStringLiteral("Selected %1: %2 · %3 again toggles fill")
              .arg(rectangle ? QStringLiteral("rectangle")
                             : QStringLiteral("ellipse"))
              .arg(fillName(selected.filled).toLower())
              .arg(rectangle ? QStringLiteral("R") : QStringLiteral("E")));
      commitPatch({selectedAnnotation_});
    } else if (tool_ == shape) {
      toggleShapeFill();
    } else {
      tool_ = shape;
    }
  } else if (event->key() == Qt::Key_S) {
    if (tool_ == Tool::Spotlight) {
      spotlightShape_ = spotlightShape_ == SpotlightShape::Ellipse
                            ? SpotlightShape::Rectangle
                            : spotlightShape_ == SpotlightShape::Rectangle
                                  ? SpotlightShape::RoundedRectangle
                                  : SpotlightShape::Ellipse;
      setStatus(toolStatus());
    } else {
      tool_ = Tool::Spotlight;
    }
    selectedAnnotation_ = -1;
  } else if (event->key() == Qt::Key_D) {
    if (selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations_.size() &&
        annotations_.at(selectedAnnotation_).kind ==
            Annotation::Kind::Redaction) {
      Annotation &redaction = annotations_[selectedAnnotation_];
      redaction.redactionStyle =
          redaction.redactionStyle == RedactionStyle::Solid
              ? RedactionStyle::Pixelate
              : RedactionStyle::Solid;
      setStatus(QStringLiteral("Selected redaction: %1 · D toggles")
                    .arg(redactionStyleName(redaction.redactionStyle)));
      commitPatch({selectedAnnotation_});
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
  } else if (event->key() == Qt::Key_X) {
    tool_ = Tool::Cut;
    setStatus(QStringLiteral("Cut: drag across a band to remove it"));
  } else if (event->key() == Qt::Key_T) {
    const bool textSelected =
        selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations_.size() &&
        annotations_.at(selectedAnnotation_).kind == Annotation::Kind::Text;
    if (tool_ == Tool::Text || textSelected)
      toggleTextBackground();
    else
      tool_ = Tool::Text;
  } else if (event->key() == Qt::Key_I) {
    if (tool_ != Tool::Eyedropper)
      toolBeforeEyedropper_ = tool_;
    tool_ = Tool::Eyedropper;
  } else if (event->key() == Qt::Key_O) {
    runOcr();
    return;
  } else if (event->key() == Qt::Key_P) {
    pinSnapshot();
    return;
  } else if (event->key() == Qt::Key_B) {
    const auto next = static_cast<BackgroundStyle>(
        (static_cast<int>(backgroundStyle_) + 1) % 5);
    setStatus(QStringLiteral("Backdrop: %1 · B cycles")
                  .arg(backgroundName(next)));
    commitBackground(next);
  } else if (event->key() >= Qt::Key_1 && event->key() <= Qt::Key_8) {
    colorIndex_ = event->key() - Qt::Key_1;
    usingCustomColor_ = false;
    if (selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations_.size() &&
        annotations_.at(selectedAnnotation_).kind !=
            Annotation::Kind::Redaction) {
      annotations_[selectedAnnotation_].color = annotationColor();
      commitPatch({selectedAnnotation_});
    }
  } else {
    QWidget::keyPressEvent(event);
    return;
  }
  // Arming a tool says what it is set to, so its options are discoverable
  // without pressing keys to find out. Handlers that report something more
  // specific (a restyled layer, a toggled fill) have already set it.
  if (tool_ != toolBefore && status_ == statusBefore)
    setStatus(toolStatus());
  updatePointerCursor();
  update();
}

void CaptureEditor::keyReleaseEvent(QKeyEvent *event) {
  modifiersSeen_ = true;
  if (event->key() == Qt::Key_Shift &&
      (creationConstraintActive_ || resizeConstraintActive_)) {
    creationConstraintActive_ = false;
    resizeConstraintActive_ = false;
    event->accept();
    update();
    return;
  }
  if (event->key() == Qt::Key_Alt && creationCenteredActive_) {
    creationCenteredActive_ = false;
    event->accept();
    update();
    return;
  }
  QWidget::keyReleaseEvent(event);
}

void CaptureEditor::mouseMoveEvent(QMouseEvent *event) {
  if (panning_) {
    panView(event->position() - panAnchor_);
    panAnchor_ = event->position();
    return;
  }
  cursor_ = event->position();
  if (capturePending_)
    return;
  if (phase_ == Phase::Select) {
    if (windowMode_)
      hoveredWindow_ = windowAt(cursor_);
    else if (dragging_)
      selection_ = normalizedSelection(dragStart_, cursor_);
  } else {
    if (tool_ == Tool::Select && marqueeSelecting_) {
      marqueeRect_ = QRectF(dragStart_, toAnnotationPoint(cursor_)).normalized();
      update();
      return;
    }
    if (tool_ == Tool::Select && dragging_ &&
        interaction_ >= Interaction::CropTopLeft) {
      if (cropDragImageRect_.width() <= 0.0 ||
          cropDragImageRect_.height() <= 0.0)
        return;
      const int handle = static_cast<int>(interaction_) -
                         static_cast<int>(Interaction::CropTopLeft);
      const QSizeF previewSize = capture_.previewSize;
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
    } else if ((tool_ == Tool::Select ||
                interaction_ == Interaction::Move ||
                isLayerResize(interaction_)) &&
               dragging_ && selectedAnnotation_ >= 0 &&
               selectedAnnotation_ < annotations_.size()) {
      const QPointF point = toAnnotationPoint(cursor_);
      if (selectedAnnotations_.size() > 1 && interaction_ == Interaction::Move) {
        const QPointF delta = point - dragStart_;
        for (int position = 0;
             position < selectedAnnotations_.size() &&
             position < originalSelectedAnnotations_.size();
             ++position) {
          const int index = selectedAnnotations_.at(position);
          Annotation &annotation = annotations_[index];
          annotation = originalSelectedAnnotations_.at(position);
          annotation.start += delta;
          if (hasEndpointHandles(annotation.kind))
            annotation.end += delta;
          if (isStrokeKind(annotation.kind)) {
            for (QPointF &strokePoint : annotation.points)
              strokePoint += delta;
          }
        }
        dragChanged_ = true;
      } else {
        Annotation &annotation = annotations_[selectedAnnotation_];
        annotation = originalAnnotation_;
      const QRectF originalBox = annotationBounds(originalAnnotation_);
      if (interaction_ == Interaction::Move) {
        translateAnnotation(annotation, point - dragStart_);
      } else if (isBoxResize(interaction_)) {
        applyBoxResize(annotation, interaction_, point, originalBox);
      } else if (interaction_ == Interaction::ResizeStart) {
        if (hasEndpointHandles(annotation.kind)) {
          annotation.start = constrainedResizeEndpoint(
              annotation, point, annotation.end, originalAnnotation_.start);
        }
      } else if (interaction_ == Interaction::ResizeEnd) {
        if (hasEndpointHandles(annotation.kind)) {
          annotation.end = constrainedResizeEndpoint(
              annotation, point, annotation.start, originalAnnotation_.end);
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
      }
      dragChanged_ = true;
    }
    if (tool_ == Tool::Cut && dragging_) {
      const QPointF point = toAnnotationPoint(cursor_);
      const QPointF delta = point - cutDragStart_;
      if (!cutDragActive_ &&
          std::max(std::abs(delta.x()), std::abs(delta.y())) > 3.0) {
        cutDragActive_ = true;
        // Dominant vertical delta cuts a horizontal band (rows); dominant
        // horizontal delta cuts a vertical band (columns).
        liveCut_.orientation = std::abs(delta.y()) >= std::abs(delta.x())
                                    ? Qt::Horizontal
                                    : Qt::Vertical;
        // Lock the source/preview mapping together with the drag axis. The
        // capture remains unchanged until this preview is committed.
        cutDragOriginOffset_ = liveCut_.orientation == Qt::Horizontal
                                    ? selection_.top()
                                    : selection_.left();
        cutDragRatio_ =
            liveCut_.orientation == Qt::Horizontal
                ? capture_.source.height() /
                      static_cast<qreal>(capture_.previewSize.height())
                : capture_.source.width() /
                      static_cast<qreal>(capture_.previewSize.width());
      }
      if (cutDragActive_) {
        const bool horizontal = liveCut_.orientation == Qt::Horizontal;
        const qreal a = horizontal ? cutDragStart_.y() : cutDragStart_.x();
        const qreal b = horizontal ? point.y() : point.x();
        const qreal lo = std::min(a, b);
        const qreal hi = std::max(a, b);
        cutBandLo_ = lo;
        cutBandHi_ = hi;
        // Annotation space is selection-relative logical px; map to
        // absolute logical, then to native source px via the cached ratio.
        liveCut_.sourceStart = static_cast<int>(
            std::round((cutDragOriginOffset_ + lo) * cutDragRatio_));
        liveCut_.sourceEnd = static_cast<int>(
            std::round((cutDragOriginOffset_ + hi) * cutDragRatio_));
        liveCut_.logicalStart =
            static_cast<int>(std::round(cutDragOriginOffset_ + lo));
        liveCut_.logicalEnd =
            static_cast<int>(std::round(cutDragOriginOffset_ + hi));
      }
    }
    if ((tool_ == Tool::Freehand || tool_ == Tool::Highlighter) && dragging_ &&
        interaction_ == Interaction::None) {
      const QPointF point = toAnnotationPoint(cursor_);
      if (freehandPoints_.isEmpty() ||
          QLineF(freehandPoints_.last(), point).length() >= 1.5)
        freehandPoints_.push_back(point);
    }
    bool overPaletteAnchor = false;
    bool overCustomAnchor = false;
    bool overShapeAnchor = false;
    bool overTextAnchor = false;
    for (const ToolbarButton &button : toolbarButtons()) {
      if (button.action == QStringLiteral("palette") &&
          button.rect.contains(cursor_)) {
        overPaletteAnchor = true;
        paletteIntentOrigin_ = cursor_;
      } else if (button.action == QStringLiteral("custom-color") &&
                 button.rect.contains(cursor_)) {
        overCustomAnchor = true;
        customColorIntentOrigin_ = cursor_;
      } else if ((button.action == QStringLiteral("tool-rectangle") ||
                  button.action == QStringLiteral("tool-ellipse")) &&
                 button.rect.contains(cursor_)) {
        overShapeAnchor = true;
        shapeIntentOrigin_ = cursor_;
      } else if (button.action == QStringLiteral("tool-text") &&
                 button.rect.contains(cursor_)) {
        overTextAnchor = true;
        textSizeIntentOrigin_ = cursor_;
      }
    }
    const bool overPalette =
        colorPaletteOpen_ &&
        colorPaletteRect().adjusted(0, -4, 0, 0).contains(cursor_);
    const bool overCustom =
        customColorPickerOpen_ && customColorPanelRect().contains(cursor_);
    const bool approachingPalette =
        colorPaletteOpen_ &&
        inDownwardSubmenuTriangle(paletteIntentOrigin_, colorPaletteRect(),
                                  cursor_);
    const bool approachingCustom =
        customColorPickerOpen_ &&
        inDownwardSubmenuTriangle(customColorIntentOrigin_,
                                  customColorPanelRect(), cursor_);
    const bool overShapes = shapeMenuOpen_ && shapeMenuRect().contains(cursor_);
    const bool approachingShapes =
        shapeMenuOpen_ &&
        inDownwardSubmenuTriangle(shapeIntentOrigin_, shapeMenuRect(), cursor_);
    shapeMenuOpen_ = overShapeAnchor || overShapes || approachingShapes;
    if (!shapeMenuOpen_)
      shapeIntentOrigin_ = {};
    const bool overTextSizes =
        textSizeMenuOpen_ && textSizePanelRect().contains(cursor_);
    const bool approachingTextSizes =
        textSizeMenuOpen_ &&
        inDownwardSubmenuTriangle(textSizeIntentOrigin_, textSizePanelRect(),
                                  cursor_);
    textSizeMenuOpen_ = overTextAnchor || overTextSizes || approachingTextSizes;
    if (!textSizeMenuOpen_)
      textSizeIntentOrigin_ = {};
    colorPaletteOpen_ = overPaletteAnchor || overPalette || overCustom ||
                        overCustomAnchor || approachingPalette ||
                        approachingCustom;
    if (!colorPaletteOpen_) {
      customColorPickerOpen_ = false;
      paletteIntentOrigin_ = {};
      customColorIntentOrigin_ = {};
    }
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
  if (busy_ || capturePending_)
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
  if (event->button() == Qt::MiddleButton) {
    if (phase_ == Phase::Edit && viewZoom_ > 1.0) {
      panning_ = true;
      panAnchor_ = event->position();
      setCursor(Qt::ClosedHandCursor);
    }
    return;
  }
  if (event->button() != Qt::LeftButton)
    return;
  cursor_ = event->position();
  endNudgeRun();
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
  if (textSizeMenuOpen_ && !colorPaletteOpen_ && !customColorPickerOpen_ &&
      textSizePanelRect().contains(cursor_)) {
    tool_ = Tool::Text;
    const qreal localX = cursor_.x() - textSizePanelRect().left();
    textSizeIndex_ = std::clamp(static_cast<int>(localX / 34.0), 0, 2);
    setStatus(QStringLiteral("Neucha · size %1 · wheel changes size")
                  .arg(QString::fromLatin1(kTextSizeNames.at(
                      static_cast<std::size_t>(textSizeIndex_)))));
    update();
    return;
  }

  // Clicking away keeps whatever was typed; Enter belongs to multiline text.
  if (textEditor_->isVisible())
    acceptText();

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
  // A layer that ran off the capture keeps its handles and body live outside
  // the canvas, so it can be resized or dragged back in instead of being
  // stranded. Drags still follow the clamped pointer, so this only ever pulls
  // layers back; anything else outside the canvas stays inert.
  const bool insideImage = editImageRect().contains(cursor_);
  const QPointF point = insideImage ? toAnnotationPoint(cursor_)
                                    : toUnclampedAnnotationPoint(cursor_);
  if (!insideImage &&
      (tool_ != Tool::Select || !selectedLayerAcceptsPoint(point)))
    return;
  if (tool_ == Tool::Eyedropper) {
    customColor_ = sampleSourceColor(capture_.source, capture_.previewSize,
                                     selection_, editImageRect(), cursor_);
    usingCustomColor_ = true;
    if (customColor_.hsvHueF() >= 0)
      customHue_ = customColor_.hsvHueF();
    if (selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations_.size() &&
        annotations_.at(selectedAnnotation_).kind !=
            Annotation::Kind::Redaction &&
        annotations_.at(selectedAnnotation_).kind !=
            Annotation::Kind::Spotlight) {
      annotations_[selectedAnnotation_].color = customColor_;
      commitPatch({selectedAnnotation_});
    }
    QString clipboardError;
    static_cast<void>(copyTextToClipboard(
        customColor_.name(QColor::HexRgb).toUpper(), clipboardError));
    // Sampling a color is something you do in order to keep working: it
    // hands back the tool that was in hand, and leaves the layer it just
    // recolored selected so another color can be tried on it. Dropping both
    // meant that taking a color cost you your place twice over.
    tool_ = toolBeforeEyedropper_;
    setStatus(QStringLiteral("Sampled %1").arg(
        customColor_.name(QColor::HexRgb).toUpper()));
    updatePointerCursor();
    update();
    return;
  }
  if (tool_ == Tool::Select) {
    const bool additive =
        heldModifiers(event->modifiers()).testFlag(Qt::ControlModifier) ||
        heldModifiers(event->modifiers()).testFlag(Qt::MetaModifier);
    // A handle of the layer already selected is a resize wherever it sits:
    // a box's corner can lie well outside the shape it belongs to.
    const Interaction handle = selectedHandleAt(point);
    const int hit =
        handle != Interaction::None ? selectedAnnotation_ : annotationAt(point);
    if (additive) {
      if (hit >= 0) {
        if (selectedAnnotations_.contains(hit)) {
          selectedAnnotations_.removeAll(hit);
          if (selectedAnnotation_ == hit)
            selectedAnnotation_ = selectedAnnotations_.isEmpty()
                                      ? -1
                                      : selectedAnnotations_.constLast();
        } else {
          selectedAnnotations_.push_back(hit);
          selectedAnnotation_ = hit;
        }
      }
      interaction_ = Interaction::None;
      dragging_ = false;
      setStatus(selectedAnnotations_.isEmpty()
                    ? QStringLiteral("No layer selected")
                    : QStringLiteral("%1 layers selected · Ctrl-click toggles")
                          .arg(selectedAnnotations_.size()));
      updatePointerCursor();
      update();
      return;
    }

    if (hit < 0) {
      marqueeSelecting_ = true;
      marqueeAdditive_ = additive;
      marqueeRect_ = QRectF(point, point);
      dragStart_ = point;
      dragging_ = true;
      interaction_ = Interaction::None;
      setStatus(QStringLiteral("Drag to select layers"));
      update();
      return;
    } else {
      if (!selectedAnnotations_.contains(hit))
        selectedAnnotations_ = {hit};
      selectedAnnotation_ = hit;
      if (selectedAnnotations_.size() > 1) {
        interaction_ = Interaction::Move;
      } else {
        // selectedAnnotation_ is the layer under the press now, so this asks
        // about that layer's own handles.
        const Interaction onHit = selectedHandleAt(point);
        interaction_ = onHit != Interaction::None ? onHit : Interaction::Move;
      }
    }
    if (selectedAnnotation_ >= 0) {
      // Snapshot before the raise, so undo puts the order back too.
      dragStartState_ = editState();
      const int wasAt = selectedAnnotation_;
      const bool raised = selectedAnnotations_.size() == 1 &&
                          raiseAnnotation(selectedAnnotation_) != wasAt;
      originalAnnotation_ = annotations_.at(selectedAnnotation_);
      originalSelectedAnnotations_.clear();
      for (const int index : selectedAnnotations_)
        originalSelectedAnnotations_.push_back(annotations_.at(index));
      dragStartStateValid_ = true;
      dragChanged_ = raised;
      dragStart_ = point;
      dragging_ = true;
      resizeConstraintActive_ = isLayerResize(interaction_) &&
                                event->modifiers().testFlag(Qt::ShiftModifier);
      setStatus(selectedAnnotations_.size() > 1
                    ? QStringLiteral("%1 layers selected · drag to move")
                          .arg(selectedAnnotations_.size())
                    : QStringLiteral(
                          "Vector layer selected · drag to move · handles "
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
  // Edges move, interiors are canvas: with any tool armed, an edge under the
  // pointer grabs that layer and the tool is left alone.
  if (tool_ != Tool::Select && !dragging_) {
    // A handle of the layer already selected wins over grabbing anything, so
    // a selected layer can still be resized without switching to Select. The
    // text tool's wrap handle is the one that would otherwise be unreachable.
    int grabbed = annotationEdgeAt(point);
    if (!toolGrabsLayer(grabbed))
      grabbed = -1;
    Interaction grabInteraction = Interaction::Move;
    // Eight handles (and a text wrap handle) of the layer already selected
    // win over grabbing anything, so a selected layer can still be resized
    // without switching to Select.
    const Interaction handle = selectedHandleAt(point);
    if (handle != Interaction::None) {
      grabbed = selectedAnnotation_;
      grabInteraction = handle;
    }
    if (grabbed >= 0) {
      const EditState before = editState();
      selectedAnnotation_ = grabbed;
      selectedAnnotations_ = {grabbed};
      const bool raised = raiseAnnotation(grabbed) != grabbed;
      grabbed = selectedAnnotation_;
      originalAnnotation_ = annotations_.at(grabbed);
      originalSelectedAnnotations_.clear();
      originalSelectedAnnotations_.push_back(annotations_.at(grabbed));
      interaction_ = grabInteraction;
      dragStartState_ = before;
      dragStartStateValid_ = true;
      dragChanged_ = raised;
      dragStart_ = point;
      dragging_ = true;
      setStatus(QStringLiteral("Moving layer · release to keep drawing"));
      updatePointerCursor();
      update();
      return;
    }
    // Nothing under the pointer: put down whatever was selected. A click that
    // goes nowhere then only deselects, because a drag shorter than the commit
    // dead-zone draws nothing; a real drag still draws, so dismissing costs no
    // extra click.
    if (selectedAnnotation_ >= 0 || !selectedAnnotations_.isEmpty()) {
      selectedAnnotation_ = -1;
      selectedAnnotations_.clear();
      setStatus(QStringLiteral("No layer selected · drag to draw"));
      // Tools that place on press must not place on the click that was only
      // meant to put the last layer down. The tool stays armed, so the next
      // click starts the next one.
      if (tool_ == Tool::Text || tool_ == Tool::Marker) {
        updatePointerCursor();
        update();
        return;
      }
    }
  }
  if (tool_ == Tool::Marker) {
    Annotation annotation;
    annotation.kind = Annotation::Kind::Marker;
    annotation.start = point;
    annotation.number = nextMarker_;
    annotation.color = annotationColor();
    annotation.size = annotationSize_;
    selectedAnnotation_ = -1;
    setStatus(QStringLiteral("Marker %1 added · Esc for select mode")
                  .arg(annotation.number));
    commitAnnotate(std::move(annotation));
    updatePointerCursor();
  } else if (tool_ == Tool::Text) {
    beginText(point);
  } else if (tool_ == Tool::Cut) {
    // Activation waits for a dominant drag axis (see mouseMoveEvent); a
    // plain click never crosses that threshold and mouseReleaseEvent treats
    // it as a no-op.
    cutDragStart_ = point;
    cutDragActive_ = false;
    dragging_ = true;
  } else {
    dragStart_ = point;
    dragging_ = true;
    interaction_ = Interaction::None;
    creationConstraintActive_ = supportsCreationConstraint(tool_) &&
                                heldModifiers(event->modifiers()).testFlag(Qt::ShiftModifier);
    creationCenteredActive_ = supportsCenteredCreation(tool_) &&
                              heldModifiers(event->modifiers()).testFlag(Qt::AltModifier);
    if (tool_ == Tool::Redact)
      activeRedactionSeed_ = freshRedactionSeed();
    if (tool_ == Tool::Freehand || tool_ == Tool::Highlighter) {
      freehandPoints_.clear();
      freehandPoints_.reserve(256);
      freehandPoints_.push_back(point);
    }
  }
  update();
}

void CaptureEditor::mouseReleaseEvent(QMouseEvent *event) {
  if (event->button() == Qt::MiddleButton && panning_) {
    panning_ = false;
    updatePointerCursor();
    return;
  }
  if (capturePending_ || event->button() != Qt::LeftButton || !dragging_)
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
  if ((interaction_ == Interaction::Move || isLayerResize(interaction_)) &&
      tool_ != Tool::Select) {
    // A grab with a drawing tool armed: keep the move, keep the tool. It is
    // an edit like any other, so it takes its own undo step. Ctrl+Z after
    // nudging a layer must put that layer back, not remove the one before it.
    dragging_ = false;
    interaction_ = Interaction::None;
    if (dragStartStateValid_ && dragChanged_)
      commitPatch(selectedAnnotations_);
    dragStartStateValid_ = false;
    dragChanged_ = false;
    setStatus(QStringLiteral("Layer moved · keep drawing, or Esc to select"));
    updatePointerCursor();
    update();
    return;
  }
  if (tool_ == Tool::Select) {
    if (marqueeSelecting_) {
      const QRectF area = marqueeRect_.normalized();
      QVector<int> matches;
      if (area.width() >= 2.0 && area.height() >= 2.0) {
        for (int index = 0; index < annotations_.size(); ++index) {
          const QRectF bounds = annotationBounds(annotations_.at(index));
          if (!bounds.isEmpty() && area.contains(bounds))
            matches.push_back(index);
        }
      }
      if (!marqueeAdditive_)
        selectedAnnotations_.clear();
      for (const int index : matches) {
        if (!selectedAnnotations_.contains(index))
          selectedAnnotations_.push_back(index);
      }
      selectedAnnotation_ = selectedAnnotations_.isEmpty()
                                ? -1
                                : selectedAnnotations_.constLast();
      marqueeSelecting_ = false;
      marqueeRect_ = {};
      dragging_ = false;
      interaction_ = Interaction::None;
      setStatus(selectedAnnotations_.isEmpty()
                    ? QStringLiteral("No layers selected")
                    : QStringLiteral("%1 layers selected · drag to move")
                          .arg(selectedAnnotations_.size()));
      updatePointerCursor();
      update();
      return;
    }
    const bool cropped = interaction_ >= Interaction::CropTopLeft;
    const bool changed = dragStartStateValid_ && dragChanged_;
    if (changed) {
      if (cropped)
        commitCrop(selection_);
      else
        commitPatch(selectedAnnotations_);
    }
    dragStartStateValid_ = false;
    dragChanged_ = false;
    dragging_ = false;
    resizeConstraintActive_ = false;
    interaction_ = Interaction::None;
    if (cropped)
      setStatus(QStringLiteral(
          "Crop updated · Select moves layers · wheel zooms selected layer"));
    updatePointerCursor();
    update();
    return;
  }

  if (tool_ == Tool::Cut) {
    dragging_ = false;
    if (!cutDragActive_) {
      // Plain click: never crossed the activation threshold, no-op.
      update();
      return;
    }
    const qreal lo = cutBandLo_;
    const qreal hi = cutBandHi_;
    const bool horizontal = liveCut_.orientation == Qt::Horizontal;
    const qreal band = hi - lo;
    const qreal extent =
        horizontal ? selection_.height() : selection_.width();
    if (band <= 0.0 || band >= extent) {
      // Full-extent (or empty) band: toAnnotationPoint clamps lo/hi to
      // [0, extent], so an edge-to-edge drag on a full selection lands
      // exactly here. removeBand() no-ops on this band, so applying it
      // would still shrink composedLogicalSize/selection_ while the actual
      // pixels don't shrink -- desyncing preview size from the source
      // aspect. Bail out instead.
      cutDragActive_ = false;
      refreshComposedCapture();
      setStatus(QStringLiteral("Cut too large — nothing left"));
      updatePointerCursor();
      update();
      return;
    }
    cutDragActive_ = false;
    commitCut(liveCut_);
    setStatus(QStringLiteral("Cut applied · Ctrl+Z to undo"));
    updatePointerCursor();
    update();
    return;
  }

  const QLineF span = creationSpan(toAnnotationPoint(event->position()));
  const QPointF start = span.p1();
  const QPointF end = span.p2();
  creationConstraintActive_ = false;
  creationCenteredActive_ = false;
  if (tool_ == Tool::Freehand || tool_ == Tool::Highlighter) {
    if (freehandPoints_.isEmpty() ||
        QLineF(freehandPoints_.last(), end).length() >= 1.0)
      freehandPoints_.push_back(end);
    qreal length = 0;
    for (int index = 1; index < freehandPoints_.size(); ++index)
      length += QLineF(freehandPoints_.at(index - 1), freehandPoints_.at(index))
                    .length();
    if (length > 4) {
      const bool highlighter = tool_ == Tool::Highlighter;
      Annotation annotation;
      annotation.kind = highlighter ? Annotation::Kind::Highlighter
                                    : Annotation::Kind::Freehand;
      annotation.start = freehandPoints_.first();
      annotation.end = freehandPoints_.last();
      annotation.color = annotationColor();
      annotation.size = annotationSize_;
      annotation.points = std::move(freehandPoints_);
      selectedAnnotation_ = -1;
      setStatus(
          highlighter
              ? QStringLiteral("Highlight added · Esc for select mode")
              : QStringLiteral("Stroke added · Esc for select mode"));
      commitAnnotate(std::move(annotation));
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
  const QRectF draggedRect(start, end);
  const bool validRedaction =
      tool_ != Tool::Redact ||
      (draggedRect.normalized().width() >= kMinimumRedactionExtent &&
       draggedRect.normalized().height() >= kMinimumRedactionExtent);
  if (QLineF(dragStart_, end).length() > 4 && validRedaction) {
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
      annotation.kind = dragShapeKind(tool_);
      if (tool_ == Tool::Rectangle || tool_ == Tool::Ellipse)
        annotation.filled = fillShapes_;
      if (tool_ == Tool::Rectangle)
        annotation.cornerRadius = cornerRadius_;
    }
    annotation.start = start;
    annotation.end = end;
    annotation.color = annotationColor();
    // A spotlight's size is its ring width, which is its own setting: it can
    // be zero, and it is not the stroke size the other tools share.
    annotation.size =
        tool_ == Tool::Spotlight ? spotlightBorder_ : annotationSize_;
    selectedAnnotation_ = -1;
    const bool redacted = tool_ == Tool::Redact;
    setStatus(redacted
                  ? QStringLiteral("%1 redaction added · Esc for select mode")
                        .arg(redactionStyleName(redactionStyle_))
                  : QStringLiteral("Layer added · Esc for select mode"));
    commitAnnotate(std::move(annotation));
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
  // High-resolution touchpads can arrive with an empty angleDelta and only a
  // pixelDelta; treat the two interchangeably, preferring the exact pixels
  // for panning and the notch units for discrete steps.
  const QPoint angle = event->angleDelta();
  const QPoint pixel = event->pixelDelta();
  const Qt::KeyboardModifiers modifiers = event->modifiers();
  const int vertical = angle.y() != 0 ? angle.y() : pixel.y();
  const int horizontal = angle.x() != 0 ? angle.x() : pixel.x();
  if (vertical == 0 && horizontal == 0) {
    event->accept();
    return; // scroll begin/end markers carry no delta
  }
  // Discrete steps follow whichever axis carried the notch. Alt+wheel often
  // arrives as a horizontal delta, so reading only the vertical one left every
  // Alt-adjusted setting able to rise and never fall.
  const int notch = vertical != 0 ? vertical : horizontal;
  const int step = notch > 0 ? 1 : -1;
  // A selected layer owns the plain wheel whatever tool is armed. Selecting a
  // layer to adjust it is the same gesture as selecting it to move it, and the
  // tool still in hand should not change the answer, and the color keys
  // already act on the selection this way. The spotlight tool keeps its own
  // wheel,
  // which deliberately follows the spotlight under the pointer.
  const bool layerSelected = selectedAnnotation_ >= 0 &&
                             selectedAnnotation_ < annotations_.size();
  const bool overLayer = layerSelected && tool_ != Tool::Spotlight &&
                         !modifiers.testFlag(Qt::AltModifier);
  const auto showZoom = [&] {
    setStatus(QStringLiteral("Zoom %1% · wheel scrolls · Ctrl+wheel zooms · "
                             "arrows and middle-drag pan · Shift+wheel goes "
                             "sideways · Ctrl+0 fits")
                  .arg(qRound(viewZoom_ *
                              (baseImageRect().width() /
                               std::max<qreal>(selection_.width(), 1)) *
                              100)));
  };
  // One notch is one step; a touchpad's finer increments are a fraction of
  // one, so a swipe glides instead of leaping a quarter at a time.
  const auto zoomByNotch = [&](const QPointF &focus) {
    const qreal steps = std::clamp(qreal(notch) / 120.0, -1.0, 1.0);
    setViewZoom(viewZoom_ * std::pow(1.25, steps), focus);
    showZoom();
  };
  if (modifiers.testFlag(Qt::ControlModifier)) {
    zoomByNotch(event->position());
    event->accept();
    return;
  }
  // Pan by the actual delta on both axes: a touchpad's sideways swipe arrives
  // on x, and its fine increments must not each jump a step. Inert at fit.
  const QPointF scrollDelta(
      pixel.x() != 0 ? qreal(pixel.x()) : angle.x() * 0.75,
      pixel.y() != 0 ? qreal(pixel.y()) : angle.y() * 0.75);
  // Shift+wheel, or Alt+wheel, goes sideways across a wide stitch: the
  // classic mapping of a vertical wheel to horizontal scrolling.
  if (modifiers.testFlag(Qt::ShiftModifier) ||
      (tool_ == Tool::Select && !layerSelected &&
       modifiers.testFlag(Qt::AltModifier))) {
    if (viewZoom_ > 1.0)
      panView(QPointF(scrollDelta.y() + scrollDelta.x(), 0));
    event->accept();
    return;
  }
  // A plain wheel scrolls a zoomed capture like a document. Where only the
  // horizontal axis overflows, the vertical wheel scrolls that one rather
  // than doing nothing.
  if (tool_ == Tool::Select && !layerSelected) {
    if (viewZoom_ > 1.0) {
      const QSizeF shown = baseImageRect().size() * viewZoom_;
      const bool verticalSlack = shown.height() > std::max(1, height() - 126);
      const bool horizontalSlack = shown.width() > std::max(1, width() - 60);
      QPointF pan = scrollDelta;
      if (!verticalSlack && horizontalSlack && pan.x() == 0.0)
        pan = QPointF(pan.y(), 0);
      panView(pan);
    }
    event->accept();
    return;
  }
  // A selected layer owns Alt+wheel too: its ring, its corners.
  if (layerSelected && modifiers.testFlag(Qt::AltModifier) &&
      adjustSelectedAnnotationRing(step)) {
    event->accept();
    update();
    return;
  }
  if (overLayer) {
    adjustSelectedAnnotation(step);
  } else if (tool_ == Tool::Text) {
    textSizeIndex_ = std::clamp(textSizeIndex_ + step, 0, 2);
    setStatus(QStringLiteral("Neucha · size %1 · wheel changes size")
                  .arg(QString::fromLatin1(kTextSizeNames.at(
                      static_cast<std::size_t>(textSizeIndex_)))));
  } else if (tool_ == Tool::Spotlight &&
             event->modifiers().testFlag(Qt::AltModifier)) {
    // Alt+wheel is the spotlight's secondary control: the ring around the
    // opening, down to none for a clean spotlight. Independent of the zoom,
    // so a plain highlight can keep a ring and a loupe can go without one.
    const int hoveredRing = hoveredSpotlightAt(event->position());
    const qreal nextBorder =
        std::clamp((hoveredRing >= 0 ? annotations_.at(hoveredRing).size
                                     : spotlightBorder_) +
                       step * 2.0,
                   0.0, 12.0);
    if (hoveredRing >= 0) {
      annotations_[hoveredRing].size = nextBorder;
      commitPatch({hoveredRing});
    }
    spotlightBorder_ = nextBorder;
    setStatus(hoveredRing >= 0
                  ? spotlightStatus(annotations_.at(hoveredRing).spotlightShape,
                                    annotations_.at(hoveredRing).magnification,
                                    nextBorder)
                  : spotlightStatus(spotlightShape_, spotlightMagnification_,
                                    nextBorder));
  } else if (tool_ == Tool::Spotlight) {
    const qreal delta = step > 0 ? 0.25 : -0.25;
    const int hovered = hoveredSpotlightAt(event->position());
    if (hovered >= 0) {
      Annotation &annotation = annotations_[hovered];
      annotation.magnification =
          std::clamp(annotation.magnification + delta, 1.0, 4.0);
      spotlightMagnification_ = annotation.magnification;
      setStatus(spotlightStatus(annotation.spotlightShape,
                                annotation.magnification, annotation.size));
      commitPatch({hovered});
    } else {
      spotlightMagnification_ =
          std::clamp(spotlightMagnification_ + delta, 1.0, 4.0);
      setStatus(spotlightStatus(spotlightShape_, spotlightMagnification_,
                                spotlightBorder_));
    }
  } else if (tool_ == Tool::Rectangle &&
             event->modifiers().testFlag(Qt::AltModifier)) {
    // Alt+wheel is the rectangle's secondary control: corner rounding.
    cornerRadius_ = std::clamp(cornerRadius_ + step * kCornerRadiusStep, 0.0,
                               kMaximumCornerRadius);
    setStatus(QStringLiteral("Rectangle · %1 · Alt+wheel adjusts")
                  .arg(cornerName(cornerRadius_)));
  } else if (tool_ == Tool::Arrow || tool_ == Tool::Line ||
             tool_ == Tool::Freehand || tool_ == Tool::Highlighter ||
             tool_ == Tool::Marker || tool_ == Tool::Rectangle ||
             tool_ == Tool::Ellipse) {
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
      (customColorPickerOpen_ && customColorPanelRect().contains(cursor_)) ||
      (shapeMenuOpen_ && shapeMenuRect().contains(cursor_))) {
    setCursor(Qt::PointingHandCursor);
    return;
  }
  if (textSizeMenuOpen_ && !colorPaletteOpen_ && !customColorPickerOpen_ &&
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
  const bool resizing = dragging_ && isLayerResize(interaction_);
  if (resizing || (!dragging_ && pointerHandle() != Interaction::None)) {
    setCursor(handleCursorShape(resizing ? interaction_ : pointerHandle()));
    return;
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
  } else if (interaction_ == Interaction::Move && dragging_)
    setCursor(Qt::SizeAllCursor);
  else if (!dragging_ && pointerGrabsLayer())
    // An I-beam over a committed text reads as "click to edit" when the click
    // will move it, so what is under the pointer decides the cursor.
    setCursor(Qt::SizeAllCursor);
  else if (tool_ == Tool::Marker)
    setCursor(Qt::PointingHandCursor);
  else if (tool_ == Tool::Text)
    setCursor(Qt::IBeamCursor);
  else if (tool_ == Tool::Cut)
    setCursor(Qt::CrossCursor);
  else
    setCursor(Qt::CrossCursor);
}

void CaptureEditor::refreshComposedCapture() {
  capture_.source = composeCuts(pristineSource_, cuts_);
  capture_.previewSize = composedLogicalSize(pristineLogicalSize_, cuts_);
  backdropKey_ = 0;           // force backdrop pixmap rebuild
  redactionBaseStale_ = true; // force redaction layer rebuild
  update();
}

void CaptureEditor::refreshBackdropCache() {
  const qreal ratio = devicePixelRatioF();
  const QSize deviceSize = (QSizeF(size()) * ratio).toSize();
  const qint64 sourceKey = capture_.source.cacheKey();
  if (deviceSize.isEmpty()) {
    backdrop_ = {};
    dimmedBackdrop_ = {};
    backdropSize_ = {};
    return;
  }
  if (!backdrop_.isNull() && backdropSize_ == deviceSize &&
      backdropKey_ == sourceKey && qFuzzyCompare(backdropRatio_, ratio))
    return;

  backdropSize_ = deviceSize;
  backdropRatio_ = ratio;
  backdropKey_ = sourceKey;
  backdrop_ = QPixmap(deviceSize);
  backdrop_.setDevicePixelRatio(ratio);
  backdrop_.fill(Qt::transparent);
  {
    QPainter cache(&backdrop_);
    cache.setRenderHint(QPainter::SmoothPixmapTransform, true);
    cache.drawImage(QRectF(QPointF(), QSizeF(deviceSize) / ratio),
                    capture_.source);
  }
  dimmedBackdrop_ = backdrop_.copy();
  {
    QPainter cache(&dimmedBackdrop_);
    cache.fillRect(QRectF(QPointF(), QSizeF(deviceSize) / ratio),
                   QColor(0, 0, 0, kBackdropDim));
  }
}

void CaptureEditor::paintSelect(QPainter &painter) {
  if (capture_.source.isNull()) {
    painter.fillRect(rect(), QColor(0, 0, 0, kBackdropDim));
    drawStatusPill(painter, rect(), status_);
    return;
  }
  refreshBackdropCache();
  painter.drawPixmap(rect(), dimmedBackdrop_);

  const bool haveHole =
      windowMode_ ? hoveredWindow_ >= 0 && hoveredWindow_ < capture_.windows.size()
                  : !selection_.isEmpty();
  if (haveHole) {
    const QRectF previewHole =
        windowMode_ ? QRectF(capture_.windows.at(hoveredWindow_).rect)
                    : mapWidgetToPreview(selection_);
    const QRectF destHole =
        windowMode_ ? mapPreviewToWidget(previewHole) : selection_;
    painter.save();
    painter.setClipRect(destHole, Qt::IntersectClip);
    painter.drawImage(destHole, capture_.source, sourceRect(previewHole));
    painter.restore();
  }

  if (windowMode_) {
    for (int index = 0; index < capture_.windows.size(); ++index) {
      const WindowTarget &window = capture_.windows.at(index);
      painter.setPen(QPen(
          index == hoveredWindow_ ? Qt::white : QColor(255, 255, 255, 72), 2));
      painter.setBrush(Qt::NoBrush);
      painter.drawRect(mapPreviewToWidget(QRectF(window.rect)));
    }
  } else if (!selection_.isEmpty()) {
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
  drawMeasureBadge(painter, rect(), cursor_, measurementText());
}

qreal selectionBoundsRadius(const Annotation &annotation, qreal inset) {
  if (annotation.kind == Annotation::Kind::Rectangle &&
      annotation.cornerRadius > 0.0)
    return annotation.cornerRadius + inset;
  if (annotation.kind == Annotation::Kind::Text &&
      annotation.textBackground == TextBackground::Pill) {
    const QRectF pill = annotationTextBounds(annotation);
    return std::min(pill.height() / 4.0, 6.0) + inset;
  }
  return 0.0;
}

void CaptureEditor::paintEdit(QPainter &painter) {
  painter.setCompositionMode(QPainter::CompositionMode_Source);
  painter.fillRect(rect(), QColor(0, 0, 0, 160));
  painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
  // When zoomed past fit the image is larger than the viewport; clip content
  // to the band between the toolbar and the status so it cannot overdraw them.
  const bool clipViewport = viewZoom_ > 1.0;
  if (clipViewport) {
    painter.save();
    painter.setClipRect(QRectF(0, 60, width(), std::max(1, height() - 116)));
  }
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
  const QSize targetSize(qRound(image.width() * devicePixelRatioF()),
                         qRound(image.height() * devicePixelRatioF()));
  // Cache the un-annotated selection at display resolution. Rebuilding it
  // from the native source every pointer move would stall large captures.
  if (redactionBaseStale_ || redactionBaseSize_ != targetSize ||
      cachedRedactionSelection_ != selection_) {
    redactionBase_ = renderSelectionBase(capture_, selection_, targetSize);
    redactionBaseSize_ = targetSize;
    cachedRedactionSelection_ = selection_;
    redactionLayerCache_ = {};
    cachedCommittedRedactions_.clear();
    redactionBaseStale_ = false;
  }

  QVector<Annotation> committedRedactions;
  for (const Annotation &annotation : annotations_) {
    if (annotationLayer(annotation.kind) == AnnotationLayer::Redaction)
      committedRedactions.push_back(annotation);
  }
  if (redactionLayerCache_.isNull() ||
      cachedCommittedRedactions_ != committedRedactions) {
    cachedCommittedRedactions_ = committedRedactions;
    redactionLayerCache_ =
        committedRedactions.isEmpty()
            ? redactionBase_
            : applyRedactionsScaled(redactionBase_, committedRedactions,
                                    selection_, QSizeF(targetSize));
  }

  QImage redactionLayer = redactionLayerCache_;
  if (dragging_ && interaction_ == Interaction::None &&
      tool_ == Tool::Redact) {
    Annotation preview;
    preview.kind = Annotation::Kind::Redaction;
    preview.start = dragStart_;
    preview.end = toAnnotationPoint(cursor_);
    preview.redactionStyle = redactionStyle_;
    preview.redactionSeed = activeRedactionSeed_;
    redactionLayer = applyRedactionsScaled(redactionLayerCache_, {preview},
                                           selection_, QSizeF(targetSize));
  }

  painter.save();
  painter.setClipPath(clip);
  if (!redactionLayer.isNull())
    painter.drawImage(image, redactionLayer);
  else
    painter.drawImage(image, capture_.source, sourceRect(selection_));

  painter.restore();

  painter.save();
  painter.translate(image.topLeft());
  painter.scale(editScale(), editScale());
  painter.save();
  painter.setClipRect(QRectF(QPointF(), selection_.size()));
  QVector<Annotation> defaultAnnotations;
  defaultAnnotations.reserve(annotations_.size() + 1);
  for (int index = 0; index < annotations_.size(); ++index) {
    if (index == editingAnnotation_)
      continue;
    if (annotationLayer(annotations_.at(index).kind) ==
        AnnotationLayer::Default)
      defaultAnnotations.push_back(annotations_.at(index));
  }
  if (dragging_ && interaction_ == Interaction::None &&
      tool_ != Tool::Select && tool_ != Tool::Redact &&
      tool_ != Tool::Cut) {
    Annotation preview;
    if (tool_ == Tool::Freehand || tool_ == Tool::Highlighter) {
      preview.kind = tool_ == Tool::Highlighter ? Annotation::Kind::Highlighter
                                                : Annotation::Kind::Freehand;
      preview.points = freehandPoints_;
    } else {
      if (tool_ == Tool::Spotlight) {
        preview.kind = Annotation::Kind::Spotlight;
        preview.magnification = spotlightMagnification_;
        preview.spotlightShape = spotlightShape_;
      } else {
        preview.kind = dragShapeKind(tool_);
        if (tool_ == Tool::Rectangle || tool_ == Tool::Ellipse)
          preview.filled = fillShapes_;
        if (tool_ == Tool::Rectangle)
          preview.cornerRadius = cornerRadius_;
      }
      const QLineF span = creationSpan(toAnnotationPoint(cursor_));
      preview.start = span.p1();
      preview.end = span.p2();
    }
    preview.color = tool_ == Tool::Ocr ? QColor(Qt::white) : annotationColor();
    preview.size = tool_ == Tool::Ocr          ? 2.0
                   : tool_ == Tool::Spotlight ? spotlightBorder_
                                              : annotationSize_;
    defaultAnnotations.push_back(std::move(preview));
  } else if (tool_ == Tool::Marker && image.contains(cursor_) && !dragging_ &&
             !pointerGrabsLayer()) {
    // The ghost counter shows where the next one would land, so it belongs
    // only where the press would actually place one: over another counter the
    // press moves that counter instead.
    Annotation preview;
    preview.kind = Annotation::Kind::Marker;
    preview.start = toAnnotationPoint(cursor_);
    preview.number = nextMarker_;
    preview.color = annotationColor();
    preview.color.setAlpha(185);
    preview.size = annotationSize_;
    defaultAnnotations.push_back(std::move(preview));
  }
  // Default-layer tools (including spotlight) sample the redaction layer,
  // never the raw capture. The layer image is already the selection crop.
  if (!redactionLayer.isNull()) {
    paintDefaultLayer(painter, redactionLayer,
                      QRectF(QPointF(), selection_.size()), defaultAnnotations);
  } else {
    for (const Annotation &annotation : defaultAnnotations)
      paintAnnotation(painter, annotation);
  }
  painter.restore();
  if (tool_ == Tool::Select && marqueeSelecting_ &&
      !marqueeRect_.isEmpty()) {
    const qreal scale = std::max<qreal>(editScale(), 0.01);
    painter.setPen(
        QPen(QColor(QStringLiteral("#0a84ff")), 2.0 / scale));
    painter.setBrush(QColor(10, 132, 255, 38));
    painter.drawRect(marqueeRect_.normalized());
  }
  if (cutDragActive_) {
    const qreal scale = std::max<qreal>(editScale(), 0.01);
    const QRectF band = liveCut_.orientation == Qt::Horizontal
                            ? QRectF(0, cutBandLo_, selection_.width(),
                                     cutBandHi_ - cutBandLo_)
                            : QRectF(cutBandLo_, 0, cutBandHi_ - cutBandLo_,
                                     selection_.height());
    painter.save();
    painter.setClipRect(band);
    painter.fillRect(band, QColor(104, 110, 120, 175));

    const qreal spacing = 14.0 / scale;
    painter.setPen(QPen(QColor(255, 255, 255, 72), 1.0 / scale));
    for (qreal x = band.left() - band.height(); x < band.right(); x += spacing)
      painter.drawLine(QPointF(x, band.bottom()),
                       QPointF(x + band.height(), band.top()));

    const QPointF center = band.center();
    const qreal crossRadius =
        std::min<qreal>(10.0 / scale,
                        std::min(band.width(), band.height()) * 0.3);
    if (crossRadius >= 3.0 / scale) {
      painter.setPen(QPen(QColor(255, 255, 255, 230), 2.0 / scale,
                          Qt::SolidLine, Qt::RoundCap));
      painter.drawLine(center + QPointF(-crossRadius, -crossRadius),
                       center + QPointF(crossRadius, crossRadius));
      painter.drawLine(center + QPointF(-crossRadius, crossRadius),
                       center + QPointF(crossRadius, -crossRadius));
    }
    painter.restore();

    painter.setPen(QPen(QColor(255, 255, 255, 190), 1.0 / scale,
                        Qt::DashLine));
    if (liveCut_.orientation == Qt::Horizontal) {
      painter.drawLine(band.topLeft(), band.topRight());
      painter.drawLine(band.bottomLeft(), band.bottomRight());
    } else {
      painter.drawLine(band.topLeft(), band.bottomLeft());
      painter.drawLine(band.topRight(), band.bottomRight());
    }
  }

  if (selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations_.size() &&
      selectedAnnotation_ != editingAnnotation_) {
    const qreal scale = std::max<qreal>(editScale(), 0.01);
    const bool multiple = selectedAnnotations_.size() > 1;
    const QRectF bounds =
        (multiple ? selectedAnnotationsBounds()
                  : annotationBounds(annotations_.at(selectedAnnotation_)))
            .adjusted(-4, -4, 4, 4);
    // Faint while a wheel adjustment is in flight: the handles sit exactly
    // where the change shows, so at full strength they hide it.
    const qreal chromeOpacity = adjustingSelection_ ? 0.25 : 1.0;
    painter.setOpacity(chromeOpacity);
    if (multiple ||
        showsSelectionBounds(annotations_.at(selectedAnnotation_).kind)) {
      // The union reads as the group's extent, so draw it faintly and outline
      // each member on top: otherwise a select-all looks like one box with no
      // way to tell what is in it, and a flat arrow shows nothing at all.
      painter.setPen(QPen(QColor(255, 255, 255, multiple ? 110 : 220),
                          1.0 / scale, Qt::DashLine));
      painter.setBrush(Qt::NoBrush);
      const qreal boxRadius =
          multiple ? 0.0
                   : selectionBoundsRadius(
                         annotations_.at(selectedAnnotation_), 4.0);
      if (boxRadius > 0.0)
        painter.drawRoundedRect(bounds, boxRadius, boxRadius);
      else
        painter.drawRect(bounds);
    }
    if (multiple) {
      painter.setPen(
          QPen(QColor(255, 255, 255, 220), 1.0 / scale, Qt::DashLine));
      painter.setBrush(Qt::NoBrush);
      for (const int index : selectedAnnotations_) {
        if (index < 0 || index >= annotations_.size() ||
            index == editingAnnotation_)
          continue;
        QRectF member = annotationBounds(annotations_.at(index));
        if (member.isNull())
          continue;
        member = member.normalized();
        // Flat arrows and lines have no extent across; the inset gives every
        // member a visible outline whatever its shape.
        painter.drawRect(member.adjusted(-3, -3, 3, 3));
      }
    }
    if (!multiple) {
      const Annotation &selected = annotations_.at(selectedAnnotation_);
      const qreal radius = 5.0 / scale;
      painter.setPen(QPen(Qt::white, 1.0 / scale));
      painter.setBrush(QColor(QStringLiteral("#0a84ff")));
      for (const auto &[position, handle] : annotationHandles(selected))
        painter.drawEllipse(position, radius, radius);
    }
    painter.setOpacity(1.0);
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

  if (shapeMenuOpen_) {
    painter.setPen(QPen(QColor(255, 255, 255, 34), 1));
    painter.setBrush(QColor(22, 22, 28, 248));
    painter.drawRoundedRect(shapeMenuRect(), 9, 9);
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

  if (textEditor_->isVisible()) {
    // Cream pill under the transparent inline editor, and
    // a caret spanning the glyph box rather than Neucha's whole line height.
    const QRectF box = textEditor_->geometry();
    if (textEditPill_) {
      const qreal radius = std::min(box.height() / 4.0, 6.0);
      painter.setPen(Qt::NoPen);
      painter.setBrush(QColor(248, 245, 235));
      painter.drawRoundedRect(box, radius, radius);
    }
    if (textCaretOn_ && textEditor_->hasFocus()) {
      const QFontMetricsF metrics(textEditor_->font());
      const QRectF cursor =
          QRectF(textEditor_->cursorRect())
              .translated(box.topLeft() + textEditor_->viewport()->pos());
      const qreal baseline = cursor.top() + metrics.ascent();
      const qreal top = baseline - metrics.capHeight() * 1.15;
      const qreal bottom = baseline + metrics.descent() * 0.35;
      painter.setPen(QPen(textColor_, std::max(1.0, box.height() / 18.0)));
      painter.drawLine(QPointF(cursor.center().x(), top),
                       QPointF(cursor.center().x(), bottom));
    }
  }
  if (clipViewport)
    painter.restore();

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
        (button.action == QStringLiteral("shape-rectangle") &&
         tool_ == Tool::Rectangle) ||
        (button.action == QStringLiteral("shape-ellipse") &&
         tool_ == Tool::Ellipse) ||
        (button.action == QStringLiteral("shape-fill") && fillShapes_) ||
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
      const QString icon = button.action == QStringLiteral("shape-rectangle")
                               ? QStringLiteral("tool-rectangle")
                               : button.action == QStringLiteral("shape-ellipse")
                                     ? QStringLiteral("tool-ellipse")
                                     : button.action == QStringLiteral("shape-fill")
                                           ? QStringLiteral("tool-rectangle")
                                           : button.action;
      drawToolbarIcon(painter, button.rect, icon, button.label,
                      QColor(245, 245, 247));
    }
  }
  if (textSizeMenuOpen_ && !colorPaletteOpen_ && !customColorPickerOpen_) {
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
  QVector<QPointF> keepVisible;
  if (selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations_.size() &&
      hasEndpointHandles(annotations_.at(selectedAnnotation_).kind)) {
    const Annotation &selected = annotations_.at(selectedAnnotation_);
    const qreal scale = editScale();
    keepVisible = {image.topLeft() + selected.start * scale,
                   image.topLeft() + selected.end * scale};
  }
  drawHotkeyLegend(
      painter, rect(), cursor_,
      {{QStringLiteral("V"), QStringLiteral("Select / move layer")},
       {QStringLiteral("A"), QStringLiteral("Arrow")},
       {QStringLiteral("L"), QStringLiteral("Line")},
       {QStringLiteral("F / H"), QStringLiteral("Freehand / Highlighter")},
       {QStringLiteral("C"), QStringLiteral("Marker")},
       {QStringLiteral("R / E"), QStringLiteral("Rectangle / Ellipse")},
       {QStringLiteral("X"), QStringLiteral("Cut out a band")},
       {QStringLiteral("T"), QStringLiteral("Text")},
       {QStringLiteral("Double click"), QStringLiteral("Edit text layer")},
       {QStringLiteral("1–8"), QStringLiteral("Color")},
       {QStringLiteral("Wheel"), QStringLiteral("Zoom selected / tool size")},
       {QStringLiteral("D / O"), QStringLiteral("Redact / OCR text")},
       {QStringLiteral("B / P"), QStringLiteral("Backdrop / Pin on screen")},
       {QStringLiteral("Ctrl+Z"), QStringLiteral("Undo")},
       {QStringLiteral("Ctrl+Shift+Z"), QStringLiteral("Redo")},
       {QStringLiteral("Enter"), QStringLiteral("Copy + save")},
       {QStringLiteral("Ctrl+C"), QStringLiteral("Copy only")},
       {QStringLiteral("Ctrl+S"), QStringLiteral("Save only")},
       {QStringLiteral("Ctrl+Shift+S"), QStringLiteral("Save As…")},
       {QStringLiteral("Esc"), QStringLiteral("Arrow / twice close")}},
      keepVisible);
  if (hoveredButton) {
    drawInstantTooltip(painter, rect(), hoveredButton->rect,
                       hoveredButton->tooltip);
  } else if (tool_ == Tool::Arrow || tool_ == Tool::Line ||
             tool_ == Tool::Freehand || tool_ == Tool::Highlighter ||
             tool_ == Tool::Marker || tool_ == Tool::Redact ||
             tool_ == Tool::Rectangle || tool_ == Tool::Ellipse ||
             tool_ == Tool::Text) {
    const QString selectedAction = toolAction(tool_);
    for (const ToolbarButton &button : buttons) {
      if (button.action == selectedAction) {
        QString tooltip;
        if (tool_ == Tool::Text) {
          tooltip = QStringLiteral(
                        "Neucha · S  M  L · current %1 · Scroll wheel · %2 · T "
                        "again toggles pill")
                        .arg(QString::fromLatin1(kTextSizeNames.at(
                            static_cast<std::size_t>(textSizeIndex_))))
                        .arg(textBackgroundName(textBackground_));
        } else if (tool_ == Tool::Redact) {
          tooltip = QStringLiteral("Redact · %1 · D toggles style")
                        .arg(redactionStyleName(redactionStyle_));
        } else if (tool_ == Tool::Rectangle) {
          tooltip = QStringLiteral("Rectangle · %1 · Size %2 · Scroll wheel · "
                                   "Alt+Wheel %3")
                        .arg(fillName(fillShapes_))
                        .arg(qRound(annotationSize_))
                        .arg(cornerName(cornerRadius_));
        } else if (tool_ == Tool::Ellipse) {
          tooltip = QStringLiteral("Ellipse · %1 · Size %2 · Scroll wheel")
                        .arg(fillName(fillShapes_))
                        .arg(qRound(annotationSize_));
        } else {
          tooltip = QStringLiteral("Size %1 · Scroll wheel")
                        .arg(qRound(annotationSize_));
        }
        drawInstantTooltip(painter, rect(), button.rect, tooltip);
        break;
      }
    }
  }
  drawMeasureBadge(painter, rect(), cursor_, measurementText());
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
