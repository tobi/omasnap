/** @fileoverview Shared overlay chrome: pinned fonts, the mode badge, hotkey
 *  guide, and status pill. */
#pragma once

#include <QColor>
#include <QPair>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QVector>

class QFont;
class QPainter;

/// The typeface for overlay chrome text (toolbar labels, hotkey legend,
/// tooltips): pinned in code, never the platform theme's system font, so
/// startup does not depend on a theme plugin and the look is the same on
/// every install. Adwaita Sans is the stock Omarchy UI font; Noto Sans is
/// the bundled fallback.
[[nodiscard]] QFont chromeFont(int pixelSize, bool bold = false);
/// The application-wide default font, installed by main() before any widget
/// exists: the same face and 11 pt size the gtk3 platform theme used to
/// supply, so text drawn with a painter's or widget's default font (pin
/// tips, scroll-panel buttons) does not shrink or change family now that
/// the external desktop theme is bypassed.
[[nodiscard]] QFont chromeDefaultFont();
/// Monospace counterpart for numeric readouts: fontconfig's `monospace`
/// alias, which is what the fixed-font lookup resolved to under every theme
/// (on Omarchy, the face `omarchy-font-set` chose).
[[nodiscard]] QFont chromeMonoFont(int pixelSize, bool bold = false);

/// The badge naming what the overlay is doing, centered at the top, with the ×
/// that leaves it. Returns the whole badge; `closeRect` is the × alone, for
/// hit-testing the click that closes.
QRectF drawModeBadge(QPainter &painter, const QRect &bounds,
                     const QString &label, const QColor &accent,
                     QRectF *closeRect = nullptr);

/// A single, backgroundless column of `key  action` pairs along the bottom
/// left, growing upward, in low-opacity text. Hotkeys are a reference, not
/// UI: there is no card, no border, and no attempt to dodge the pointer or
/// dodge anything else — draw it early (right after the overlay's initial
/// dim fill, before the image, toolbar, or any popup) and
/// normal paint order does the rest, since whatever is drawn afterward
/// simply covers it wherever the two overlap.
void drawHotkeyLegend(QPainter &painter, const QRect &bounds,
                      const QVector<QPair<QString, QString>> &entries);

/// The instruction line along the bottom.
void drawStatusPill(QPainter &painter, const QRect &bounds,
                    const QString &text);
