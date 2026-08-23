#pragma once

#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QVector>

enum class ShelfPresentation { Stacked, Expanded, Tucked };

struct CaptureShelfLayout {
  QSize size;
  QVector<QRectF> thumbnails;
  QRectF handle;
};

/** Returns the bottom-right Shelf layout. Index zero is always the newest. */
[[nodiscard]] CaptureShelfLayout
captureShelfLayout(int itemCount, ShelfPresentation presentation);

/** Returns the front-most thumbnail at `position`, or -1 outside the stack. */
[[nodiscard]] int captureShelfItemAt(const CaptureShelfLayout &layout,
                                     const QPointF &position);

[[nodiscard]] constexpr int captureShelfMaximumItems() { return 5; }
