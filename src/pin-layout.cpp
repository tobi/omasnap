/** @fileoverview Implements pinned-window stacking and dispatch helpers. */
#include "pin-layout.hpp"

#include <algorithm>
#include <cmath>

QSize pinFrameSize(const QSize &screenSize) {
  constexpr int width = 200;
  const double aspect =
      screenSize.width() > 0 && screenSize.height() > 0
          ? static_cast<double>(screenSize.height()) / screenSize.width()
          : 9.0 / 16.0;
  const int height = std::clamp(static_cast<int>(std::lround(width * aspect)),
                                width / 4, width * 2);
  return {width, height};
}

QPoint pinPackedPosition(const QVector<QRect> &blockers,
                         const QSize &screenSize, const QSize &frame, int gap,
                         int margin) {
  int x = screenSize.width() - margin - frame.width();
  for (int column = 0; column < 8; ++column) {
    int y = screenSize.height() - margin - frame.height();
    while (y >= margin) {
      const QRect candidate(x, y, frame.width(), frame.height());
      int lowestTop = -1;
      for (const QRect &blocker : blockers) {
        if (candidate.intersects(blocker))
          lowestTop = std::max(lowestTop, blocker.top());
      }
      if (lowestTop < 0)
        return {x, y};
      // Climb to one gap above the lowest pin in the way, then look again:
      // the spot up there may graze another one.
      y = lowestTop - gap - frame.height();
    }
    x -= frame.width() + gap;
  }
  return {screenSize.width() - margin - frame.width(),
          screenSize.height() - margin - frame.height()};
}

PinInsertionPlan pinInsertionPlan(QVector<QPair<QString, QRect>> column,
                                  const QVector<QRect> &blockers,
                                  const QRect &dragged,
                                  const QSize &screenSize, int gap,
                                  int margin) {
  PinInsertionPlan plan;
  std::sort(column.begin(), column.end(),
            [](const auto &a, const auto &b) {
              return a.second.y() > b.second.y();
            });
  // The dragged pin's place in the order comes from its center against the
  // column as it would pack, not against the possibly already-spread live
  // positions, so the preview does not chase its own moves.
  QVector<QRect> seed = blockers;
  QVector<QRect> packed;
  QVector<int> packedCenters;
  for (const auto &pair : column) {
    const QPoint at =
        pinPackedPosition(seed, screenSize, pair.second.size(), gap, margin);
    seed.push_back(QRect(at, pair.second.size()));
    packed.push_back(QRect(at, pair.second.size()));
    packedCenters.push_back(at.y() + pair.second.height() / 2);
  }
  // Touching any part of the stack joins it; fully outside stays out. The
  // stack includes the open spot on top, which is where a pin dragged off
  // the top of the stack came from: it snaps back until it has been
  // dragged fully past where it would sit. For an empty column that spot
  // is the corner itself.
  QVector<QRect> stack = packed;
  stack.push_back(
      QRect(pinPackedPosition(seed, screenSize, dragged.size(), gap, margin),
            dragged.size()));
  // The pins' live positions count too: a stack that has not packed down
  // yet is still the stack the user sees and aims for.
  for (const auto &pair : column)
    stack.push_back(pair.second);
  // The region a drag folds back into is the whole column band: the
  // bounding box of every pin and seat, which always reaches the bottom
  // corner because packing anchors there. Anywhere inside it is where some
  // pin would live, not just the dragged pin's own former spot; only fully
  // outside it stays out.
  QRect band;
  for (const QRect &rect : stack)
    band |= rect;
  if (!dragged.intersects(band))
    return plan;
  int index = 0;
  for (const int centerY : packedCenters)
    index += centerY > dragged.center().y() ? 1 : 0;
  plan.index = index;

  // Pack again with a dragged-sized hole at the insertion point.
  seed = blockers;
  for (int position = 0; position < column.size(); ++position) {
    if (position == index) {
      const QPoint at =
          pinPackedPosition(seed, screenSize, dragged.size(), gap, margin);
      plan.spot = QRect(at, dragged.size());
      seed.push_back(plan.spot);
    }
    const auto &pair = column.at(position);
    const QPoint at =
        pinPackedPosition(seed, screenSize, pair.second.size(), gap, margin);
    seed.push_back(QRect(at, pair.second.size()));
    plan.spread.push_back({pair.first, QRect(at, pair.second.size())});
  }
  if (index == column.size()) {
    const QPoint at =
        pinPackedPosition(seed, screenSize, dragged.size(), gap, margin);
    plan.spot = QRect(at, dragged.size());
  }
  return plan;
}

bool pinInColumn(const QRect &rect, const QSize &screenSize, int margin) {
  constexpr int tolerance = 6;
  return std::abs(rect.right() + 1 - (screenSize.width() - margin)) <=
         tolerance;
}

namespace {
// The whole expression is one dispatch argument; the title has a space in
// it, so the selector is quoted inside the expression rather than around it.
QString windowSelector(const QString &title) {
  return QStringLiteral("window = \"title:^(%1)$\"").arg(title);
}
} // namespace

QString pinFloatDispatch(const QString &title) {
  return QStringLiteral("hl.dsp.window.float({ %1 })")
      .arg(windowSelector(title));
}

QString pinPinDispatch(const QString &title) {
  return QStringLiteral("hl.dsp.window.pin({ %1 })").arg(windowSelector(title));
}

QString pinMoveDispatch(const QString &title, int x, int y) {
  return QStringLiteral(
             "hl.dsp.window.move({ x = %1, y = %2, relative = false, %3 })")
      .arg(x)
      .arg(y)
      .arg(windowSelector(title));
}

QString pinSwayArrangeCommand(const QString &title, int x, int y) {
  return QStringLiteral("[title=\"^%1$\"] floating enable, sticky enable, "
                        "move absolute position %2 %3")
      .arg(title)
      .arg(x)
      .arg(y);
}

QString pinSwayMoveCommand(const QString &title, int x, int y) {
  return QStringLiteral("[title=\"^%1$\"] move absolute position %2 %3")
      .arg(title)
      .arg(x)
      .arg(y);
}

QString pinControlTip(int index) {
  switch (index) {
  case 0:
    return QStringLiteral("Close · Esc or middle-click");
  case 1:
    return QStringLiteral("Copy image to clipboard");
  case 2:
    return QStringLiteral("Copy file path");
  case 3:
    return QStringLiteral("Edit in omasnap");
  case 4:
    return QStringLiteral("Drag this image out");
  default:
    return {};
  }
}
