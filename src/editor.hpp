#pragma once

#include "background-config.hpp"
#include "capture.hpp"
#include "cut.hpp"
#include "overlay-chrome.hpp"
#include "palette-config.hpp"
#include "recent-snaps.hpp"

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QRegion>
#include <QLineF>
#include <QTimer>
#include <QWidget>

#include <optional>

class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QWheelEvent;

class QPainter;

class InlineTextEdit;
class ScrollCapturePanel;
namespace LayerShellQt {
class Window;
}
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
  enum class CaptureMode { Region, Scroll, Window, Fullscreen, File };

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
  /**
   * Blocks until an in-flight export (finish) has completed, letting the
   * event loop run so completeFinish() can act. Headless smoke suite only.
   */
  void waitForExport();
  /**
   * Blocks until an in-flight recents-shelf reopen has completed, letting
   * the event loop run. Headless smoke suite only.
   */
  void waitForReopen();
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
  /** Annotation-space canvas, including any strips grown past the source. */
  [[nodiscard]] QRectF currentCanvasForTest() const { return canvasRect_; }
  [[nodiscard]] CanvasBoundaryMode currentCanvasBoundaryForTest() const {
    return canvasBoundaryMode_;
  }
  /** Fitted canvas and source-frame geometry used by headless interactions. */
  [[nodiscard]] QRectF sourceFrameWidgetRectForTest() const {
    return sourceFrameWidgetRect();
  }
  [[nodiscard]] QPointF annotationPointToWidgetForTest(
      const QPointF &point) const {
    return sourceFrameWidgetRect().topLeft() + point * editScale();
  }
  [[nodiscard]] const QVector<Annotation> &currentAnnotationsForTest() const {
    return annotations_;
  }
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
  void leaveEvent(QEvent *event) override;
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
  enum class Phase { Select, Export, Edit };
  enum class OutputMode { Copy, Save, Both };
  enum class HighlighterMode { Snap, Normal };
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
  /// What the export worker hands back: the saved path (Save/Both) or an
  /// error. Rendering, PNG encoding and the clipboard round trip all run off
  /// the UI thread; a tall scroll capture takes seconds to encode.
  struct FinishResult {
    OutputMode mode = OutputMode::Copy;
    QString saved;
    QString error;
    /// Small flattened preview for the recents shelf.
    QImage thumbnail;
  };
  /// What reopening a shelved capture reads off disk: the full-resolution
  /// source plus its operation log. Loaded on the worker pool, not the UI
  /// thread, since a shelved stitched capture can be tens of megapixels.
  struct ReopenResult {
    RecentSnap recent;
    QImage image;
    OperationLog log;
    QString error;
  };

  struct EditState {
    QVector<Annotation> annotations;
    BackgroundStyle backgroundStyle = BackgroundStyle::None;
    bool imageShadow = true;
    CanvasBoundaryMode canvasBoundary = CanvasBoundaryMode::Framed;
    QRectF selection;
    int selectedAnnotation = -1;
    QVector<int> selectedAnnotations;
    int nextMarker = 1;
    QVector<CutOp> cuts;
  };

  [[nodiscard]] QRectF annotationBounds(const Annotation &annotation) const;
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
  /// Apply a cut as if the user had dragged that band. Test hook: operate on
  /// a fixture raster without going through widget coordinates.
  void applyCutForTest(CutOp cut) { commitCut(std::move(cut)); }
  /// Number of selected layers. Test accessor.
  [[nodiscard]] int selectedCountForTest() const {
    return static_cast<int>(selectedAnnotations_.size());
  }
  /// The layer surface this editor lives on. The scroll state toggles its
  /// keyboard interactivity and input mask while the page underneath is live.
  void setLayerWindow(LayerShellQt::Window *layer) { layer_ = layer; }
  /// Whether the select phase is in scroll mode. Test accessor.
  [[nodiscard]] bool scrollModeForTest() const { return scrollMode_; }
  /// Hands a stitched image to the editor as the scroll panel would. Test hook.
  void adoptStitchedForTest(const QImage &image) { adoptStitched(image); }
  /// Whether the scroll panel is up. Test accessor.
  [[nodiscard]] bool scrollPanelActiveForTest() const {
    return scrollPanel_ != nullptr;
  }
  /// Blocks until the shelf has been listed; false when it is empty.
  bool waitForRecents();
  /// Whether the shelf is fanned out. Test accessor.
  [[nodiscard]] bool recentsOpenForTest() const { return recentsOpen_; }
  /// Widget rect of shelf card `index` at its settled position (fanned when
  /// open, stacked when not), or null. Test accessor.
  [[nodiscard]] QRectF recentCardRectForTest(int index) const;
  [[nodiscard]] int recentCountForTest() const {
    return static_cast<int>(recents_.size());
  }
  /// Current status line. Test accessor.
  [[nodiscard]] QString statusForTest() const { return status_; }
  /// Text-height I-beam in annotation coordinates, or an empty rect when the
  /// Snap highlighter has no text row near the pointer. Test accessor.
  [[nodiscard]] QRectF highlighterPreviewRectForTest() const;
  /// Whether the highlighter is in its default text-row snapping mode.
  [[nodiscard]] bool highlighterSnapModeForTest() const {
    return highlighterMode_ == HighlighterMode::Snap;
  }
  /// Active highlighter button geometry. Test accessor for repeated-tool
  /// behavior without duplicating the responsive toolbar layout.
  [[nodiscard]] QRectF highlighterToolbarRectForTest() const;
  [[nodiscard]] QRegion pointerMotionRegionForTest(const QPointF &point) const {
    return pointerMotionRegion(point);
  }
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
  /// Whether window selection is active in the select phase. Test accessor.
  [[nodiscard]] bool windowModeForTest() const { return windowMode_; }
  /// Where the image is drawn on screen right now (widget pixels), and the
  /// annotation-space-to-widget scale. Test accessor: lets a test compute
  /// exact click/expectation points from real geometry instead of hand math.
  [[nodiscard]] QRectF editImageRectForTest() const { return editImageRect(); }
  [[nodiscard]] qreal editScaleForTest() const { return editScale(); }
  [[nodiscard]] QPointF toAnnotationPointForTest(const QPointF &widget) const {
    return toAnnotationPoint(widget);
  }
  /// Inverse of toAnnotationPoint: annotation-space → widget pixels.
  [[nodiscard]] QPointF toScreenPointForTest(const QPointF &annotation) const {
    const qreal scale = editScale();
    return editImageRect().topLeft() +
           annotation * (scale > 0.001 ? scale : 0.001);
  }
  /// Center of the toolbar button whose action is `action` (e.g.
  /// "tool-arrow"), or (-1,-1) if not found. Test accessor: a click position
  /// that survives the toolbar's own layout changing.
  [[nodiscard]] QPoint toolbarButtonCenterForTest(const QString &action) const {
    const QRectF button = toolbarButtonRectForTest(action);
    return button.isEmpty() ? QPoint(-1, -1) : button.center().toPoint();
  }
  [[nodiscard]] QRectF toolbarButtonRectForTest(const QString &action) const {
    for (const ToolbarButton &button : toolbarButtons())
      if (button.action == action)
        return button.rect;
    return {};
  }
  [[nodiscard]] QRectF colorPaletteRectForTest() const {
    return colorPaletteRect();
  }
  [[nodiscard]] QRectF customColorPanelRectForTest() const {
    return customColorPanelRect();
  }
  [[nodiscard]] QRectF textSizePanelRectForTest() const {
    return textSizePanelRect();
  }
  /// Whether the overlay is still in the select phase. Test accessor.
  [[nodiscard]] bool selectingForTest() const { return phase_ == Phase::Select; }
  /// Whether a confirmed quick capture is being exported without showing Edit.
  [[nodiscard]] bool exportingForTest() const { return phase_ == Phase::Export; }
  /// Whether the annotation editor is visible. Test accessor.
  [[nodiscard]] bool editingForTest() const { return phase_ == Phase::Edit; }
  /// Widget rect of the capture-kind tab with `label`, or null. Test accessor.
  [[nodiscard]] QRectF selectTabRectForTest(const QString &label) const {
    for (const CaptureTab &item : selectTabItems())
      if (captureTabLabel(item.kind) == label)
        return item.rect;
    return {};
  }
  [[nodiscard]] bool textSizeMenuOpenForTest() const { return textSizeMenuOpen_; }

private:
  /// What the armed tool is currently set to, for the status line: shown on
  /// arming so its options are discoverable without trying them.
  [[nodiscard]] QString toolStatus() const;
  [[nodiscard]] QString highlighterStatus() const;
  [[nodiscard]] QString highlighterTooltip() const;
  void activateHighlighter();
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
  /// Top of the toolbar row: just under the tab strip's fixed bottom edge,
  /// independent of the image, so the two can never overlap.
  [[nodiscard]] qreal toolbarTop() const;
  /// How much vertical room the tab strip and toolbar actually need, at the
  /// current window width — the image's top margin, not a guessed constant.
  [[nodiscard]] qreal imageTopMargin() const;
  /// baseImageRect transformed by the current view zoom and pan (content and
  /// annotations map through this). Equals baseImageRect at zoom 1.
  [[nodiscard]] QRectF editImageRect() const;
  /// The unmodified screenshot's frame inside the possibly-grown canvas.
  [[nodiscard]] QRectF sourceFrameWidgetRect() const;
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
  /// Counters sit just ahead of the pointing-hand hotspot, as though its tip
  /// is placing them. The lead is screen-space so zooming never moves the
  /// counter closer to or farther from the cursor.
  [[nodiscard]] QPointF markerPlacementPoint(const QPointF &position) const;
  [[nodiscard]] bool selectedLayerAcceptsPoint(const QPointF &point) const;
  [[nodiscard]] QRectF sourceRect(const QRectF &logicalRect) const;
  [[nodiscard]] QPointF sourcePoint(const QPointF &logicalPoint) const;
  [[nodiscard]] QRectF mapWidgetToPreview(const QRectF &widgetRect) const;
  [[nodiscard]] QRectF mapPreviewToWidget(const QRectF &previewRect) const;
  [[nodiscard]] int windowAt(const QPointF &position) const;
  [[nodiscard]] int windowInDirection(int current, int key) const;
  /// Toolbar buttons laid out left to right in their logical groups
  /// (history, style, tools, actions). When `groupDividers` is non-null, the
  /// midpoint x of each gap between groups is appended to it, in widget
  /// space, for the divider lines drawn between clusters.
  [[nodiscard]] QVector<ToolbarButton>
  toolbarButtons(QVector<qreal> *groupDividers = nullptr,
                 bool includeSubmenus = true) const;
  [[nodiscard]] QRectF toolbarButtonRect(const QString &action) const;
  [[nodiscard]] QColor annotationColor() const;
  [[nodiscard]] QLineF creationSpan(const QPointF &rawEnd) const;
  [[nodiscard]] QPointF
  constrainedResizeEndpoint(const Annotation &annotation,
                            const QPointF &candidate, const QPointF &fixed,
                            const QPointF &originalMoving) const;

  /// Commits what is in the inline editor. `keepSelected` leaves the text
  /// layer selected afterwards (Esc), so Backspace or Del can remove it.
  void acceptText(bool keepSelected = false);
  void applyCustomColor(const QPointF &position);
  void applyEditState(const EditState &state);
  void cancelActiveDragForHistory();
  /// Opens the inline editor at `point` with room for `lineCapacity` lines:
  /// Enter moves to the next line while there is room, and commits on the
  /// last one; Shift+Enter always adds a line's room.
  void beginText(const QPointF &point, int annotationIndex = -1,
                 int lineCapacity = 1);
  void chooseWindow(int index);
  /// Capture-kind tabs across the top of the select overlay. Region and
  /// Window are modes (one is always lit); Fullscreen acts at once.
  using SelectTab = CaptureKind;
  [[nodiscard]] QVector<CaptureTab> selectTabItems() const;
  [[nodiscard]] int selectTabAt(const QPointF &position) const;
  void activateSelectTab(SelectTab tab);
  void setWindowMode(bool enabled);
  void setScrollMode(bool enabled);
  void selectFullscreen();
  /// Back from the editor to the select phase: the op log is dropped and the
  /// frozen screen is offered again for a new region or window.
  void returnToSelect(bool windowMode);
  /// Scroll capture takes over the surface with `region` drawn.
  void startScrollCapture(const QRect &region);
  /// Tears the scroll panel down; the surface is whole again.
  void endScrollCapture();
  /// A stitched scroll capture becomes the image being edited.
  void adoptStitched(const QImage &image);
  /// The editor's other mode of working: not a region of the frozen screen
  /// but an image handed to it, with the op log it was last edited with.
  /// `kind` is the tab lit for it.
  void adoptImage(QImage image, OperationLog log, SelectTab kind,
                  const QString &status);
  /// Leaves the select phase with a drawn region: edit it, or scroll it.
  void commitRegion(const QRectF &region, const QString &editStatus);
  /// Whether there is a live screen behind this capture to re-select from
  /// (not a file, clipboard image or stitched result).
  [[nodiscard]] bool hasLiveScreen() const;
  /// Small pill under the image in the edit phase offering scroll capture of
  /// the drawn region; null when not offered.
  [[nodiscard]] QRectF scrollPillRect() const;
  void paintSelectTabs(QPainter &painter);
  /// The shelf of earlier captures along the right edge of the select
  /// overlay: a stack of small cards that fans out under the pointer, each
  /// reopening its capture in place of taking a new one.
  struct RecentCard {
    QRectF rect;
    qreal rotation = 0.0;
  };
  void loadRecents();
  [[nodiscard]] QVector<RecentCard> recentCards(qreal fan) const;
  [[nodiscard]] QRectF recentsHotZone() const;
  [[nodiscard]] int recentAt(const QPointF &position) const;
  void setRecentsOpen(bool open);
  void trackRecentsHover();
  void paintRecents(QPainter &painter);
  void reopenRecent(int index);
  void completeReopenRecent(const ReopenResult &result);
  void completeBackdropLoad();
  void seedConfiguredBackground(BackgroundStyle style);
  void duplicateSelectedAnnotation();
  [[nodiscard]] EditState editState() const;
  void refreshCanvasRect();
  [[nodiscard]] bool canvasGrown() const;
  [[nodiscard]] BackgroundStyle effectiveBackgroundStyle() const;
  void enterEdit(QString status);
  /// Routes a confirmed screen selection to quick export or the editor.
  void enterSelectedCapture(QString editStatus);
  /// A confirmed quick capture exports in place without exposing editor chrome.
  void enterExport();
  void ensureTextEditor();
  [[nodiscard]] bool textEditing() const;
public:

private:
  void scheduleSnapshot();
  void startSnapshotRender();
  void pinSnapshot();
  void commitOp(Operation op);
  void commitAnnotate(Annotation annotation);
  void commitPatch(const QVector<int> &indices);
  void commitDelete(const QVector<int> &indices);
  void commitCrop(const QRectF &crop);
  void commitCut(CutOp cut);
  void commitBackground(BackgroundStyle style, bool imageShadow);
  void commitCanvasBoundary(CanvasBoundaryMode mode);
  void cycleCanvasBoundary(bool reverse);
  void cycleBackground();
  void replayLog();
  void redoEdit();
  void selectWindowInDirection(int key);
  void finish(OutputMode mode);
  void completeFinish(const FinishResult &result);
  void handleEscape();
  void handleToolbar(const QString &action);
  void paintEdit(QPainter &painter);
  void paintSelect(QPainter &painter);
  void refreshBackdropCache();
  void refreshComposedCapture();
  void runOcr(const QRectF &localSelection = {});
  void dismissOcrOverlay();
  void paintOcrOverlay(QPainter &painter, const QRectF &image, qreal scale);
  void setStatus(QString status);
  [[nodiscard]] QRegion pointerMotionRegion(const QPointF &point) const;
  void queuePointerRepaint(const QRegion &damage);
  void toggleShapeFill();
  void toggleTextBackground();
  void cycleTextFont();
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
  bool scrollMode_ = false;
  /// The image being edited was handed to the editor (stitched scroll,
  /// shelved capture) rather than cut from the frozen screen.
  bool handedImage_ = false;
  /// The monitor as captured, kept apart from capture_.monitor (which a
  /// stitched result replaces) so the screen can be captured again.
  MonitorInfo liveMonitor_;
  [[nodiscard]] CaptureKind selectKind() const;
  LayerShellQt::Window *layer_ = nullptr;
  ScrollCapturePanel *scrollPanel_ = nullptr;
  CaptureMode captureMode_ = CaptureMode::Region;
  /// Which tab produced the capture being edited; lit in the edit phase.
  SelectTab editedKind_ = SelectTab::Region;
  std::optional<RecentSnap> editingRecent_;
  QVector<RecentSnap> recents_;
  QFutureWatcher<QVector<RecentSnap>> recentsWatcher_;
  bool recentsLoading_ = false;
  bool recentsOpen_ = false;
  int hoveredRecent_ = -1;
  /// 0 = stacked, 1 = fanned; eased between the two by recentsAnimTimer_.
  qreal recentsFan_ = 0.0;
  qreal recentsFanFrom_ = 0.0;
  QElapsedTimer recentsAnimClock_;
  QTimer recentsAnimTimer_;
  QRectF selection_;
  // Annotation coordinates stay anchored to the source frame at 0,0. This
  // derived rect expands around them without translating either the source or
  // existing layers; replaying the op log reconstructs it exactly.
  QRectF canvasRect_;
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
  struct HighlighterLock {
    qreal centerY = 0.0;
    qreal annotationSize = 0.0;
  };
  struct HighlighterProbeResult {
    quint64 generation = 0;
    QPointF annotationPoint;
    std::optional<HighlighterLock> lock;
  };
  void scheduleHighlighterProbe(const QPointF &annotationPoint);
  void completeHighlighterProbe();
  /// Set when a highlighter drag begins near a detected screenshot text row.
  /// Coordinates and size are selection-relative logical pixels, so the lock
  /// survives view zoom and native/fractional monitor scaling.
  std::optional<HighlighterLock> highlighterLock_;
  /// Detected row supplying the Snap cursor's height. Before mouse-down its
  /// center follows the pointer; during a locked drag it follows the row.
  std::optional<HighlighterLock> highlighterPreview_;
  std::optional<QPointF> highlighterPreviewPoint_;
  std::optional<QPointF> pendingHighlighterProbePoint_;
  quint64 highlighterProbeGeneration_ = 0;
  QFutureWatcher<HighlighterProbeResult> highlighterProbeWatcher_;
  HighlighterMode highlighterMode_ = HighlighterMode::Snap;
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
  bool imageShadow_ = true;
  CanvasBoundaryMode canvasBoundaryMode_ = CanvasBoundaryMode::Framed;
  /** `[background]` config: custom image path and the style a fresh capture
   *  starts with. Loaded once in the constructor. */
  BackgroundConfig backgroundConfig_;
  /** Loaded from `backgroundConfig_.imagePath`; null when unset or the file
   *  failed to load, in which case `BackgroundStyle::Custom` is unavailable. */
  QImage customBackdrop_;
  bool configuredCustomDefaultPending_ = false;
  std::optional<QString> pendingSelectedCapture_;
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
  /// Typeface for the next label; Shift+T cycles it without changing Neucha's
  /// role as the session default.
  TextFont textFont_ = TextFont::Neucha;
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
  // Select-phase capture scaled and dimmed once per source, widget size, and DPR.
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
  bool firstPaintReported_ = false;
  CaptureMode pendingMode_ = CaptureMode::Region;
  // Background render for --pin.
  /// Path on success, empty + error set on failure. The render and the PNG
  /// write both happen in the worker; nothing but launching the pin process
  /// happens back on the UI thread.
  struct PinResult {
    QString path;
    QString error;
  };
  QFutureWatcher<PinResult> pinWatcher_;
  bool pinPending_ = false;
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
  /// Typeface held by the active inline draft (existing layer or next-label
  /// default), kept alongside textSize_ so its baseline does not jump.
  TextFont textEditFont_ = TextFont::Neucha;
  QElapsedTimer escapeTimer_;
  /// The inline editor's pill and caret are painted by the editor itself
  /// (the multiline editor stays transparent with its own caret hidden) so the
  /// caret follows the selected face's glyph box instead of its whole line box.
  bool textEditPill_ = false;
  /// How many lines the current text entry has room for (see beginText).
  int textLineCapacity_ = 1;
  /// Wrap width of the text being typed, in image px; 0 wraps at the canvas
  /// edge. Carried onto the layer when the text is committed.
  qreal textEditWrapWidth_ = 0.0;
  bool textCaretOn_ = true;
  QTimer textCaretTimer_;
  QElapsedTimer nudgeTimer_;
  QTimer nudgePersistTimer_;
  QTimer pointerRepaintTimer_;
  QRegion pendingPointerDamage_;
  /// View transform for navigating an oversized capture (e.g. a tall scroll
  /// stitch). `viewZoom_` multiplies the fit scale (1 = whole image visible);
  /// `viewOffset_` pans in widget pixels. Reset on entering edit.
  qreal viewZoom_ = 1.0;
  QPointF viewOffset_;
  bool panning_ = false;
  QPointF panAnchor_;
  QColor textColor_;
  QFutureWatcher<OcrResult> ocrWatcher_;
  QFutureWatcher<FinishResult> finishWatcher_;
  QFutureWatcher<ReopenResult> reopenWatcher_;
  QFutureWatcher<QImage> backdropWatcher_;
  bool reopenPending_ = false;
  /// The region being read (annotation coordinates). While tesseract runs a
  /// scan band sweeps it; once done the recognized text sits over it for a
  /// few seconds (or until the next click/key) so you can see what was copied.
  QRectF ocrRegion_;
  QString ocrResultText_;
  QElapsedTimer ocrClock_;
  QTimer ocrAnimTimer_;
  QTimer ocrResultTimer_;
};

[[nodiscard]] QPointF constrainedCreationEndpoint(CaptureEditor::Tool tool,
                                                  const QPointF &start,
                                                  const QPointF &end);
/** Start point that centers a drag-created shape on `center` given `end`. */
[[nodiscard]] QPointF centeredCreationStart(CaptureEditor::Tool tool,
                                            const QPointF &center,
                                            const QPointF &end);
