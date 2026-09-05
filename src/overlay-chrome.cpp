/** @fileoverview Shared overlay chrome (see overlay-chrome.hpp). */
#include "overlay-chrome.hpp"

#include <QFont>
#include <QFontMetricsF>
#include <QString>
#include <QStringList>
#include <QPainter>

#include <algorithm>

QFont chromeFont(int pixelSize, bool bold) {
  static const QFont base = [] {
    QFont font;
    font.setFamilies({QStringLiteral("Adwaita Sans"),
                      QStringLiteral("Noto Sans")});
    return font;
  }();
  QFont font = base;
  font.setPixelSize(pixelSize);
  font.setBold(bold);
  return font;
}

QFont chromeDefaultFont() {
  QFont font;
  font.setFamilies({QStringLiteral("Adwaita Sans"),
                    QStringLiteral("Noto Sans")});
  font.setPointSize(11);
  return font;
}

QFont chromeMonoFont(int pixelSize, bool bold) {
  static const QFont base = [] {
    QFont font;
    font.setFamilies({QStringLiteral("monospace")});
    return font;
  }();
  QFont font = base;
  font.setPixelSize(pixelSize);
  font.setBold(bold);
  return font;
}

QString captureTabLabel(CaptureKind kind) {
  switch (kind) {
  case CaptureKind::Region:
    return QStringLiteral("REGION");
  case CaptureKind::Scroll:
    return QStringLiteral("SCROLLING REGION");
  case CaptureKind::Window:
    return QStringLiteral("WINDOW");
  case CaptureKind::Fullscreen:
    return QStringLiteral("FULLSCREEN");
  }
  return {};
}

namespace {
QFont captureTabFont() {
  QFont font(QStringLiteral("Noto Sans"));
  font.setBold(true);
  font.setPixelSize(11);
  return font;
}
QColor captureTabAccent(CaptureKind kind) {
  switch (kind) {
  case CaptureKind::Window:
    return QColor(QStringLiteral("#ffd60a"));
  case CaptureKind::Fullscreen:
    return QColor(QStringLiteral("#0a84ff"));
  case CaptureKind::Region:
  case CaptureKind::Scroll:
    break;
  }
  return QColor(QStringLiteral("#30d158"));
}
} // namespace

QVector<CaptureTab> captureTabLayout(const QRect &bounds) {
  static const CaptureKind order[] = {CaptureKind::Region, CaptureKind::Window,
                                      CaptureKind::Scroll,
                                      CaptureKind::Fullscreen};
  const QFontMetricsF metrics(captureTabFont());
  constexpr qreal kPad = 14.0;
  constexpr qreal kGap = 2.0;
  constexpr qreal kHeight = 26.0;
  // Flush to the top edge (see drawCaptureTabs' -30 background extension):
  // derived from kCaptureTabBarBottom rather than a separate magic number,
  // so the two can't drift apart.
  constexpr qreal kTop = kCaptureTabBarBottom - kHeight - 5.0;
  QVector<CaptureTab> tabs;
  qreal total = 0.0;
  for (const CaptureKind kind : order) {
    const qreal w = metrics.horizontalAdvance(captureTabLabel(kind)) + 2 * kPad;
    tabs.push_back({kind, QRectF(total, kTop, w, kHeight)});
    total += w + kGap;
  }
  total -= kGap;
  const qreal left = bounds.left() + (bounds.width() - total) / 2.0;
  for (CaptureTab &tab : tabs)
    tab.rect.translate(left, 0);
  return tabs;
}

int captureTabAt(const QVector<CaptureTab> &tabs, const QPointF &position) {
  for (int index = 0; index < tabs.size(); ++index) {
    if (tabs.at(index).rect.adjusted(-2, -6, 2, 6).contains(position))
      return index;
  }
  return -1;
}

void drawCaptureTabs(QPainter &painter, const QVector<CaptureTab> &tabs,
                     CaptureKind active, const QPointF &cursor) {
  if (tabs.isEmpty())
    return;
  // Hangs off the top edge like a tab strip: square at the top (drawn past
  // the edge so only the bottom corners round), not a floating pill.
  const QRectF bar = tabs.constFirst().rect.united(tabs.constLast().rect)
                         .adjusted(-5, -30, 5, 5);
  painter.setPen(QPen(QColor(255, 255, 255, 32), 1));
  painter.setBrush(QColor(18, 18, 22, 235));
  painter.drawRoundedRect(bar, 12, 12);
  painter.setFont(captureTabFont());
  const int hovered = captureTabAt(tabs, cursor);
  for (int index = 0; index < tabs.size(); ++index) {
    const CaptureTab &tab = tabs.at(index);
    painter.setPen(Qt::NoPen);
    if (tab.kind == active) {
      painter.setBrush(captureTabAccent(tab.kind));
      painter.drawRoundedRect(tab.rect, 9, 9);
      painter.setPen(QColor(18, 18, 22));
    } else {
      if (index == hovered) {
        painter.setBrush(QColor(255, 255, 255, 28));
        painter.drawRoundedRect(tab.rect, 9, 9);
      }
      painter.setPen(QColor(255, 255, 255, index == hovered ? 255 : 190));
    }
    painter.drawText(tab.rect, Qt::AlignCenter, captureTabLabel(tab.kind));
  }
}

QRectF drawModeBadge(QPainter &painter, const QRect &bounds,
                     const QString &label, const QColor &accent,
                     QRectF *closeRect) {
  QFont badgeFont(QStringLiteral("Noto Sans"));
  badgeFont.setBold(true);
  badgeFont.setPixelSize(11);
  painter.setFont(badgeFont);
  const QString badge = label + QStringLiteral("  ×");
  const int badgeWidth = painter.fontMetrics().horizontalAdvance(badge) + 24;
  const QRectF badgeRect((bounds.width() - badgeWidth) / 2.0, 12, badgeWidth,
                         32);
  painter.setPen(QPen(QColor(255, 255, 255, 32), 1));
  painter.setBrush(QColor(18, 18, 22, 235));
  painter.drawRoundedRect(badgeRect, 10, 10);
  painter.setPen(accent);
  painter.drawText(badgeRect, Qt::AlignCenter, badge);
  if (closeRect) {
    // The × and a little around it, so the click that closes has a target
    // rather than a pixel.
    const qreal closeWidth =
        painter.fontMetrics().horizontalAdvance(QStringLiteral("×")) + 18;
    *closeRect = QRectF(badgeRect.right() - closeWidth, badgeRect.top(),
                        closeWidth, badgeRect.height());
  }
  return badgeRect;
}

void drawHotkeyLegend(QPainter &painter, const QRect &bounds,
                      const QVector<QPair<QString, QString>> &entries) {
  if (entries.isEmpty())
    return;
  const QFont font = chromeFont(11);
  const QFontMetricsF metrics(font);
  constexpr qreal keyGap = 10;    // between a key and what it does
  constexpr qreal marginLeft = 14;
  constexpr qreal marginBottom = 14;
  constexpr qreal rowHeight = 17;
  qreal keyWidth = 0.0;
  for (const auto &entry : entries)
    keyWidth = std::max(keyWidth, metrics.horizontalAdvance(entry.first));
  painter.setFont(font);
  // Bottom-left, growing upward: entry 0 is the bottom-most row. No card, no
  // border, no dodging the pointer or anything else — the caller draws this
  // early, so real chrome painted afterward simply covers it where the two
  // overlap, and the low opacity keeps it out of the way where nothing does.
  for (int index = 0; index < entries.size(); ++index) {
    const qreal y =
        bounds.height() - marginBottom - (index + 1) * rowHeight;
    painter.setPen(QColor(169, 182, 203, 165));
    painter.drawText(QRectF(marginLeft, y, keyWidth, rowHeight - 2),
                     Qt::AlignLeft | Qt::AlignVCenter, entries.at(index).first);
    painter.setPen(QColor(199, 204, 214, 130));
    painter.drawText(
        QRectF(marginLeft + keyWidth + keyGap, y,
               bounds.width() - marginLeft - keyWidth - keyGap - 14,
               rowHeight - 2),
        Qt::AlignLeft | Qt::AlignVCenter, entries.at(index).second);
  }
}

namespace {
constexpr qreal kLegendKeyGapAnchored = 8;
constexpr qreal kLegendColumnGapAnchored = 14;
constexpr qreal kLegendPaddingAnchored = 12;

struct AnchoredLegendLayout {
  int columns = 2;
  int rows = 0;
  QVector<qreal> keyWidth;
  QVector<qreal> textWidth;
  QVector<qreal> columnWidth;
  qreal width = 0;
};

QFont anchoredLegendFont() {
  QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
  font.setPixelSize(11);
  return font;
}

AnchoredLegendLayout measureAnchoredLegend(
    const QVector<QPair<QString, QString>> &entries, int columns) {
  const QFontMetricsF metrics{anchoredLegendFont()};
  AnchoredLegendLayout layout;
  layout.columns = columns;
  layout.rows = (entries.size() + columns - 1) / columns;
  layout.keyWidth = QVector<qreal>(columns, 0.0);
  layout.textWidth = QVector<qreal>(columns, 0.0);
  for (int index = 0; index < entries.size(); ++index) {
    const int column = std::min(index / layout.rows, columns - 1);
    layout.keyWidth[column] =
        std::max(layout.keyWidth[column],
                 metrics.horizontalAdvance(entries.at(index).first));
    layout.textWidth[column] =
        std::max(layout.textWidth[column],
                 metrics.horizontalAdvance(entries.at(index).second));
  }
  layout.columnWidth = QVector<qreal>(columns, 0.0);
  layout.width = 2 * kLegendPaddingAnchored;
  for (int column = 0; column < columns; ++column) {
    if (layout.keyWidth[column] <= 0 && layout.textWidth[column] <= 0)
      continue;
    layout.columnWidth[column] = layout.keyWidth[column] +
                                 kLegendKeyGapAnchored +
                                 layout.textWidth[column];
    layout.width += layout.columnWidth[column];
    if (column > 0)
      layout.width += kLegendColumnGapAnchored;
  }
  return layout;
}

AnchoredLegendLayout fitAnchoredLegend(
    const QVector<QPair<QString, QString>> &entries, qreal maxWidth) {
  AnchoredLegendLayout layout;
  for (int tryRows = 4;; ++tryRows) {
    layout = measureAnchoredLegend(
        entries, std::max(1, static_cast<int>((entries.size() + tryRows - 1) /
                                              tryRows)));
    if (layout.width <= maxWidth || layout.columns == 1)
      return layout;
  }
}
} // namespace

QSize hotkeyLegendAnchoredSize(const QVector<QPair<QString, QString>> &entries,
                               qreal maxWidth) {
  if (entries.isEmpty())
    return {};
  const AnchoredLegendLayout layout = fitAnchoredLegend(entries, maxWidth);
  return {qRound(std::min(layout.width, maxWidth)), layout.rows * 19 + 24};
}

void drawAnchoredHotkeyLegend(QPainter &painter, const QRect &bounds,
                              const QVector<QPair<QString, QString>> &entries,
                              const QRectF &anchorAbove) {
  if (entries.isEmpty())
    return;
  const QFont font = anchoredLegendFont();
  const AnchoredLegendLayout layout =
      fitAnchoredLegend(entries, bounds.width() - 28.0);
  const qreal width = std::min(layout.width, bounds.width() - 28.0);
  const qreal height = layout.rows * 19 + 24;
  // Pinned to the window's top, centered over the anchor: the windowed
  // layout reserves the room, so the guide never covers the toolbar or the
  // canvas.
  const qreal x =
      std::clamp(anchorAbove.center().x() - width / 2.0, 14.0,
                 std::max(14.0, bounds.width() - width - 14.0));
  const QRectF panel(x, 14.0, width, height);
  painter.setPen(QPen(QColor(255, 255, 255, 34), 1));
  painter.setBrush(QColor(13, 15, 20, 224));
  painter.drawRoundedRect(panel, 11, 11);
  painter.setFont(font);
  for (int index = 0; index < entries.size(); ++index) {
    const int column = std::min(index / layout.rows, layout.columns - 1);
    const int row = index % layout.rows;
    qreal cell = panel.left() + kLegendPaddingAnchored;
    for (int before = 0; before < column; ++before)
      cell += layout.columnWidth[before] + kLegendColumnGapAnchored;
    const qreal top = panel.top() + 12 + row * 19;
    painter.setPen(QColor(QStringLiteral("#a9b6cb")));
    painter.drawText(QRectF(cell, top, layout.keyWidth[column], 18),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     entries.at(index).first);
    painter.setPen(QColor(QStringLiteral("#f5f5f7")));
    painter.drawText(QRectF(cell + layout.keyWidth[column] +
                                kLegendKeyGapAnchored,
                            top, layout.textWidth[column], 18),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     entries.at(index).second);
  }
}

void drawStatusPill(QPainter &painter, const QRect &bounds,
                    const QString &text) {
  QFont font(QStringLiteral("Noto Sans"));
  font.setPixelSize(13);
  painter.setFont(font);
  const int width = painter.fontMetrics().horizontalAdvance(text) + 28;
  const QRectF pill((bounds.width() - width) / 2.0, bounds.height() - 42.0,
                    width, 30);
  painter.setPen(QPen(QColor(255, 255, 255, 32), 1));
  painter.setBrush(QColor(18, 18, 22, 232));
  painter.drawRoundedRect(pill, 10, 10);
  painter.setPen(Qt::white);
  painter.drawText(pill, Qt::AlignCenter, text);
}
