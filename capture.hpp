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
  /** Logical size the source is presented at; source pixels stay native. */
  QSize previewSize;
  QVector<WindowTarget> windows;
};

enum class BackgroundStyle { None, Aurora, Sunset, Lagoon, Violet };
enum class QuickOutputMode { None, Copy, Save, Both };

enum class SpotlightShape { Ellipse, Rectangle, RoundedRectangle };
enum class RedactionStyle { Solid, Pixelate };

/** How much zlib effort a snapshot write spends. Working snapshots are
 *  crash-recovery files on tmpfs that every edit rewrites, so they use Fast.
 *  Anything the user keeps — the saved screenshot, a pinned capture — uses
 *  Default so the file stays small. */
enum class SnapshotEncoding { Default, Fast };

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
/** Captures the focused monitor; window discovery runs only when requested. */
[[nodiscard]] bool captureFocusedMonitor(CaptureData &capture,
                                         bool includeWindows, QString &error);
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
/** Creates or repairs a private directory owned by the current user. */
[[nodiscard]] bool ensurePrivateDirectory(const QString &path);
/** Returns Omasnap's private runtime directory, or empty on failure. */
[[nodiscard]] QString secureRuntimeDirectory();
[[nodiscard]] QString moveSnapshotToScreenshots(const QString &sourcePath,
                                                QString &error);
[[nodiscard]] QString temporarySnapshotPath();
[[nodiscard]] QString pinnedSnapshotPath(int index);
void prunePinnedSnapshots();
[[nodiscard]] bool saveTemporarySnapshot(
    const QImage &image, QString path, QString &error,
    SnapshotEncoding encoding = SnapshotEncoding::Default);
[[nodiscard]] QString recognizeText(const QImage &image, QString &error);
void sendCaptureNotification(const QString &message,
                             const QString &imagePath = {});
