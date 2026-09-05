/** @fileoverview Adjustable post-stroke smoothing over raw pen input. */
#pragma once

#include <QPointF>
#include <QVector>
#include <QtTypes>

namespace stroke {

inline constexpr int minimumSmoothingLevel = 0;
inline constexpr int maximumSmoothingLevel = 6;
inline constexpr int defaultSmoothingLevel = 3;

/**
 * Release-time pen levels: 0 preserves the raw pointer path, 1--2 apply one or
 * two anchored Chaikin passes, and 3--6 progressively simplify with
 * Ramer-Douglas-Peucker before two or three Chaikin passes. The first and last
 * points are always preserved, and strokes shorter than three points
 * (including stored dots) pass through untouched.
 */
[[nodiscard]] QVector<QPointF>
smoothFreehand(const QVector<QPointF> &points,
               int level = defaultSmoothingLevel);

} // namespace stroke
