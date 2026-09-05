/** @fileoverview Exercises adjustable pen smoothing, history and rendering. */
#include "stroke-smoothing-smoke.hpp"

#include "capture.hpp"
#include "editor.hpp"
#include "stroke-smoothing.hpp"

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QLineF>
#include <QPainter>
#include <QPoint>
#include <QPointF>
#include <QRectF>
#include <QTemporaryDir>
#include <QVector>
#include <QWheelEvent>
#include <QtGlobal>
#include <QtTest/QTest>
#include <QtTest/qtestkeyboard.h>
#include <QtTest/qtestmouse.h>

#include <algorithm>

namespace {

bool nearPoint(const QPointF &actual, const QPointF &wanted,
               qreal tolerance = 0.001) {
  return QLineF(actual, wanted).length() <= tolerance;
}

qreal polylineLength(const QVector<QPointF> &points) {
  qreal length = 0.0;
  for (qsizetype index = 1; index < points.size(); ++index)
    length += QLineF(points.at(index - 1), points.at(index)).length();
  return length;
}

QRectF pointBounds(const QVector<QPointF> &points) {
  if (points.isEmpty())
    return {};
  qreal left = points.first().x();
  qreal right = left;
  qreal top = points.first().y();
  qreal bottom = top;
  for (const QPointF &point : points) {
    left = std::min(left, point.x());
    right = std::max(right, point.x());
    top = std::min(top, point.y());
    bottom = std::max(bottom, point.y());
  }
  return {QPointF(left, top), QPointF(right, bottom)};
}

bool hasInkNear(const QImage &image, const QPointF &point,
                const QColor &background) {
  const int centerX = qRound(point.x());
  const int centerY = qRound(point.y());
  for (int y = centerY - 3; y <= centerY + 3; ++y) {
    for (int x = centerX - 3; x <= centerX + 3; ++x) {
      if (x >= 0 && y >= 0 && x < image.width() && y < image.height() &&
          image.pixelColor(x, y) != background)
        return true;
    }
  }
  return false;
}

} // namespace

bool runStrokeSmoothingSmoke(QApplication &application, QString &error) {
  // Empty, click/dot and two-point strokes are already the simplest possible
  // geometry. They must survive every release level byte-for-byte.
  const QVector<QPointF> empty;
  const QVector<QPointF> dot{{7, 9}};
  const QVector<QPointF> repeatedDot{{7, 9}, {7, 9}};
  const QVector<QPointF> segment{{2, 3}, {40, 30}};
  for (int level = stroke::minimumSmoothingLevel;
       level <= stroke::maximumSmoothingLevel; ++level) {
    if (stroke::smoothFreehand(empty, level) != empty ||
        stroke::smoothFreehand(dot, level) != dot ||
        stroke::smoothFreehand(repeatedDot, level) != repeatedDot ||
        stroke::smoothFreehand(segment, level) != segment) {
      error = QStringLiteral("Pen smoothing changed a short or dot stroke");
      return false;
    }
  }

  const QVector<QPointF> noisy{{0, 0},   {10, 2},  {20, -2}, {30, 2},
                               {40, 20}, {50, 42}, {60, 46}, {70, 41},
                               {80, 20}, {90, 2},  {100, -1}, {110, 0}};
  const QVector<QPointF> levelZero = stroke::smoothFreehand(noisy, 0);
  const QVector<QPointF> levelOne = stroke::smoothFreehand(noisy, 1);
  const QVector<QPointF> levelTwo = stroke::smoothFreehand(noisy, 2);
  const QVector<QPointF> smoothed = stroke::smoothFreehand(noisy);
  if (stroke::defaultSmoothingLevel != 3 || levelZero != noisy ||
      stroke::smoothFreehand(noisy, -20) != levelZero ||
      stroke::smoothFreehand(noisy, 20) !=
          stroke::smoothFreehand(noisy, stroke::maximumSmoothingLevel) ||
      levelOne.size() != noisy.size() * 2 ||
      levelTwo.size() != noisy.size() * 4 ||
      smoothed != stroke::smoothFreehand(noisy, 3) ||
      levelOne == levelTwo || levelTwo == smoothed) {
    error = QStringLiteral("Pen levels did not match the configured 0--6 stages");
    return false;
  }
  for (int level = 1; level <= stroke::maximumSmoothingLevel; ++level) {
    const QVector<QPointF> result = stroke::smoothFreehand(noisy, level);
    if (!nearPoint(result.first(), noisy.first()) ||
        !nearPoint(result.last(), noisy.last())) {
      error = QStringLiteral("A pen smoothing level moved an endpoint");
      return false;
    }
  }
  const auto highest = std::ranges::max_element(
      smoothed, {}, [](const QPointF &point) { return point.y(); });
  if (smoothed == noisy || smoothed.size() < 3 ||
      !nearPoint(smoothed.first(), noisy.first()) ||
      !nearPoint(smoothed.last(), noisy.last()) ||
      highest == smoothed.end() || highest->y() < 25.0 ||
      polylineLength(smoothed) >= polylineLength(noisy)) {
    error = QStringLiteral(
        "RDP/Chaikin did not smooth jitter while preserving the gesture");
    return false;
  }

  // A long, low-amplitude trace collapses before Chaikin expands it. This is
  // the common high-sample-rate case and guards against point-count blow-up.
  QVector<QPointF> dense;
  dense.reserve(50000);
  for (int index = 0; index < 50000; ++index)
    dense.push_back({index * 0.25, index % 2 == 0 ? -1.0 : 1.0});
  const QVector<QPointF> denseSmoothed = stroke::smoothFreehand(dense, 5);
  if (denseSmoothed.size() != 2 ||
      denseSmoothed.first() != dense.first() ||
      denseSmoothed.last() != dense.last()) {
    error =
        QStringLiteral("Dense pen jitter was not simplified before smoothing");
    return false;
  }
  for (int index = 0; index < dense.size(); ++index)
    dense[index].setY(index % 2 == 0 ? -10.0 : 10.0);
  const QVector<QPointF> adversarial = stroke::smoothFreehand(dense, 6);
  if (adversarial.size() > static_cast<qsizetype>(2048 * 8) ||
      adversarial.first() != dense.first() ||
      adversarial.last() != dense.last()) {
    error = QStringLiteral("A long pen scribble escaped the smoothing budget");
    return false;
  }

  const QColor background(QStringLiteral("#182030"));
  QImage painted(140, 100, QImage::Format_ARGB32_Premultiplied);
  painted.fill(background);
  Annotation rendered;
  rendered.kind = Annotation::Kind::Freehand;
  rendered.color = QColor(QStringLiteral("#ffd60a"));
  rendered.size = 6;
  rendered.points = smoothed;
  rendered.start = smoothed.first();
  rendered.end = smoothed.last();
  for (QPointF &point : rendered.points)
    point += QPointF(10, 25);
  rendered.start = rendered.points.first();
  rendered.end = rendered.points.last();
  {
    QPainter painter(&painted);
    painter.setRenderHint(QPainter::Antialiasing, true);
    paintAnnotation(painter, rendered);
  }
  if (!hasInkNear(painted, rendered.start, background) ||
      !hasInkNear(painted, rendered.end, background)) {
    error = QStringLiteral("Smoothed pen endpoints did not render");
    return false;
  }

  // Exercise the complete editor path: raw input -> release smoothing -> op
  // log -> rendering -> stroke hit-test -> box resize -> undo/redo.
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 400, 300};
  capture.monitor.pixelSize = {400, 300};
  capture.monitor.scale = 1.0;
  capture.source = QImage(400, 300, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(background);
  capture.previewSize = capture.source.size();

  CaptureEditor editor(capture, CaptureEditor::CaptureMode::File);
  editor.setSuppressSnapshots(true);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();
  // A 400x300 file fits at 1:1 in the editor's content band; the measured
  // origin keeps the drag coordinates exact whatever the chrome height is.
  const QPointF imageOrigin = editor.editImageRectForTest().topLeft();
  const QVector<QPointF> raw{{35, 65},  {55, 68},  {75, 62},  {95, 70},
                             {115, 90}, {135, 118}, {155, 124}, {175, 116},
                             {195, 92}, {215, 72},  {235, 65}};
  QTest::keyClick(&editor, Qt::Key_F);
  if (!editor.statusForTest().contains(QStringLiteral("smoothing 3/6")) ||
      !editor.statusForTest().contains(QStringLiteral("Alt+wheel"))) {
    error = QStringLiteral("Arming the pen did not show its smoothing control");
    return false;
  }
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier,
                    (imageOrigin + raw.first()).toPoint());
  for (qsizetype index = 1; index + 1 < raw.size(); ++index)
    QTest::mouseMove(&editor, (imageOrigin + raw.at(index)).toPoint(), 1);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      (imageOrigin + raw.last()).toPoint());
  application.processEvents();
  if (editor.annotationCountForTest() != 1 ||
      editor.operationLog().isEmpty() ||
      editor.operationLog().last().type != Operation::Type::Annotate ||
      editor.operationLog().last().annotations.size() != 1) {
    error = QStringLiteral("A smoothed pen drag did not commit one operation");
    return false;
  }
  const Annotation committed = editor.operationLog().last().annotations.first();
  if (committed.kind != Annotation::Kind::Freehand ||
      committed.smoothingLevel != stroke::defaultSmoothingLevel ||
      committed.rawPoints != raw || committed.points.size() < 3 ||
      committed.points == committed.rawPoints ||
      committed.points != stroke::smoothFreehand(committed.rawPoints, 3) ||
      !nearPoint(committed.points.first(), raw.first()) ||
      !nearPoint(committed.points.last(), raw.last()) ||
      !nearPoint(committed.rawPoints.first(), raw.first()) ||
      !nearPoint(committed.rawPoints.last(), raw.last()) ||
      committed.start != committed.points.first() ||
      committed.end != committed.points.last()) {
    error = QStringLiteral(
        "Committed pen geometry lost its level, baseline or endpoints");
    return false;
  }
  const QImage committedOutput = editor.renderCurrentOutput();
  if (!hasInkNear(committedOutput, committed.start, background) ||
      !hasInkNear(committedOutput, committed.end, background)) {
    error = QStringLiteral(
        "Committed smoothed pen endpoints were absent from export");
    return false;
  }

  // Level zero must keep the exact samples delivered to the editor. The
  // default starts at three, so explicitly wheel it down before drawing.
  {
    CaptureEditor rawEditor(capture, CaptureEditor::CaptureMode::File);
    rawEditor.setSuppressSnapshots(true);
    rawEditor.resize(800, 600);
    rawEditor.show();
    application.processEvents();
    QTest::keyClick(&rawEditor, Qt::Key_F);
    for (int level = stroke::defaultSmoothingLevel;
         level > stroke::minimumSmoothingLevel; --level) {
      QWheelEvent wheel(QPointF(400, 300), QPointF(400, 300), {}, {0, -120},
                        Qt::NoButton, Qt::AltModifier, Qt::NoScrollPhase,
                        false);
      QApplication::sendEvent(&rawEditor, &wheel);
    }
    QTest::mousePress(&rawEditor, Qt::LeftButton, Qt::NoModifier,
                      (imageOrigin + raw.first()).toPoint());
    for (qsizetype index = 1; index + 1 < raw.size(); ++index)
      QTest::mouseMove(&rawEditor,
                       (imageOrigin + raw.at(index)).toPoint(), 1);
    QTest::mouseRelease(&rawEditor, Qt::LeftButton, Qt::NoModifier,
                        (imageOrigin + raw.last()).toPoint());
    application.processEvents();
    if (rawEditor.operationLog().isEmpty()) {
      error = QStringLiteral("Raw pen stroke did not commit");
      return false;
    }
    const Annotation rawStroke =
        rawEditor.operationLog().last().annotations.constFirst();
    if (rawStroke.smoothingLevel != stroke::minimumSmoothingLevel ||
        rawStroke.rawPoints != raw || rawStroke.points != rawStroke.rawPoints) {
      error = QStringLiteral("Pen level zero did not preserve raw input");
      return false;
    }
  }

  QTest::keyClick(&editor, Qt::Key_V);
  const QPointF body = committed.points.at(committed.points.size() / 2);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                    (imageOrigin + body).toPoint());
  application.processEvents();
  if (editor.selectedCountForTest() != 1) {
    error = QStringLiteral("Smoothed pen geometry was not hit-test selectable");
    return false;
  }

  const int beforeSmoothingIndex = editor.operationIndex();
  QWheelEvent smoothWheel(imageOrigin + body, imageOrigin + body, {}, {0, 120},
                          Qt::NoButton, Qt::AltModifier, Qt::NoScrollPhase,
                          false);
  QApplication::sendEvent(&editor, &smoothWheel);
  application.processEvents();
  if (editor.operationIndex() != beforeSmoothingIndex + 1 ||
      editor.operationLog().last().type != Operation::Type::Patch ||
      editor.operationLog().last().annotations.size() != 1) {
    error = QStringLiteral("Alt+wheel did not patch the selected pen stroke");
    return false;
  }
  const Annotation adjusted =
      editor.operationLog().last().annotations.constFirst();
  if (adjusted.smoothingLevel != 4 ||
      adjusted.rawPoints != committed.rawPoints ||
      adjusted.points != stroke::smoothFreehand(committed.rawPoints, 4) ||
      !editor.statusForTest().contains(QStringLiteral("smoothing 4/6"))) {
    error = QStringLiteral(
        "Selected pen smoothing compounded or changed its baseline");
    return false;
  }
  const QImage adjustedOutput = editor.renderCurrentOutput();
  if (adjustedOutput == committedOutput) {
    error = QStringLiteral("Changing pen smoothing did not change its output");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (editor.renderCurrentOutput() != committedOutput) {
    error = QStringLiteral("Undo did not restore the prior pen smoothing");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
  application.processEvents();
  if (editor.renderCurrentOutput() != adjustedOutput) {
    error = QStringLiteral("Redo did not restore adjusted pen smoothing");
    return false;
  }
  // Leave the baseline level active for the independent resize checks below.
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();

  const QImage beforeResize = editor.renderCurrentOutput();
  const QRectF bounds = pointBounds(committed.points);
  const QPoint resizeStart = (imageOrigin + bounds.bottomRight()).toPoint();
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, resizeStart);
  QTest::mouseMove(&editor, resizeStart + QPoint(35, 25), 1);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      resizeStart + QPoint(35, 25));
  application.processEvents();
  const QImage afterResize = editor.renderCurrentOutput();
  const Annotation resized =
      editor.operationLog().last().annotations.constFirst();
  if (afterResize == beforeResize || editor.operationLog().isEmpty() ||
      editor.operationLog().last().type != Operation::Type::Patch ||
      resized.points.size() != committed.points.size() ||
      resized.rawPoints.size() != committed.rawPoints.size() ||
      resized.rawPoints == committed.rawPoints) {
    error = QStringLiteral(
        "A smoothed pen stroke or its baseline did not resize as a layer");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (editor.renderCurrentOutput() != beforeResize) {
    error = QStringLiteral("Undo did not restore the smoothed pen resize");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
  application.processEvents();
  if (editor.renderCurrentOutput() != afterResize) {
    error = QStringLiteral("Redo did not restore the smoothed pen resize");
    return false;
  }

  // A click still belongs to the editor's dead zone; smoothing must not turn
  // a deselection click into a stray dot layer.
  QTest::keyClick(&editor, Qt::Key_F);
  const int beforeClick = editor.annotationCountForTest();
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                    (imageOrigin + QPointF(310, 220)).toPoint());
  application.processEvents();
  if (editor.annotationCountForTest() != beforeClick) {
    error = QStringLiteral("Pen smoothing turned a click into a stray layer");
    return false;
  }

  // With no pen selected, Alt+wheel adjusts the armed default and the next
  // stroke records that level rather than mutating an unrelated layer.
  {
    CaptureEditor defaultsEditor(capture, CaptureEditor::CaptureMode::File);
    defaultsEditor.setSuppressSnapshots(true);
    defaultsEditor.resize(800, 600);
    defaultsEditor.show();
    application.processEvents();
    QTest::keyClick(&defaultsEditor, Qt::Key_F);
    QWheelEvent defaultWheel(QPointF(400, 300), QPointF(400, 300), {},
                             {0, 120}, Qt::NoButton, Qt::AltModifier,
                             Qt::NoScrollPhase, false);
    QApplication::sendEvent(&defaultsEditor, &defaultWheel);
    application.processEvents();
    if (!defaultsEditor.statusForTest().contains(
            QStringLiteral("smoothing 4/6"))) {
      error = QStringLiteral("Alt+wheel did not change the armed pen default");
      return false;
    }
    QTest::mousePress(&defaultsEditor, Qt::LeftButton, Qt::NoModifier,
                      (imageOrigin + raw.first()).toPoint());
    for (qsizetype index = 1; index + 1 < raw.size(); ++index)
      QTest::mouseMove(&defaultsEditor,
                       (imageOrigin + raw.at(index)).toPoint(), 1);
    QTest::mouseRelease(&defaultsEditor, Qt::LeftButton, Qt::NoModifier,
                        (imageOrigin + raw.last()).toPoint());
    application.processEvents();
    if (defaultsEditor.operationLog().isEmpty() ||
        defaultsEditor.operationLog().last().type !=
            Operation::Type::Annotate) {
      error = QStringLiteral("Adjusted pen default did not create a stroke");
      return false;
    }
    const Annotation defaultAdjusted =
        defaultsEditor.operationLog().last().annotations.constFirst();
    if (defaultAdjusted.smoothingLevel != 4 ||
        defaultAdjusted.points !=
            stroke::smoothFreehand(defaultAdjusted.rawPoints, 4)) {
      error = QStringLiteral("The next pen stroke ignored its armed smoothing");
      return false;
    }
    defaultsEditor.close();
  }

  const QTemporaryDir directory;
  if (!directory.isValid()) {
    error = QStringLiteral("Could not create pen operation-log test directory");
    return false;
  }
  const OperationLog saved{editor.operationLog(), editor.operationIndex(), 2,
                           1, capture.previewSize};
  const QString logPath = directory.filePath(QStringLiteral("pen.json"));
  OperationLog reloaded;
  if (!saveOperationLog(logPath, saved, error) ||
      !loadOperationLog(logPath, reloaded, error) || reloaded != saved) {
    if (error.isEmpty())
      error = QStringLiteral(
          "Smoothed pen points did not round-trip in the op log");
    return false;
  }
  editor.close();
  return true;
}
