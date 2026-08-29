/** @fileoverview Drives the cut tool over banded BMP fixtures.
 *
 *  The source is ten named color bands. A cut is specified in annotation
 *  space (the pixels you see), converted to a widget drag through the
 *  editor's live geometry, then checked against an expected BMP of the
 *  remaining bands. Failures name the first wrong row by band index.
 */
#include "cut-mapping-smoke.hpp"

#include "capture.hpp"
#include "editor.hpp"
#include "image-fixture.hpp"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
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
  return editor.toScreenPointForTest(QPointF(ax, ay)).toPoint();
}

bool dragCut(QApplication &application, CaptureEditor &editor, QPoint from,
             QPoint to, QString &error) {
  QTest::keyClick(&editor, Qt::Key_X);
  application.processEvents();
  if (editor.armedToolForTest() != CaptureEditor::Tool::Cut) {
    error = QStringLiteral("X did not arm the cut tool");
    return false;
  }
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, from);
  QTest::mouseMove(&editor, to, 10);
  application.processEvents();
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, to);
  application.processEvents();
  return true;
}

bool checkCut(const CaptureEditor &editor, const QImage &expected,
              const QString &label, QString &error) {
  const QImage actual = editor.renderCurrentOutput();
  QString mismatch;
  if (sameRaster(actual, expected, mismatch))
    return true;
  error = label + QStringLiteral(": ") + mismatch;
  const auto lastCut = [&]() -> QString {
    for (int i = editor.operationIndex() - 1; i >= 0; --i) {
      const Operation &op = editor.operationLog().at(i);
      if (op.type != Operation::Type::Cut)
        continue;
      return QStringLiteral(" cut src=[%1,%2) logical=[%3,%4) sel=%5,%6 %7x%8")
          .arg(op.cut.sourceStart)
          .arg(op.cut.sourceEnd)
          .arg(op.cut.logicalStart)
          .arg(op.cut.logicalEnd)
          .arg(editor.currentSelection().x())
          .arg(editor.currentSelection().y())
          .arg(editor.currentSelection().width())
          .arg(editor.currentSelection().height());
    }
    return QStringLiteral(" (no cut in log)");
  };
  error += lastCut();
  error += QStringLiteral(" imageRect=%1,%2 %3x%4 scale=%5")
               .arg(editor.editImageRectForTest().x())
               .arg(editor.editImageRectForTest().y())
               .arg(editor.editImageRectForTest().width())
               .arg(editor.editImageRectForTest().height())
               .arg(editor.editScaleForTest());
  return false;
}

} // namespace

bool runCutMappingSmoke(QApplication &application, const QString &outputRoot,
                        QString &error) {
  constexpr int kWidth = 160;
  constexpr int kBand = 16;
  constexpr int kBands = 10;
  const QImage source = rowBandImage(kWidth, kBand, kBands);
  QDir().mkpath(QFileInfo(outputRoot).path());
  if (!saveBmp(source, outputRoot + QStringLiteral("-cut-source.bmp"), error))
    return false;

  // Direct apply: no widget mapping. Green band (rows 48..64) must vanish.
  {
    CaptureEditor editor(fixtureCapture(source),
                         CaptureEditor::CaptureMode::File);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    editor.applyCutForTest(CutOp{Qt::Horizontal, 48, 64, 48, 64});
    const QImage expected =
        rowBandsKept(kWidth, kBand, {0, 1, 2, 4, 5, 6, 7, 8, 9});
    if (!saveBmp(editor.renderCurrentOutput(),
                 outputRoot + QStringLiteral("-cut-direct-actual.bmp"),
                 error) ||
        !saveBmp(expected,
                 outputRoot + QStringLiteral("-cut-direct-expected.bmp"),
                 error))
      return false;
    if (!checkCut(editor, expected, QStringLiteral("direct green band"),
                  error))
      return false;
    editor.close();
  }

  // HiDPI: source is 2× preview. Cut is specified in both spaces.
  {
    const QImage logical = rowBandImage(kWidth, kBand, kBands);
    QImage native = logical.scaled(kWidth * 2, logical.height() * 2,
                                   Qt::IgnoreAspectRatio, Qt::FastTransformation);
    CaptureData hidpi = fixtureCapture(native);
    hidpi.previewSize = logical.size();
    hidpi.monitor.scale = 2.0;
    hidpi.monitor.pixelSize = native.size();
    CaptureEditor editor(hidpi, CaptureEditor::CaptureMode::File);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    editor.applyCutForTest(CutOp{Qt::Horizontal, 96, 128, 48, 64});
    const QImage expected =
        rowBandsKept(kWidth, kBand, {0, 1, 2, 4, 5, 6, 7, 8, 9})
            .scaled(kWidth * 2, (kBands - 1) * kBand * 2, Qt::IgnoreAspectRatio,
                    Qt::FastTransformation);
    if (!saveBmp(editor.renderCurrentOutput(),
                 outputRoot + QStringLiteral("-cut-hidpi-actual.bmp"), error))
      return false;
    if (!checkCut(editor, expected, QStringLiteral("hidpi green band"), error))
      return false;
    editor.close();
  }

  // Vertical cut: drop the green column from a column-banded fixture.
  {
    const QImage columns = columnBandImage(kWidth, kBand, kBands);
    CaptureEditor editor(fixtureCapture(columns),
                         CaptureEditor::CaptureMode::File);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    editor.applyCutForTest(CutOp{Qt::Vertical, 48, 64, 48, 64});
    QImage expected(kBand * (kBands - 1), kWidth, QImage::Format_RGB32);
    const QVector<int> keep{0, 1, 2, 4, 5, 6, 7, 8, 9};
    for (int out = 0; out < keep.size(); ++out)
      for (int y = 0; y < kWidth; ++y)
        for (int x = 0; x < kBand; ++x)
          expected.setPixelColor(out * kBand + x, y,
                                 fixtureBandColor(keep.at(out)));
    if (!saveBmp(editor.renderCurrentOutput(),
                 outputRoot + QStringLiteral("-cut-columns-actual.bmp"),
                 error))
      return false;
    if (!checkCut(editor, expected, QStringLiteral("vertical green column"),
                  error))
      return false;
    editor.close();
  }

  // File mode, image small enough to sit at scale 1. Cut the green band
  // (index 3, annotation y 48..64). Remaining: 0 1 2 4 5 6 7 8 9.
  {
    CaptureEditor editor(fixtureCapture(source),
                         CaptureEditor::CaptureMode::File);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();

    const QPoint from = screenOf(editor, kWidth / 2.0, 48);
    const QPoint to = screenOf(editor, kWidth / 2.0, 64);
    if (!dragCut(application, editor, from, to, error))
      return false;

    const QImage expected =
        rowBandsKept(kWidth, kBand, {0, 1, 2, 4, 5, 6, 7, 8, 9});
    if (!saveBmp(editor.renderCurrentOutput(),
                 outputRoot + QStringLiteral("-cut-file-actual.bmp"), error) ||
        !saveBmp(expected,
                 outputRoot + QStringLiteral("-cut-file-expected.bmp"),
                 error))
      return false;
    if (!checkCut(editor, expected, QStringLiteral("file-mode green band"),
                  error))
      return false;
    editor.close();
  }

  // Monitor-sized source in a same-size window: the new header means the
  // image no longer fits at 1:1, so the drag must go through editScale().
  {
    constexpr int wide = 800;
    constexpr int tallBand = 60;
    const QImage big = rowBandImage(wide, tallBand, kBands);
    CaptureEditor editor(fixtureCapture(big),
                         CaptureEditor::CaptureMode::File);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    if (editor.editScaleForTest() >= 0.999) {
      error = QStringLiteral("expected a scaled-down 800x600 file image");
      return false;
    }

    // Green band is index 3: annotation y 180..240.
    const QPoint from = screenOf(editor, wide / 2.0, 180);
    const QPoint to = screenOf(editor, wide / 2.0, 240);
    if (!dragCut(application, editor, from, to, error))
      return false;

    const QImage actual = editor.renderCurrentOutput();
    if (!saveBmp(actual,
                 outputRoot + QStringLiteral("-cut-scaled-actual.bmp"),
                 error))
      return false;
    // Widget pixels cannot hit annotation edges exactly when scale ≠ 1, so
    // the cut may nibble one pixel of a neighbour. Green (band 3) must be
    // gone; red/orange/yellow (0-2) and cyan-onward (4-9) must remain.
    bool sawGreen = false;
    bool sawYellow = false;
    bool sawCyan = false;
    for (int y = 0; y < actual.height(); ++y) {
      const int band = fixtureBandIndex(actual.pixelColor(0, y));
      sawGreen = sawGreen || band == 3;
      sawYellow = sawYellow || band == 2;
      sawCyan = sawCyan || band == 4;
    }
    if (sawGreen || !sawYellow || !sawCyan) {
      error = QStringLiteral(
                  "scaled-window: green gone=%1 yellow=%2 cyan=%3 size=%4x%5")
                  .arg(!sawGreen)
                  .arg(sawYellow)
                  .arg(sawCyan)
                  .arg(actual.width())
                  .arg(actual.height());
      return false;
    }
    editor.close();
  }

  // Region mode: select the middle six bands (y 32..128 of an 800x600
  // canvas that starts with the same 160px strip at the top), then cut
  // the green band of that crop (annotation y 16..32 of the selection,
  // which is source y 48..64).
  {
    QImage canvas(800, 600, QImage::Format_RGB32);
    canvas.fill(QColor(10, 10, 10));
    {
      const QImage strip = rowBandImage(kWidth, kBand, kBands);
      for (int y = 0; y < strip.height(); ++y)
        for (int x = 0; x < strip.width(); ++x)
          canvas.setPixelColor(x, y, strip.pixelColor(x, y));
    }
    CaptureEditor editor(fixtureCapture(canvas));
    editor.resize(800, 600);
    editor.show();
    application.processEvents();

    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(0, 32));
    QTest::mouseMove(&editor, QPoint(kWidth, 128), 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(kWidth, 128));
    application.processEvents();
    if (editor.selectingForTest()) {
      error = QStringLiteral("region select did not enter edit");
      return false;
    }

    const QPoint from = screenOf(editor, kWidth / 2.0, 16);
    const QPoint to = screenOf(editor, kWidth / 2.0, 32);
    if (!dragCut(application, editor, from, to, error))
      return false;

    // Selection is y 32..128: bands 2-7. Cutting green (3) leaves 2,4,5,6,7.
    const QImage expected = rowBandsKept(kWidth, kBand, {2, 4, 5, 6, 7});
    if (!saveBmp(editor.renderCurrentOutput(),
                 outputRoot + QStringLiteral("-cut-region-actual.bmp"),
                 error) ||
        !saveBmp(expected,
                 outputRoot + QStringLiteral("-cut-region-expected.bmp"),
                 error))
      return false;
    if (!checkCut(editor, expected, QStringLiteral("region-mode green band"),
                  error))
      return false;
    editor.close();
  }

  // Ctrl+drag inserts a transparent band and grows the image.
  {
    CaptureEditor editor(fixtureCapture(source),
                         CaptureEditor::CaptureMode::File);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    QTest::keyClick(&editor, Qt::Key_X, Qt::ControlModifier);
    application.processEvents();
    if (editor.armedToolForTest() != CaptureEditor::Tool::Cut) {
      error = QStringLiteral("Ctrl+X did not arm the cut tool");
      return false;
    }
    const QPoint from = screenOf(editor, kWidth / 2.0, 80);
    const QPoint to = screenOf(editor, kWidth / 2.0, 96);
    QTest::mousePress(&editor, Qt::LeftButton, Qt::ControlModifier, from);
    QTest::mouseMove(&editor, to, 10);
    application.processEvents();
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::ControlModifier, to);
    application.processEvents();
    bool inserted = false;
    for (const Operation &op : editor.operationLog()) {
      if (op.type == Operation::Type::Cut && op.cut.insert)
        inserted = true;
    }
    if (!inserted) {
      error = QStringLiteral("Ctrl+drag did not log an insert band");
      return false;
    }
    const QImage composed = editor.composedSourceForTest();
    if (composed.height() != kBand * kBands + 16 ||
        composed.pixelColor(0, 80).alpha() != 0) {
      error = QStringLiteral("insert band did not open a transparent gap");
      return false;
    }
    editor.close();
  }

  return true;
}
