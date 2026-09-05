/** @fileoverview The chrome every full-screen overlay wears: the mode badge at
 *  the top, the hotkey guide in the corner, and the status pill along the
 *  bottom. Capture and scroll capture are the same tool in two moods, so they
 *  are drawn by the same code rather than by two that drift apart. */
#pragma once

#include <QColor>
#include <QPair>
#include <QRect>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

class QFont;
class QPainter;

/// The typeface for overlay chrome text (toolbar labels, hotkey legend,
/// tooltips): pinned in code, never the platform theme's system font, so
/// startup does not depend on a theme plugin and the look is the same on
/// every install. Adwaita Sans is the stock Omarchy UI font; Noto Sans is
/// the fallback the tab strip already uses.
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

/// The kinds of capture the tab strip across the top offers, on every
/// overlay. Region and Window are modes of the area overlay, Scroll is the
/// scroll overlay, and Fullscreen acts at once.
enum class CaptureKind { Region, Scroll, Window, Fullscreen };
struct CaptureTab {
  CaptureKind kind;
  QRectF rect;
};
/// Visible height of the tab strip's background, from the top edge (the
/// strip is flush against it) to its rounded bottom — fixed regardless of
/// window size, since only the horizontal layout changes with the surface.
/// Chrome stacked below the strip anchors to this, not a guessed constant.
constexpr qreal kCaptureTabBarBottom = 31.0;
[[nodiscard]] QString captureTabLabel(CaptureKind kind);
/// Tab positions for a surface of `bounds`, hanging off the top edge.
[[nodiscard]] QVector<CaptureTab> captureTabLayout(const QRect &bounds);
/// Index of the tab under `position`, or -1.
[[nodiscard]] int captureTabAt(const QVector<CaptureTab> &tabs,
                               const QPointF &position);
/// Draws the strip; `active` is lit, the tab under `cursor` is hinted.
void drawCaptureTabs(QPainter &painter, const QVector<CaptureTab> &tabs,
                     CaptureKind active, const QPointF &cursor);

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
/// dim fill, before the image, the tab strip, the toolbar, any popup) and
/// normal paint order does the rest, since whatever is drawn afterward
/// simply covers it wherever the two overlap.
void drawHotkeyLegend(QPainter &painter, const QRect &bounds,
                      const QVector<QPair<QString, QString>> &entries);

/// The size the key guide takes in its wide anchored form (a windowed
/// editor's toolbar band) when it may be at most `maxWidth` wide: rows are
/// added until the card fits.
[[nodiscard]] QSize hotkeyLegendAnchoredSize(
    const QVector<QPair<QString, QString>> &entries, qreal maxWidth);

/// A wide key card a few rows tall, centered over `anchorAbove`: a compact
/// window has no clear margin for the overlay's backgroundless column, so
/// the windowed layout reserves this card room instead.
void drawAnchoredHotkeyLegend(QPainter &painter, const QRect &bounds,
                              const QVector<QPair<QString, QString>> &entries,
                              const QRectF &anchorAbove);

/// The instruction line along the bottom.
void drawStatusPill(QPainter &painter, const QRect &bounds,
                    const QString &text);
