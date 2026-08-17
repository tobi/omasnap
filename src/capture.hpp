/** @fileoverview Declares screenshot capture, rendering, and output types. */
#pragma once

#include <cstdint>

#include <QPainterPath>
#include <QColor>
#include <QImage>
#include <QRect>
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
  QImage preview;
  QVector<WindowTarget> windows;
};

enum class BackgroundStyle { None, Aurora, Sunset, Lagoon, Violet };
enum class QuickOutputMode { None, Copy, Save, Both };

enum class SpotlightShape { Ellipse, Rectangle, RoundedRectangle };
enum class RedactionStyle { Solid, Pixelate };

struct Annotation {
  enum class Kind {
    Arrow,
    Line,
    Freehand,
    Highlighter,
    Marker,
    Rectangle,
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
  RedactionStyle redactionStyle = RedactionStyle::Pixelate;
  qreal magnification = 2.0;
  SpotlightShape spotlightShape = SpotlightShape::Ellipse;
  quint32 redactionSeed = 0;

  bool operator==(const Annotation &) const = default;
};

[[nodiscard]] bool loadCaptureFonts();
[[nodiscard]] QFont annotationTextFont(qreal size);
/**
 * Discovers the focused monitor (name, geometry, scale). Fast: only one
 * `hyprctl monitors` call. Safe to call on the main thread to position the
 * overlay before the pixel capture itself runs in the background.
 */
[[nodiscard]] bool probeFocusedMonitor(MonitorInfo &monitor, QString &error);
/**
 * Captures the focused monitor's pixels and window list onto the given
 * monitor. Pure I/O and image work (no GUI objects): safe on any thread.
 */
[[nodiscard]] bool captureMonitorPixels(const MonitorInfo &monitor,
                                        CaptureData &capture, QString &error);
/** Convenience: probes the focused monitor, then captures its pixels. */
[[nodiscard]] bool captureFocusedMonitor(CaptureData &capture, QString &error);
[[nodiscard]] bool captureWindowSurface(const WindowTarget &window,
                                        QImage &image, QString &error);
/** Returns an upright image for captured Wayland buffer contents. */
[[nodiscard]] QImage normalizeWaylandCapture(const QImage &image,
                                             std::uint32_t transform);
[[nodiscard]] QImage renderCapture(const CaptureData &capture,
                                   const QRectF &selection,
                                   const QVector<Annotation> &annotations,
                                   BackgroundStyle backgroundStyle);
[[nodiscard]] bool copyPngFileToClipboard(const QString &path, QString &error);
[[nodiscard]] bool quickOutput(const QImage &image, QuickOutputMode mode,
                               QString &error);
[[nodiscard]] bool copyTextToClipboard(const QString &text, QString &error);
void paintAnnotation(QPainter &painter, const Annotation &annotation);
[[nodiscard]] QPainterPath spotlightPath(const Annotation &annotation);
void paintSpotlights(QPainter &painter, const QImage &source,
                     const QRectF &targetBounds, const QRectF &sourceRect,
                     const QVector<Annotation> &annotations);
void paintCaptureBackground(QPainter &painter, const QRectF &bounds,
                            BackgroundStyle backgroundStyle);
/**
 * Renders the selection region at `targetSize` for redaction previews. The
 * result carries no annotations; callers overlay redactions with
 * applyRedactionsScaled and cache it while the selection is unchanged.
 */
[[nodiscard]] QImage renderSelectionBase(const CaptureData &capture,
                                         const QRectF &selection,
                                         const QSize &targetSize);
/**
 * Paints redaction annotations over a display-resolution selection image. The
 * source image MUST be the exact selection region scaled to `targetSize`;
 * annotations live in the same logical coordinate space as `selection`.
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
void sendCaptureNotification(const QString &message,
                             const QString &imagePath = {});
