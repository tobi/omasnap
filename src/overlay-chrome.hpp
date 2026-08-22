/** @fileoverview The chrome every full-screen overlay wears: the mode badge at
 *  the top, the hotkey guide in the corner, and the status pill along the
 *  bottom. Capture and scroll capture are the same tool in two moods, so they
 *  are drawn by the same code rather than by two that drift apart. */
#pragma once

#include <QColor>
#include <QPair>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QVector>

class QPainter;

/// The badge naming what the overlay is doing, centered at the top, with the ×
/// that leaves it. Returns the whole badge; `closeRect` is the × alone, for
/// hit-testing the click that closes.
QRectF drawModeBadge(QPainter &painter, const QRect &bounds,
                     const QString &label, const QColor &accent,
                     QRectF *closeRect = nullptr);

/// The two-column key guide, pinned to the top-right corner, and moved to the
/// left when the pointer is over it, so it never hides what is underneath.
void drawHotkeyLegend(QPainter &painter, const QRect &bounds,
                      const QPointF &cursor,
                      const QVector<QPair<QString, QString>> &entries);

/// The instruction line along the bottom.
void drawStatusPill(QPainter &painter, const QRect &bounds,
                    const QString &text);
