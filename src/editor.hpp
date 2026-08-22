#pragma once

#include "capture.hpp"
#include "cut.hpp"
#include "palette-config.hpp"

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QLineF>
#include <QTimer>
#include <QWidget>

class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QWheelEvent;

class QPainter;

class InlineTextEdit;
/// Corner radius for the dashed selection box around `annotation`, drawn
/// `inset` px outside its bounds. A rounded rectangle or text pill inside a
/// square box reads as a mistake, and while the radius is being set the box
/// is the only thing large enough to see it change on. 0 for every other kind.
[[nodiscard]] qreal selectionBoundsRadius(const Annotation &annotation,
                                          qreal inset);
/// Status text for a spotlight's magnification; 1x reads as "no zoom".
/// Status text for a spotlight: shape, zoom and ring, whichever one moved.
[[nodiscard]] QString spotlightStatusForTest(SpotlightShape shape,
                                             qreal magnification, qreal border);

class CaptureEditor final : public QWidget {
  Q_OBJECT
public:
  enum class CaptureMode { Region, Window, Fullscreen, File };

  explicit CaptureEditor(CaptureData capture,
                         CaptureMode mode = CaptureMode::Region,
                         QuickOutputMode quickOutput = QuickOutputMode::None,
                         OperationLog log = {},
                         QWidget *parent = nullptr);
  ~CaptureEditor() override;

signals:
  /** Emitted (GUI thread) after a background monitor capture finishes. */
  void captureReady(bool ok, const QString &error);

public:
  /**
   * Kicks off the monitor pixel capture in the background. The overlay stays
   * interactive (showing a "Capturing…" state) until it lands, then emits
   * captureReady. Safe to call once, before entering the event loop. Window
   * discovery runs alongside the grab and is skipped when `includeWindows` is
   * false, for callers that never show the overlay.
   */
  void startCapture(CaptureMode mode, bool includeWindows);
  /**
   * Blocks until the in-flight snapshot persistence has drained, letting the
   * event loop run meanwhile. Returns whether the last write succeeded.
   * Used by finish() and the headless smoke suite.
   */
  bool waitForSnapshot();
  /** Renders the current selection and layer data for headless verification. */
  [[nodiscard]] QImage renderCurrentOutput() const;
  /**
   * Native-pixel readout drawn next to the pointer: the size of whatever frame
   * is being drawn, or the pointer position while no frame exists yet. Empty
   * when nothing is being measured. Public so the smoke suite can read the
   * number without scraping it back out of the rendered overlay.
   */
  [[nodiscard]] QString measurementText() const;
  /** Current monitor data (background capture may be in flight). */
  const CaptureData &captureData() const { return capture_; }
  [[nodiscard]] QRectF currentSelection() const { return selection_; }
  [[nodiscard]] const QVector<Operation> &operationLog() const { return ops_; }
  [[nodiscard]] int operationIndex() const { return opIndex_; }
  [[nodiscard]] QString workingSourcePath() const { return snapshotPath_; }
  [[nodiscard]] QString workingLogPath() const;
  bool restoreOperationLog(const QString &path, QString &error);

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
    Ellipse,
    Redact,
    Cut,
    Text,
    Ocr,
    Eyedropper
  };

private:
  enum class Phase { Select, Edit };
  enum class OutputMode { Copy, Save, Both };
public:
  enum class Interaction {
    None,
    Move,
    /// The two ends of a line or arrow, and the single handle on the kinds
    /// that have only one (a text's wrap width).
    ResizeStart,
    ResizeEnd,
    /// A box's eight handles, in the same clockwise order as the crop ones.
    ResizeTopLeft,
    ResizeTop,
    ResizeTopRight,
    ResizeRight,
    ResizeBottomRight,
    ResizeBottom,
    ResizeBottomLeft,
    ResizeLeft,
    CropTopLeft,
    CropTop,
    CropTopRight,
    CropRight,
    CropBottomRight,
    CropBottom,
    CropBottomLeft,
    CropLeft
  };

private:

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
    QVector<CutOp> cuts;
  };

  [[nodiscard]] QRectF annotationBounds(const Annotation &annotation) const;
  [[nodiscard]] QRectF selectedAnnotationsBounds() const;
  void selectAllAnnotations();
  [[nodiscard]] bool annotationSelected(int index) const;
  /// What a pointer event reports, or nothing until a key event has confirmed
  /// it. A binding with a modifier in it, such as the README's ALT + SHIFT + 4,
  /// leaves that modifier held as the overlay takes focus. Its release goes to
  /// the compositor's binding rather than to us, so Qt keeps reporting it
  /// held.
  [[nodiscard]] Qt::KeyboardModifiers
  heldModifiers(Qt::KeyboardModifiers reported) const {
    return modifiersSeen_ ? reported : Qt::KeyboardModifiers(Qt::NoModifier);
  }
  /// Every handle a layer offers, with what dragging it does: two ends for a
  /// line or arrow, four corners for a counter, eight for anything with a box
  /// (the four sides stretch one axis), one for a text's wrap width.
  [[nodiscard]] QVector<QPair<QPointF, Interaction>>
  annotationHandles(const Annotation &annotation) const;
  /// Which handle of the selected layer is under `point`, if any. Asked
  /// before what shape is under the pointer, since a handle can sit outside
  /// the layer it belongs to.
  [[nodiscard]] Interaction selectedHandleAt(const QPointF &point) const;
  [[nodiscard]] Interaction pointerHandle() const;
  [[nodiscard]] Qt::CursorShape handleCursorShape(Interaction handle) const;
  /// Moves the edges a box handle owns, keeping the opposite ones put; Shift
  /// on a corner keeps the proportions.
  void applyBoxResize(Annotation &annotation, Interaction handle,
                      const QPointF &point, const QRectF &original);
public:
  /// Number of annotation layers. Test accessor.
  [[nodiscard]] int annotationCountForTest() const {
    return static_cast<int>(annotations_.size());
  }
  /// Currently armed tool. Test accessor: cursor shape is a poor proxy, since
  /// what is under the pointer changes it.
  [[nodiscard]] Tool armedToolForTest() const { return tool_; }
  /// Number of selected layers. Test accessor.
  [[nodiscard]] int selectedCountForTest() const {
    return static_cast<int>(selectedAnnotations_.size());
  }
  /// Current status line. Test accessor.
  [[nodiscard]] QString statusForTest() const { return status_; }
  /// Whether the selection chrome is currently stepped back for an adjustment.
  /// Test accessor.
  [[nodiscard]] bool selectionFadedForTest() const {
    return adjustingSelection_;
  }
  [[nodiscard]] bool colorPaletteOpenForTest() const {
    return colorPaletteOpen_;
  }
  [[nodiscard]] bool customColorPickerOpenForTest() const {
    return customColorPickerOpen_;
  }
  [[nodiscard]] bool shapeMenuOpenForTest() const { return shapeMenuOpen_; }
  [[nodiscard]] bool textSizeMenuOpenForTest() const { return textSizeMenuOpen_; }

private:
  /// What the armed tool is currently set to, for the status line: shown on
  /// arming so its options are discoverable without trying them.
  [[nodiscard]] QString toolStatus() const;
  [[nodiscard]] int annotationAt(const QPointF &point) const;
  /// Whether the armed tool picks a layer up rather than working over it.
  /// Moves a layer to the top of the stack, remapping every index that
  /// pointed into it. Returns where it ended up.
  int raiseAnnotation(int index);
  [[nodiscard]] bool toolGrabsLayer(int index) const;
  /// Whether a press here would grab a layer, which is what the cursor reads.
  [[nodiscard]] bool pointerGrabsLayer() const;
  /// Topmost layer whose *edge* is under `point`. Areas that a drawing tool
  /// should be able to draw across, such as redactions, spotlights and filled
  /// shapes, are grabbable only by their border, so their interiors stay
  /// canvas.
  [[nodiscard]] int annotationEdgeAt(const QPointF &point) const;
  [[nodiscard]] bool annotationContains(const Annotation &annotation,
                                        const QPointF &point,
                                        bool edgeOnly) const;
  [[nodiscard]] int hoveredSpotlightAt(const QPointF &position) const;
  [[nodiscard]] QRectF normalizedSelection(const QPointF &first,
                                           const QPointF &second) const;
  [[nodiscard]] QRectF colorPaletteRect() const;
  [[nodiscard]] QRectF customColorPanelRect() const;
  [[nodiscard]] QRectF shapeMenuRect() const;
  [[nodiscard]] QRectF textSizePanelRect() const;
  [[nodiscard]] QVector<QRectF> cropHandleRects() const;
  [[nodiscard]] int cropHandleAt(const QPointF &point) const;
  /// Fit-to-window rect for the selection (unaffected by the view zoom/pan).
  [[nodiscard]] QRectF baseImageRect() const;
  /// Top edge the chrome (toolbar, popovers) anchors above: the fit rect at
  /// zoom 1, the viewport band once zoomed (the content fills it then).
  [[nodiscard]] qreal chromeAnchorTop() const;
  /// baseImageRect transformed by the current view zoom and pan (content and
  /// annotations map through this). Equals baseImageRect at zoom 1.
  [[nodiscard]] QRectF editImageRect() const;
  [[nodiscard]] qreal editScale() const;
  /// Multiplier from the fit scale to the largest useful zoom (1 source px ->
  /// a few screen px); 1.0 when the image already fits comfortably.
  [[nodiscard]] qreal maxViewZoom() const;
  /// Set the view zoom (clamped to [1, maxViewZoom]) keeping `focus` (a widget
  /// point) over the same image pixel; then re-clamp the pan.
  void setViewZoom(qreal zoom, const QPointF &focus);
  void panView(const QPointF &delta);
  void resetView();
  void clampViewOffset();
  [[nodiscard]] QPointF toAnnotationPoint(const QPointF &position) const;
  [[nodiscard]] QPointF toUnclampedAnnotationPoint(const QPointF &position) const;
  [[nodiscard]] bool selectedLayerAcceptsPoint(const QPointF &point) const;
  [[nodiscard]] QRectF sourceRect(const QRectF &logicalRect) const;
  [[nodiscard]] QPointF sourcePoint(const QPointF &logicalPoint) const;
  [[nodiscard]] QRectF mapWidgetToPreview(const QRectF &widgetRect) const;
  [[nodiscard]] QRectF mapPreviewToWidget(const QRectF &previewRect) const;
  [[nodiscard]] int windowAt(const QPointF &position) const;
  [[nodiscard]] int windowInDirection(int current, int key) const;
  [[nodiscard]] QVector<ToolbarButton> toolbarButtons() const;
  [[nodiscard]] QColor annotationColor() const;
  [[nodiscard]] QLineF creationSpan(const QPointF &rawEnd) const;
  [[nodiscard]] QPointF
  constrainedResizeEndpoint(const Annotation &annotation,
                            const QPointF &candidate, const QPointF &fixed,
                            const QPointF &originalMoving) const;

  void acceptText();
  void applyCustomColor(const QPointF &position);
  void applyEditState(const EditState &state);
  void cancelActiveDragForHistory();
  void beginText(const QPointF &point, int annotationIndex = -1);
  void chooseWindow(int index);
  void duplicateSelectedAnnotation();
  [[nodiscard]] EditState editState() const;
  void enterEdit(QString status);
  void scheduleSnapshot();
  void startSnapshotRender();
  void pinSnapshot();
  void commitOp(Operation op);
  void commitAnnotate(Annotation annotation);
  void commitPatch(const QVector<int> &indices);
  void commitDelete(const QVector<int> &indices);
  void commitCrop(const QRectF &crop);
  void commitCut(CutOp cut);
  void commitBackground(BackgroundStyle style);
  void replayLog();
  void redoEdit();
  void selectWindowInDirection(int key);
  void finish(OutputMode mode);
  void saveAs();
  void handleEscape();
  void handleToolbar(const QString &action);
  void paintEdit(QPainter &painter);
  void paintSelect(QPainter &painter);
  void refreshBackdropCache();
  void refreshComposedCapture();
  void runOcr(const QRectF &localSelection = {});
  void setStatus(QString status);
  void toggleShapeFill();
  void toggleTextBackground();
  void nudgeSelectedAnnotation(const QPointF &delta);
  void endNudgeRun();
  /// Wheel over a selected layer: weight, not size. Thickness for anything
  /// with a stroke, the counter or text's own size, magnification for a
  /// spotlight, extent for the one kind that is all fill.
  void adjustSelectedAnnotation(int step);
  /// Alt+wheel on the selected layer: a spotlight's ring. False when the layer
  /// has no second setting to move.
  bool adjustSelectedAnnotationRing(int step);
  /// Starts (or extends) the window in which the selection chrome steps back
  /// so a wheel adjustment can be seen. The handles sit exactly where a
  /// corner radius or a spotlight's ring changes, so at full strength they
  /// hide the thing being adjusted.
  void beginSelectionAdjust();
  void undoEdit();
  void updatePointerCursor();

  CaptureData capture_;
  // Untouched capture, kept alongside cuts_ so cuts can be recomposed from
  // scratch (undo/redo, in-progress cut preview) without accumulating error.
  QImage pristineSource_;
  QSize pristineLogicalSize_;
  QVector<CutOp> cuts_;
  Phase phase_ = Phase::Select;
  Tool tool_ = Tool::Select;
  /// Set by the first key event, which carries a fresh modifier snapshot.
  bool modifiersSeen_ = false;
  /// What to hand back to once a color has been sampled: taking a color is
  /// not a change of tool.
  Tool toolBeforeEyedropper_ = Tool::Select;
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
  bool creationCenteredActive_ = false;
  bool resizeConstraintActive_ = false;
  Interaction interaction_ = Interaction::None;
  QVector<QPointF> freehandPoints_;
  // Cut tool live-drag state. cutDragStart_/cutBandLo_/cutBandHi_ and
  // liveCut_.orientation are in annotation space (selection-relative logical
  // px); the source stays untouched while a shaded removal band previews the
  // drag, and the cut is applied only when the pointer is released.
  bool cutDragActive_ = false;
  QPointF cutDragStart_;
  CutOp liveCut_;
  qreal cutBandLo_ = 0.0;
  qreal cutBandHi_ = 0.0;
  qreal cutDragRatio_ = 1.0;
  qreal cutDragOriginOffset_ = 0.0;
  bool windowMode_ = false;
  BackgroundStyle backgroundStyle_ = BackgroundStyle::None;
  bool busy_ = false;
  bool colorPaletteOpen_ = false;
  bool customColorPickerOpen_ = false;
  bool shapeMenuOpen_ = false;
  bool textSizeMenuOpen_ = false;
  QPointF paletteIntentOrigin_;
  QPointF customColorIntentOrigin_;
  QPointF shapeIntentOrigin_;
  QPointF textSizeIntentOrigin_;
  bool usingCustomColor_ = false;
  int hoveredWindow_ = -1;
  int colorIndex_ = 0;
  PaletteConfig paletteConfig_ = defaultPaletteConfig();
  QColor customColor_;
  qreal customHue_ = 0.98;
  int nextMarker_ = 1;
  qreal annotationSize_ = 4.0;
  bool fillShapes_ = false;
  qreal cornerRadius_ = 0.0;
  /// True while a wheel adjustment is in flight; the selection chrome draws
  /// faintly until it settles.
  bool adjustingSelection_ = false;
  QTimer adjustSettleTimer_;
  int textSizeIndex_ = 1;
  TextBackground textBackground_ = TextBackground::Pill;
  qreal spotlightMagnification_ = 2.0;
  /// Ring drawn around a spotlight's opening; 0 draws none.
  qreal spotlightBorder_ = 4.0;
  SpotlightShape spotlightShape_ = SpotlightShape::Ellipse;
  RedactionStyle redactionStyle_ = RedactionStyle::Pixelate;
  quint32 activeRedactionSeed_ = 0;
  QRectF cachedRedactionSelection_;
  QVector<Annotation> cachedCommittedRedactions_;
  // Committed redaction layer at display resolution. Live drag paints the
  // in-progress rect on a copy so committed blocks are not rebuilt per move.
  QImage redactionLayerCache_;
  // Display-resolution selection image reused across redaction drag frames.
  QImage redactionBase_;
  QSize redactionBaseSize_;
  bool redactionBaseStale_ = true;
  // Select-phase capture scaled once per source, widget size, and DPR.
  QPixmap backdrop_;
  QPixmap dimmedBackdrop_;
  QSize backdropSize_;
  qreal backdropRatio_ = 0.0;
  qint64 backdropKey_ = 0;
  // Background/gui-thread snapshot persistence with latest-wins coalescing.
  QFutureWatcher<bool> snapshotWatcher_;
  bool snapshotBusy_ = false;
  bool snapshotDirty_ = false;
  bool snapshotWriteOk_ = true;
  // Set once finish() runs: the working snapshot becomes the exported file and
  // is written at default PNG compression instead of the fast edit encoding.
  bool snapshotOutputRequested_ = false;
  bool suppressSnapshots_ = false;
  bool sourceWritten_ = false;
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
  // Background render for --pin.
  QFutureWatcher<QImage> pinWatcher_;
  bool pinPending_ = false;
  QString pendingPinPath_;
  QVector<Annotation> annotations_;
  QVector<Operation> ops_;
  int opIndex_ = 0;
  quint64 nextAnnotationId_ = 1;
  int selectedAnnotation_ = -1;
  int editingAnnotation_ = -1;
  Annotation originalAnnotation_;
  EditState dragStartState_;
  bool dragStartStateValid_ = false;
  bool dragChanged_ = false;
  QString snapshotPath_;
  QuickOutputMode quickOutputMode_ = QuickOutputMode::None;
  int pinCount_ = 0;
  QString status_ =
      QStringLiteral("Drag to select an area · Space selects a window");
  InlineTextEdit *textEditor_ = nullptr;
  QPointF textPoint_;
  QVector<Annotation> originalSelectedAnnotations_;
  QVector<int> selectedAnnotations_;
  qreal textSize_ = 4.0;
  QElapsedTimer escapeTimer_;
  /// The inline editor's pill and caret are painted by the editor itself
  /// (the multiline editor stays transparent with its own caret hidden) so the
  /// caret can be shorter than Neucha's tall line box.
  bool textEditPill_ = false;
  bool textCaretOn_ = true;
  QTimer textCaretTimer_;
  QElapsedTimer nudgeTimer_;
  QTimer nudgePersistTimer_;
  /// View transform for navigating an oversized capture (e.g. a tall scroll
  /// stitch). `viewZoom_` multiplies the fit scale (1 = whole image visible);
  /// `viewOffset_` pans in widget pixels. Reset on entering edit.
  qreal viewZoom_ = 1.0;
  QPointF viewOffset_;
  bool panning_ = false;
  QPointF panAnchor_;
  QColor textColor_;
  QFutureWatcher<OcrResult> ocrWatcher_;
};

[[nodiscard]] QPointF constrainedCreationEndpoint(CaptureEditor::Tool tool,
                                                  const QPointF &start,
                                                  const QPointF &end);
/** Start point that centers a drag-created shape on `center` given `end`. */
[[nodiscard]] QPointF centeredCreationStart(CaptureEditor::Tool tool,
                                            const QPointF &center,
                                            const QPointF &end);
