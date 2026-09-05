/** @fileoverview Implements adjustable release-time pen smoothing. */
#include "stroke-smoothing.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include <QLineF>
#include <QPointF>
#include <QVector>
#include <QtTypes>

namespace stroke {
namespace {

constexpr qsizetype maximumSmoothingInput = 2048;

struct SmoothingStage {
  int chaikinPasses;
  qreal rdpTolerance;
};

constexpr SmoothingStage stageForLevel(int level) {
  switch (level) {
  case 1:
    return {1, 0.0};
  case 2:
    return {2, 0.0};
  case 3:
    return {2, 1.0};
  case 4:
    return {2, 2.5};
  case 5:
    return {3, 5.0};
  case 6:
    return {3, 9.0};
  default:
    return {0, 0.0};
  }
}

qreal pointSegmentDistanceSquared(const QPointF &point, const QPointF &a,
                                  const QPointF &b) {
  const QPointF segment = b - a;
  const qreal lengthSquared = QPointF::dotProduct(segment, segment);
  if (lengthSquared <= std::numeric_limits<qreal>::epsilon()) {
    const QPointF delta = point - a;
    return QPointF::dotProduct(delta, delta);
  }
  const qreal position =
      std::clamp(QPointF::dotProduct(point - a, segment) / lengthSquared, 0.0,
                 1.0);
  const QPointF delta = point - (a + segment * position);
  return QPointF::dotProduct(delta, delta);
}

QVector<QPointF> resampleToBudget(const QVector<QPointF> &points,
                                  qsizetype maximumPoints) {
  if (points.size() <= maximumPoints)
    return points;
  qreal totalLength = 0.0;
  for (qsizetype index = 1; index < points.size(); ++index)
    totalLength += QLineF(points.at(index - 1), points.at(index)).length();
  if (totalLength <= std::numeric_limits<qreal>::epsilon())
    return {points.first(), points.last()};

  QVector<QPointF> sampled;
  sampled.reserve(maximumPoints);
  sampled.push_back(points.first());
  const qreal spacing =
      totalLength / static_cast<qreal>(maximumPoints - 1);
  qreal nextDistance = spacing;
  qreal traversed = 0.0;
  for (qsizetype index = 1;
       index < points.size() && sampled.size() + 1 < maximumPoints; ++index) {
    const QPointF &a = points.at(index - 1);
    const QPointF &b = points.at(index);
    const qreal segmentLength = QLineF(a, b).length();
    if (segmentLength <= std::numeric_limits<qreal>::epsilon())
      continue;
    while (nextDistance <= traversed + segmentLength &&
           sampled.size() + 1 < maximumPoints) {
      const qreal position = (nextDistance - traversed) / segmentLength;
      sampled.push_back(a + (b - a) * position);
      nextDistance += spacing;
    }
    traversed += segmentLength;
  }
  sampled.push_back(points.last());
  return sampled;
}

QVector<QPointF> rdpSimplify(const QVector<QPointF> &points,
                            qreal tolerance) {
  if (points.size() < 3 || tolerance <= 0.0)
    return points;

  struct Span {
    qsizetype first;
    qsizetype last;
  };
  QVector<bool> keep(points.size(), false);
  keep[0] = true;
  keep[points.size() - 1] = true;
  QVector<Span> pending{{0, points.size() - 1}};
  const qreal toleranceSquared = tolerance * tolerance;

  // An explicit stack avoids recursive call depth on long scribbles. Typical
  // pen input is simplified by the first few spans, so the release stays fast.
  while (!pending.isEmpty()) {
    const Span span = pending.takeLast();
    if (span.last - span.first < 2)
      continue;
    qreal farthestDistance = -1.0;
    qsizetype farthest = span.first;
    for (qsizetype index = span.first + 1; index < span.last; ++index) {
      const qreal distance = pointSegmentDistanceSquared(
          points.at(index), points.at(span.first), points.at(span.last));
      if (distance > farthestDistance) {
        farthestDistance = distance;
        farthest = index;
      }
    }
    if (farthestDistance <= toleranceSquared)
      continue;
    keep[farthest] = true;
    pending.push_back({span.first, farthest});
    pending.push_back({farthest, span.last});
  }

  QVector<QPointF> simplified;
  simplified.reserve(points.size());
  for (qsizetype index = 0; index < points.size(); ++index) {
    if (keep.at(index))
      simplified.push_back(points.at(index));
  }
  return simplified;
}

QVector<QPointF> chaikinSmooth(const QVector<QPointF> &points, int passes) {
  if (points.size() < 3 || passes <= 0)
    return points;
  QVector<QPointF> current = points;
  for (int pass = 0; pass < passes; ++pass) {
    QVector<QPointF> next;
    next.reserve(current.size() * 2);
    next.push_back(current.first());
    for (qsizetype index = 0; index + 1 < current.size(); ++index) {
      const QPointF &a = current.at(index);
      const QPointF &b = current.at(index + 1);
      next.push_back(a * 0.75 + b * 0.25);
      next.push_back(a * 0.25 + b * 0.75);
    }
    next.push_back(current.last());
    current = std::move(next);
  }
  return current;
}

} // namespace

QVector<QPointF> smoothFreehand(const QVector<QPointF> &points, int level) {
  const int clampedLevel =
      std::clamp(level, minimumSmoothingLevel, maximumSmoothingLevel);
  if (points.size() < 3 || clampedLevel == minimumSmoothingLevel)
    return points;
  const SmoothingStage stage = stageForLevel(clampedLevel);
  // Bound RDP's quadratic worst case and Chaikin's point expansion before
  // either sees an adversarial scribble. Arc-length resampling keeps corners
  // independent of the input event rate; ordinary strokes pass through
  // byte-for-byte before their configured stage.
  const QVector<QPointF> smoothingInput =
      resampleToBudget(points, maximumSmoothingInput);
  return chaikinSmooth(rdpSimplify(smoothingInput, stage.rdpTolerance),
                       stage.chaikinPasses);
}

} // namespace stroke
