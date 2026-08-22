/** @fileoverview Declares screenshot capture, rendering, and output types. */
#pragma once

#include "cut.hpp"

#include <cstdint>
#include <memory>

#include <QPainterPath>
#include <QColor>
#include <QImage>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QVector>

class QFont;
class QPainter;
class QProcess;

struct MonitorInfo {
  QString name;
  QRect geometry;
  QSize pixelSize;
  qreal scale = 1.0;
  int workspaceId = 0;
};

struct WindowTarget {
  QRect rect;
  QString stableId;
  QString title;
};

struct CaptureData {
  MonitorInfo monitor;
  QImage source;
  /** Logical size the native source image is presented at. */
  QSize previewSize;
  QVector<WindowTarget> windows;
};

enum class BackgroundStyle { None, Aurora, Sunset, Lagoon, Violet };
enum class QuickOutputMode { None, Copy, Save, Both };

enum class SpotlightShape { Ellipse, Rectangle, RoundedRectangle };
enum class RedactionStyle { Solid, Pixelate };
enum class TextBackground { Plain, Pill };

struct Annotation {
  enum class Kind {
    Arrow,
    Line,
    Freehand,
    Highlighter,
    Marker,
    Rectangle,
    Ellipse,
    Text,
    Redaction,
    Spotlight
  };

  Kind kind = Kind::Arrow;
  QPointF start;
  QPointF end;
  QString text;
  QColor color;
  qreal size = 4.0;
  int number = 0;
  QVector<QPointF> points;
  bool filled = false;
  qreal cornerRadius = 0.0;
  RedactionStyle redactionStyle = RedactionStyle::Pixelate;
  qreal magnification = 2.0;
  SpotlightShape spotlightShape = SpotlightShape::Ellipse;
  quint32 redactionSeed = 0;
  TextBackground textBackground = TextBackground::Pill;
  quint64 id = 0;

  bool operator==(const Annotation &) const = default;
};

struct Operation {
  enum class Type { Crop, Background, Annotate, Patch, Delete, Cut };

  Type type = Type::Annotate;
  QRectF crop;
  BackgroundStyle background = BackgroundStyle::None;
  QVector<Annotation> annotations;
  QVector<quint64> ids;
  CutOp cut;

  bool operator==(const Operation &) const = default;
};

struct OperationLog {
  QVector<Operation> ops;
  int index = 0;
  quint64 nextId = 1;
  int nextMarker = 1;

  bool operator==(const OperationLog &) const = default;
};

enum class AnnotationLayer { Redaction, Default };

[[nodiscard]] constexpr AnnotationLayer annotationLayer(Annotation::Kind kind) {
  return kind == Annotation::Kind::Redaction ? AnnotationLayer::Redaction
                                             : AnnotationLayer::Default;
}

[[nodiscard]] bool loadCaptureFonts();
[[nodiscard]] QFont annotationTextFont(qreal size);
/**
 * Discovers the focused monitor (name, geometry, scale). Fast: only one
 * `hyprctl monitors` call. Safe to call on the main thread to position the
 * overlay after the pixel capture has already produced a frozen frame.
 */
[[nodiscard]] bool probeFocusedMonitor(MonitorInfo &monitor, QString &error);
/**
 * Captures the focused monitor's pixels onto the given monitor, and its window
 * list when `includeWindows` is set. Window discovery runs alongside the screen
 * grab, so callers that never show the overlay should skip it. Pure I/O and
 * image work (no GUI objects): safe on any thread.
 */
[[nodiscard]] bool captureMonitorPixels(const MonitorInfo &monitor,
                                        CaptureData &capture,
                                        bool includeWindows, QString &error);
/** Convenience: probes the focused monitor, then captures its pixels. */
[[nodiscard]] bool captureFocusedMonitor(CaptureData &capture,
                                         bool includeWindows, QString &error);
/** Bounds of a text layer's glyph box, or of its readability pill when it
 *  has one; `start` is the baseline origin. */
[[nodiscard]] QRectF annotationTextBounds(const Annotation &annotation);
/** Captures the named output through ext-image-copy-capture. */
/** A live native capture session for one output (`MonitorInfo::name`, e.g.
 *  "DP-3") over its own Wayland connection: open once, then grab frames
 *  repeatedly into the same buffer: a scroll capture takes many per second
 *  and must not pay a process spawn or a session handshake for each. Frames
 *  are captured without the cursor and returned upright in output pixels. */
class OutputCapture {
public:
  OutputCapture();
  ~OutputCapture();
  OutputCapture(const OutputCapture &) = delete;
  OutputCapture &operator=(const OutputCapture &) = delete;
  [[nodiscard]] bool open(const QString &outputName, QString &error);
  /// Grab the next frame. `timeoutMs` bounds the wait for the compositor to
  /// deliver damage (a fully static output would otherwise block up to 2 s).
  /// Returns false on timeout as well as on real failures, and `error` is set
  /// either way; poll sessionStopped() to tell a dead session from a quiet
  /// screen and simply retry the rest.
  [[nodiscard]] bool grab(QImage &image, QString &error, int timeoutMs = 2000);
  [[nodiscard]] bool isOpen() const;
  /// True once the compositor has stopped the session (output gone, mode
  /// change it will not resume from); further grabs cannot succeed.
  [[nodiscard]] bool sessionStopped() const;
  /** Pixel size the compositor announced for frames (empty until open). */
  [[nodiscard]] QSize bufferSize() const;
  void close();

private:
  struct State;
  std::unique_ptr<State> state_;
};

[[nodiscard]] bool captureOutputSurface(const MonitorInfo &monitor,
                                        QImage &image, QString &error);
[[nodiscard]] QString operationLogPath(const QString &imagePath);
[[nodiscard]] bool saveOperationLog(const QString &path, const OperationLog &log,
                                    QString &error);
[[nodiscard]] bool loadOperationLog(const QString &path, OperationLog &log,
                                    QString &error);
[[nodiscard]] QString temporaryExportPath();
/** Returns an upright image for captured Wayland buffer contents. */
[[nodiscard]] QImage normalizeWaylandCapture(const QImage &image,
                                             std::uint32_t transform);
[[nodiscard]] QImage renderCapture(const CaptureData &capture,
                                   const QRectF &selection,
                                   const QVector<Annotation> &annotations,
                                   BackgroundStyle backgroundStyle);
/** Loads the current Wayland clipboard image. */
[[nodiscard]] bool loadClipboardImage(QImage &image, QString &error);
[[nodiscard]] bool copyPngFileToClipboard(const QString &path, QString &error);
[[nodiscard]] bool copyImageToClipboard(const QImage &image, QString &error);
[[nodiscard]] bool quickOutput(const QImage &image, QuickOutputMode mode,
                               QString &error);
[[nodiscard]] bool copyTextToClipboard(const QString &text, QString &error);
void paintAnnotation(QPainter &painter, const Annotation &annotation);
[[nodiscard]] QPainterPath spotlightPath(const Annotation &annotation);
void paintSpotlights(QPainter &painter, const QImage &source,
                     const QRectF &targetBounds, const QRectF &sourceRect,
                     const QVector<Annotation> &annotations);
/**
 * Paints the default annotation layer (spotlights, then vectors) in selection
 * space. Spotlights sample `redacted`, which must already include the
 * redaction layer so a loupe cannot magnify source pixels.
 */
void paintDefaultLayer(QPainter &painter, const QImage &redacted,
                       const QRectF &logicalBounds,
                       const QVector<Annotation> &annotations);
void paintCaptureBackground(QPainter &painter, const QRectF &bounds,
                            BackgroundStyle backgroundStyle);
/**
 * Renders the selection region at `targetSize` for the redaction layer. The
 * result carries no annotations; callers overlay redactions with
 * applyRedactionsScaled and cache it while the selection is unchanged.
 */
[[nodiscard]] QImage renderSelectionBase(const CaptureData &capture,
                                         const QRectF &selection,
                                         const QSize &targetSize);
/**
 * Paints redaction annotations over a display-resolution selection image. The
 * source image MUST be the exact selection region scaled to `targetSize`;
 * annotations are selection-relative, spanning 0..`selection` size.
 */
QImage applyRedactionsScaled(QImage image, const QVector<Annotation> &redactions,
                             const QRectF &selection, const QSizeF &targetSize);
/** Creates or repairs a private directory owned by the current user. */
[[nodiscard]] bool ensurePrivateDirectory(const QString &path);
/** Returns Omasnap's private runtime directory, or empty on failure. */
[[nodiscard]] QString secureRuntimeDirectory();
[[nodiscard]] QString moveSnapshotToScreenshots(const QString &sourcePath,
                                                QString &error);
[[nodiscard]] QString temporarySnapshotPath();
[[nodiscard]] QString pinnedSnapshotPath(int index);
void prunePinnedSnapshots();
/**
 * Writes `image` into the private runtime directory. `quality` is the Qt PNG
 * quality knob, which maps inversely onto zlib levels: -1 keeps the default
 * level, higher values compress less and encode faster.
 */
[[nodiscard]] bool saveTemporarySnapshot(const QImage &image, QString path,
                                         QString &error, int quality = -1);
[[nodiscard]] QString recognizeText(const QImage &image, QString &error);
/** Quotes a string for a shell argument passed to omarchy-notification-send. */
[[nodiscard]] QString shellQuote(QString value);
void sendCaptureNotification(const QString &message,
                             const QString &imagePath = {});
