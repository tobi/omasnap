#include "icons.hpp"

#include "overlay-chrome.hpp"

#include <QConicalGradient>
#include <QPainter>
#include <QPainterPath>

void drawToolbarIcon(QPainter &painter, const QRectF &bounds,
                     const QString &action, const QString &label,
                     const QColor &color) {
  if (action == QStringLiteral("size")) {
    painter.setPen(color);
    painter.drawText(bounds, Qt::AlignCenter, label);
    return;
  }

  painter.save();
  constexpr qreal iconSize = 19.0;
  painter.translate(bounds.center().x() - iconSize / 2.0,
                    bounds.center().y() - iconSize / 2.0);
  painter.scale(iconSize / 24.0, iconSize / 24.0);
  painter.setPen(QPen(color, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  painter.setBrush(Qt::NoBrush);

  if (action == QStringLiteral("tool-select")) {
    QPainterPath pointer;
    pointer.moveTo(5, 4);
    pointer.lineTo(17, 15);
    pointer.lineTo(12, 16);
    pointer.lineTo(9, 21);
    pointer.closeSubpath();
    painter.drawPath(pointer);
  } else if (action == QStringLiteral("tool-arrow")) {
    painter.drawLine(QPointF(7, 17), QPointF(17, 7));
    painter.drawLine(QPointF(7, 7), QPointF(17, 7));
    painter.drawLine(QPointF(17, 7), QPointF(17, 17));
  } else if (action == QStringLiteral("tool-line")) {
    painter.drawLine(QPointF(5, 19), QPointF(19, 5));
  } else if (action == QStringLiteral("tool-freehand")) {
    QPainterPath stroke;
    stroke.moveTo(4, 16);
    stroke.cubicTo(7, 5, 10, 20, 14, 11);
    stroke.cubicTo(17, 5, 18, 11, 21, 7);
    painter.drawPath(stroke);
  } else if (action == QStringLiteral("tool-highlighter")) {
    painter.setPen(
        QPen(color, 5.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawLine(QPointF(5, 16), QPointF(19, 8));
  } else if (action == QStringLiteral("tool-spotlight")) {
    painter.drawEllipse(QRectF(4, 4, 13, 13));
    painter.drawLine(QPointF(15.5, 15.5), QPointF(20, 20));
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(QPointF(10.5, 10.5), 2.3, 2.3);
  } else if (action == QStringLiteral("tool-marker")) {
    painter.drawEllipse(QPointF(12, 12), 8, 8);
    painter.setFont(chromeFont(11, true));
    painter.drawText(QRectF(4, 4, 16, 16), Qt::AlignCenter,
                     QStringLiteral("1"));
  } else if (action == QStringLiteral("tool-rectangle")) {
    if (label == QStringLiteral("filled"))
      painter.setBrush(color);
    painter.drawRoundedRect(QRectF(4, 4, 16, 16), 2, 2);
  } else if (action == QStringLiteral("tool-ellipse")) {
    if (label == QStringLiteral("filled"))
      painter.setBrush(color);
    painter.drawEllipse(QRectF(3, 6, 18, 12));
  } else if (action == QStringLiteral("tool-redact")) {
    QPainterPath shield;
    shield.moveTo(12, 3);
    shield.lineTo(20, 6);
    shield.lineTo(19, 13);
    shield.cubicTo(18.5, 17, 15.5, 20, 12, 21);
    shield.cubicTo(8.5, 20, 5.5, 17, 5, 13);
    shield.lineTo(4, 6);
    shield.closeSubpath();
    painter.drawPath(shield);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    for (int y = 8; y <= 14; y += 3) {
      for (int x = 8; x <= 14; x += 3) {
        if ((x + y) % 2 == 0)
          painter.drawRect(QRectF(x, y, 3, 3));
      }
    }
  } else if (action == QStringLiteral("tool-cut")) {
    // Two image halves with the removed band collapsing between them.
    painter.drawRect(QRectF(5, 4, 14, 5.5));
    painter.drawRect(QRectF(5, 14.5, 14, 5.5));
    painter.setPen(QPen(color, 1.4, Qt::DashLine, Qt::FlatCap));
    painter.drawLine(QPointF(5, 12), QPointF(19, 12));
  } else if (action == QStringLiteral("tool-cut-insert")) {
    // Two image halves opening, plus in the gap (insert a band).
    painter.drawRect(QRectF(5, 3, 14, 5));
    painter.drawRect(QRectF(5, 16, 14, 5));
    painter.drawLine(QPointF(12, 9.5), QPointF(12, 14.5));
    painter.drawLine(QPointF(9.5, 12), QPointF(14.5, 12));
  } else if (action == QStringLiteral("tool-text")) {
    painter.drawLine(QPointF(5, 5), QPointF(19, 5));
    painter.drawLine(QPointF(12, 5), QPointF(12, 19));
    painter.drawLine(QPointF(9, 19), QPointF(15, 19));
  } else if (action == QStringLiteral("tool-ocr")) {
    QPainterPath path;
    path.moveTo(9, 4);
    path.lineTo(5, 4);
    path.lineTo(5, 8);
    path.moveTo(15, 4);
    path.lineTo(19, 4);
    path.lineTo(19, 8);
    path.moveTo(9, 20);
    path.lineTo(5, 20);
    path.lineTo(5, 16);
    path.moveTo(15, 20);
    path.lineTo(19, 20);
    path.lineTo(19, 16);
    path.moveTo(8, 9);
    path.lineTo(16, 9);
    path.moveTo(8, 13);
    path.lineTo(16, 13);
    path.moveTo(8, 17);
    path.lineTo(13, 17);
    painter.drawPath(path);
  } else if (action == QStringLiteral("tool-eyedropper")) {
    painter.setBrush(color);
    painter.drawEllipse(QPointF(8, 7), 4.2, 4.2);
    painter.setBrush(Qt::NoBrush);
    painter.drawLine(QPointF(11, 10.2), QPointF(15.2, 14.4));
    painter.drawLine(QPointF(13.6, 13.2), QPointF(19.2, 18.8));
    painter.drawLine(QPointF(15.4, 11.4), QPointF(21, 17));
    painter.drawLine(QPointF(18.2, 20.2), QPointF(21.2, 17.2));
  } else if (action == QStringLiteral("custom-color")) {
    QConicalGradient gradient(QPointF(12, 12), 90);
    gradient.setColorAt(0.0, QColor(QStringLiteral("#ff375f")));
    gradient.setColorAt(0.17, QColor(QStringLiteral("#ff9f0a")));
    gradient.setColorAt(0.34, QColor(QStringLiteral("#ffd60a")));
    gradient.setColorAt(0.51, QColor(QStringLiteral("#30d158")));
    gradient.setColorAt(0.68, QColor(QStringLiteral("#0a84ff")));
    gradient.setColorAt(0.85, QColor(QStringLiteral("#bf5af2")));
    gradient.setColorAt(1.0, QColor(QStringLiteral("#ff375f")));
    painter.setPen(QPen(color, 1.5));
    painter.setBrush(gradient);
    painter.drawEllipse(QPointF(12, 12), 8, 8);
    painter.setPen(QPen(Qt::white, 1.5, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(QPointF(12, 8), QPointF(12, 16));
    painter.drawLine(QPointF(8, 12), QPointF(16, 12));
  } else if (action == QStringLiteral("background")) {
    painter.drawRoundedRect(QRectF(3, 4, 18, 16), 2, 2);
    painter.drawEllipse(QPointF(8, 9), 1.5, 1.5);
    QPainterPath path;
    path.moveTo(3, 17);
    path.lineTo(8, 12);
    path.lineTo(11, 15);
    path.lineTo(14, 12);
    path.lineTo(21, 19);
    painter.drawPath(path);
  } else if (action == QStringLiteral("undo") ||
             action == QStringLiteral("redo")) {
    const bool forward = action == QStringLiteral("redo");
    const qreal tip = forward ? 20 : 4;
    const qreal elbow = forward ? 9.5 : 14.5;
    QPainterPath head;
    head.moveTo(tip + (forward ? -5 : 5), 4);
    head.lineTo(tip, 9);
    head.lineTo(tip + (forward ? -5 : 5), 14);
    painter.drawPath(head);
    QPainterPath shaft;
    shaft.moveTo(tip, 9);
    shaft.lineTo(elbow, 9);
    shaft.arcTo(QRectF(elbow - 5.5, 9, 11, 11), 90, forward ? 180 : -180);
    shaft.lineTo(elbow + (forward ? 3.5 : -3.5), 20);
    painter.drawPath(shaft);
  } else if (action == QStringLiteral("copy") ||
             action == QStringLiteral("both")) {
    painter.drawRoundedRect(QRectF(8, 8, 12, 12), 2, 2);
    QPainterPath path;
    path.moveTo(16, 8);
    path.lineTo(16, 6);
    path.quadTo(16, 4, 14, 4);
    path.lineTo(6, 4);
    path.quadTo(4, 4, 4, 6);
    path.lineTo(4, 14);
    path.quadTo(4, 16, 6, 16);
    path.lineTo(8, 16);
    painter.drawPath(path);
    if (action == QStringLiteral("both")) {
      painter.setBrush(QColor(QStringLiteral("#0a84ff")));
      painter.setPen(
          QPen(Qt::white, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      painter.drawEllipse(QPointF(18, 18), 4.5, 4.5);
      painter.drawLine(QPointF(16, 18), QPointF(17.5, 19.5));
      painter.drawLine(QPointF(17.5, 19.5), QPointF(20.5, 16.5));
    }
  } else if (action == QStringLiteral("edit")) {
    QPainterPath pencil;
    pencil.moveTo(4, 20);
    pencil.lineTo(8.5, 18.8);
    pencil.lineTo(19, 8.3);
    pencil.quadTo(20.5, 6.8, 19, 5.3);
    pencil.quadTo(17.5, 3.8, 16, 5.3);
    pencil.lineTo(5.5, 15.8);
    pencil.closeSubpath();
    painter.drawPath(pencil);
    painter.drawLine(QPointF(14.5, 6.8), QPointF(17.5, 9.8));
  } else if (action == QStringLiteral("path")) {
    QPainterPath link;
    link.moveTo(9, 7);
    link.lineTo(7, 7);
    link.cubicTo(3, 7, 3, 17, 7, 17);
    link.lineTo(9, 17);
    link.moveTo(15, 7);
    link.lineTo(17, 7);
    link.cubicTo(21, 7, 21, 17, 17, 17);
    link.lineTo(15, 17);
    painter.drawPath(link);
    painter.drawLine(QPointF(8, 12), QPointF(16, 12));
  } else if (action == QStringLiteral("drag-handle")) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    for (const qreal x : {9.0, 15.0}) {
      for (const qreal y : {6.0, 12.0, 18.0})
        painter.drawEllipse(QPointF(x, y), 1.7, 1.7);
    }
  } else if (action == QStringLiteral("save")) {
    painter.drawLine(QPointF(12, 4), QPointF(12, 15));
    painter.drawLine(QPointF(8, 11), QPointF(12, 15));
    painter.drawLine(QPointF(12, 15), QPointF(16, 11));
    QPainterPath tray;
    tray.moveTo(5, 18);
    tray.lineTo(5, 20);
    tray.lineTo(19, 20);
    tray.lineTo(19, 18);
    painter.drawPath(tray);
  } else if (action == QStringLiteral("pin")) {
    painter.drawRoundedRect(QRectF(8, 4, 8, 6), 2, 2);
    painter.drawLine(QPointF(5, 10), QPointF(19, 10));
    painter.drawLine(QPointF(12, 10), QPointF(12, 20));
  } else if (action == QStringLiteral("close")) {
    painter.drawLine(QPointF(6, 6), QPointF(18, 18));
    painter.drawLine(QPointF(18, 6), QPointF(6, 18));
  }
  painter.restore();
}
