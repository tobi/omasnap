/** @fileoverview Band cut engine: removes a horizontal or vertical band and
 *  collapses the gap, or inserts transparent space at the seam. */
#include "cut.hpp"

#include <QPainter>
#include <algorithm>
#include <cstring>

QImage removeBand(const QImage &source, Qt::Orientation orientation,
                  int start, int end) {
  if (source.isNull())
    return source;
  if (orientation == Qt::Horizontal) {
    const int height = source.height();
    start = std::clamp(start, 0, height);
    end = std::clamp(end, start, height);
    const int band = end - start;
    if (band <= 0 || band >= height)
      return source;
    QImage out(source.width(), height - band, source.format());
    out.setDevicePixelRatio(source.devicePixelRatio());
    for (int y = 0; y < start; ++y)
      std::memcpy(out.scanLine(y), source.constScanLine(y),
                  source.bytesPerLine());
    for (int y = end; y < height; ++y)
      std::memcpy(out.scanLine(y - band), source.constScanLine(y),
                  source.bytesPerLine());
    return out;
  }
  const int width = source.width();
  start = std::clamp(start, 0, width);
  end = std::clamp(end, start, width);
  const int band = end - start;
  if (band <= 0 || band >= width)
    return source;
  QImage out(width - band, source.height(), source.format());
  out.setDevicePixelRatio(source.devicePixelRatio());
  QPainter painter(&out);
  painter.setCompositionMode(QPainter::CompositionMode_Source);
  painter.drawImage(QPoint(0, 0), source, QRect(0, 0, start, source.height()));
  painter.drawImage(QPoint(start, 0), source,
                    QRect(end, 0, width - end, source.height()));
  return out;
}

QImage insertBand(const QImage &source, Qt::Orientation orientation,
                  int start, int end) {
  if (source.isNull())
    return source;
  const int extent =
      orientation == Qt::Horizontal ? source.height() : source.width();
  start = std::clamp(start, 0, extent);
  end = std::clamp(end, start, extent);
  const int band = end - start;
  if (band <= 0)
    return source;
  QImage image = source;
  if (image.format() != QImage::Format_ARGB32 &&
      image.format() != QImage::Format_ARGB32_Premultiplied)
    image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
  if (orientation == Qt::Horizontal) {
    const int height = image.height();
    QImage out(image.width(), height + band, image.format());
    out.setDevicePixelRatio(image.devicePixelRatio());
    out.fill(Qt::transparent);
    for (int y = 0; y < start; ++y)
      std::memcpy(out.scanLine(y), image.constScanLine(y), image.bytesPerLine());
    for (int y = start; y < height; ++y)
      std::memcpy(out.scanLine(y + band), image.constScanLine(y),
                  image.bytesPerLine());
    return out;
  }
  const int width = image.width();
  QImage out(width + band, image.height(), image.format());
  out.setDevicePixelRatio(image.devicePixelRatio());
  out.fill(Qt::transparent);
  QPainter painter(&out);
  painter.setCompositionMode(QPainter::CompositionMode_Source);
  painter.drawImage(QPoint(0, 0), image, QRect(0, 0, start, image.height()));
  painter.drawImage(QPoint(start + band, 0), image,
                    QRect(start, 0, width - start, image.height()));
  return out;
}

QImage applyCutOp(const QImage &source, const CutOp &cut) {
  return cut.insert ? insertBand(source, cut.orientation, cut.sourceStart,
                                 cut.sourceEnd)
                    : removeBand(source, cut.orientation, cut.sourceStart,
                                 cut.sourceEnd);
}

QImage composeCuts(const QImage &pristine, const QVector<CutOp> &cuts) {
  QImage image = pristine;
  for (const CutOp &cut : cuts)
    image = applyCutOp(image, cut);
  return image;
}

QSize composedLogicalSize(QSize pristineLogical, const QVector<CutOp> &cuts) {
  for (const CutOp &cut : cuts) {
    const int band = cut.logicalEnd - cut.logicalStart;
    if (band <= 0)
      continue;
    if (cut.insert) {
      if (cut.orientation == Qt::Horizontal)
        pristineLogical.setHeight(pristineLogical.height() + band);
      else
        pristineLogical.setWidth(pristineLogical.width() + band);
      continue;
    }
    if (cut.orientation == Qt::Horizontal) {
      const int extent = pristineLogical.height();
      // Mirror removeBand()'s no-op guard so this can never disagree with
      // what composeCuts() actually produces: an empty or full-extent band
      // leaves the image unchanged.
      if (band >= extent)
        continue;
      pristineLogical.setHeight(std::max(1, extent - band));
    } else {
      const int extent = pristineLogical.width();
      if (band >= extent)
        continue;
      pristineLogical.setWidth(std::max(1, extent - band));
    }
  }
  return pristineLogical;
}

qreal shiftForCut(qreal value, qreal start, qreal end) {
  if (value <= start)
    return value;
  if (value >= end)
    return value - (end - start);
  return start;
}

qreal shiftForInsert(qreal value, qreal start, qreal size) {
  if (size <= 0.0 || value < start)
    return value;
  return value + size;
}
