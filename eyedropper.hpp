/** @fileoverview Pure source-pixel sampling helper for the editor eyedropper. */
#pragma once

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QSizeF>

/** Samples the original source image at a point in the displayed selection. */
[[nodiscard]] QColor sampleSourceColor(const QImage &source,
                                       const QSizeF &previewSize,
                                       const QRectF &selection,
                                       const QRectF &displayRect,
                                       const QPointF &displayPoint);
