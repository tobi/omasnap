/** @fileoverview Declares screenshot capture, rendering, and output types. */
#pragma once

#include <cstdint>

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

struct Annotation {
  enum class Kind {
    Arrow,
    Line,
    Freehand,
    Highlighter,
    Marker,
    Rectangle,
    Text
  };

  Kind kind = Kind::Arrow;
  QPointF start;
  QPointF end;
  QString text;
  QColor color;
  qreal size = 4.0;
  int number = 0;
  QVector<QPointF> points;
};

[[nodiscard]] bool loadCaptureFonts();
[[nodiscard]] QFont annotationTextFont(qreal size);
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
[[nodiscard]] bool copyTextToClipboard(const QString &text, QString &error);
void paintAnnotation(QPainter &painter, const Annotation &annotation);
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
/** Removes abandoned pin files without touching active pins. */
void prunePinnedSnapshots();
[[nodiscard]] bool saveTemporarySnapshot(const QImage &image, QString path,
                                         QString &error);
[[nodiscard]] QString recognizeText(const QImage &image, QString &error);
void sendCaptureNotification(const QString &message,
                             const QString &imagePath = {});
