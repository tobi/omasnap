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
public:
  enum class CaptureMode { Region, Window, Fullscreen, File };

  explicit CaptureEditor(CaptureData capture,
                         CaptureMode mode = CaptureMode::Region,
                         QWidget *parent = nullptr);
  ~CaptureEditor() override;

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;
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
    Marker,
    Rectangle,
    Redact,
    Text,
    Ocr
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
    int nextMarker = 1;
  };

  [[nodiscard]] QRectF annotationBounds(const Annotation &annotation) const;
  [[nodiscard]] int annotationAt(const QPointF &point) const;
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
  void persistSnapshot();
  void pinSnapshot();
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
  QPointF cursor_;
  bool dragging_ = false;
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
  RedactionStyle redactionStyle_ = RedactionStyle::Pixelate;
  quint32 activeRedactionSeed_ = 0;
  QRectF cachedRedactionSelection_;
  QVector<Annotation> cachedPreviewRedactions_;
  QImage redactionPreviewCache_;
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
  int pinCount_ = 0;
  QString status_ =
      QStringLiteral("Drag to select an area · Space selects a window");
  QLineEdit *textEditor_ = nullptr;
  QPointF textPoint_;
  QElapsedTimer escapeTimer_;
  QColor textColor_;
  qreal textSize_ = 4.0;
  QFutureWatcher<OcrResult> ocrWatcher_;
};
