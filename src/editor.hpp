#pragma once

#include "capture.hpp"

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QLineEdit>
#include <QWidget>

class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QWheelEvent;

class QPainter;
class CaptureEditor final : public QWidget {
  Q_OBJECT
public:
  enum class CaptureMode { Region, Window, Fullscreen, File };

  explicit CaptureEditor(CaptureData capture,
                         CaptureMode mode = CaptureMode::Region,
                         QuickOutputMode quickOutput = QuickOutputMode::None,
                         QWidget *parent = nullptr);
  ~CaptureEditor() override;

signals:
  /** Emitted (GUI thread) after a background monitor capture finishes. */
  void captureReady(bool ok, const QString &error);

public:
  /**
   * Kicks off the monitor pixel capture in the background. The overlay stays
   * interactive (showing a "Capturing…" state) until it lands, then emits
   * captureReady. Safe to call once, before entering the event loop.
   */
  void startCapture(CaptureMode mode);
  /**
   * Blocks until the in-flight snapshot persistence has drained, letting the
   * event loop run meanwhile. Returns whether the last write succeeded.
   * Used by finish() and the headless smoke suite.
   */
  bool waitForSnapshot();
  /** Renders the current selection and layer data for headless verification. */
  [[nodiscard]] QImage renderCurrentOutput() const;
  /** Current monitor data (background capture may be in flight). */
  const CaptureData &captureData() const { return capture_; }

  /**
   * Disables working-snapshot persistence. The hidden editor behind instant
   * fullscreen quick output has no overlay to check, so persisting a snapshot
   * for it would render the full capture for nothing and stall process exit.
   */
  void setSuppressSnapshots(bool suppress) { suppressSnapshots_ = suppress; }

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;
  void keyReleaseEvent(QKeyEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseDoubleClickEvent(QMouseEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void paintEvent(QPaintEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;

public:
  enum class Tool {
    Select,
    Arrow,
    Line,
    Freehand,
    Highlighter,
    Spotlight,
    Marker,
    Rectangle,
    Redact,
    Text,
    Ocr,
    Eyedropper
  };

private:
  enum class Phase { Select, Edit };
  enum class OutputMode { Copy, Save, Both };
  enum class Interaction {
    None,
    Move,
    ResizeStart,
    ResizeEnd,
    CropTopLeft,
    CropTop,
    CropTopRight,
    CropRight,
    CropBottomRight,
    CropBottom,
    CropBottomLeft,
    CropLeft
  };

  struct ToolbarButton {
    QRectF rect;
    QString action;
    QString label;
    QString tooltip;
    QColor color;
  };

  struct OcrResult {
    QString text;
    QString error;
  };

  struct EditState {
    QVector<Annotation> annotations;
    BackgroundStyle backgroundStyle = BackgroundStyle::None;
    QRectF selection;
    int selectedAnnotation = -1;
    QVector<int> selectedAnnotations;
    int nextMarker = 1;
  };

  [[nodiscard]] QRectF annotationBounds(const Annotation &annotation) const;
  [[nodiscard]] QRectF selectedAnnotationsBounds() const;
  [[nodiscard]] bool annotationSelected(int index) const;
  [[nodiscard]] int annotationAt(const QPointF &point) const;
  [[nodiscard]] int hoveredSpotlightAt(const QPointF &position) const;
  [[nodiscard]] QRectF normalizedSelection(const QPointF &first,
                                           const QPointF &second) const;
  [[nodiscard]] QRectF colorPaletteRect() const;
  [[nodiscard]] QRectF customColorPanelRect() const;
  [[nodiscard]] QRectF textSizePanelRect() const;
  [[nodiscard]] QVector<QRectF> cropHandleRects() const;
  [[nodiscard]] int cropHandleAt(const QPointF &point) const;
  [[nodiscard]] QRectF editImageRect() const;
  [[nodiscard]] qreal editScale() const;
  [[nodiscard]] QPointF toAnnotationPoint(const QPointF &position) const;
  [[nodiscard]] QRectF sourceRect(const QRectF &logicalRect) const;
  [[nodiscard]] int windowAt(const QPointF &position) const;
  [[nodiscard]] int windowInDirection(int current, int key) const;
  [[nodiscard]] QVector<ToolbarButton> toolbarButtons() const;
  [[nodiscard]] QColor annotationColor() const;

  void acceptText();
  void applyCustomColor(const QPointF &position);
  void applyEditState(const EditState &state);
  void cancelActiveDragForHistory();
  void beginText(const QPointF &point, int annotationIndex = -1);
  void chooseWindow(int index);
  [[nodiscard]] EditState editState() const;
  void enterEdit(QString status);
  void scheduleSnapshot();
  void startSnapshotRender();
  void pinSnapshot();
  void startWindowCleanCapture(int index);
  void pushUndoState(const EditState &state);
  void recordEdit();
  void redoEdit();
  void selectWindowInDirection(int key);
  void finish(OutputMode mode);
  void handleEscape();
  void handleToolbar(const QString &action);
  void paintEdit(QPainter &painter);
  void paintSelect(QPainter &painter);
  void runOcr(const QRectF &localSelection = {});
  void setStatus(QString status);
  void scaleSelectedAnnotation(qreal factor);
  void undoEdit();
  void updatePointerCursor();

  CaptureData capture_;
  Phase phase_ = Phase::Select;
  Tool tool_ = Tool::Select;
  QRectF selection_;
  QPointF dragStart_;
  QRectF originalSelection_;
  QRectF cropDragImageRect_;
  QRectF marqueeRect_;
  QPointF cursor_;
  bool dragging_ = false;
  bool creationConstraintActive_ = false;
  bool marqueeSelecting_ = false;
  bool marqueeAdditive_ = false;
  Interaction interaction_ = Interaction::None;
  QVector<QPointF> freehandPoints_;
  bool windowMode_ = false;
  BackgroundStyle backgroundStyle_ = BackgroundStyle::None;
  bool busy_ = false;
  bool colorPaletteOpen_ = false;
  bool customColorPickerOpen_ = false;
  bool usingCustomColor_ = false;
  int hoveredWindow_ = -1;
  int colorIndex_ = 0;
  QColor customColor_ = QColor(QStringLiteral("#ff375f"));
  qreal customHue_ = 0.98;
  int nextMarker_ = 1;
  qreal annotationSize_ = 4.0;
  int textSizeIndex_ = 1;
  qreal spotlightMagnification_ = 2.0;
  SpotlightShape spotlightShape_ = SpotlightShape::Ellipse;
  RedactionStyle redactionStyle_ = RedactionStyle::Pixelate;
  quint32 activeRedactionSeed_ = 0;
  QRectF cachedRedactionSelection_;
  QVector<Annotation> cachedPreviewRedactions_;
  QImage redactionPreviewCache_;
  // Display-resolution selection image reused across redaction drag frames.
  QImage redactionBase_;
  QSize redactionBaseSize_;
  bool redactionBaseStale_ = true;
  // Background/gui-thread snapshot persistence with latest-wins coalescing.
  QFutureWatcher<bool> snapshotWatcher_;
  bool snapshotBusy_ = false;
  bool snapshotDirty_ = false;
  bool snapshotWriteOk_ = true;
  bool snapshotOutputRequested_ = false;
  bool suppressSnapshots_ = false;
  // Background monitor capture fed to CaptureEditor::CaptureMode dispatch.
  struct CaptureJob {
    bool ok = false;
    CaptureData capture;
    QString error;
  };
  QFutureWatcher<CaptureJob> captureWatcher_;
  bool capturePending_ = false;
  bool captureStarted_ = false;
  CaptureMode pendingMode_ = CaptureMode::Region;
  // Background clean window surface capture.
  struct WindowJob {
    bool ok = false;
    QImage image;
    QSize scaledSize;
    QString error;
  };
  QFutureWatcher<WindowJob> windowWatcher_;
  bool windowPending_ = false;
  // Background render for --pin.
  QFutureWatcher<QImage> pinWatcher_;
  bool pinPending_ = false;
  QString pendingPinPath_;
  QVector<Annotation> annotations_;
  int selectedAnnotation_ = -1;
  int editingAnnotation_ = -1;
  Annotation originalAnnotation_;
  QVector<EditState> undoStack_;
  QVector<EditState> redoStack_;
  EditState dragStartState_;
  bool dragStartStateValid_ = false;
  bool dragChanged_ = false;
  QString snapshotPath_;
  QuickOutputMode quickOutputMode_ = QuickOutputMode::None;
  int pinCount_ = 0;
  QString status_ =
      QStringLiteral("Drag to select an area · Space selects a window");
  QLineEdit *textEditor_ = nullptr;
  QPointF textPoint_;
  QVector<Annotation> originalSelectedAnnotations_;
  QVector<int> selectedAnnotations_;
  qreal textSize_ = 4.0;
  QElapsedTimer escapeTimer_;
  QColor textColor_;
  QFutureWatcher<OcrResult> ocrWatcher_;
};

[[nodiscard]] QPointF constrainedCreationEndpoint(CaptureEditor::Tool tool,
                                                  const QPointF &start,
                                                  const QPointF &end);
