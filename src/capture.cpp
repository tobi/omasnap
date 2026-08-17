/** @fileoverview Captures, renders, saves, and shares screenshots. */
#include "capture.hpp"

#include <QBuffer>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
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
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>

#include <QUrl>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

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

bool ensurePrivateDirectory(const QString &path) {
  if (path.isEmpty())
    return false;

  const QString cleanPath = QDir::cleanPath(path);
  const QByteArray encoded = QFile::encodeName(cleanPath);
  const int fd = ::open(encoded.constData(),
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd >= 0) {
    struct stat info{};
    const bool ownedDirectory = ::fstat(fd, &info) == 0 &&
                                S_ISDIR(info.st_mode) &&
                                info.st_uid == ::geteuid();
    const bool secured = ownedDirectory && ::fchmod(fd, S_IRWXU) == 0;
    ::close(fd);
    return secured;
  }
  if (errno != ENOENT)
    return false;

  const QString parent = QFileInfo(cleanPath).dir().absolutePath();
  if (parent.isEmpty() || parent == cleanPath)
    return false;
  if (!QFileInfo(parent).isDir() && !ensurePrivateDirectory(parent))
    return false;

  if (::mkdir(encoded.constData(), S_IRWXU) == 0)
    return true;
  return errno == EEXIST && ensurePrivateDirectory(cleanPath);
}

QString secureRuntimeDirectory() {
  QString runtime =
      QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
  if (runtime.isEmpty()) {
    runtime = QDir(QDir::tempPath())
                  .filePath(QStringLiteral("omasnap-%1").arg(::getuid()));
  } else {
    runtime = QDir(runtime).filePath(QStringLiteral("omasnap"));
  }
  return ensurePrivateDirectory(runtime) ? QDir::cleanPath(runtime) : QString();
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

/**
 * Runs a process whose stdout carries a large payload, draining the pipe as it
 * arrives into one buffer sized up front. Letting QProcess buffer the whole
 * payload first costs an extra copy of the full frame.
 */
ProcessResult runStreamingProcess(const QString &program,
                                  const QStringList &arguments,
                                  qsizetype expectedBytes, int timeoutMs) {
  QProcess process;
  process.setProcessChannelMode(QProcess::SeparateChannels);
  process.start(program, arguments, QIODevice::ReadOnly);
  if (!process.waitForStarted(2000))
    return {{}, process.errorString().toUtf8(), -1, false};

  QByteArray output;
  output.reserve(std::max<qsizetype>(expectedBytes, 0));
  const auto drain = [&process, &output] {
    const qint64 available = process.bytesAvailable();
    if (available <= 0)
      return;
    const qsizetype offset = output.size();
    output.resize(offset + available);
    const qint64 read = process.read(output.data() + offset, available);
    output.resize(offset + std::max<qint64>(read, 0));
  };

  QElapsedTimer clock;
  clock.start();
  const auto remainingMs = [&clock, timeoutMs] {
    return std::max<qint64>(0, timeoutMs - clock.elapsed());
  };
  while (remainingMs() > 0 &&
         process.waitForReadyRead(static_cast<int>(remainingMs())))
    drain();

  const bool finished =
      process.state() == QProcess::NotRunning ||
      process.waitForFinished(static_cast<int>(remainingMs()));
  if (!finished)
    process.kill();
  drain();
  return {std::move(output), process.readAllStandardError(),
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
  const QString runtime = secureRuntimeDirectory();
  return runtime.isEmpty() ? QString() : QDir(runtime).filePath(name);
}

QString shellQuote(QString value) {
  value.replace('\'', QStringLiteral("'\"'\"'"));
  return QStringLiteral("'%1'").arg(value);
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
    int logicalWidth = qRound(rawWidth / std::max<qreal>(scale, 0.01));
    int logicalHeight = qRound(rawHeight / std::max<qreal>(scale, 0.01));
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
  // Redactions replace source pixels in renderCapture before ordinary vector
  // annotations are painted. They must never be approximated by a translucent
  // overlay here because that could leave recoverable source data in exports.
  if (annotation.kind == Annotation::Kind::Redaction ||
      annotation.kind == Annotation::Kind::Spotlight)
    return;

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

  if (annotation.kind == Annotation::Kind::Freehand ||
      annotation.kind == Annotation::Kind::Highlighter) {
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
    if (annotation.kind == Annotation::Kind::Highlighter) {
      QColor ink = annotation.color;
      if (ink.alpha() >= 255)
        ink.setAlpha(120);
      const qreal highlightWidth = std::max<qreal>(6.0, annotation.size * 3.0);
      painter.setPen(QPen(ink, highlightWidth, Qt::SolidLine, Qt::RoundCap,
                          Qt::RoundJoin));
    }
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

quint32 nextRedactionRandom(quint32 &state) {
  if (state == 0)
    state = 0x6d2b79f5U;
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

QRect redactionPixelRect(const Annotation &annotation, const QSize &imageSize,
                         qreal scaleX, qreal scaleY,
                         const QPointF &originOffset) {
  const QRectF logical(annotation.start, annotation.end);
  const QRectF normalized = logical.normalized();
  const int left = static_cast<int>(
      std::floor(normalized.left() * scaleX + originOffset.x()));
  const int top = static_cast<int>(
      std::floor(normalized.top() * scaleY + originOffset.y()));
  const int right = static_cast<int>(
      std::ceil(normalized.right() * scaleX + originOffset.x()));
  const int bottom = static_cast<int>(
      std::ceil(normalized.bottom() * scaleY + originOffset.y()));
  return QRect(left, top, std::max(0, right - left), std::max(0, bottom - top))
      .intersected(QRect(QPoint(), imageSize));
}

QVector<QColor> aggregateRedactionPalette(const QImage &image,
                                          const QRect &region) {
  struct Bucket {
    quint64 red = 0;
    quint64 green = 0;
    quint64 blue = 0;
    quint64 count = 0;
  };
  std::array<Bucket, 64> buckets{};
  // A bounded, uniform sample keeps live dragging responsive on 4K captures.
  // Coordinates are used only to choose samples; their positions are discarded
  // before the synthetic mosaic is generated.
  constexpr int maximumSamples = 4096;
  const qreal regionArea =
      static_cast<qreal>(region.width()) * static_cast<qreal>(region.height());
  const qreal sampleStride = std::max<qreal>(
      1.0, std::sqrt(regionArea / static_cast<qreal>(maximumSamples)));
  for (qreal sampleY = region.top() + sampleStride / 2.0;
       sampleY < region.bottom() + 1.0; sampleY += sampleStride) {
    for (qreal sampleX = region.left() + sampleStride / 2.0;
         sampleX < region.right() + 1.0; sampleX += sampleStride) {
      const QColor color = image.pixelColor(
          std::clamp(static_cast<int>(sampleX), region.left(), region.right()),
          std::clamp(static_cast<int>(sampleY), region.top(), region.bottom()));
      const int bucketIndex = (color.red() >> 6) * 16 +
                              (color.green() >> 6) * 4 + (color.blue() >> 6);
      Bucket &bucket = buckets.at(static_cast<std::size_t>(bucketIndex));
      bucket.red += static_cast<quint64>(color.red());
      bucket.green += static_cast<quint64>(color.green());
      bucket.blue += static_cast<quint64>(color.blue());
      ++bucket.count;
    }
  }

  std::array<int, 64> order{};
  for (int index = 0; index < static_cast<int>(order.size()); ++index)
    order.at(static_cast<std::size_t>(index)) = index;
  std::ranges::sort(order, [&buckets](int first, int second) {
    return buckets.at(static_cast<std::size_t>(first)).count >
           buckets.at(static_cast<std::size_t>(second)).count;
  });

  QVector<QColor> palette;
  palette.reserve(6);
  for (const int bucketIndex : order) {
    const Bucket &bucket = buckets.at(static_cast<std::size_t>(bucketIndex));
    if (bucket.count == 0)
      break;
    palette.push_back(QColor(static_cast<int>(bucket.red / bucket.count),
                             static_cast<int>(bucket.green / bucket.count),
                             static_cast<int>(bucket.blue / bucket.count),
                             255));
    if (palette.size() == 6)
      break;
  }
  if (palette.isEmpty())
    palette.push_back(QColor(QStringLiteral("#121216")));
  return palette;
}

void applyRedactions(QImage &image, const QVector<Annotation> &annotations,
                     qreal scaleX, qreal scaleY,
                     const QPointF &originOffset = {}) {
  for (const Annotation &annotation : annotations) {
    if (annotation.kind != Annotation::Kind::Redaction)
      continue;
    const QRect region = redactionPixelRect(annotation, image.size(), scaleX,
                                            scaleY, originOffset);
    if (region.isEmpty())
      continue;
    QVector<QColor> palette;
    if (annotation.redactionStyle == RedactionStyle::Pixelate)
      palette = aggregateRedactionPalette(image, region);

    // Finish all source reads before activating a painter on the same image.
    QPainter painter(&image);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.setRenderHint(QPainter::Antialiasing, false);
    if (annotation.redactionStyle == RedactionStyle::Solid) {
      painter.fillRect(region, QColor(QStringLiteral("#121216")));
      continue;
    }

    quint32 randomState = annotation.redactionSeed;
    if (randomState == 0) {
      randomState = static_cast<quint32>(region.x()) * 73856093U ^
                    static_cast<quint32>(region.y()) * 19349663U ^
                    static_cast<quint32>(region.width()) * 83492791U ^
                    static_cast<quint32>(region.height()) * 2654435761U;
    }
    const int blockWidth = std::max(1, qRound(12.0 * scaleX));
    const int blockHeight = std::max(1, qRound(12.0 * scaleY));
    for (int y = region.top(); y <= region.bottom(); y += blockHeight) {
      for (int x = region.left(); x <= region.right(); x += blockWidth) {
        const QColor color = palette.at(
            static_cast<qsizetype>(nextRedactionRandom(randomState) %
                                   static_cast<quint32>(palette.size())));
        painter.fillRect(QRect(x, y,
                               std::min(blockWidth, region.right() - x + 1),
                               std::min(blockHeight, region.bottom() - y + 1)),
                         color);
      }
    }
  }
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

QPainterPath spotlightPath(const Annotation &annotation) {
  const QRectF bounds = QRectF(annotation.start, annotation.end).normalized();
  QPainterPath path;
  if (annotation.spotlightShape == SpotlightShape::Ellipse) {
    path.addEllipse(bounds);
  } else if (annotation.spotlightShape == SpotlightShape::RoundedRectangle) {
    const qreal shorterEdge = std::min(bounds.width(), bounds.height());
    const qreal radius =
        std::min(shorterEdge / 2.0, std::clamp(shorterEdge * 0.12, 3.0, 28.0));
    path.addRoundedRect(bounds, radius, radius);
  } else {
    path.addRect(bounds);
  }
  return path;
}

void paintSpotlights(QPainter &painter, const QImage &source,
                     const QRectF &targetBounds, const QRectF &sourceRect,
                     const QVector<Annotation> &annotations) {
  if (source.isNull() || targetBounds.isEmpty() || sourceRect.isEmpty())
    return;

  QVector<const Annotation *> spotlights;
  QPainterPath dimmed;
  dimmed.addRect(targetBounds);
  for (const Annotation &annotation : annotations) {
    if (annotation.kind != Annotation::Kind::Spotlight)
      continue;
    const QRectF lens =
        QRectF(annotation.start, annotation.end).normalized().intersected(
            targetBounds);
    if (lens.width() < 1 || lens.height() < 1)
      continue;
    QPainterPath opening = spotlightPath(annotation);
    QPainterPath targetClip;
    targetClip.addRect(targetBounds);
    opening = opening.intersected(targetClip);
    dimmed = dimmed.subtracted(opening);
    spotlights.push_back(&annotation);
  }
  if (spotlights.isEmpty())
    return;

  painter.save();
  painter.setClipRect(targetBounds, Qt::IntersectClip);
  painter.fillPath(dimmed, QColor(0, 0, 0, 154));
  for (const Annotation *annotation : spotlights) {
    const QRectF lens = QRectF(annotation->start, annotation->end).normalized();
    const qreal magnification = std::clamp(annotation->magnification, 1.0, 4.0);
    QSizeF sampleSize(sourceRect.width() * lens.width() / targetBounds.width() /
                          magnification,
                      sourceRect.height() * lens.height() /
                          targetBounds.height() / magnification);
    sampleSize.setWidth(std::min(sampleSize.width(), sourceRect.width()));
    sampleSize.setHeight(std::min(sampleSize.height(), sourceRect.height()));
    const QPointF normalizedCenter(
        (lens.center().x() - targetBounds.left()) / targetBounds.width(),
        (lens.center().y() - targetBounds.top()) / targetBounds.height());
    const QPointF sampleCenter(
        sourceRect.left() + normalizedCenter.x() * sourceRect.width(),
        sourceRect.top() + normalizedCenter.y() * sourceRect.height());
    QRectF sample(sampleCenter.x() - sampleSize.width() / 2.0,
                  sampleCenter.y() - sampleSize.height() / 2.0,
                  sampleSize.width(), sampleSize.height());
    sample.moveLeft(std::clamp(sample.left(), sourceRect.left(),
                               sourceRect.right() - sample.width()));
    sample.moveTop(std::clamp(sample.top(), sourceRect.top(),
                              sourceRect.bottom() - sample.height()));

    const QPainterPath lensClip = spotlightPath(*annotation);
    painter.save();
    painter.setClipPath(lensClip, Qt::IntersectClip);
    painter.drawImage(lens, source, sample);
    painter.restore();

    const QColor outline =
        annotation->color.isValid() ? annotation->color : QColor(Qt::white);
    painter.setPen(QPen(outline, std::max<qreal>(2.0, annotation->size / 2.0),
                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(lensClip);
  }
  painter.restore();
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

bool probeFocusedMonitor(MonitorInfo &monitor, QString &error) {
  const ProcessResult monitors =
      runProcess(QStringLiteral("hyprctl"),
                 {QStringLiteral("monitors"), QStringLiteral("-j")});
  if (!monitors.finished || monitors.exitCode != 0 ||
      !parseMonitor(monitors.output, monitor, error)) {
    if (error.isEmpty())
      error = QString::fromUtf8(monitors.error).trimmed();
    return false;
  }
  return true;
}

bool captureMonitorPixels(const MonitorInfo &monitor, CaptureData &capture,
                          bool includeWindows, QString &error) {
  capture.monitor = monitor;
  const QRect geometry = capture.monitor.geometry;
  if (geometry.size().isEmpty()) {
    error = QStringLiteral("Focused monitor reported an empty geometry");
    return false;
  }

  // Window discovery is independent of the screen grab, so let the hyprctl
  // round trip overlap grim instead of running after it.
  QProcess clients;
  if (includeWindows) {
    clients.setProcessChannelMode(QProcess::SeparateChannels);
    clients.start(QStringLiteral("hyprctl"),
                  {QStringLiteral("clients"), QStringLiteral("-j")});
    clients.closeWriteChannel();
  }

  const QString grimGeometry = QStringLiteral("%1,%2 %3x%4")
                                   .arg(geometry.x())
                                   .arg(geometry.y())
                                   .arg(geometry.width())
                                   .arg(geometry.height());
  // A PPM frame is three bytes per pixel plus a short header.
  const qsizetype expectedBytes =
      static_cast<qsizetype>(capture.monitor.pixelSize.width()) *
          capture.monitor.pixelSize.height() * 3 +
      64;
  const ProcessResult grim = runStreamingProcess(
      QStringLiteral("grim"),
      {QStringLiteral("-t"), QStringLiteral("ppm"), QStringLiteral("-s"),
       QString::number(capture.monitor.scale, 'g', 8), QStringLiteral("-g"),
       grimGeometry, QStringLiteral("-")},
      expectedBytes, 10000);

  if (!grim.finished || grim.exitCode != 0) {
    QString detail = QString::fromUtf8(grim.error).trimmed();
    if (detail.isEmpty()) {
      detail = grim.finished ? QStringLiteral("grim exited with code %1")
                                   .arg(grim.exitCode)
                             : QStringLiteral("grim did not finish in time");
    }
    error = QStringLiteral("Screen capture failed: %1").arg(detail);
    return false;
  }
  if (!capture.source.loadFromData(grim.output, "PPM")) {
    error = QStringLiteral(
                "Screen capture failed: could not decode %1 bytes of PPM data "
                "from grim")
                .arg(grim.output.size());
    return false;
  }

  capture.preview = capture.source.scaled(
      geometry.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  if (capture.preview.isNull()) {
    error = QStringLiteral("Could not prepare screenshot preview");
    return false;
  }

  if (includeWindows) {
    if (!clients.waitForFinished(10000))
      clients.kill();
    else if (clients.exitCode() == 0)
      capture.windows =
          parseWindows(clients.readAllStandardOutput(), capture.monitor);
  }
  return true;
}

bool captureFocusedMonitor(CaptureData &capture, bool includeWindows,
                           QString &error) {
  if (!probeFocusedMonitor(capture.monitor, error))
    return false;
  return captureMonitorPixels(capture.monitor, capture, includeWindows, error);
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
  const QPointF sourceOriginOffset(
      selection.left() * sourceScaleX - pixels.left(),
      selection.top() * sourceScaleY - pixels.top());
  bool resized = false;
  if (highDpi) {
    const QSize impliedSize(std::max(1, qRound(selection.width() * scaleX)),
                            std::max(1, qRound(selection.height() * scaleY)));
    if (cropped.size() != impliedSize) {
      // Remove sensitive source pixels before SmoothTransformation can blend
      // them outside the final redaction boundary. The crop may begin between
      // native pixels, so preserve that fractional origin in local coordinates.
      applyRedactions(cropped, annotations, sourceScaleX, sourceScaleY,
                      sourceOriginOffset);
      cropped = cropped.scaled(impliedSize, Qt::IgnoreAspectRatio,
                               Qt::SmoothTransformation);
      resized = true;
    }
  }
  applyRedactions(cropped, annotations, resized ? scaleX : sourceScaleX,
                  resized ? scaleY : sourceScaleY,
                  resized ? QPointF{} : sourceOriginOffset);
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
  painter.setClipRect(QRectF(QPointF(), selection.size()));
  paintSpotlights(painter, cropped, QRectF(QPointF(), selection.size()),
                  QRectF(cropped.rect()), annotations);
  for (const Annotation &annotation : annotations)
    drawAnnotation(painter, annotation);
  painter.restore();
  painter.end();
  return output;
}

QImage renderSelectionBase(const CaptureData &capture, const QRectF &selection,
                           const QSize &targetSize) {
  const QRect pixels = pixelSelection(capture, selection);
  if (pixels.isEmpty() || targetSize.isEmpty())
    return {};
  QImage cropped = capture.source.copy(pixels).convertToFormat(
      QImage::Format_ARGB32_Premultiplied);
  if (cropped.size() != targetSize)
    cropped = cropped.scaled(targetSize, Qt::IgnoreAspectRatio,
                             Qt::SmoothTransformation);
  return cropped;
}

QImage applyRedactionsScaled(QImage image, const QVector<Annotation> &redactions,
                             const QRectF &selection, const QSizeF &targetSize) {
  if (image.isNull() || redactions.isEmpty() || selection.isEmpty() ||
      targetSize.isEmpty())
    return image;
  const qreal scaleX = targetSize.width() / selection.width();
  const qreal scaleY = targetSize.height() / selection.height();
  applyRedactions(image, redactions, scaleX, scaleY,
                  QPointF(-selection.left() * scaleX,
                          -selection.top() * scaleY));
  return image;
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

bool quickOutput(const QImage &image, QuickOutputMode mode, QString &error) {
  if (image.isNull() || mode == QuickOutputMode::None) {
    error = QStringLiteral("Could not prepare screenshot snapshot");
    return false;
  }
  const QString path = temporarySnapshotPath();
  if (path.isEmpty() || !saveTemporarySnapshot(image, path, error))
    return false;

  if (mode == QuickOutputMode::Copy || mode == QuickOutputMode::Both) {
    if (!copyPngFileToClipboard(path, error)) {
      QFile::remove(path);
      return false;
    }
  }
  if (mode == QuickOutputMode::Save || mode == QuickOutputMode::Both) {
    const QString saved = moveSnapshotToScreenshots(path, error);
    if (saved.isEmpty())
      return false;
    if (mode == QuickOutputMode::Save)
      sendCaptureNotification(QStringLiteral("Screenshot saved"), saved);
    else
      sendCaptureNotification(QStringLiteral("Screenshot saved and copied"),
                              saved);
  } else {
    QFile::remove(path);
    sendCaptureNotification(QStringLiteral("Screenshot copied to clipboard"));
  }
  return true;
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
  // Stable per process so repeated saves overwrite one working snapshot.
  static const quint32 nonce = QRandomGenerator::global()->generate();
  return runtimePath(QStringLiteral("snapshot-%1-%2.png")
                         .arg(QCoreApplication::applicationPid())
                         .arg(nonce, 8, 16, QChar('0')));
}

QString pinnedSnapshotPath(int index) {
  // A fresh nonce prevents collisions when the editor PID is recycled.
  return runtimePath(QStringLiteral("pin-%1-%2-%3.png")
                         .arg(QCoreApplication::applicationPid())
                         .arg(index)
                         .arg(QRandomGenerator::global()->generate64(), 16, 16,
                              QChar('0')));
}

void prunePinnedSnapshots() {
  const QString runtime = secureRuntimeDirectory();
  if (runtime.isEmpty())
    return;
  const QDateTime cutoff = QDateTime::currentDateTime().addDays(-1);
  const QFileInfoList stale =
      QDir(runtime).entryInfoList({QStringLiteral("pin-*.png")}, QDir::Files);
  for (const QFileInfo &entry : stale) {
    if (entry.lastModified() >= cutoff)
      continue;
    const QByteArray encodedPath = QFile::encodeName(entry.absoluteFilePath());
    const int fd = ::open(encodedPath.constData(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
      continue;
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0)
      QFile::remove(entry.absoluteFilePath());
    ::close(fd);
  }
}

bool saveTemporarySnapshot(const QImage &image, QString path, QString &error) {
  if (image.isNull()) {
    error = QStringLiteral("Temporary snapshot is empty");
    return false;
  }
  const QString runtime = secureRuntimeDirectory();
  if (runtime.isEmpty()) {
    error = QStringLiteral("Could not create private runtime directory");
    return false;
  }
  if (path.isEmpty())
    path = temporarySnapshotPath();
  path = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
  if (QFileInfo(path).absolutePath() != runtime) {
    error =
        QStringLiteral("Temporary snapshots must stay inside %1").arg(runtime);
    return false;
  }

  QSaveFile file(path);
  file.setDirectWriteFallback(false);
  if (!file.open(QIODevice::WriteOnly) ||
      !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
    error = QStringLiteral("Could not open secure snapshot file: %1")
                .arg(file.errorString());
    return false;
  }

  if (!image.save(&file, "PNG")) {
    file.cancelWriting();
    error = QStringLiteral("Could not save temporary snapshot: %1").arg(path);
    return false;
  }
  if (!file.commit()) {
    error = QStringLiteral("Could not replace temporary snapshot: %1")
                .arg(file.errorString());
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
  QByteArray payload;
  QBuffer buffer(&payload);
  if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) {
    error = QStringLiteral("Could not prepare image for OCR");
    return {};
  }

  QString languages = qEnvironmentVariable("OMASNAP_OCR_LANGS");
  if (languages.isEmpty())
    languages =
        qEnvironmentVariable("OMARCHY_OCR_LANGS", QStringLiteral("eng"));
  languages = languages.trimmed();
  const ProcessResult result = runProcess(
      QStringLiteral("tesseract"),
      {QStringLiteral("stdin"), QStringLiteral("stdout"),
       QStringLiteral("--oem"), QStringLiteral("1"),
       QStringLiteral("--psm"), QStringLiteral("6"),
       QStringLiteral("-l"), languages,
       QStringLiteral("--dpi"), QStringLiteral("300"),
       QStringLiteral("-c"), QStringLiteral("preserve_interword_spaces=1")},
      payload, 30000);
  if (!result.finished || result.exitCode != 0) {
    error = QStringLiteral("OCR failed for languages %1: %2")
                .arg(languages, QString::fromUtf8(result.error).trimmed());
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
    QString omasnap = QDir(QCoreApplication::applicationDirPath())
                          .filePath(QStringLiteral("omasnap"));
    if (!QFileInfo::exists(omasnap))
      omasnap = QStringLiteral("omasnap");
    arguments << QStringLiteral("Click to edit") << QStringLiteral("--image")
              << imagePath << QStringLiteral("--exec")
              << QStringLiteral("%1 %2").arg(shellQuote(omasnap),
                                             shellQuote(imageUrl));
  }
  arguments << QStringLiteral("-t") << QStringLiteral("4500");
  QProcess::startDetached(QStringLiteral("omarchy-notification-send"),
                          arguments);
}
