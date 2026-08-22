/** @fileoverview Shared overlay chrome (see overlay-chrome.hpp). */
#include "overlay-chrome.hpp"

#include <QFontDatabase>
#include <QPainter>

#include <algorithm>

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
                      const QPointF &cursor,
                      const QVector<QPair<QString, QString>> &entries) {
  if (entries.isEmpty())
    return;
  constexpr int columns = 2;
  const int rows = (entries.size() + columns - 1) / columns;
  QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
  font.setPixelSize(11);
  painter.setFont(font);
  // Measured, not guessed: a fixed column width clipped the longer lines, and
  // a guide that trails off is worse than no guide.
  const QFontMetricsF metrics(font);
  constexpr qreal keyGap = 12;      // between a key and what it does
  constexpr qreal columnGap = 24;   // between the two columns
  constexpr qreal padding = 12;     // panel edge to text
  qreal keyWidth[columns] = {};
  qreal textWidth[columns] = {};
  for (int index = 0; index < entries.size(); ++index) {
    const int column = std::min(index / rows, columns - 1);
    keyWidth[column] =
        std::max(keyWidth[column], metrics.horizontalAdvance(entries.at(index).first));
    textWidth[column] = std::max(
        textWidth[column], metrics.horizontalAdvance(entries.at(index).second));
  }
  qreal columnWidth[columns] = {};
  qreal width = 2 * padding;
  for (int column = 0; column < columns; ++column) {
    if (keyWidth[column] <= 0 && textWidth[column] <= 0)
      continue;
    columnWidth[column] = keyWidth[column] + keyGap + textWidth[column];
    width += columnWidth[column];
    if (column > 0)
      width += columnGap;
  }
  width = std::min(width, bounds.width() - 28.0);
  const qreal height = rows * 19 + 24;
  QRectF panel(bounds.width() - width - 14, 14, width, height);
  if (panel.adjusted(-28, -28, 28, 28).contains(cursor))
    panel.moveLeft(14);

  painter.setPen(QPen(QColor(255, 255, 255, 34), 1));
  painter.setBrush(QColor(13, 15, 20, 224));
  painter.drawRoundedRect(panel, 11, 11);
  painter.setFont(font);
  for (int index = 0; index < entries.size(); ++index) {
    const int column = std::min(index / rows, columns - 1);
    const int row = index % rows;
    qreal x = panel.left() + padding;
    for (int before = 0; before < column; ++before)
      x += columnWidth[before] + columnGap;
    const qreal y = panel.top() + 12 + row * 19;
    painter.setPen(QColor(QStringLiteral("#a9b6cb")));
    painter.drawText(QRectF(x, y, keyWidth[column], 18),
                     Qt::AlignLeft | Qt::AlignVCenter, entries.at(index).first);
    painter.setPen(QColor(QStringLiteral("#f5f5f7")));
    painter.drawText(QRectF(x + keyWidth[column] + keyGap, y, textWidth[column],
                            18),
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
