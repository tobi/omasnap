#pragma once

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
  enum class Kind { Arrow, Line, Freehand, Marker, Rectangle, Text };

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
[[nodiscard]] QImage renderCapture(const CaptureData &capture,
                                   const QRectF &selection,
                                   const QVector<Annotation> &annotations,
                                   BackgroundStyle backgroundStyle);
[[nodiscard]] bool copyPngFileToClipboard(const QString &path, QString &error);
[[nodiscard]] bool copyTextToClipboard(const QString &text, QString &error);
void paintAnnotation(QPainter &painter, const Annotation &annotation);
void paintCaptureBackground(QPainter &painter, const QRectF &bounds,
                            BackgroundStyle backgroundStyle);
[[nodiscard]] QString moveSnapshotToScreenshots(const QString &sourcePath,
                                                QString &error);
[[nodiscard]] QString temporarySnapshotPath();
[[nodiscard]] QString pinnedSnapshotPath(int index);
void prunePinnedSnapshots();
[[nodiscard]] bool saveTemporarySnapshot(const QImage &image, QString path,
                                         QString &error);
[[nodiscard]] QString recognizeText(const QImage &image, QString &error);
void sendCaptureNotification(const QString &message,
                             const QString &imagePath = {});
