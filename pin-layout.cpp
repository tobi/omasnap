/** @fileoverview Implements pinned-window stacking and clamping helpers. */
#include "pin-layout.hpp"

#include <algorithm>

QPoint pinPositionFromGlobalPointer(const QPoint &globalPointer,
                                    const QPoint &screenOrigin,
                                    const QPoint &pressOffset) {
  return globalPointer - screenOrigin - pressOffset;
}

QPoint pinSlotPosition(const QSize &screenSize, const QSize &pinSize,
                       const QSize &slotSize, int index, int gap, int margin) {
  const int verticalSpace = std::max(1, screenSize.height() - 2 * margin);
  const int rowHeight = std::max(1, slotSize.height() + gap);
  const int rows = std::max(1, (verticalSpace + gap) / rowHeight);
  const int row = std::max(0, index) % rows;
  const int column = std::max(0, index) / rows;
  const int x = screenSize.width() - margin - pinSize.width() -
                column * (std::max(1, slotSize.width()) + gap);
  const int y = screenSize.height() - margin - pinSize.height() -
                row * rowHeight;
  return {x, y};
}

QRect clampPinGeometry(const QRect &pin, const QRect &bounds) {
  if (bounds.isEmpty())
    return pin;
  const int x = std::clamp(pin.x(), bounds.left(),
                          std::max(bounds.left(), bounds.right() - pin.width() + 1));
  const int y = std::clamp(pin.y(), bounds.top(),
                          std::max(bounds.top(), bounds.bottom() - pin.height() + 1));
  return {x, y, pin.width(), pin.height()};
}
