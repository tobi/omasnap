/** @fileoverview Provides pure pinned-window layout helpers. */
#pragma once

#include <QPoint>
#include <QRect>
#include <QSize>

/** Maps a global pointer position to the pin's screen-local top-left. */
[[nodiscard]] QPoint pinPositionFromGlobalPointer(
    const QPoint &globalPointer, const QPoint &screenOrigin,
    const QPoint &pressOffset);
/** Returns the bottom-right slot for an active pin index. */
[[nodiscard]] QPoint pinSlotPosition(const QSize &screenSize,
                                     const QSize &pinSize,
                                     const QSize &slotSize, int index, int gap,
                                     int margin);
/** Keeps a pin rectangle fully inside screen bounds where possible. */
[[nodiscard]] QRect clampPinGeometry(const QRect &pin, const QRect &bounds);
