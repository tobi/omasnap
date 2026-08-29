/** @fileoverview Tests the clip engine: copy, punch, native mapping, snap. */
#include "clip-smoke.hpp"

#include "clip.hpp"
#include "capture.hpp"

#include <QBuffer>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <optional>

bool runClipSmoke(QString &error) {
  QImage source(8, 8, QImage::Format_ARGB32_Premultiplied);
  source.fill(QColor(40, 180, 60, 255));
  source.setPixelColor(2, 3, QColor(220, 50, 0, 255));

  QImage page(32, 32, QImage::Format_ARGB32_Premultiplied);
  page.fill(QColor(80, 120, 200, 255));
  {
    QPainter painter(&page);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(220, 50, 0, 255));
    painter.drawRect(QRect(8, 8, 16, 16));
  }
  const QColor around = sampleClipSurroundings(page, QRect(8, 8, 16, 16));
  if (around.red() < 70 || around.red() > 90 || around.blue() < 190) {
    error = QStringLiteral("sampleClipSurroundings missed the page colour");
    return false;
  }
  if (clipFillOpaque(sampleClipSurroundings(page, page.rect()))) {
    error = QStringLiteral(
        "sampleClipSurroundings invented a colour with no outside ring");
    return false;
  }

  const QRect region(2, 2, 3, 3);
  const QImage tile = copyRect(source, region);
  if (tile.size() != QSize(3, 3) ||
      tile.pixelColor(0, 1) != QColor(220, 50, 0, 255)) {
    error = QStringLiteral("copyRect did not keep the marked pixel");
    return false;
  }

  QImage punched = source;
  punchRect(punched, region);
  if (punched.size() != source.size()) {
    error = QStringLiteral("punchRect must not collapse the image");
    return false;
  }
  if (punched.pixelColor(2, 3).alpha() != 0) {
    error = QStringLiteral("punchRect left pixels in the hole");
    return false;
  }
  if (punched.pixelColor(0, 0) != QColor(40, 180, 60, 255)) {
    error = QStringLiteral("punchRect touched pixels outside the rect");
    return false;
  }

  QImage filled = source;
  fillHole(filled, region, QColor(10, 132, 255, 255));
  if (filled.pixelColor(2, 3) != QColor(10, 132, 255, 255)) {
    error = QStringLiteral("fillHole did not paint the solid infill");
    return false;
  }
  if (filled.pixelColor(0, 0) != QColor(40, 180, 60, 255)) {
    error = QStringLiteral("fillHole touched pixels outside the rect");
    return false;
  }
  QImage viaTransparent = source;
  fillHole(viaTransparent, region, QColor(0, 0, 0, 0));
  if (viaTransparent.pixelColor(2, 3).alpha() != 0) {
    error = QStringLiteral("fillHole with alpha 0 did not punch");
    return false;
  }

  // Empty / out-of-bounds are no-ops.
  if (!copyRect(source, QRect(20, 20, 2, 2)).isNull()) {
    error = QStringLiteral("copyRect of a miss should be null");
    return false;
  }
  QImage untouched = source;
  punchRect(untouched, QRect());
  if (untouched != source) {
    error = QStringLiteral("empty punchRect changed the image");
    return false;
  }

  const QRect native =
      nativeClipRect(QRectF(2, 2, 3, 3), QSize(8, 8), QSize(8, 8));
  if (native != QRect(2, 2, 3, 3)) {
    error = QStringLiteral("nativeClipRect 1:1 mapping wrong");
    return false;
  }

  // 2× source: logical 2,2 3×3 → native 4,4 6×6.
  const QRect hidpi =
      nativeClipRect(QRectF(2, 2, 3, 3), QSize(8, 8), QSize(16, 16));
  if (hidpi != QRect(4, 4, 6, 6)) {
    error = QStringLiteral("nativeClipRect hidpi mapping wrong");
    return false;
  }

  if (!clipDestSnapped(QRectF(10, 10, 20, 20), QRectF(12, 11, 20, 20), 8.0) ||
      clipDestSnapped(QRectF(10, 10, 20, 20), QRectF(40, 40, 20, 20), 8.0)) {
    error = QStringLiteral("clipDestSnapped threshold wrong");
    return false;
  }
  if (clipSnapEnterThreshold(2.0) != 7.0 ||
      clipSnapLeaveThreshold(2.0) != 10.0) {
    error = QStringLiteral("clip snap hysteresis thresholds wrong");
    return false;
  }

  QImage composed(8, 8, QImage::Format_ARGB32_Premultiplied);
  composed.fill(QColor(220, 50, 0, 255));
  QImage existing(3, 3, QImage::Format_ARGB32_Premultiplied);
  existing.fill(QColor(10, 132, 255, 255));
  const QImage kept =
      resolveClipTile(composed, QRect(2, 2, 3, 3), existing, false);
  if (kept.size() != existing.size() ||
      kept.pixelColor(1, 1) != QColor(10, 132, 255, 255)) {
    error = QStringLiteral("resolveClipTile recopied when the tile was present");
    return false;
  }
  const QImage recopied =
      resolveClipTile(composed, QRect(2, 2, 3, 3), existing, true);
  if (recopied.pixelColor(0, 0) != QColor(220, 50, 0, 255)) {
    error = QStringLiteral("resolveClipTile skipped copy when prefix changed");
    return false;
  }
  const QImage missing =
      resolveClipTile(composed, QRect(2, 2, 3, 3), QImage(), false);
  if (missing.pixelColor(0, 0) != QColor(220, 50, 0, 255)) {
    error = QStringLiteral("resolveClipTile skipped copy when the tile was null");
    return false;
  }

  QImage pngTile(2, 2, QImage::Format_ARGB32);
  pngTile.fill(QColor(9, 8, 7, 255));
  QByteArray pngBytes;
  QBuffer pngBuffer(&pngBytes);
  pngBuffer.open(QIODevice::WriteOnly);
  pngTile.save(&pngBuffer, "PNG");
  const QString b64 = QString::fromLatin1(pngBytes.toBase64());
  const QString jsonPath =
      QDir::temp().filePath(QStringLiteral("omasnap-clip-png-gate.json"));
  const auto writeLog = [&](const QString &tool) {
    QFile file(jsonPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
      return false;
    const QString json = QStringLiteral(
        "{\"version\":1,\"index\":1,\"nextId\":\"2\",\"nextMarker\":1,\"ops\":["
        "{\"type\":\"annotate\",\"annotation\":{\"id\":\"1\",\"tool\":\"%1\","
        "\"start\":[0,0],\"end\":[4,4],\"color\":\"#ffff0000\",\"size\":4,"
        "\"png\":\"%2\"}}]}")
                             .arg(tool, b64);
    return file.write(json.toUtf8()) > 0;
  };
  if (!writeLog(QStringLiteral("rectangle"))) {
    error = QStringLiteral("could not write rectangle png-gate log");
    return false;
  }
  OperationLog rectangleLog;
  QString loadError;
  if (!loadOperationLog(jsonPath, rectangleLog, loadError) ||
      rectangleLog.ops.isEmpty() ||
      rectangleLog.ops.constFirst().annotations.isEmpty()) {
    error = QStringLiteral("rectangle png-gate log failed to load: %1")
                .arg(loadError);
    return false;
  }
  if (!rectangleLog.ops.constFirst().annotations.constFirst().image.isNull()) {
    error = QStringLiteral("annotationFromJson loaded png onto a non-Clip kind");
    return false;
  }
  if (!writeLog(QStringLiteral("clip"))) {
    error = QStringLiteral("could not write clip png-gate log");
    return false;
  }
  OperationLog clipLog;
  if (!loadOperationLog(jsonPath, clipLog, loadError) ||
      clipLog.ops.isEmpty() || clipLog.ops.constFirst().annotations.isEmpty() ||
      clipLog.ops.constFirst().annotations.constFirst().image.isNull() ||
      clipLog.ops.constFirst().annotations.constFirst().image.pixelColor(0, 0) !=
          QColor(9, 8, 7, 255)) {
    error = QStringLiteral("annotationFromJson skipped png on a Clip kind");
    return false;
  }
  QFile::remove(jsonPath);

  QImage disk(32, 32, QImage::Format_ARGB32_Premultiplied);
  disk.fill(QColor(20, 20, 40, 255));
  {
    QPainter painter(&disk);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(220, 80, 40, 255));
    painter.drawEllipse(QRect(6, 6, 20, 20));
  }
  ClipOp ellipse;
  ellipse.shape = ClipShape::Ellipse;
  ellipse.sourceRect = QRect(6, 6, 20, 20);
  const QImage ellipseTile = copyMasked(disk, ellipse);
  if (ellipseTile.size() != QSize(20, 20)) {
    error = QStringLiteral("copyMasked ellipse size wrong");
    return false;
  }
  if (ellipseTile.pixelColor(10, 10).alpha() == 0) {
    error = QStringLiteral("copyMasked ellipse dropped the disk interior");
    return false;
  }
  if (ellipseTile.pixelColor(0, 0).alpha() != 0) {
    error = QStringLiteral("copyMasked ellipse kept bbox corners");
    return false;
  }
  QImage ellipsePunched = disk;
  fillHole(ellipsePunched, ellipse);
  if (ellipsePunched.pixelColor(16, 16).alpha() != 0) {
    error = QStringLiteral("fillHole ellipse left the disk interior");
    return false;
  }
  if (ellipsePunched.pixelColor(0, 0) != QColor(20, 20, 40, 255)) {
    error = QStringLiteral("fillHole ellipse touched the field");
    return false;
  }

  ClipOp lasso;
  lasso.shape = ClipShape::Lasso;
  lasso.points = {QPointF(2, 2), QPointF(14, 2), QPointF(8, 14)};
  lasso.sourceRect = QRect(2, 2, 13, 13);
  QImage lassoField(16, 16, QImage::Format_ARGB32_Premultiplied);
  lassoField.fill(QColor(10, 80, 180, 255));
  const QImage lassoTile = copyMasked(lassoField, lasso);
  if (lassoTile.pixelColor(0, 0).alpha() == 0) {
    error = QStringLiteral("copyMasked lasso dropped a vertex interior");
    return false;
  }
  if (lassoTile.pixelColor(12, 12).alpha() != 0) {
    error = QStringLiteral("copyMasked lasso kept a point outside the triangle");
    return false;
  }

  const std::optional<QRect> snapped = snapEllipseRect(disk, QPoint(16, 16));
  if (!snapped || !snapped->contains(16, 16) || snapped->width() < 12 ||
      snapped->height() < 12) {
    error = QStringLiteral("snapEllipseRect missed the synthetic disk");
    return false;
  }
  if (snapEllipseRect(disk, QPoint(1, 1))) {
    error = QStringLiteral("snapEllipseRect snapped empty field");
    return false;
  }

  // A round portrait sitting on a rectangular card: the circle, not the card.
  QImage card(64, 64, QImage::Format_ARGB32_Premultiplied);
  card.fill(QColor(18, 18, 22, 255));
  {
    QPainter painter(&card);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(40, 44, 52, 255));
    painter.drawRect(QRect(6, 8, 52, 48));
    painter.setBrush(QColor(180, 190, 200, 255));
    painter.drawEllipse(QRect(16, 16, 32, 32));
  }
  const std::optional<QRect> portrait =
      snapEllipseRect(card, QPoint(32, 32));
  if (!portrait || portrait->width() > 40 || portrait->height() > 40 ||
      portrait->width() < 24 || !portrait->contains(32, 32)) {
    error = QStringLiteral(
        "snapEllipseRect took the card instead of the circular portrait");
    return false;
  }

  if (!clipTraceSnapFits(QRectF(4, 4, 28, 28), QRectF(6, 6, 20, 20))) {
    error = QStringLiteral("clipTraceSnapFits rejected a tracing ellipse");
    return false;
  }
  if (clipTraceSnapFits(QRectF(0, 0, 32, 32), QRectF(6, 6, 8, 8))) {
    error = QStringLiteral("clipTraceSnapFits accepted a tiny object in a huge drag");
    return false;
  }
  if (clipTraceSnapFits(QRectF(14, 14, 8, 8), QRectF(6, 6, 20, 20))) {
    error = QStringLiteral("clipTraceSnapFits accepted a drag that has not covered the object");
    return false;
  }
  if (clipTraceSnapFits(QRectF(0, 0, 10, 10), QRectF(20, 20, 8, 8))) {
    error = QStringLiteral("clipTraceSnapFits accepted a snap whose centre is outside the drag");
    return false;
  }
  if (clipTraceSnapFits(QRectF(20, 20, 70, 70), QRectF(12, 47, 112, 34))) {
    error = QStringLiteral("clipTraceSnapFits accepted a wide band as a traced circle");
    return false;
  }

  QImage plate(32, 32, QImage::Format_ARGB32_Premultiplied);
  plate.fill(QColor(20, 20, 40, 255));
  {
    QPainter painter(&plate);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(220, 80, 40, 255));
    painter.drawRect(QRect(6, 8, 20, 16));
  }
  const std::optional<QRect> snappedRect = snapRectRect(plate, QPoint(16, 16));
  if (!snappedRect || !snappedRect->contains(16, 16) ||
      snappedRect->width() < 16 || snappedRect->height() < 12 ||
      snappedRect->width() > 24 || snappedRect->height() > 20) {
    error = QStringLiteral("snapRectRect missed the synthetic rectangle");
    return false;
  }
  if (snapRectRect(plate, QPoint(1, 1))) {
    error = QStringLiteral("snapRectRect snapped empty field");
    return false;
  }
  if (!clipRectTraceSnapFits(QRectF(2, 2, 28, 28), QRectF(6, 8, 20, 16))) {
    error = QStringLiteral("clipRectTraceSnapFits rejected a tracing rectangle");
    return false;
  }
  if (clipRectTraceSnapFits(QRectF(20, 20, 70, 70), QRectF(12, 47, 112, 34))) {
    error = QStringLiteral(
        "clipRectTraceSnapFits accepted a wide band as a traced rectangle");
    return false;
  }

  QImage roundField(48, 48, QImage::Format_ARGB32_Premultiplied);
  roundField.fill(QColor(20, 20, 40, 255));
  {
    QPainter painter(&roundField);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(220, 80, 40, 255));
    painter.drawRoundedRect(QRect(6, 6, 36, 36), 8, 8);
  }
  const std::optional<ClipSnapHit> rounded =
      snapObject(roundField, QPoint(24, 24));
  if (!rounded || rounded->box.width() < 28 || rounded->radius < 5.0) {
    error = QStringLiteral("snapObject missed the rounded rectangle");
    return false;
  }
  ClipOp roundOp;
  roundOp.shape = ClipShape::Rect;
  roundOp.sourceRect = rounded->box;
  roundOp.radius = rounded->radius;
  const QImage roundTile = copyMasked(roundField, roundOp);
  if (roundTile.isNull() || roundTile.pixelColor(0, 0).alpha() != 0) {
    error = QStringLiteral("copyMasked rounded rect kept a bbox corner");
    return false;
  }
  const QPoint inner(roundTile.width() / 2, roundTile.height() / 2);
  if (roundTile.pixelColor(inner).alpha() == 0) {
    error = QStringLiteral("copyMasked rounded rect dropped the interior");
    return false;
  }
  if (rounded->contour.size() < 8) {
    error = QStringLiteral("snapObject did not trace a silhouette");
    return false;
  }

  // Two square top corners must not zero out a round-bottom card.
  QImage banner(48, 48, QImage::Format_ARGB32_Premultiplied);
  banner.fill(QColor(20, 20, 40, 255));
  {
    QPainter painter(&banner);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(220, 80, 40, 255));
    painter.drawRoundedRect(QRect(6, 6, 36, 36), 8, 8);
    painter.drawRect(QRect(6, 6, 36, 10));
  }
  const std::optional<ClipSnapHit> roundBottom =
      snapObject(banner, QPoint(24, 24));
  if (!roundBottom || roundBottom->radius < 5.0) {
    error = QStringLiteral(
        "snapObject dropped corner radius because two corners were square");
    return false;
  }

  // A 1 px close must not swallow a nearby glyph and kill the card radius.
  QImage chat(48, 48, QImage::Format_ARGB32_Premultiplied);
  chat.fill(QColor(18, 18, 22, 255));
  {
    QPainter painter(&chat);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(40, 42, 48, 255));
    painter.drawRoundedRect(QRect(12, 8, 32, 32), 8, 8);
    painter.setBrush(QColor(200, 200, 210, 255));
    painter.drawRect(QRect(1, 18, 8, 10));
  }
  const std::optional<ClipSnapHit> chatCard =
      snapObject(chat, QPoint(28, 24), QRect(0, 0, 48, 48));
  if (!chatCard || chatCard->box.left() < 10 || chatCard->radius < 5.0) {
    error = QStringLiteral(
        "snapObject merged a nearby glyph into the rounded card");
    return false;
  }

  // Gray card chrome on gray paper: the bright thumb must not steal the left
  // edge. The drag (roi) is the search window.
  QImage letter(48, 48, QImage::Format_ARGB32_Premultiplied);
  letter.fill(QColor(80, 80, 84, 255));
  {
    QPainter painter(&letter);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(102, 102, 110, 255));
    painter.drawRect(QRect(8, 10, 32, 24));
    painter.setBrush(QColor(220, 180, 40, 255));
    painter.drawRect(QRect(11, 13, 12, 12));
  }
  const QRect cardRoi(6, 8, 36, 28);
  const std::optional<ClipSnapHit> chrome =
      snapObject(letter, QPoint(24, 22), cardRoi);
  if (!chrome || chrome->box.left() > 8) {
    error = QStringLiteral(
        "snapObject missed gray card chrome to the left of the thumbnail");
    return false;
  }

  // Circle with a pin: a drag around it must cover the protrusion.
  QImage badge(48, 48, QImage::Format_ARGB32_Premultiplied);
  badge.fill(QColor(16, 16, 20, 255));
  {
    QPainter painter(&badge);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(200, 190, 80, 255));
    painter.drawEllipse(QRect(8, 8, 24, 24));
    painter.drawRect(QRect(20, 18, 16, 4));
  }
  const std::optional<QRect> withPin =
      snapEllipseRect(badge, QPoint(20, 20), QRect(4, 4, 40, 40));
  if (!withPin || withPin->right() < 35) {
    error = QStringLiteral(
        "snapEllipseRect inside a drag clipped the circular badge's pin");
    return false;
  }
  const std::optional<ClipSnapHit> pinBlob =
      snapObject(badge, QPoint(20, 20), QRect(4, 4, 40, 40));
  if (!pinBlob || pinBlob->box.right() < 35 || pinBlob->contour.size() < 8) {
    error = QStringLiteral(
        "snapObject lasso silhouette dropped the badge pin");
    return false;
  }

  // Achievements-wheel case: a checker disk on dark paper. Dark cells sit
  // near paper luma so 4-connected flood from one light square dies; Hough
  // then votes the clean circle and clips the yellow pin.
  QImage wheel(48, 48, QImage::Format_ARGB32_Premultiplied);
  wheel.fill(QColor(10, 10, 12, 255));
  for (int y = 8; y <= 31; ++y) {
    for (int x = 8; x <= 31; ++x) {
      const qreal dx = x - 19.5;
      const qreal dy = y - 19.5;
      if (dx * dx + dy * dy > 12.0 * 12.0)
        continue;
      wheel.setPixelColor(x, y,
                          ((x + y) % 2 == 0) ? QColor(200, 190, 80, 255)
                                             : QColor(16, 16, 18, 255));
    }
  }
  {
    QPainter painter(&wheel);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(240, 200, 40, 255));
    painter.drawRect(QRect(28, 18, 12, 4));
  }
  const QRect wheelRoi(4, 4, 40, 40);
  const std::optional<QRect> checkerPin =
      snapEllipseRect(wheel, QPoint(20, 20), wheelRoi);
  if (!checkerPin || checkerPin->right() < 38) {
    error = QStringLiteral(
        "snapEllipseRect on a checker wheel clipped the yellow pin");
    return false;
  }
  const std::optional<QRect> darkSeed =
      snapEllipseRect(wheel, QPoint(21, 20), wheelRoi);
  if (!darkSeed || darkSeed->right() < 38) {
    error = QStringLiteral(
        "snapEllipseRect from a dark checker cell clipped the yellow pin");
    return false;
  }
  const std::optional<ClipSnapHit> checkerBlob =
      snapObject(wheel, QPoint(21, 20), wheelRoi);
  if (!checkerBlob || checkerBlob->box.right() < 38) {
    error = QStringLiteral(
        "snapObject from a dark checker cell dropped the yellow pin");
    return false;
  }
  const std::optional<QRect> clickPin = snapEllipseRect(wheel, QPoint(20, 20));
  if (!clickPin || clickPin->right() < 38) {
    error = QStringLiteral(
        "click-snap on a checker wheel clipped the yellow pin");
    return false;
  }

  return true;
}
