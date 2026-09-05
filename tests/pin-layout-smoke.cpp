/** @fileoverview Tests stacked pin slots and compositor dispatch strings. */
#include "pin-layout-smoke.hpp"

#include "pin-layout.hpp"

#include <QSet>

bool runPinLayoutSmoke(QString &error) {
  // The frame follows the display's shape at a fixed width, clamps the
  // extremes, and guesses 16:9 when the display cannot be asked.
  if (pinFrameSize(QSize(2560, 1600)) != QSize(200, 125) ||
      pinFrameSize(QSize(3440, 1440)) != QSize(200, 84) ||
      pinFrameSize(QSize(1080, 1920)) != QSize(200, 356) ||
      pinFrameSize(QSize(1000, 5000)) != QSize(200, 400) ||
      pinFrameSize(QSize(5000, 500)) != QSize(200, 50) ||
      pinFrameSize(QSize()) != QSize(200, 113)) {
    error = QStringLiteral("Pin frames did not follow the display's shape");
    return false;
  }

  // Every control explains itself; an index outside the controls is empty.
  QSet<QString> tips;
  for (int control = 0; control < 5; ++control) {
    if (pinControlTip(control).isEmpty()) {
      error = QStringLiteral("A pin control has no tooltip");
      return false;
    }
    tips.insert(pinControlTip(control));
  }
  if (tips.size() != 5 || !pinControlTip(5).isEmpty() ||
      !pinControlTip(-1).isEmpty()) {
    error = QStringLiteral("Pin control tooltips repeat or overflow");
    return false;
  }

  const QSize screen(400, 300);
  const QSize pin(100, 80);

  // An empty corner takes the first pin snug against the margins; the next
  // ones pack one gap above whatever is there, whatever its size, and a
  // full column starts a new one to the left.
  const QPoint first = pinPackedPosition({}, screen, pin, 10, 14);
  if (first != QPoint(286, 206)) {
    error = QStringLiteral("The first pin did not land in the corner");
    return false;
  }
  const QPoint second =
      pinPackedPosition({QRect(first, pin)}, screen, pin, 10, 14);
  if (second != QPoint(286, 116)) {
    error = QStringLiteral("The second pin did not pack above the first");
    return false;
  }
  const QRect oddSize(QPoint(280, 150), QSize(110, 130));
  if (pinPackedPosition({oddSize}, screen, pin, 10, 14) != QPoint(286, 60)) {
    error = QStringLiteral("An odd-sized pin was not packed above snugly");
    return false;
  }
  const QVector<QRect> fullColumn{QRect(286, 206, 100, 80),
                                  QRect(286, 116, 100, 80),
                                  QRect(286, 26, 100, 80)};
  if (pinPackedPosition(fullColumn, screen, pin, 10, 14) !=
      QPoint(176, 206)) {
    error = QStringLiteral("A full column did not wrap to a new one");
    return false;
  }
  const QRect elsewhere(QPoint(20, 20), QSize(100, 80));
  if (pinPackedPosition({elsewhere}, screen, pin, 10, 14) !=
      QPoint(286, 206)) {
    error = QStringLiteral("A pin away from the column blocked the corner");
    return false;
  }

  // Column membership is hugging the right edge; dragging a pin away from
  // it takes the pin out of the column, whatever its height.
  if (!pinInColumn(QRect(286, 26, 100, 80), screen, 14) ||
      !pinInColumn(QRect(282, 140, 104, 120), screen, 14) ||
      pinInColumn(QRect(200, 26, 100, 80), screen, 14)) {
    error = QStringLiteral("Column membership did not follow the right edge");
    return false;
  }

  // Dragging a pin over the column spreads the others around a hole where
  // it would land; covering half the hole or more is close enough to snap.
  const QVector<QPair<QString, QRect>> column{
      {QStringLiteral("low"), QRect(286, 206, 100, 80)},
      {QStringLiteral("high"), QRect(286, 116, 100, 80)}};
  const PinInsertionPlan between = pinInsertionPlan(
      column, {}, QRect(286, 140, 100, 80), screen, 10, 14);
  if (between.index != 1 || between.spot != QRect(286, 116, 100, 80) ||
      between.spread.size() != 2 ||
      between.spread.at(0) !=
          QPair(QStringLiteral("low"), QRect(286, 206, 100, 80)) ||
      between.spread.at(1) !=
          QPair(QStringLiteral("high"), QRect(286, 26, 100, 80))) {
    error = QStringLiteral("Hovering between pins did not open a hole there");
    return false;
  }
  // Any overlap with the stack joins it, however slight; a drag that
  // clears the stack entirely, even right beside it, stays out.
  const PinInsertionPlan grazing = pinInsertionPlan(
      column, {}, QRect(200, 140, 100, 80), screen, 10, 14);
  if (grazing.index != 1) {
    error = QStringLiteral("A partial overlap with the stack did not join it");
    return false;
  }
  if (pinInsertionPlan(column, {}, QRect(120, 140, 100, 80), screen, 10, 14)
          .index != -1) {
    error = QStringLiteral("A drag clear of the stack joined it anyway");
    return false;
  }
  const QVector<QPair<QString, QRect>> emptyColumn;
  if (pinInsertionPlan(emptyColumn, {}, QRect(240, 180, 100, 80), screen, 10,
                       14)
          .index != 0 ||
      pinInsertionPlan(emptyColumn, {}, QRect(60, 60, 100, 80), screen, 10,
                       14)
          .index != -1) {
    error = QStringLiteral("An empty stack's corner spot did not gate joining");
    return false;
  }
  // A pin nudged off the top of the stack still overlaps the spot it came
  // from and snaps back there; dragged fully past it, it is free.
  const PinInsertionPlan nudged = pinInsertionPlan(
      column, {}, QRect(250, 10, 100, 80), screen, 10, 14);
  if (nudged.index != 2 || nudged.spot != QRect(286, 26, 100, 80)) {
    error = QStringLiteral("A nudged top pin did not snap back to its seat");
    return false;
  }
  if (pinInsertionPlan(column, {}, QRect(150, 20, 100, 80), screen, 10, 14)
          .index != -1) {
    error = QStringLiteral("A pin dragged past its seat was still captured");
    return false;
  }
  // A stack that has not packed down yet is still the stack the user sees:
  // overlapping a pin's live position joins even when the packed baseline
  // is elsewhere.
  const QVector<QPair<QString, QRect>> floating{
      {QStringLiteral("high"), QRect(286, 40, 100, 80)}};
  if (pinInsertionPlan(floating, {}, QRect(240, 20, 100, 80), screen, 10, 14)
          .index == -1) {
    error = QStringLiteral("Overlapping a live pin did not join the stack");
    return false;
  }
  // The band spans the vacancies too: with one pin floating high on a
  // tall screen, a drag into the empty stretch between it and the bottom
  // seats still folds in; beside the band it stays out.
  const QSize tall(400, 600);
  const QVector<QPair<QString, QRect>> lofty{
      {QStringLiteral("high"), QRect(286, 30, 100, 80)}};
  if (pinInsertionPlan(lofty, {}, QRect(240, 200, 100, 80), tall, 10, 14)
          .index == -1) {
    error = QStringLiteral("A drag into the column's vacancy stayed out");
    return false;
  }
  if (pinInsertionPlan(lofty, {}, QRect(100, 200, 100, 80), tall, 10, 14)
          .index != -1) {
    error = QStringLiteral("A drag beside the column folded in");
    return false;
  }
  const PinInsertionPlan below = pinInsertionPlan(
      column, {}, QRect(286, 216, 100, 80), screen, 10, 14);
  if (below.index != 0 || below.spot != QRect(286, 206, 100, 80) ||
      below.spread.at(0) !=
          QPair(QStringLiteral("low"), QRect(286, 116, 100, 80))) {
    error = QStringLiteral("Hovering the corner did not open the bottom slot");
    return false;
  }
  if (pinInsertionPlan(column, {}, QRect(60, 140, 100, 80), screen, 10, 14)
          .index != -1) {
    error = QStringLiteral("A drag far from the column planned an insertion");
    return false;
  }

  // The dispatch expressions are Lua for a Lua-configured Hyprland and the
  // classic criteria grammar for sway; a placement that silently does
  // nothing is exactly the failure these guard.
  const QString title = QStringLiteral("omasnap-pin 1234");
  if (pinFloatDispatch(title) !=
          QStringLiteral(
              "hl.dsp.window.float({ window = \"title:^(omasnap-pin 1234)$\" })") ||
      pinPinDispatch(title) !=
          QStringLiteral(
              "hl.dsp.window.pin({ window = \"title:^(omasnap-pin 1234)$\" })") ||
      pinMoveDispatch(title, 120, 40) !=
          QStringLiteral("hl.dsp.window.move({ x = 120, y = 40, relative = "
                         "false, window = \"title:^(omasnap-pin 1234)$\" })")) {
    error = QStringLiteral("Hyprland dispatch expressions were malformed");
    return false;
  }
  if (pinSwayArrangeCommand(title, 120, 40) !=
          QStringLiteral("[title=\"^omasnap-pin 1234$\"] floating enable, "
                         "sticky enable, move absolute position 120 40") ||
      pinSwayMoveCommand(title, 120, 40) !=
          QStringLiteral(
              "[title=\"^omasnap-pin 1234$\"] move absolute position 120 40")) {
    error = QStringLiteral("Sway commands were malformed");
    return false;
  }
  return true;
}
