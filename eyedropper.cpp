/** @fileoverview Maps displayed editor coordinates to source pixels. */
#include "eyedropper.hpp"

#include <algorithm>
#include <cmath>

QColor sampleSourceColor(const QImage &source, const QSizeF &previewSize,
                         const QRectF &selection, const QRectF &displayRect,
                         const QPointF &displayPoint) {
  if (source.isNull() || previewSize.width() <= 0 || previewSize.height() <= 0 ||
      selection.isEmpty() || displayRect.isEmpty())
    return {};
  const QRectF boundedSelection = selection.normalized().intersected(
      QRectF(QPointF(), previewSize));
  if (boundedSelection.isEmpty())
    return {};
  const qreal u = std::clamp((displayPoint.x() - displayRect.left()) /
                                 displayRect.width(), 0.0, 1.0);
  const qreal v = std::clamp((displayPoint.y() - displayRect.top()) /
                                 displayRect.height(), 0.0, 1.0);
  const auto sourcePixel = [](qreal start, qreal end, qreal position,
                              int sourceExtent, qreal previewExtent) {
    const int first = std::clamp(
        static_cast<int>(std::floor(start * sourceExtent / previewExtent)), 0,
        sourceExtent - 1);
    const int pastLast = std::clamp(
        static_cast<int>(std::ceil(end * sourceExtent / previewExtent)),
        first + 1, sourceExtent);
    return std::clamp(
        first + static_cast<int>(std::floor(position * (pastLast - first))),
        first, pastLast - 1);
  };
  const int x = sourcePixel(boundedSelection.left(), boundedSelection.right(),
                            u, source.width(), previewSize.width());
  const int y = sourcePixel(boundedSelection.top(), boundedSelection.bottom(),
                            v, source.height(), previewSize.height());
  return source.pixelColor(x, y);
}
