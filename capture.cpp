/** @fileoverview Captures, renders, saves, and shares screenshots. */
#include "capture.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryFile>

#include <QUrl>
#include <algorithm>
#include <cmath>

bool loadCaptureFonts() {
  static const int fontId =
      QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Neucha.ttf"));
  return fontId >= 0;
}

QFont annotationTextFont(qreal size) {
  static_cast<void>(loadCaptureFonts());
  QFont font(QStringLiteral("Neucha"));
  font.setWeight(QFont::Normal);
  font.setItalic(false);
  font.setPixelSize(qRound(std::max<qreal>(18.0, size * 5.0)));
  return font;
}

namespace {
struct ProcessResult {
  QByteArray output;
  QByteArray error;
  int exitCode = -1;
  bool finished = false;
};

ProcessResult runProcess(const QString &program, const QStringList &arguments,
                         const QByteArray &input = {}, int timeoutMs = 10000) {
  QProcess process;
  process.setProcessChannelMode(QProcess::SeparateChannels);
  process.start(program, arguments);
  if (!process.waitForStarted(2000))
    return {{}, process.errorString().toUtf8(), -1, false};

  if (!input.isEmpty())
    process.write(input);
  process.closeWriteChannel();
  const bool finished = process.waitForFinished(timeoutMs);
  if (!finished)
    process.kill();
  return {process.readAllStandardOutput(), process.readAllStandardError(),
          finished ? process.exitCode() : -1, finished};
}

bool copyToWaylandClipboard(const QString &mimeType, const QByteArray &payload,
                            QString &error) {
  QByteArray lastError;
  for (int attempt = 0; attempt < 2; ++attempt) {
    const ProcessResult copied =
        runProcess(QStringLiteral("wl-copy"),
                   {QStringLiteral("--type"), mimeType}, payload, 5000);
    if (!copied.finished || copied.exitCode != 0) {
      lastError = copied.error;
      continue;
    }

    const ProcessResult verified = runProcess(
        QStringLiteral("wl-paste"),
        {QStringLiteral("--no-newline"), QStringLiteral("--type"), mimeType},
        {}, 5000);
    if (verified.finished && verified.exitCode == 0 &&
        verified.output == payload)
      return true;
    lastError = verified.error;
    if (lastError.isEmpty())
      lastError = QByteArrayLiteral("clipboard verification did not match");
  }
  error = QStringLiteral("Could not persist clipboard: %1")
              .arg(QString::fromUtf8(lastError).trimmed());
  return false;
}

QString runtimePath(const QString &name) {
  QString runtime =
      QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
  if (runtime.isEmpty())
    runtime = QDir::tempPath();
  return QDir(runtime).filePath(name);
}

QString screenshotTargetPath(QString &error) {
  QString root = qEnvironmentVariable("OMASNAP_SCREENSHOT_DIR");
  if (root.isEmpty())
    root =
        QDir(QStandardPaths::writableLocation(QStandardPaths::PicturesLocation))
            .filePath(QStringLiteral("Screenshots"));
  if (!QDir().mkpath(root)) {
    error =
        QStringLiteral("Could not create screenshot directory: %1").arg(root);
    return {};
  }

  const QString stem = QStringLiteral("screenshot-%1")
                           .arg(QDateTime::currentDateTime().toString(
                               QStringLiteral("yyyy-MM-dd_HH-mm-ss")));
  QString path = QDir(root).filePath(stem + QStringLiteral(".png"));
  for (int suffix = 2; QFile::exists(path); ++suffix)
    path =
        QDir(root).filePath(QStringLiteral("%1-%2.png").arg(stem).arg(suffix));
  return path;
}

bool parseMonitor(const QByteArray &json, MonitorInfo &monitor,
                  QString &error) {
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
    error = QStringLiteral("Could not parse Hyprland monitors: %1")
                .arg(parseError.errorString());
    return false;
  }

  for (const QJsonValue value : document.array()) {
    const QJsonObject object = value.toObject();
    if (!object.value(QStringLiteral("focused")).toBool())
      continue;

    const qreal scale = object.value(QStringLiteral("scale")).toDouble(1.0);
    const int rawWidth = object.value(QStringLiteral("width")).toInt();
    const int rawHeight = object.value(QStringLiteral("height")).toInt();
    const int transform = object.value(QStringLiteral("transform")).toInt();
    int logicalWidth =
        static_cast<int>(std::floor(rawWidth / std::max<qreal>(scale, 0.01)));
    int logicalHeight =
        static_cast<int>(std::floor(rawHeight / std::max<qreal>(scale, 0.01)));
    if (transform == 1 || transform == 3 || transform == 5 || transform == 7)
      std::swap(logicalWidth, logicalHeight);

    monitor.name = object.value(QStringLiteral("name")).toString();
    monitor.geometry = {object.value(QStringLiteral("x")).toInt(),
                        object.value(QStringLiteral("y")).toInt(), logicalWidth,
                        logicalHeight};
    monitor.pixelSize = {rawWidth, rawHeight};
    monitor.scale = scale;
    monitor.workspaceId = object.value(QStringLiteral("activeWorkspace"))
                              .toObject()
                              .value(QStringLiteral("id"))
                              .toInt();
    return !monitor.name.isEmpty() && logicalWidth > 0 && logicalHeight > 0;
  }

  error = QStringLiteral("Hyprland did not report a focused monitor");
  return false;
}

QVector<WindowTarget> parseWindows(const QByteArray &json,
                                   const MonitorInfo &monitor) {
  QVector<WindowTarget> result;
  const QJsonDocument document = QJsonDocument::fromJson(json);
  if (!document.isArray())
    return result;

  for (const QJsonValue value : document.array()) {
    const QJsonObject object = value.toObject();
    if (object.value(QStringLiteral("workspace"))
            .toObject()
            .value(QStringLiteral("id"))
            .toInt() != monitor.workspaceId)
      continue;

    const QJsonArray at = object.value(QStringLiteral("at")).toArray();
    const QJsonArray size = object.value(QStringLiteral("size")).toArray();
    if (at.size() < 2 || size.size() < 2)
      continue;

    QRect rect(at.at(0).toInt() - monitor.geometry.x(),
               at.at(1).toInt() - monitor.geometry.y(), size.at(0).toInt(),
               size.at(1).toInt());
    rect = rect.intersected(QRect(QPoint(), monitor.geometry.size()));
    if (rect.isEmpty())
      continue;

    QString title = object.value(QStringLiteral("title")).toString();
    if (title.isEmpty())
      title = object.value(QStringLiteral("class"))
                  .toString(QStringLiteral("window"));
    result.push_back({rect, object.value(QStringLiteral("stableId")).toString(),
                      std::move(title)});
  }
  return result;
}

void drawAnnotation(QPainter &painter, const Annotation &annotation) {
  const qreal width = std::max<qreal>(2.0, annotation.size);
  QPen pen(annotation.color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  painter.setPen(pen);
  painter.setBrush(annotation.color);

  if (annotation.kind == Annotation::Kind::Rectangle) {
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(QRectF(annotation.start, annotation.end).normalized());
    return;
  }

  if (annotation.kind == Annotation::Kind::Line) {
    painter.drawLine(annotation.start, annotation.end);
    return;
  }

  if (annotation.kind == Annotation::Kind::Freehand) {
    if (annotation.points.size() < 2)
      return;
    QPainterPath stroke(annotation.points.first());
    for (int index = 1; index + 1 < annotation.points.size(); ++index) {
      const QPointF midpoint =
          (annotation.points.at(index) + annotation.points.at(index + 1)) / 2.0;
      stroke.quadTo(annotation.points.at(index), midpoint);
    }
    stroke.lineTo(annotation.points.last());
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(stroke);
    return;
  }

  if (annotation.kind == Annotation::Kind::Arrow) {
    const QLineF line(annotation.start, annotation.end);
    if (line.length() < 1.0)
      return;
    const qreal angle = std::atan2(line.dy(), line.dx());
    const qreal headLength = std::max<qreal>(14.0, annotation.size * 4.2);
    const qreal halfWidth = headLength * 0.46;
    const QPointF direction(std::cos(angle), std::sin(angle));
    const QPointF perpendicular(-direction.y(), direction.x());
    const QPointF base = annotation.end - direction * headLength;
    const QPointF stemEnd = annotation.end - direction * (headLength * 0.5);
    painter.drawLine(annotation.start, stemEnd);
    QPolygonF head;
    head << annotation.end << base + perpendicular * halfWidth
         << base - perpendicular * halfWidth;
    painter.setPen(Qt::NoPen);
    painter.drawPolygon(head);
    return;
  }

  if (annotation.kind == Annotation::Kind::Marker) {
    const qreal diameter = std::max<qreal>(24.0, annotation.size * 6.0);
    const QRectF marker(annotation.start.x() - diameter / 2.0,
                        annotation.start.y() - diameter / 2.0, diameter,
                        diameter);
    painter.setPen(
        QPen(Qt::white, std::max<qreal>(1.0, annotation.size * 0.35)));
    painter.setBrush(annotation.color);
    painter.drawEllipse(marker);
    QFont font(QStringLiteral("Noto Sans"));
    font.setBold(true);
    font.setPixelSize(
        static_cast<int>(std::max<qreal>(11.0, annotation.size * 3.2)));
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.drawText(marker, Qt::AlignCenter,
                     QString::number(annotation.number));
    return;
  }

  const QFont font = annotationTextFont(annotation.size);
  painter.setFont(font);
  painter.setPen(annotation.color);
  painter.setBrush(Qt::NoBrush);
  painter.drawText(annotation.start, annotation.text);
}

QRect pixelSelection(const CaptureData &capture, const QRectF &selection) {
  const QRectF bounded = selection.normalized().intersected(
      QRectF(QPointF(), capture.preview.size()));
  const qreal scaleX =
      capture.source.width() / static_cast<qreal>(capture.preview.width());
  const qreal scaleY =
      capture.source.height() / static_cast<qreal>(capture.preview.height());
  const int left =
      std::clamp(static_cast<int>(std::floor(bounded.left() * scaleX)), 0,
                 capture.source.width());
  const int top =
      std::clamp(static_cast<int>(std::floor(bounded.top() * scaleY)), 0,
                 capture.source.height());
  const int right =
      std::clamp(static_cast<int>(std::ceil(bounded.right() * scaleX)), left,
                 capture.source.width());
  const int bottom =
      std::clamp(static_cast<int>(std::ceil(bounded.bottom() * scaleY)), top,
                 capture.source.height());
  return QRect(QPoint(left, top), QPoint(right - 1, bottom - 1));
}

} // namespace

void paintAnnotation(QPainter &painter, const Annotation &annotation) {
  drawAnnotation(painter, annotation);
}

void paintCaptureBackground(QPainter &painter, const QRectF &bounds,
                            BackgroundStyle backgroundStyle) {
  if (backgroundStyle == BackgroundStyle::None)
    return;

  struct Blob {
    QPointF center;
    qreal radius;
    QColor color;
  };
  QColor base;
  std::array<Blob, 4> blobs;
  if (backgroundStyle == BackgroundStyle::Aurora) {
    base = QColor(QStringLiteral("#101827"));
    blobs = {Blob{QPointF(bounds.left() + bounds.width() * 0.15,
                          bounds.top() + bounds.height() * 0.18),
                  bounds.width() * 0.75, QColor(QStringLiteral("#2dd4bf"))},
             Blob{bounds.topRight(), bounds.width() * 0.78,
                  QColor(QStringLiteral("#7c3aed"))},
             Blob{QPointF(bounds.center().x(), bounds.bottom()),
                  bounds.width() * 0.72, QColor(QStringLiteral("#2563eb"))},
             Blob{bounds.bottomLeft(), bounds.width() * 0.55,
                  QColor(QStringLiteral("#0f766e"))}};
  } else if (backgroundStyle == BackgroundStyle::Sunset) {
    base = QColor(QStringLiteral("#251328"));
    blobs = {Blob{bounds.topLeft(), bounds.width() * 0.82,
                  QColor(QStringLiteral("#f97316"))},
             Blob{QPointF(bounds.right(), bounds.top() + bounds.height() * 0.2),
                  bounds.width() * 0.7, QColor(QStringLiteral("#ec4899"))},
             Blob{QPointF(bounds.center().x(), bounds.bottom()),
                  bounds.width() * 0.75, QColor(QStringLiteral("#7c3aed"))},
             Blob{bounds.bottomLeft(), bounds.width() * 0.5,
                  QColor(QStringLiteral("#ef4444"))}};
  } else if (backgroundStyle == BackgroundStyle::Lagoon) {
    base = QColor(QStringLiteral("#071c2a"));
    blobs = {
        Blob{QPointF(bounds.left() + bounds.width() * 0.12, bounds.top()),
             bounds.width() * 0.68, QColor(QStringLiteral("#06b6d4"))},
        Blob{QPointF(bounds.right(), bounds.top() + bounds.height() * 0.25),
             bounds.width() * 0.75, QColor(QStringLiteral("#1d4ed8"))},
        Blob{QPointF(bounds.center().x(), bounds.bottom()),
             bounds.width() * 0.75, QColor(QStringLiteral("#0f766e"))},
        Blob{bounds.bottomLeft(), bounds.width() * 0.5,
             QColor(QStringLiteral("#22d3ee"))}};
  } else {
    base = QColor(QStringLiteral("#171225"));
    blobs = {
        Blob{QPointF(bounds.left(), bounds.top() + bounds.height() * 0.15),
             bounds.width() * 0.7, QColor(QStringLiteral("#a855f7"))},
        Blob{bounds.topRight(), bounds.width() * 0.72,
             QColor(QStringLiteral("#4f46e5"))},
        Blob{bounds.bottomRight(), bounds.width() * 0.65,
             QColor(QStringLiteral("#db2777"))},
        Blob{QPointF(bounds.left() + bounds.width() * 0.25, bounds.bottom()),
             bounds.width() * 0.62, QColor(QStringLiteral("#4338ca"))}};
  }

  painter.fillRect(bounds, base);
  for (const Blob &blob : blobs) {
    QRadialGradient gradient(blob.center, blob.radius);
    QColor center = blob.color;
    center.setAlpha(220);
    QColor edge = blob.color;
    edge.setAlpha(0);
    gradient.setColorAt(0, center);
    gradient.setColorAt(1, edge);
    painter.fillRect(bounds, gradient);
  }
}

bool captureFocusedMonitor(CaptureData &capture, QString &error) {
  const ProcessResult monitors =
      runProcess(QStringLiteral("hyprctl"),
                 {QStringLiteral("monitors"), QStringLiteral("-j")});
  if (!monitors.finished || monitors.exitCode != 0 ||
      !parseMonitor(monitors.output, capture.monitor, error)) {
    if (error.isEmpty())
      error = QString::fromUtf8(monitors.error).trimmed();
    return false;
  }

  QTemporaryFile sourceFile(
      runtimePath(QStringLiteral("omasnap-capture-XXXXXX.ppm")));
  sourceFile.setAutoRemove(true);
  if (!sourceFile.open()) {
    error = sourceFile.errorString();
    return false;
  }
  const QString sourcePath = sourceFile.fileName();
  sourceFile.close();

  const QRect geometry = capture.monitor.geometry;
  const QString grimGeometry = QStringLiteral("%1,%2 %3x%4")
                                   .arg(geometry.x())
                                   .arg(geometry.y())
                                   .arg(geometry.width())
                                   .arg(geometry.height());
  const ProcessResult grim = runProcess(
      QStringLiteral("grim"),
      {QStringLiteral("-t"), QStringLiteral("ppm"), QStringLiteral("-s"),
       QString::number(capture.monitor.scale, 'g', 8), QStringLiteral("-g"),
       grimGeometry, sourcePath},
      {}, 10000);

  if (!grim.finished || grim.exitCode != 0 ||
      !capture.source.load(sourcePath)) {
    error = QStringLiteral("Screen capture failed: %1")
                .arg(QString::fromUtf8(grim.error).trimmed());
    return false;
  }

  capture.preview = capture.source.scaled(
      geometry.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  if (capture.preview.isNull()) {
    error = QStringLiteral("Could not prepare screenshot preview");
    return false;
  }

  const ProcessResult clients =
      runProcess(QStringLiteral("hyprctl"),
                 {QStringLiteral("clients"), QStringLiteral("-j")});
  if (clients.finished && clients.exitCode == 0)
    capture.windows = parseWindows(clients.output, capture.monitor);
  return true;
}

QImage renderCapture(const CaptureData &capture, const QRectF &selection,
                     const QVector<Annotation> &annotations,
                     BackgroundStyle backgroundStyle) {
  const QRect pixels = pixelSelection(capture, selection);
  if (pixels.isEmpty())
    return {};

  QImage cropped = capture.source.copy(pixels).convertToFormat(
      QImage::Format_ARGB32_Premultiplied);
  const qreal sourceScaleX =
      capture.source.width() / static_cast<qreal>(capture.preview.width());
  const qreal sourceScaleY =
      capture.source.height() / static_cast<qreal>(capture.preview.height());
  const bool highDpi = capture.monitor.scale > 1.0;
  const qreal scaleX = highDpi ? capture.monitor.scale : sourceScaleX;
  const qreal scaleY = highDpi ? capture.monitor.scale : sourceScaleY;
  if (highDpi) {
    const QSize impliedSize(std::max(1, qRound(selection.width() * scaleX)),
                            std::max(1, qRound(selection.height() * scaleY)));
    if (cropped.size() != impliedSize) {
      cropped = cropped.scaled(impliedSize, Qt::IgnoreAspectRatio,
                               Qt::SmoothTransformation);
    }
  }
  const bool hasBackground = backgroundStyle != BackgroundStyle::None;
  const int marginX =
      hasBackground ? static_cast<int>(std::round(64.0 * scaleX)) : 0;
  const int marginY =
      hasBackground ? static_cast<int>(std::round(64.0 * scaleY)) : 0;
  QImage output(cropped.width() + marginX * 2, cropped.height() + marginY * 2,
                QImage::Format_ARGB32_Premultiplied);
  output.fill(Qt::transparent);

  QPainter painter(&output);
  painter.setRenderHints(QPainter::Antialiasing |
                         QPainter::SmoothPixmapTransform |
                         QPainter::TextAntialiasing);
  if (hasBackground) {
    paintCaptureBackground(painter, output.rect(), backgroundStyle);
    const QRectF imageRect(marginX, marginY, cropped.width(), cropped.height());
    painter.setPen(Qt::NoPen);
    for (int layer = 24; layer > 0; --layer) {
      const qreal spread = layer * std::max(scaleX, scaleY) * 0.85;
      painter.setBrush(QColor(0, 0, 0, 2 + (24 - layer) / 5));
      painter.drawRoundedRect(imageRect.adjusted(-spread, -spread + 14 * scaleY,
                                                 spread, spread + 14 * scaleY),
                              16 * scaleX + spread, 16 * scaleY + spread);
    }
    for (int layer = 12; layer > 0; --layer) {
      const qreal spread = layer * std::max(scaleX, scaleY) * 0.45;
      painter.setBrush(QColor(0, 0, 0, 3 + (12 - layer)));
      painter.drawRoundedRect(imageRect.adjusted(-spread, -spread + 8 * scaleY,
                                                 spread, spread + 8 * scaleY),
                              14 * scaleX + spread, 14 * scaleY + spread);
    }
    QPainterPath clip;
    clip.addRoundedRect(imageRect, 14 * scaleX, 14 * scaleY);
    painter.save();
    painter.setClipPath(clip);
    painter.drawImage(imageRect.topLeft(), cropped);
    painter.restore();
  } else {
    painter.drawImage(QPoint(0, 0), cropped);
  }

  painter.save();
  painter.translate(marginX, marginY);
  painter.scale(scaleX, scaleY);
  for (const Annotation &annotation : annotations)
    drawAnnotation(painter, annotation);
  painter.restore();
  painter.end();
  return output;
}

bool copyPngFileToClipboard(const QString &path, QString &error) {
  QFile input(path);
  if (!input.open(QIODevice::ReadOnly)) {
    error = QStringLiteral("Could not read screenshot snapshot: %1").arg(path);
    return false;
  }
  const QByteArray png = input.readAll();
  if (png.isEmpty()) {
    error = QStringLiteral("Screenshot snapshot is empty: %1").arg(path);
    return false;
  }
  return copyToWaylandClipboard(QStringLiteral("image/png"), png, error);
}

QString moveSnapshotToScreenshots(const QString &sourcePath, QString &error) {
  const QString targetPath = screenshotTargetPath(error);
  if (targetPath.isEmpty())
    return {};
  if (QFile::rename(sourcePath, targetPath))
    return targetPath;
  if (QFile::copy(sourcePath, targetPath)) {
    QFile::remove(sourcePath);
    return targetPath;
  }
  error = QStringLiteral("Could not move screenshot snapshot to: %1")
              .arg(targetPath);
  return {};
}

QString temporarySnapshotPath() {
  return QDir(QStringLiteral("/tmp/omasnap"))
      .filePath(QStringLiteral("snapshot-%1.png")
                    .arg(QCoreApplication::applicationPid()));
}

bool saveTemporarySnapshot(const QImage &image, QString path, QString &error) {
  if (image.isNull()) {
    error = QStringLiteral("Temporary snapshot is empty");
    return false;
  }
  if (path.isEmpty())
    path = temporarySnapshotPath();
  const QDir root = QFileInfo(path).absoluteDir();
  if (!QDir().mkpath(root.absolutePath())) {
    error = QStringLiteral("Could not create snapshot directory: %1")
                .arg(root.absolutePath());
    return false;
  }
  if (!image.save(path, "PNG")) {
    error = QStringLiteral("Could not save temporary snapshot: %1").arg(path);
    return false;
  }
  return true;
}

bool copyTextToClipboard(const QString &text, QString &error) {
  if (text.isEmpty()) {
    error = QStringLiteral("No text found in selection");
    return false;
  }
  return copyToWaylandClipboard(QStringLiteral("text/plain;charset=utf-8"),
                                text.toUtf8(), error);
}

QString recognizeText(const QImage &image, QString &error) {
  QTemporaryFile input(runtimePath(QStringLiteral("omasnap-ocr-XXXXXX.png")));
  input.setAutoRemove(true);
  if (!input.open()) {
    error = input.errorString();
    return {};
  }
  const QString path = input.fileName();
  input.close();
  if (!image.save(path, "PNG")) {
    error = QStringLiteral("Could not prepare image for OCR");
    return {};
  }

  const QString languages =
      qEnvironmentVariable("OMASNAP_OCR_LANGS", QStringLiteral("eng"));
  const ProcessResult result = runProcess(
      QStringLiteral("tesseract"),
      {path, QStringLiteral("stdout"), QStringLiteral("--oem"),
       QStringLiteral("1"), QStringLiteral("--psm"), QStringLiteral("6"),
       QStringLiteral("-l"), languages, QStringLiteral("--dpi"),
       QStringLiteral("300"), QStringLiteral("-c"),
       QStringLiteral("preserve_interword_spaces=1")},
      {}, 30000);
  if (!result.finished || result.exitCode != 0) {
    error = QStringLiteral("OCR failed: %1")
                .arg(QString::fromUtf8(result.error).trimmed());
    return {};
  }
  const QString text = QString::fromUtf8(result.output).trimmed();
  if (text.isEmpty())
    error = QStringLiteral("No text found in selection");
  return text;
}

void sendCaptureNotification(const QString &message, const QString &imagePath) {
  QStringList arguments{QStringLiteral("-g"), QStringLiteral(""),
                        QStringLiteral("--app-name"), QStringLiteral("omasnap"),
                        message};
  if (!imagePath.isEmpty()) {
    const QString imageUrl =
        QUrl::fromLocalFile(imagePath).toString(QUrl::FullyEncoded);
    arguments << QStringLiteral("Click to open") << QStringLiteral("--image")
              << imagePath << QStringLiteral("--exec")
              << QStringLiteral("xdg-open %1").arg(imageUrl);
  }
  arguments << QStringLiteral("-t") << QStringLiteral("4500");
  QProcess::startDetached(QStringLiteral("omarchy-notification-send"),
                          arguments);
}
