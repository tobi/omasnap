/** @fileoverview Drives pixel-clip: marquee, lift, hole, backdrop, undo.
 *
 *  A banded fixture makes the torn-off tile and the hole checkable by colour.
 *  Direct applyClipForTest covers the operation log; a widget drag covers the
 *  empty-marquee → lift path. Failures name the first wrong pixel.
 */
#include "clip-mapping-smoke.hpp"

#include "capture.hpp"
#include "clip.hpp"
#include "editor.hpp"
#include "image-fixture.hpp"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QPainter>
#include <QTest>

namespace {

CaptureData fixtureCapture(const QImage &source) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
  capture.previewSize = source.size();
  return capture;
}

QPoint screenOf(const CaptureEditor &editor, qreal ax, qreal ay) {
  return editor.annotationPointToWidgetForTest(QPointF(ax, ay)).toPoint();
}

bool saveGrab(CaptureEditor &editor, const QString &path, QString &error) {
  const QImage grab = editor.grab().toImage();
  if (grab.save(path, "PNG"))
    return true;
  error = QStringLiteral("could not write %1").arg(path);
  return false;
}

QImage diskImage() {
  QImage disk(64, 64, QImage::Format_ARGB32_Premultiplied);
  disk.fill(QColor(20, 20, 40, 255));
  QPainter painter(&disk);
  painter.setRenderHint(QPainter::Antialiasing, false);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(220, 80, 40, 255));
  painter.drawEllipse(QRect(8, 8, 48, 48));
  return disk;
}

QImage squareImage() {
  QImage square(64, 64, QImage::Format_ARGB32_Premultiplied);
  square.fill(QColor(20, 20, 40, 255));
  QPainter painter(&square);
  painter.setRenderHint(QPainter::Antialiasing, false);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(220, 80, 40, 255));
  painter.drawRect(QRect(8, 8, 48, 48));
  return square;
}

QImage roundedCardImage() {
  QImage card(64, 64, QImage::Format_ARGB32_Premultiplied);
  card.fill(QColor(18, 18, 22, 255));
  QPainter painter(&card);
  painter.setRenderHint(QPainter::Antialiasing, false);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(40, 42, 48, 255));
  painter.drawRoundedRect(QRect(8, 8, 48, 48), 10, 10);
  return card;
}

QImage checkerPinImage() {
  QImage wheel(64, 64, QImage::Format_ARGB32_Premultiplied);
  wheel.fill(QColor(10, 10, 12, 255));
  for (int y = 8; y <= 47; ++y) {
    for (int x = 8; x <= 47; ++x) {
      const qreal dx = x - 27.5;
      const qreal dy = y - 27.5;
      if (dx * dx + dy * dy > 20.0 * 20.0)
        continue;
      wheel.setPixelColor(x, y,
                          ((x + y) % 2 == 0) ? QColor(200, 190, 80, 255)
                                             : QColor(16, 16, 18, 255));
    }
  }
  QPainter painter(&wheel);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(240, 200, 40, 255));
  painter.drawRect(QRect(44, 26, 14, 6));
  return wheel;
}

bool ensureSnap(CaptureEditor &editor, QApplication &application, bool on,
                QString &error) {
  if (editor.pixelClipSnapEnabledForTest() == on)
    return true;
  const QRectF snapChip =
      editor.toolbarButtonRectForTest(QStringLiteral("clip-shape-snap"));
  if (snapChip.isEmpty()) {
    error = QStringLiteral("snap chip was missing from the clip strip");
    return false;
  }
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                    snapChip.center().toPoint());
  application.processEvents();
  if (editor.pixelClipSnapEnabledForTest() != on) {
    error = on ? QStringLiteral("could not turn snap on")
               : QStringLiteral("could not turn snap off");
    return false;
  }
  return true;
}

bool lockWasLooseDrag(const QRectF &locked) {
  return locked.isEmpty() || locked.width() > 56.0 || locked.height() > 56.0 ||
         locked.left() < 3.0 || locked.top() < 3.0 ||
         !locked.contains(QPointF(32, 32));
}

} // namespace

bool runClipMappingSmoke(QApplication &application, const QString &outputRoot,
                         QString &error) {
  constexpr int kWidth = 160;
  constexpr int kBand = 16;
  constexpr int kBands = 10;
  const QImage source = rowBandImage(kWidth, kBand, kBands);
  QDir().mkpath(QFileInfo(outputRoot).path());

  // Direct apply: punch the green band (index 3, y 48..64) and park it to
  // the right of the image so the canvas grows and the hole shows Slate.
  {
    CaptureEditor editor(fixtureCapture(source),
                         CaptureEditor::CaptureMode::File);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    editor.applyClipForTest(QRectF(0, 48, kWidth, kBand),
                            QRectF(kWidth + 20, 48, kWidth, kBand));
    application.processEvents();

    const QImage composed = editor.composedSourceForTest();
    if (composed.pixelColor(80, 56).alpha() != 0) {
      error = QStringLiteral("direct clip left the green band in the source");
      return false;
    }
    if (composed.pixelColor(80, 8).alpha() == 0) {
      error = QStringLiteral("direct clip punched outside the rect");
      return false;
    }

    bool foundClip = false;
    for (const Annotation &annotation : editor.currentAnnotationsForTest()) {
      if (annotation.kind != Annotation::Kind::Clip)
        continue;
      foundClip = true;
      if (annotation.image.isNull() ||
          annotation.image.pixelColor(0, 0) != fixtureBandColor(3)) {
        error = QStringLiteral("clip tile is not the green band");
        return false;
      }
    }
    if (!foundClip) {
      error = QStringLiteral("direct clip did not add a clip layer");
      return false;
    }

    if (!saveBmp(editor.renderCurrentOutput(),
                 outputRoot + QStringLiteral("-clip-direct-actual.bmp"),
                 error) ||
        !saveGrab(editor, outputRoot + QStringLiteral("-clip-direct.png"),
                  error))
      return false;

    const int beforeUndo = editor.operationIndex();
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    application.processEvents();
    if (editor.operationIndex() != beforeUndo - 1) {
      error = QStringLiteral("clip undo did not move the log cursor");
      return false;
    }
    if (editor.composedSourceForTest().pixelColor(80, 56).alpha() == 0) {
      error = QStringLiteral("clip undo left the hole");
      return false;
    }
    if (!editor.currentAnnotationsForTest().isEmpty()) {
      error = QStringLiteral("clip undo left the clip layer");
      return false;
    }
    editor.close();
  }

  // Solid hole fill is stored on the Clip op and replayed.
  {
    CaptureEditor editor(fixtureCapture(source),
                         CaptureEditor::CaptureMode::File);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    const QColor infill(10, 132, 255, 255);
    editor.applyClipForTest(QRectF(0, 48, kWidth, kBand),
                            QRectF(kWidth + 20, 48, kWidth, kBand), infill);
    application.processEvents();
    const QColor hole = editor.composedSourceForTest().pixelColor(80, 56);
    if (hole.red() != infill.red() || hole.green() != infill.green() ||
        hole.blue() != infill.blue() || hole.alpha() == 0) {
      error = QStringLiteral("solid clip fill did not land in the hole");
      return false;
    }
    editor.close();
  }

  // Empty Select marquee on a source with no layers becomes a pixel clip;
  // dragging that rect lifts and commits.
  {
    CaptureEditor editor(fixtureCapture(source),
                         CaptureEditor::CaptureMode::File);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    if (!ensureSnap(editor, application, false, error))
      return false;
    QTest::keyClick(&editor, Qt::Key_B);
    application.processEvents();
    QTest::keyClick(&editor, Qt::Key_V);
    application.processEvents();
    if (editor.armedToolForTest() != CaptureEditor::Tool::Select) {
      error = QStringLiteral("V did not arm Select");
      return false;
    }

    const QPoint fidgetFrom = screenOf(editor, 20, 20);
    const QPoint fidgetTo = screenOf(editor, 22, 21);
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, fidgetFrom);
    application.processEvents();
    if (!editor.statusForTest().contains(QStringLiteral("Drag to select layers"))) {
      error = QStringLiteral("empty marquee status was not Drag to select layers");
      return false;
    }
    QTest::mouseMove(&editor, fidgetTo, 10);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, fidgetTo);
    application.processEvents();
    if (!editor.pixelClipRectForTest().isEmpty()) {
      error = QStringLiteral("2px fidget armed a pixel clip");
      return false;
    }

    const QPoint from = screenOf(editor, 10, 48);
    const QPoint to = screenOf(editor, kWidth - 10, 64);
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, from);
    QTest::mouseMove(&editor, to, 10);
    application.processEvents();
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, to);
    application.processEvents();
    if (editor.pixelClipRectForTest().isEmpty()) {
      error = QStringLiteral("empty marquee did not lock a pixel clip");
      return false;
    }
    editor.applyClipForTest(QRectF(0, 80, kWidth, kBand),
                            QRectF(0, 96, kWidth, kBand));
    application.processEvents();
    const int priorOps = editor.operationIndex();
    if (priorOps <= 0) {
      error = QStringLiteral("prior clip did not land in the log");
      return false;
    }
    if (editor.clipFillMenuRectForTest().isEmpty()) {
      error = QStringLiteral("pixel clip did not show a hole-fill fly-out");
      return false;
    }
    if (!clipFillOpaque(editor.clipFillForTest())) {
      error = QStringLiteral(
          "hole fill did not default to match surroundings");
      return false;
    }
    QTest::keyClick(&editor, Qt::Key_T);
    application.processEvents();
    if (editor.armedToolForTest() != CaptureEditor::Tool::Eyedropper) {
      error = QStringLiteral(
          "T did not cycle hole fill from surroundings to Sample from image");
      return false;
    }
    QTest::keyClick(&editor, Qt::Key_T);
    application.processEvents();
    if (editor.armedToolForTest() != CaptureEditor::Tool::Select ||
        clipFillOpaque(editor.clipFillForTest())) {
      error = QStringLiteral(
          "T did not cycle hole fill from Sample from image to transparent");
      return false;
    }
    QTest::keyClick(&editor, Qt::Key_T);
    application.processEvents();
    if (!clipFillOpaque(editor.clipFillForTest())) {
      error = QStringLiteral(
          "T did not cycle hole fill from transparent to match surroundings");
      return false;
    }
    QTest::keyClick(&editor, Qt::Key_1);
    application.processEvents();
    if (!clipFillOpaque(editor.clipFillForTest())) {
      error = QStringLiteral("1 did not set a solid hole fill");
      return false;
    }
    const QRectF fillMenu = editor.clipFillMenuRectForTest();
    QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                      fillMenu.topLeft().toPoint() + QPoint(16, 18));
    application.processEvents();
    if (clipFillOpaque(editor.clipFillForTest())) {
      error = QStringLiteral("transparent swatch did not clear the hole fill");
      return false;
    }
    const QRectF eyedropper =
        editor.toolbarButtonRectForTest(QStringLiteral("tool-eyedropper"));
    if (eyedropper.isEmpty()) {
      error = QStringLiteral("clip fill fly-out missing Sample from image");
      return false;
    }
    QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                      eyedropper.center().toPoint());
    application.processEvents();
    QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                      screenOf(editor, 80, 8));
    application.processEvents();
    if (editor.clipFillForTest().rgb() != fixtureBandColor(0).rgb()) {
      error = QStringLiteral(
          "eyedropper did not set hole fill from the sampled pixel");
      return false;
    }
    const QRectF surroundings = editor.toolbarButtonRectForTest(
        QStringLiteral("clip-fill-surroundings"));
    if (surroundings.isEmpty()) {
      error = QStringLiteral("clip fill fly-out missing match surroundings");
      return false;
    }
    QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                      surroundings.center().toPoint());
    application.processEvents();
    if (!clipFillOpaque(editor.clipFillForTest())) {
      error = QStringLiteral("match surroundings did not set a solid hole fill");
      return false;
    }
    QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                      fillMenu.topLeft().toPoint() + QPoint(16, 18));
    application.processEvents();
    if (clipFillOpaque(editor.clipFillForTest())) {
      error = QStringLiteral(
          "transparent swatch did not restore a clear hole after sampling");
      return false;
    }
    if (!saveGrab(editor, outputRoot + QStringLiteral("-clip-marquee.png"),
                  error))
      return false;

    const QPoint liftFrom = screenOf(editor, kWidth / 2.0, 56);
    const QPoint liftTo = screenOf(editor, kWidth / 2.0, 120);
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, liftFrom);
    application.processEvents();
    if (!editor.clipLiftActiveForTest()) {
      error = QStringLiteral("drag inside the pixel clip did not start a lift");
      return false;
    }
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    application.processEvents();
    if (editor.clipLiftActiveForTest()) {
      error = QStringLiteral("Ctrl+Z during a lift did not cancel the lift");
      return false;
    }
    if (editor.operationIndex() != priorOps) {
      error = QStringLiteral("Ctrl+Z during a lift undid the previous op");
      return false;
    }
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, liftFrom);
    application.processEvents();
    if (!editor.clipLiftActiveForTest()) {
      error = QStringLiteral("lift did not restart after key-cancel");
      return false;
    }
    QTest::mouseMove(&editor, liftTo, 10);
    application.processEvents();
    if (!saveGrab(editor, outputRoot + QStringLiteral("-clip-lift.png"), error))
      return false;
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, liftTo);
    application.processEvents();
    if (editor.clipLiftActiveForTest()) {
      error = QStringLiteral("clip lift did not commit on release");
      return false;
    }
    if (editor.composedSourceForTest()
            .pixelColor(kWidth / 2, 56)
            .alpha() != 0) {
      error = QStringLiteral("lifted clip left the band in the source");
      return false;
    }
    if (!saveGrab(editor, outputRoot + QStringLiteral("-clip-committed.png"),
                  error) ||
        !saveBmp(editor.renderCurrentOutput(),
                 outputRoot + QStringLiteral("-clip-committed.bmp"), error))
      return false;

    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    application.processEvents();
    if (!saveGrab(editor, outputRoot + QStringLiteral("-clip-undo.png"), error))
      return false;
    if (editor.composedSourceForTest().pixelColor(kWidth / 2, 56).alpha() ==
        0) {
      error = QStringLiteral("widget clip undo left the hole");
      return false;
    }
    editor.close();
  }

  // Replay keeps a stored clip tile instead of copyRect.
  {
    CaptureEditor editor(fixtureCapture(source),
                         CaptureEditor::CaptureMode::File);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    editor.applyClipForTest(QRectF(0, 48, kWidth, kBand),
                            QRectF(kWidth + 20, 48, kWidth, kBand));
    const QColor mark(9, 9, 9, 255);
    editor.markLastClipTileForTest(mark);
    editor.replayLogForTest();
    application.processEvents();
    bool kept = false;
    for (const Annotation &annotation : editor.currentAnnotationsForTest()) {
      if (annotation.kind != Annotation::Kind::Clip)
        continue;
      if (annotation.image.isNull() ||
          annotation.image.pixelColor(0, 0) != mark) {
        error = QStringLiteral("replay recopied a clip tile that was already present");
        return false;
      }
      kept = true;
    }
    if (!kept) {
      error = QStringLiteral("replay dropped the marked clip tile");
      return false;
    }
    editor.close();
  }

  // Repeat: two clips from different bands both stay as layers.
  {
    CaptureEditor editor(fixtureCapture(source),
                         CaptureEditor::CaptureMode::File);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    editor.applyClipForTest(QRectF(0, 48, kWidth, kBand),
                            QRectF(kWidth + 8, 48, kWidth, kBand));
    editor.applyClipForTest(QRectF(0, 80, kWidth, kBand),
                            QRectF(kWidth + 8, 80, kWidth, kBand));
    int clips = 0;
    for (const Annotation &annotation : editor.currentAnnotationsForTest()) {
      if (annotation.kind == Annotation::Kind::Clip)
        ++clips;
    }
    if (clips != 2) {
      error = QStringLiteral("repeat clip did not keep both layers");
      return false;
    }
    editor.close();
  }

  // Closed lasso: start and end sit on the same pixel, so the start–end
  // marquee is a fidget. The path bbox must still lock.
  {
    CaptureEditor editor(fixtureCapture(source),
                         CaptureEditor::CaptureMode::File);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    QTest::keyClick(&editor, Qt::Key_V);
    application.processEvents();
    QTest::keyClick(&editor, Qt::Key_V);
    application.processEvents();
    if (editor.pixelClipShapeForTest() !=
        CaptureEditor::PixelClipShape::Lasso) {
      error = QStringLiteral("two V presses did not arm clip lasso");
      return false;
    }
    const QPoint start = screenOf(editor, 20, 20);
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, start);
    application.processEvents();
    const QPoint corners[] = {screenOf(editor, 90, 20), screenOf(editor, 90, 90),
                              screenOf(editor, 20, 90), screenOf(editor, 22, 22)};
    for (const QPoint &corner : corners) {
      QTest::mouseMove(&editor, corner, 5);
      application.processEvents();
    }
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        screenOf(editor, 22, 22));
    application.processEvents();
    if (editor.pixelClipRectForTest().isEmpty()) {
      error = QStringLiteral("closed lasso vanished on release");
      return false;
    }
    if (editor.pixelClipPointsForTest().size() < 3) {
      error = QStringLiteral("closed lasso dropped its path on release");
      return false;
    }
    if (editor.pixelClipRectForTest().width() < 40.0 ||
        editor.pixelClipRectForTest().height() < 40.0) {
      error = QStringLiteral("closed lasso locked the start–end fidget box");
      return false;
    }
    if (editor.lockedClipOpForTest().shape != ClipShape::Lasso) {
      error = QStringLiteral("closed lasso did not lock a lasso clip op");
      return false;
    }
    editor.close();
  }

  // V cycles Rect → Ellipse → Lasso → Rect. Snap is a toggle, not a shape.
  {
    CaptureEditor editor(fixtureCapture(source),
                         CaptureEditor::CaptureMode::File);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    if (editor.pixelClipShapeForTest() != CaptureEditor::PixelClipShape::Rect) {
      error = QStringLiteral("clip shape did not start on Rect");
      return false;
    }
    QTest::keyClick(&editor, Qt::Key_V);
    application.processEvents();
    if (editor.pixelClipShapeForTest() !=
        CaptureEditor::PixelClipShape::Ellipse) {
      error = QStringLiteral("first V did not cycle to Ellipse");
      return false;
    }
    QTest::keyClick(&editor, Qt::Key_V);
    application.processEvents();
    if (editor.pixelClipShapeForTest() !=
        CaptureEditor::PixelClipShape::Lasso) {
      error = QStringLiteral("second V did not cycle to Lasso");
      return false;
    }
    QTest::keyClick(&editor, Qt::Key_V);
    application.processEvents();
    if (editor.pixelClipShapeForTest() != CaptureEditor::PixelClipShape::Rect) {
      error = QStringLiteral("third V did not return to Rect (snap is a toggle)");
      return false;
    }
    editor.close();
  }

  // Snap chip is a toggle: Rect stays the draw shape; empty click then snaps.
  {
    QImage disk(64, 64, QImage::Format_ARGB32_Premultiplied);
    disk.fill(QColor(20, 20, 40, 255));
    {
      QPainter painter(&disk);
      painter.setRenderHint(QPainter::Antialiasing, false);
      painter.setPen(Qt::NoPen);
      painter.setBrush(QColor(220, 80, 40, 255));
      painter.drawEllipse(QRect(8, 8, 48, 48));
    }
    CaptureEditor editor(fixtureCapture(disk),
                         CaptureEditor::CaptureMode::File);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    if (!editor.pixelClipSnapEnabledForTest()) {
      error = QStringLiteral("snap was not on by default");
      return false;
    }
    const QRectF snapChip =
        editor.toolbarButtonRectForTest(QStringLiteral("clip-shape-snap"));
    if (snapChip.isEmpty()) {
      error = QStringLiteral("snap chip was missing from the clip strip");
      return false;
    }
    if (editor.pixelClipShapeForTest() != CaptureEditor::PixelClipShape::Rect) {
      error = QStringLiteral("snap chip stole the Rect/Ellipse/Lasso shape");
      return false;
    }
    QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                      screenOf(editor, 32, 32));
    application.processEvents();
    if (editor.pixelClipRectForTest().isEmpty()) {
      error = QStringLiteral("snap-on click did not lock a mask");
      return false;
    }
    if (editor.pixelClipShapeForTest() != CaptureEditor::PixelClipShape::Rect) {
      error = QStringLiteral("snap click changed the draw shape away from Rect");
      return false;
    }
    if (editor.lockedClipOpForTest().shape != ClipShape::Rect) {
      error = QStringLiteral("rect+snap click did not lock a rectangle clip op");
      return false;
    }
    QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                      snapChip.center().toPoint());
    application.processEvents();
    if (editor.pixelClipSnapEnabledForTest()) {
      error = QStringLiteral("second snap-chip click did not turn snap off");
      return false;
    }
    QTest::keyClick(&editor, Qt::Key_Escape);
    application.processEvents();
    if (!editor.pixelClipRectForTest().isEmpty()) {
      error = QStringLiteral("Esc did not clear the snapped mask");
      return false;
    }
    QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                      screenOf(editor, 32, 32));
    application.processEvents();
    if (!editor.pixelClipRectForTest().isEmpty()) {
      error = QStringLiteral("snap-off click still locked a mask");
      return false;
    }
    editor.close();
  }

  // Ellipse + Snap: a loose drag around a disk locks the detected circle.
  {
    CaptureEditor editor(fixtureCapture(diskImage()),
                         CaptureEditor::CaptureMode::File);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    QTest::keyClick(&editor, Qt::Key_V);
    application.processEvents();
    if (editor.pixelClipShapeForTest() !=
        CaptureEditor::PixelClipShape::Ellipse) {
      error = QStringLiteral("V did not arm clip ellipse");
      return false;
    }
    if (!ensureSnap(editor, application, true, error))
      return false;
    if (!editor.pixelClipSnapEnabledForTest()) {
      error = QStringLiteral("ellipse+snap could not turn snap on");
      return false;
    }
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier,
                      screenOf(editor, 2, 2));
    QTest::mouseMove(&editor, screenOf(editor, 62, 62), 10);
    application.processEvents();
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        screenOf(editor, 62, 62));
    application.processEvents();
    const QRectF locked = editor.pixelClipRectForTest();
    if (locked.isEmpty()) {
      error = QStringLiteral("ellipse+snap drag did not lock a mask");
      return false;
    }
    if (editor.pixelClipShapeForTest() !=
        CaptureEditor::PixelClipShape::Ellipse) {
      error = QStringLiteral("ellipse+snap changed the draw shape");
      return false;
    }
    if (editor.lockedClipOpForTest().shape != ClipShape::Ellipse) {
      error = QStringLiteral("ellipse+snap did not lock an ellipse");
      return false;
    }
    if (lockWasLooseDrag(locked)) {
      error = QStringLiteral(
          "ellipse+snap kept the loose drag instead of the disk");
      return false;
    }
    editor.close();
  }

  // Ellipse + Snap around a checker wheel must enclose the pin, not Hough
  // the clean circle. One grab only — do not dump drag frames.
  {
    CaptureEditor editor(fixtureCapture(checkerPinImage()),
                         CaptureEditor::CaptureMode::File);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    QTest::keyClick(&editor, Qt::Key_V);
    application.processEvents();
    if (editor.pixelClipShapeForTest() !=
        CaptureEditor::PixelClipShape::Ellipse) {
      error = QStringLiteral("V did not arm clip ellipse for the pin wheel");
      return false;
    }
    if (!ensureSnap(editor, application, true, error))
      return false;
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier,
                      screenOf(editor, 2, 2));
    QTest::mouseMove(&editor, screenOf(editor, 62, 62), 10);
    application.processEvents();
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        screenOf(editor, 62, 62));
    application.processEvents();
    if (editor.lockedClipOpForTest().shape != ClipShape::Ellipse) {
      error = QStringLiteral("ellipse+snap on a pin wheel did not lock an ellipse");
      return false;
    }
    const QRect native = editor.lockedClipOpForTest().sourceRect;
    if (native.left() < 4 || native.right() < 56 || native.width() >= 60) {
      error = QStringLiteral(
          "ellipse+snap drag on a checker wheel missed the pin or kept the "
          "loose drag (left=%1 right=%2 width=%3)")
          .arg(native.left())
          .arg(native.right())
          .arg(native.width());
      return false;
    }
    if (!saveGrab(editor, outputRoot + QStringLiteral("-clip-pin-wheel.png"),
                  error))
      return false;
    editor.close();
  }

  // Lasso + Snap around a disk magnets to the circle (the people-icon case).
  {
    CaptureEditor editor(fixtureCapture(diskImage()),
                         CaptureEditor::CaptureMode::File);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    QTest::keyClick(&editor, Qt::Key_V);
    application.processEvents();
    QTest::keyClick(&editor, Qt::Key_V);
    application.processEvents();
    if (editor.pixelClipShapeForTest() !=
        CaptureEditor::PixelClipShape::Lasso) {
      error = QStringLiteral("two V presses did not arm clip lasso");
      return false;
    }
    if (!ensureSnap(editor, application, true, error))
      return false;
    if (!editor.pixelClipSnapEnabledForTest()) {
      error = QStringLiteral("lasso+snap could not turn snap on");
      return false;
    }
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier,
                      screenOf(editor, 2, 2));
    application.processEvents();
    const QPoint around[] = {screenOf(editor, 62, 2), screenOf(editor, 62, 62),
                             screenOf(editor, 2, 62), screenOf(editor, 4, 4)};
    for (const QPoint &corner : around) {
      QTest::mouseMove(&editor, corner, 5);
      application.processEvents();
    }
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        screenOf(editor, 4, 4));
    application.processEvents();
    if (editor.pixelClipShapeForTest() !=
        CaptureEditor::PixelClipShape::Lasso) {
      error = QStringLiteral("lasso+snap changed the draw shape");
      return false;
    }
    if (editor.lockedClipOpForTest().shape != ClipShape::Lasso) {
      error = QStringLiteral("lasso+snap around a disk did not lock an outline");
      return false;
    }
    if (editor.pixelClipPointsForTest().size() < 8) {
      error = QStringLiteral("lasso+snap around a disk did not keep a silhouette");
      return false;
    }
    if (lockWasLooseDrag(editor.pixelClipRectForTest())) {
      error = QStringLiteral(
          "lasso+snap around a disk kept the loose path instead of the circle");
      return false;
    }
    editor.close();
  }

  // Rect + Snap around a square locks the detected rectangle, not a circle.
  {
    CaptureEditor editor(fixtureCapture(squareImage()),
                         CaptureEditor::CaptureMode::File);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    if (editor.pixelClipShapeForTest() != CaptureEditor::PixelClipShape::Rect) {
      error = QStringLiteral("clip shape did not start on Rect");
      return false;
    }
    if (!ensureSnap(editor, application, true, error))
      return false;
    if (!editor.pixelClipSnapEnabledForTest()) {
      error = QStringLiteral("rect+snap could not turn snap on");
      return false;
    }
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier,
                      screenOf(editor, 2, 2));
    QTest::mouseMove(&editor, screenOf(editor, 62, 62), 10);
    application.processEvents();
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        screenOf(editor, 62, 62));
    application.processEvents();
    if (editor.pixelClipShapeForTest() != CaptureEditor::PixelClipShape::Rect) {
      error = QStringLiteral("rect+snap changed the draw shape");
      return false;
    }
    if (editor.lockedClipOpForTest().shape != ClipShape::Rect) {
      error = QStringLiteral("rect+snap around a square did not lock a rectangle");
      return false;
    }
    if (lockWasLooseDrag(editor.pixelClipRectForTest())) {
      error = QStringLiteral(
          "rect+snap around a square kept the loose drag instead of the square");
      return false;
    }
    editor.close();
  }

  // Rect + Snap around a rounded card locks a rounded rect, not a sharp box.
  {
    CaptureEditor editor(fixtureCapture(roundedCardImage()),
                         CaptureEditor::CaptureMode::File);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    if (!ensureSnap(editor, application, true, error))
      return false;
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier,
                      screenOf(editor, 2, 2));
    QTest::mouseMove(&editor, screenOf(editor, 62, 62), 10);
    application.processEvents();
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        screenOf(editor, 62, 62));
    application.processEvents();
    const ClipOp locked = editor.lockedClipOpForTest();
    if (locked.shape != ClipShape::Rect || locked.radius < 5.0) {
      error = QStringLiteral(
                  "rect+snap on a rounded card did not lock a corner radius "
                  "(radius=%1)")
                  .arg(locked.radius);
      return false;
    }
    if (!saveGrab(editor, outputRoot + QStringLiteral("-clip-rounded-card.png"),
                  error))
      return false;
    editor.close();
  }

  // Snap on does not replace a real lasso with a click-snap on release.
  {
    CaptureEditor editor(fixtureCapture(source),
                         CaptureEditor::CaptureMode::File);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    QTest::keyClick(&editor, Qt::Key_V);
    application.processEvents();
    QTest::keyClick(&editor, Qt::Key_V);
    application.processEvents();
    if (!ensureSnap(editor, application, true, error))
      return false;
    if (!editor.pixelClipSnapEnabledForTest() ||
        editor.pixelClipShapeForTest() !=
            CaptureEditor::PixelClipShape::Lasso) {
      error = QStringLiteral("could not arm lasso with snap on");
      return false;
    }
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier,
                      screenOf(editor, 20, 20));
    application.processEvents();
    const QPoint lasso[] = {screenOf(editor, 90, 20), screenOf(editor, 90, 90),
                            screenOf(editor, 20, 90), screenOf(editor, 22, 22)};
    for (const QPoint &corner : lasso) {
      QTest::mouseMove(&editor, corner, 5);
      application.processEvents();
    }
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        screenOf(editor, 22, 22));
    application.processEvents();
    if (editor.pixelClipPointsForTest().size() < 3 ||
        editor.lockedClipOpForTest().shape != ClipShape::Lasso) {
      error = QStringLiteral("snap-on closed lasso did not keep the lasso path");
      return false;
    }
    editor.close();
  }

  return true;
}
