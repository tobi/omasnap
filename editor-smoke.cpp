/** @fileoverview Exercises capture editor behavior without a live compositor.
 */
#include "capture.hpp"
#include "editor.hpp"
#include "transform-smoke.hpp"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QPainterPath>
#include <QTemporaryDir>
#include <QWheelEvent>
#include <QtTest/QTest>

#include <algorithm>
#include <cstdio>
#include <numbers>

namespace {
/** Guards the working-snapshot lifecycle: create and overwrite in place. */
bool runTemporarySnapshotChecks(QString &error) {
  const QString path = temporarySnapshotPath();
  QImage firstImage(64, 64, QImage::Format_ARGB32_Premultiplied);
  firstImage.fill(QColor(QStringLiteral("#123456")));
  // PNG loading yields straight-alpha ARGB32; compare in our paint format so
  // pixel equality alone decides the outcome.
  const auto reload = [](const QString &file) {
    return QImage(file).convertToFormat(QImage::Format_ARGB32_Premultiplied);
  };
  if (!saveTemporarySnapshot(firstImage, path, error) ||
      reload(path) != firstImage || reload(path).isNull()) {
    if (error.isEmpty())
      error = QStringLiteral("Snapshot reload did not match the saved image");
    return false;
  }

  QImage secondImage(64, 64, QImage::Format_ARGB32_Premultiplied);
  secondImage.fill(QColor(QStringLiteral("#654321")));
  if (!saveTemporarySnapshot(secondImage, path, error))
    return false; // O_EXCL used to fail on the second call.
  if (reload(path) != secondImage) {
    error = QStringLiteral("Overwritten snapshot did not replace the first");
    return false;
  }

  const QFileInfo fileInfo(path);
  // The runtime directory must stay private and the snapshot must not be
  // readable by group or other.
  const QFileInfo dirInfo = QFileInfo(fileInfo.absolutePath());
  const QFileDevice::Permissions owner =
      QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
  const QFileDevice::Permissions shared =
      QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
      QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
  if ((dirInfo.permissions() & owner) != owner ||
      (dirInfo.permissions() & shared) != 0 ||
      (fileInfo.permissions() & shared) != 0)
    return false;
  QTemporaryDir securityRoot;
  const QString repairPath =
      QDir(securityRoot.path()).filePath(QStringLiteral("repair-me"));
  if (!securityRoot.isValid() || !QDir().mkdir(repairPath) ||
      !QFile::setPermissions(repairPath, owner | shared) ||
      !ensurePrivateDirectory(repairPath) ||
      (QFileInfo(repairPath).permissions() & shared) != 0) {
    error = QStringLiteral("Private directory permissions were not repaired");
    return false;
  }
  const QString symlinkPath =
      QDir(securityRoot.path()).filePath(QStringLiteral("directory-link"));
  if (!QFile::link(repairPath, symlinkPath) ||
      ensurePrivateDirectory(symlinkPath)) {
    error = QStringLiteral("Private directory accepted a symbolic link");
    return false;
  }
  const QString outsideDirectory =
      QDir(securityRoot.path()).filePath(QStringLiteral("outside"));
  const QFileDevice::Permissions visibleToGroup =
      QFileDevice::ReadGroup | QFileDevice::ExeGroup;
  if (!QDir().mkdir(outsideDirectory) ||
      !QFile::setPermissions(outsideDirectory, owner | visibleToGroup)) {
    error = QStringLiteral("Could not prepare snapshot boundary check");
    return false;
  }
  error.clear();
  const QString outsideSnapshot =
      QDir(outsideDirectory).filePath(QStringLiteral("snapshot.png"));
  if (saveTemporarySnapshot(secondImage, outsideSnapshot, error) ||
      QFile::exists(outsideSnapshot) ||
      (QFileInfo(outsideDirectory).permissions() & visibleToGroup) !=
          visibleToGroup) {
    error = QStringLiteral("Temporary snapshot escaped its private directory");
    return false;
  }
  QFile::remove(path);
  return true;
}

bool runHighlighterRenderingCheck(QString &error) {
  error = QStringLiteral("Highlighter rendering check failed");
  CaptureData capture;
  capture.monitor.scale = 1.0;
  capture.monitor.pixelSize = {200, 100};
  // Transparent canvas: the selected capture is composited underneath, so a
  // white source would make every alpha check read 255.
  capture.source = QImage(200, 100, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(Qt::transparent);
  capture.preview = capture.source;

  Annotation highlight;
  highlight.kind = Annotation::Kind::Highlighter;
  highlight.color = QColor(QStringLiteral("#ffd60a"));
  highlight.size = 4;
  highlight.points = {{50, 50}, {90, 50}, {130, 50}, {170, 50}};
  const QImage rendered = renderCapture(capture, QRectF(0, 0, 200, 100),
                                        {highlight}, BackgroundStyle::None);
  if (rendered.isNull())
    return false;

  int midAlpha = 0;
  bool ok = true;
  for (const int x : {60, 90, 120, 150}) {
    const int a = rendered.pixelColor(x, 50).alpha();
    midAlpha = std::max(midAlpha, a);
    if (a < 96 || a > 160)
      ok = false; // translucent middle of the stroke
  }
  for (const int offset : {-5, 5}) {
    if (rendered.pixelColor(100, 50 + offset).alpha() <= 0)
      ok = false; // width ~= size*3 keeps the band opaque enough
  }
  if (rendered.pixelColor(10, 50).alpha() != 0)
    ok = false; // untouched area stays transparent
  if (midAlpha == 255)
    ok = false; // highlight must blend, not paint solid
  if (!ok) {
    error = QStringLiteral("Highlighter stroke was not translucent/wide");
    return false;
  }
  return true;
}

bool runSpotlightRenderingCheck(QString &error) {
  error = QStringLiteral("Spotlight/loupe rendering check failed");
  CaptureData capture;
  capture.monitor.scale = 1.0;
  capture.monitor.pixelSize = {200, 100};
  capture.source = QImage(200, 100, QImage::Format_ARGB32_Premultiplied);
  for (int y = 0; y < capture.source.height(); ++y) {
    for (int x = 0; x < capture.source.width(); ++x)
      capture.source.setPixelColor(x, y, QColor(x, x, x));
  }
  capture.preview = capture.source;

  Annotation spotlight;
  spotlight.kind = Annotation::Kind::Spotlight;
  spotlight.start = {50, 20};
  spotlight.end = {150, 80};
  spotlight.color = Qt::white;
  spotlight.size = 4;
  spotlight.magnification = 2.0;
  const QImage rendered = renderCapture(capture, QRectF(0, 0, 200, 100),
                                        {spotlight}, BackgroundStyle::None);
  if (rendered.isNull())
    return false;

  const int magnified = rendered.pixelColor(125, 50).red();
  const int dimmed = rendered.pixelColor(10, 50).red();
  const int dimmedCorner = rendered.pixelColor(55, 25).red();
  if (magnified < 108 || magnified > 117 || dimmed > 5 ||
      dimmedCorner > 28) {
    error = QStringLiteral(
        "Spotlight did not magnify its lens while dimming the surroundings");
    return false;
  }

  const QPointF corner(51, 21);
  if (spotlightPath(spotlight).contains(corner)) {
    error = QStringLiteral("Ellipse spotlight included its bounding corner");
    return false;
  }
  spotlight.spotlightShape = SpotlightShape::Rectangle;
  const QImage rectangle = renderCapture(capture, QRectF(0, 0, 200, 100),
                                         {spotlight}, BackgroundStyle::None);
  if (!spotlightPath(spotlight).contains(corner) ||
      rectangle.pixelColor(55, 25).red() < 60) {
    error = QStringLiteral("Rectangle spotlight did not include its corners");
    return false;
  }
  spotlight.spotlightShape = SpotlightShape::RoundedRectangle;
  const QImage rounded = renderCapture(capture, QRectF(0, 0, 200, 100),
                                       {spotlight}, BackgroundStyle::None);
  const bool roundedCorner = spotlightPath(spotlight).contains(corner);
  const bool roundedInterior =
      spotlightPath(spotlight).contains(QPointF(60, 30));
  const int roundedCornerRed = rounded.pixelColor(50, 20).red();
  if (roundedCorner || !roundedInterior || roundedCornerRed > 35) {
    error = QStringLiteral("Rounded spotlight geometry: corner=%1 interior=%2 "
                           "corner-red=%3")
                .arg(roundedCorner)
                .arg(roundedInterior)
                .arg(roundedCornerRed);
    return false;
  }

  CaptureData highDpi = capture;
  highDpi.monitor.scale = 2.0;
  highDpi.monitor.pixelSize = {400, 200};
  highDpi.source = capture.source.scaled(
      highDpi.monitor.pixelSize, Qt::IgnoreAspectRatio,
      Qt::SmoothTransformation);
  spotlight.spotlightShape = SpotlightShape::Ellipse;
  const QImage highDpiRendered = renderCapture(
      highDpi, QRectF(0, 0, 200, 100), {spotlight}, BackgroundStyle::None);
  const int highDpiMagnified = highDpiRendered.pixelColor(250, 100).red();
  if (highDpiRendered.size() != QSize(400, 200) || highDpiMagnified < 108 ||
      highDpiMagnified > 117) {
    error = QStringLiteral("Spotlight magnification changed at native DPI");
    return false;
  }
  return true;
}

bool runCreationConstraintCheck(QString &error) {
  const QPointF start(10, 10);
  const QPointF square = constrainedCreationEndpoint(
      CaptureEditor::Tool::Rectangle, start, QPointF(40, 30));
  const QPointF circle = constrainedCreationEndpoint(
      CaptureEditor::Tool::Spotlight, start, QPointF(-10, 45));
  if (square != QPointF(40, 40) || circle != QPointF(-25, 45)) {
    error = QStringLiteral("Area creation constraint did not preserve a 1:1 "
                           "bounding box");
    return false;
  }

  for (const CaptureEditor::Tool tool : {CaptureEditor::Tool::Line,
                                         CaptureEditor::Tool::Arrow}) {
    const QPointF rawEnd(40, 22);
    const QPointF snapped = constrainedCreationEndpoint(tool, start, rawEnd);
    const QPointF delta = snapped - start;
    const qreal snappedAngle = std::atan2(delta.y(), delta.x());
    constexpr qreal angleStep = std::numbers::pi_v<qreal> / 4.0;
    const qreal steps = snappedAngle / angleStep;
    if (std::abs(steps - std::round(steps)) > 0.0001 ||
        std::abs(QLineF(start, snapped).length() -
                 QLineF(start, rawEnd).length()) > 0.0001) {
      error = QStringLiteral(
          "Line creation constraint did not snap to 45 degrees");
      return false;
    }
  }

  const QPointF unchanged = constrainedCreationEndpoint(
      CaptureEditor::Tool::Freehand, start, QPointF(33, 52));
  if (unchanged != QPointF(33, 52)) {
    error = QStringLiteral("Creation constraint changed an unrelated tool");
    return false;
  }
  return true;
}
} // namespace
/** Runs the interaction and rendering smoke checks. */
int main(int argc, char **argv) {
  QApplication application(argc, argv);
  if (!loadCaptureFonts())
    return 17;
  QString snapshotError;
  if (!runTemporarySnapshotChecks(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 68;
  }
  if (!runHighlighterRenderingCheck(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 69;
  }
  if (!runSpotlightRenderingCheck(snapshotError)) {
    std::fprintf(stderr, "%s\n", qPrintable(snapshotError));
    return 71;
  }
  if (!runCreationConstraintCheck(snapshotError)) {
    std::fprintf(stderr, "%s\n", qPrintable(snapshotError));
    return 90;
  }
  const QString outputRoot =
      argc > 1 ? QString::fromLocal8Bit(argv[1])
               : QDir(QDir::tempPath())
                     .filePath(QStringLiteral("omasnap-native-smoke"));
  const QString snapshotPath = temporarySnapshotPath();
  QFile::remove(snapshotPath);
  const QString savedRoot = QDir(outputRoot).filePath(QStringLiteral("saved"));
  QDir(savedRoot).removeRecursively();
  qputenv("OMASNAP_SCREENSHOT_DIR", savedRoot.toUtf8());

  const QString nativeStableId =
      qEnvironmentVariable("OMASNAP_SMOKE_NATIVE_STABLE_ID");
  if (!nativeStableId.isEmpty()) {
    QImage nativeSurface;
    QString nativeError;
    if (!captureWindowSurface(
            {{}, nativeStableId, QStringLiteral("native smoke")}, nativeSurface,
            nativeError)) {
      qWarning().noquote() << nativeError;
      return 10;
    }
    if (!nativeSurface.save(outputRoot + QStringLiteral("-native-window.png"),
                            "PNG"))
      return 11;
    return 0;
  }

  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.workspaceId = 42;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  {
    QPainter painter(&capture.source);
    QLinearGradient gradient(0, 0, 800, 600);
    gradient.setColorAt(0, QColor(QStringLiteral("#172033")));
    gradient.setColorAt(1, QColor(QStringLiteral("#5278b5")));
    painter.fillRect(capture.source.rect(), gradient);
    painter.setPen(Qt::white);
    painter.setFont(QFont(QStringLiteral("Noto Sans"), 34, QFont::Bold));
    painter.drawText(capture.source.rect(), Qt::AlignCenter,
                     QStringLiteral("Native Qt capture editor"));
  }
  capture.preview = capture.source;
  capture.windows = {
      {{80, 80, 300, 220}, QStringLiteral("1"), QStringLiteral("first")},
      {{420, 120, 300, 320}, QStringLiteral("2"), QStringLiteral("second")}};

  {
    CaptureEditor constraintEditor(capture);
    constraintEditor.resize(800, 600);
    constraintEditor.show();
    application.processEvents();
    QTest::mousePress(&constraintEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(100, 100));
    QTest::mouseMove(&constraintEditor, QPoint(650, 470), 20);
    QTest::mouseRelease(&constraintEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(650, 470));
    application.processEvents();

    const auto expectedRectangle = [&](const QPointF &end) {
      Annotation rectangle;
      rectangle.kind = Annotation::Kind::Rectangle;
      rectangle.start = {175, 100};
      rectangle.end = end;
      rectangle.color = QColor(QStringLiteral("#ff375f"));
      rectangle.size = 4;
      return renderCapture(capture, QRectF(100, 100, 550, 370), {rectangle},
                           BackgroundStyle::None);
    };
    const auto dragRectangle = [&](bool releaseShiftBeforeMouse) {
      QTest::keyClick(&constraintEditor, Qt::Key_R);
      QTest::mousePress(&constraintEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(300, 220));
      QTest::mouseMove(&constraintEditor, QPoint(420, 280), 20);
      QTest::keyPress(&constraintEditor, Qt::Key_Shift);
      application.processEvents();
      if (releaseShiftBeforeMouse)
        QTest::keyRelease(&constraintEditor, Qt::Key_Shift);
      QTest::mouseRelease(&constraintEditor, Qt::LeftButton, Qt::NoModifier,
                          QPoint(420, 280));
      if (!releaseShiftBeforeMouse)
        QTest::keyRelease(&constraintEditor, Qt::Key_Shift);
      application.processEvents();
    };

    dragRectangle(true);
    const QImage freeExpected = expectedRectangle(QPointF(295, 160));
    if (QImage(snapshotPath).convertToFormat(freeExpected.format()) !=
        freeExpected)
      return 91;
    QTest::keyClick(&constraintEditor, Qt::Key_Z, Qt::ControlModifier);
    application.processEvents();

    dragRectangle(false);
    const QImage constrainedExpected = expectedRectangle(QPointF(295, 220));
    if (QImage(snapshotPath).convertToFormat(constrainedExpected.format()) !=
        constrainedExpected)
      return 92;
    constraintEditor.close();
  }

  {
    CaptureEditor spotlightEditor(capture);
    spotlightEditor.resize(800, 600);
    spotlightEditor.show();
    application.processEvents();
    QTest::mousePress(&spotlightEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(100, 100));
    QTest::mouseMove(&spotlightEditor, QPoint(650, 470), 20);
    QTest::mouseRelease(&spotlightEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(650, 470));
    application.processEvents();
    const QImage beforeSpotlight(snapshotPath);
    QTest::keyClick(&spotlightEditor, Qt::Key_S);
    QTest::mouseMove(&spotlightEditor, QPoint(236, 138), 20);
    application.processEvents();
    if (spotlightEditor.cursor().shape() != Qt::PointingHandCursor)
      return 81;
    QTest::mouseClick(&spotlightEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(236, 138));
    QTest::mousePress(&spotlightEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(300, 220));
    QTest::mouseMove(&spotlightEditor, QPoint(500, 380), 20);
    QTest::mouseRelease(&spotlightEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(500, 380));
    application.processEvents();
    const QImage spotlightSnapshot(snapshotPath);
    if (spotlightSnapshot.isNull() || spotlightSnapshot == beforeSpotlight)
      return 72;
    QTest::keyClick(&spotlightEditor, Qt::Key_S);
    QTest::mouseClick(&spotlightEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(236, 138));
    QTest::mousePress(&spotlightEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(160, 180));
    QTest::mouseMove(&spotlightEditor, QPoint(250, 260), 20);
    QTest::mouseRelease(&spotlightEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(250, 260));
    application.processEvents();
    if (QImage(snapshotPath) == spotlightSnapshot)
      return 87;
    QTest::keyClick(&spotlightEditor, Qt::Key_Z, Qt::ControlModifier);
    application.processEvents();
    if (QImage(snapshotPath) != spotlightSnapshot)
      return 88;
    QTest::mouseClick(&spotlightEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(400, 300));
    QTest::mouseClick(&spotlightEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(200, 138));
    application.processEvents();
    const QImage ellipseSnapshot(snapshotPath);
    if (ellipseSnapshot == spotlightSnapshot)
      return 82;
    QTest::keyClick(&spotlightEditor, Qt::Key_Z, Qt::ControlModifier);
    application.processEvents();
    if (QImage(snapshotPath) != spotlightSnapshot)
      return 83;
    QTest::keyClick(&spotlightEditor, Qt::Key_Y, Qt::ControlModifier);
    application.processEvents();
    if (QImage(snapshotPath) != ellipseSnapshot)
      return 84;
    QTest::mouseClick(&spotlightEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(400, 300));
    QTest::mouseClick(&spotlightEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(272, 138));
    application.processEvents();
    if (QImage(snapshotPath) == ellipseSnapshot)
      return 85;
    QTest::keyClick(&spotlightEditor, Qt::Key_Z, Qt::ControlModifier);
    QTest::keyClick(&spotlightEditor, Qt::Key_Z, Qt::ControlModifier);
    application.processEvents();
    if (QImage(snapshotPath) != spotlightSnapshot)
      return 86;
    QWheelEvent magnifyWheel(QPointF(400, 300), QPointF(400, 300), {},
                             {0, 120}, Qt::NoButton, Qt::NoModifier,
                             Qt::NoScrollPhase, false);
    QApplication::sendEvent(&spotlightEditor, &magnifyWheel);
    application.processEvents();
    if (QImage(snapshotPath) == spotlightSnapshot)
      return 73;
    QTest::keyClick(&spotlightEditor, Qt::Key_Z, Qt::ControlModifier);
    application.processEvents();
    if (QImage(snapshotPath) != spotlightSnapshot)
      return 74;
    QTest::mousePress(&spotlightEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(500, 380));
    QTest::mouseMove(&spotlightEditor, QPoint(530, 400), 20);
    QTest::mouseRelease(&spotlightEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(530, 400));
    application.processEvents();
    if (QImage(snapshotPath) == spotlightSnapshot)
      return 75;
    QTest::keyClick(&spotlightEditor, Qt::Key_Z, Qt::ControlModifier);
    application.processEvents();
    if (QImage(snapshotPath) != spotlightSnapshot)
      return 76;
    QTest::mousePress(&spotlightEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(400, 300));
    QTest::mouseMove(&spotlightEditor, QPoint(420, 315), 20);
    QTest::mouseRelease(&spotlightEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(420, 315));
    application.processEvents();
    if (QImage(snapshotPath) == spotlightSnapshot)
      return 77;
    QTest::keyClick(&spotlightEditor, Qt::Key_Z, Qt::ControlModifier);
    application.processEvents();
    if (QImage(snapshotPath) != spotlightSnapshot)
      return 78;
    QTest::keyClick(&spotlightEditor, Qt::Key_Delete);
    application.processEvents();
    if (QImage(snapshotPath) != beforeSpotlight)
      return 79;
    spotlightEditor.close();
  }
  {
    CaptureEditor narrowEditor(capture);
    narrowEditor.resize(680, 600);
    narrowEditor.show();
    application.processEvents();
    QTest::keyClick(&narrowEditor, Qt::Key_A, Qt::ControlModifier);
    QTest::mouseMove(&narrowEditor, QPoint(670, 46), 20);
    application.processEvents();
    if (narrowEditor.cursor().shape() != Qt::PointingHandCursor)
      return 80;
    narrowEditor.close();
  }

  CaptureEditor editor(capture);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();
  QTest::keyClick(&editor, Qt::Key_Space);
  QTest::mouseMove(&editor, QPoint(200, 160), 20);
  application.processEvents();
  const QImage hoverUi = editor.grab().toImage();
  if (hoverUi.pixelColor(200, 160) != capture.preview.pixelColor(200, 160))
    return 7;
  QTest::keyClick(&editor, Qt::Key_Right, Qt::MetaModifier);
  application.processEvents();
  const QImage keyboardWindowUi = editor.grab().toImage();
  if (keyboardWindowUi.pixelColor(500, 200) !=
          capture.preview.pixelColor(500, 200) ||
      keyboardWindowUi.pixelColor(200, 160) ==
          capture.preview.pixelColor(200, 160))
    return 8;
  QTest::keyClick(&editor, Qt::Key_Space);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
  QTest::mouseMove(&editor, QPoint(650, 470), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(650, 470));
  application.processEvents();
  const QFileInfo initialSnapshotInfo(snapshotPath);
  const QImage initialSnapshot(snapshotPath);
  if (!initialSnapshotInfo.exists() || initialSnapshotInfo.size() <= 0 ||
      initialSnapshot.isNull())
    return 37;
  if (editor.cursor().shape() != Qt::ArrowCursor ||
      !editor.grab().save(outputRoot + QStringLiteral("-selector-default.png"),
                          "PNG"))
    return 29;
  const QImage editBackdropUi = editor.grab().toImage();
  const QColor backdropCorner = editBackdropUi.pixelColor(5, 5);
  if (backdropCorner.alpha() != 160 || backdropCorner.red() != 0 ||
      backdropCorner.green() != 0 || backdropCorner.blue() != 0)
    return 9;
  QTest::keyClick(&editor, Qt::Key_A);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(230, 250));
  QTest::mouseMove(&editor, QPoint(570, 350), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(570, 350));
  application.processEvents();
  if (editor.cursor().shape() != Qt::ArrowCursor)
    return 26;
  const QImage arrowSnapshot(snapshotPath);
  if (arrowSnapshot.isNull() || arrowSnapshot == initialSnapshot)
    return 38;
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(400, 300));
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(400, 300));
  QTest::mouseMove(&editor, QPoint(420, 315), 20);
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(420, 315));
  application.processEvents();
  if (QImage(snapshotPath) != initialSnapshot)
    return 56;
  QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
  application.processEvents();
  if (QImage(snapshotPath) != arrowSnapshot)
    return 57;
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(140, 140));
  application.processEvents();
  QTest::keyClick(&editor, Qt::Key_Delete);
  application.processEvents();
  if (QImage(snapshotPath) != arrowSnapshot)
    return 39;
  QTest::keyClick(&editor, Qt::Key_L);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(260, 210));
  QTest::mouseMove(&editor, QPoint(520, 265), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(520, 265));
  application.processEvents();
  if (editor.cursor().shape() != Qt::ArrowCursor)
    return 27;
  const QImage lineSnapshot(snapshotPath);
  if (lineSnapshot.isNull() || lineSnapshot == arrowSnapshot)
    return 40;
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (QImage(snapshotPath) != arrowSnapshot)
    return 41;
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
  application.processEvents();
  if (QImage(snapshotPath) != lineSnapshot)
    return 42;

  const QImage lineUnselectedUi = editor.grab().toImage();
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(390, 238));
  application.processEvents();
  const QImage lineSelectedUi = editor.grab().toImage();
  if (lineSelectedUi.pixelColor(260, 210) !=
          QColor(QStringLiteral("#0a84ff")) ||
      lineSelectedUi.pixelColor(520, 265) != QColor(QStringLiteral("#0a84ff")))
    return 43;
  for (int x = 280; x <= 500; ++x) {
    if (lineSelectedUi.pixelColor(x, 206) !=
        lineUnselectedUi.pixelColor(x, 206))
      return 44;
  }

  const QImage beforeSelectorZoom =
      editor.grab().toImage().copy(QRect(100, 150, 600, 350));
  QWheelEvent selectorZoom(QPointF(520, 265), QPointF(520, 265), {}, {0, 120},
                           Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase,
                           false);
  QApplication::sendEvent(&editor, &selectorZoom);
  application.processEvents();
  const QImage afterSelectorZoom =
      editor.grab().toImage().copy(QRect(100, 150, 600, 350));
  if (beforeSelectorZoom == afterSelectorZoom)
    return 30;
  const QImage scaledLineSnapshot(snapshotPath);
  if (scaledLineSnapshot.isNull() || scaledLineSnapshot == lineSnapshot)
    return 45;
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (QImage(snapshotPath) != lineSnapshot)
    return 46;
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
  application.processEvents();
  if (QImage(snapshotPath) != scaledLineSnapshot)
    return 47;
  const QImage beforeFreehandSnapshot(snapshotPath);
  QTest::keyClick(&editor, Qt::Key_F);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(270, 390));
  QTest::mouseMove(&editor, QPoint(330, 360), 10);
  QTest::mouseMove(&editor, QPoint(390, 410), 10);
  QTest::mouseMove(&editor, QPoint(460, 370), 10);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(530, 420));
  application.processEvents();
  if (editor.cursor().shape() != Qt::ArrowCursor ||
      QImage(snapshotPath) == beforeFreehandSnapshot)
    return 28;
  const QImage beforeMarkerSnapshot(snapshotPath);
  QTest::keyClick(&editor, Qt::Key_2);
  QTest::keyClick(&editor, Qt::Key_C);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(470, 300));
  application.processEvents();
  if (QImage(snapshotPath) == beforeMarkerSnapshot)
    return 48;
  const QImage beforeTextSnapshot(snapshotPath);
  QTest::keyClick(&editor, Qt::Key_T);
  // Text size panel under the Text toolbar button (shifted by Highlighter).
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(341, 133));
  QWheelEvent textSizeWheel(QPointF(360, 320), QPointF(360, 320), {}, {0, -120},
                            Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase,
                            false);
  QApplication::sendEvent(&editor, &textSizeWheel);
  application.processEvents();
  if (!editor.grab().save(outputRoot + QStringLiteral("-text-sizes.png"),
                          "PNG"))
    return 18;
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(360, 320));
  QTest::keyClicks(QApplication::focusWidget(), QStringLiteral("Inline text"));
  application.processEvents();
  if (!editor.grab().save(outputRoot + QStringLiteral("-text-inline.png"),
                          "PNG"))
    return 19;
  QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
  application.processEvents();
  if (!editor.grab().save(outputRoot + QStringLiteral("-text-committed.png"),
                          "PNG") ||
      QImage(snapshotPath) == beforeTextSnapshot)
    return 20;
  const QImage beforeMoveSnapshot(snapshotPath);
  QTest::keyClick(&editor, Qt::Key_V);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(400, 300));
  QTest::mouseMove(&editor, QPoint(420, 315), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(420, 315));
  application.processEvents();
  const QImage movedSnapshot(snapshotPath);
  if (movedSnapshot.isNull() || movedSnapshot == beforeMoveSnapshot)
    return 49;
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (QImage(snapshotPath) != beforeMoveSnapshot)
    return 50;
  QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
  application.processEvents();
  if (QImage(snapshotPath) != movedSnapshot)
    return 51;
  const QImage beforeEndpointSnapshot(snapshotPath);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(590, 365));
  QTest::mouseMove(&editor, QPoint(620, 380), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(620, 380));
  application.processEvents();
  const QImage resizedSnapshot(snapshotPath);
  if (resizedSnapshot.isNull() || resizedSnapshot == beforeEndpointSnapshot)
    return 52;
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (QImage(snapshotPath) != beforeEndpointSnapshot)
    return 53;
  QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
  application.processEvents();
  if (QImage(snapshotPath) != resizedSnapshot)
    return 54;
  if (!editor.grab().save(outputRoot + QStringLiteral("-vector-selected.png"),
                          "PNG"))
    return 21;
  QTest::mouseDClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(380, 330));
  if (QApplication::focusWidget() != nullptr &&
      QApplication::focusWidget() != &editor) {
    QTest::keyClicks(QApplication::focusWidget(),
                     QStringLiteral("Edited Neucha"));
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
  } else {
    return 22;
  }
  const QImage beforeBackdropSnapshot(snapshotPath);
  QTest::keyClick(&editor, Qt::Key_B);
  application.processEvents();
  if (QImage(snapshotPath) == beforeBackdropSnapshot)
    return 55;
  // Palette toolbar button (index 9 after Spotlight was inserted).
  QTest::mouseMove(&editor, QPoint(418, 92), 20);
  application.processEvents();
  if (!editor.grab().save(outputRoot + QStringLiteral("-palette.png"), "PNG"))
    return 14;
  // Custom color control in the open palette strip.
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(500, 134));
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(360, 200));
  // Freehand toolbar button.
  QTest::mouseMove(&editor, QPoint(180, 92), 20);
  application.processEvents();
  if (editor.cursor().shape() != Qt::PointingHandCursor)
    return 12;
  if (!editor.grab().save(outputRoot + QStringLiteral("-ui.png"), "PNG") ||
      !hoverUi.save(outputRoot + QStringLiteral("-window-hover.png"), "PNG") ||
      !keyboardWindowUi.save(
          outputRoot + QStringLiteral("-window-keyboard.png"), "PNG"))
    return 2;

  CaptureEditor fullscreenEditor(capture,
                                 CaptureEditor::CaptureMode::Fullscreen);
  fullscreenEditor.resize(800, 600);
  fullscreenEditor.show();
  application.processEvents();
  CaptureEditor windowModeEditor(capture, CaptureEditor::CaptureMode::Window);
  windowModeEditor.resize(800, 600);
  windowModeEditor.show();
  application.processEvents();
  if (!windowModeEditor.grab().save(
          outputRoot + QStringLiteral("-window-mode.png"), "PNG"))
    return 36;

  CaptureData failedWindowCapture = capture;
  failedWindowCapture.windows = {{{80, 80, 300, 220},
                                  QStringLiteral("missing-stable-id"),
                                  QStringLiteral("missing")}};
  CaptureEditor failedWindowEditor(failedWindowCapture,
                                   CaptureEditor::CaptureMode::Window);
  failedWindowEditor.resize(800, 600);
  failedWindowEditor.show();
  QTest::mouseMove(&failedWindowEditor, QPoint(200, 160), 20);
  QTest::keyClick(&failedWindowEditor, Qt::Key_Return);
  application.processEvents();
  if (failedWindowEditor.cursor().shape() != Qt::PointingHandCursor ||
      !failedWindowEditor.grab().save(
          outputRoot + QStringLiteral("-window-capture-failed.png"), "PNG"))
    return 63;

  CaptureData nativePreviewCapture = capture;
  nativePreviewCapture.monitor.scale = 2.0;
  nativePreviewCapture.monitor.pixelSize = {1600, 1200};
  nativePreviewCapture.source = QImage(1600, 1200, QImage::Format_RGB32);
  nativePreviewCapture.source.fill(QColor(QStringLiteral("#123456")));
  nativePreviewCapture.preview = QImage(800, 600, QImage::Format_RGB32);
  nativePreviewCapture.preview.fill(QColor(QStringLiteral("#ff00ff")));
  CaptureEditor nativePreviewEditor(nativePreviewCapture,
                                    CaptureEditor::CaptureMode::Fullscreen);
  nativePreviewEditor.resize(800, 600);
  nativePreviewEditor.show();
  application.processEvents();
  const QImage nativePreviewUi = nativePreviewEditor.grab().toImage();
  if (nativePreviewUi.pixelColor(400, 300) !=
          QColor(QStringLiteral("#123456")) ||
      !nativePreviewUi.save(outputRoot + QStringLiteral("-native-preview.png"),
                            "PNG"))
    return 23;
  if (!fullscreenEditor.grab().save(
          outputRoot + QStringLiteral("-fullscreen-editor.png"), "PNG"))
    return 16;

  // File mode opens an existing image straight into the edit phase: whole
  // image selected, arrow cursor, annotation canvas showing the pixels.
  CaptureData fileData;
  fileData.monitor.scale = 1.0;
  fileData.monitor.pixelSize = {300, 200};
  fileData.source = QImage(300, 200, QImage::Format_RGB32);
  fileData.source.fill(QColor(QStringLiteral("#1a2b3c")));
  fileData.preview = fileData.source;
  CaptureEditor fileEditor(fileData, CaptureEditor::CaptureMode::File);
  fileEditor.resize(800, 600);
  fileEditor.show();
  application.processEvents();
  const QImage fileUi = fileEditor.grab().toImage();
  if (fileEditor.cursor().shape() != Qt::ArrowCursor ||
      fileUi.pixelColor(400, 305) != QColor(QStringLiteral("#1a2b3c")) ||
      !fileUi.save(outputRoot + QStringLiteral("-file-mode.png"), "PNG"))
    return 70;

  CaptureData windowSurfaceCapture = capture;
  windowSurfaceCapture.monitor.scale = 1.0;
  windowSurfaceCapture.monitor.pixelSize = {320, 180};
  windowSurfaceCapture.source =
      QImage(320, 180, QImage::Format_ARGB32_Premultiplied);
  windowSurfaceCapture.source.fill(QColor(QStringLiteral("#d12f45")));
  windowSurfaceCapture.preview = windowSurfaceCapture.source;
  CaptureEditor windowSurfaceEditor(windowSurfaceCapture,
                                    CaptureEditor::CaptureMode::Fullscreen);
  windowSurfaceEditor.resize(800, 600);
  windowSurfaceEditor.show();
  application.processEvents();
  const QImage windowSurfaceUi = windowSurfaceEditor.grab().toImage();
  const QColor windowSurfaceCorner = windowSurfaceUi.pixelColor(5, 5);
  if (windowSurfaceCorner.alpha() != 160 || windowSurfaceCorner.red() != 0 ||
      windowSurfaceCorner.green() != 0 || windowSurfaceCorner.blue() != 0 ||
      windowSurfaceUi.pixelColor(400, 300) !=
          QColor(QStringLiteral("#d12f45")) ||
      !windowSurfaceUi.save(
          outputRoot + QStringLiteral("-window-surface-editor.png"), "PNG"))
    return 62;

  CaptureEditor cropEditor(capture);
  cropEditor.resize(800, 600);
  cropEditor.show();
  application.processEvents();
  QTest::mousePress(&cropEditor, Qt::LeftButton, Qt::NoModifier,
                    QPoint(100, 100));
  QTest::mouseMove(&cropEditor, QPoint(650, 470), 20);
  QTest::mouseRelease(&cropEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(650, 470));
  application.processEvents();
  QTest::keyClick(&cropEditor, Qt::Key_L);
  QTest::mousePress(&cropEditor, Qt::LeftButton, Qt::NoModifier,
                    QPoint(260, 210));
  QTest::mouseMove(&cropEditor, QPoint(520, 265), 20);
  QTest::mouseRelease(&cropEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(520, 265));
  QTest::mouseClick(&cropEditor, Qt::LeftButton, Qt::NoModifier,
                    QPoint(390, 238));
  application.processEvents();
  const QImage beforeCrop = cropEditor.grab().toImage();
  if (!beforeCrop.save(outputRoot + QStringLiteral("-crop-handles.png"), "PNG"))
    return 31;
  QTest::mouseMove(&cropEditor, QPoint(682, 497), 20);
  QTest::mousePress(&cropEditor, Qt::LeftButton, Qt::NoModifier,
                    QPoint(682, 497));
  QTest::mouseMove(&cropEditor, QPoint(620, 440), 20);
  QTest::mouseRelease(&cropEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(620, 440));
  application.processEvents();
  const QImage afterCrop = cropEditor.grab().toImage();
  if (beforeCrop == afterCrop ||
      !afterCrop.save(outputRoot + QStringLiteral("-cropped.png"), "PNG"))
    return 32;
  QTest::keyClick(&cropEditor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (cropEditor.grab().toImage().pixelColor(260, 210) !=
      QColor(QStringLiteral("#0a84ff")))
    return 58;

  CaptureEditor previewClipEditor(capture);
  previewClipEditor.resize(800, 600);
  previewClipEditor.show();
  application.processEvents();
  QTest::mousePress(&previewClipEditor, Qt::LeftButton, Qt::NoModifier,
                    QPoint(100, 100));
  QTest::mouseMove(&previewClipEditor, QPoint(650, 470), 20);
  QTest::mouseRelease(&previewClipEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(650, 470));
  QTest::keyClick(&previewClipEditor, Qt::Key_C);
  QTest::mouseClick(&previewClipEditor, Qt::LeftButton, Qt::NoModifier,
                    QPoint(640, 300));
  QTest::keyClick(&previewClipEditor, Qt::Key_B);
  QTest::mousePress(&previewClipEditor, Qt::LeftButton, Qt::NoModifier,
                    QPoint(682, 305));
  QTest::mouseMove(&previewClipEditor, QPoint(500, 305), 20);
  QTest::mouseRelease(&previewClipEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(500, 305));
  application.processEvents();
  const QImage clippedPreview = previewClipEditor.grab().toImage();
  for (int y = 270; y <= 330; ++y) {
    for (int x = 690; x <= 760; ++x) {
      const QColor pixel = clippedPreview.pixelColor(x, y);
      if (pixel.red() > 240 && pixel.green() < 96 && pixel.blue() < 128)
        return 66;
    }
  }

  QVector<Annotation> annotations;
  annotations.push_back({Annotation::Kind::Arrow,
                         {50, 60},
                         {390, 150},
                         {},
                         QColor(QStringLiteral("#ff375f")),
                         5,
                         0,
                         {}});
  annotations.push_back({Annotation::Kind::Line,
                         {40, 260},
                         {430, 250},
                         {},
                         QColor(QStringLiteral("#bf5af2")),
                         4,
                         0,
                         {}});
  Annotation freehand;
  freehand.kind = Annotation::Kind::Freehand;
  freehand.color = QColor(QStringLiteral("#ffd60a"));
  freehand.size = 4;
  freehand.points = {{40, 170}, {110, 145}, {165, 185}, {225, 150}};
  annotations.push_back(std::move(freehand));
  Annotation highlighter;
  highlighter.kind = Annotation::Kind::Highlighter;
  highlighter.color = QColor(QStringLiteral("#ffd60a"));
  highlighter.size = 5;
  highlighter.points = {{50, 210}, {140, 205}, {220, 215}, {300, 200}};
  annotations.push_back(std::move(highlighter));
  annotations.push_back({Annotation::Kind::Marker,
                         {260, 180},
                         {},
                         {},
                         QColor(QStringLiteral("#ff9f0a")),
                         5,
                         1,
                         {}});
  annotations.push_back({Annotation::Kind::Rectangle,
                         {30, 30},
                         {450, 230},
                         {},
                         QColor(QStringLiteral("#30d158")),
                         4,
                         0,
                         {}});
  annotations.push_back({Annotation::Kind::Text,
                         {80, 210},
                         {},
                         QStringLiteral("Qt C++"),
                         QColor(QStringLiteral("#0a84ff")),
                         4,
                         0,
                         {}});
  const QImage rendered = renderCapture(capture, QRectF(100, 100, 500, 300),
                                        annotations, BackgroundStyle::Aurora);
  if (rendered.isNull() || rendered.size() != QSize(628, 428) ||
      !rendered.save(outputRoot + QStringLiteral("-render.png"), "PNG"))
    return 3;

  CaptureData clippingCapture;
  clippingCapture.monitor.scale = 1.0;
  clippingCapture.source = QImage(100, 100, QImage::Format_RGB32);
  clippingCapture.source.fill(Qt::white);
  clippingCapture.preview = clippingCapture.source;
  Annotation croppedOut;
  croppedOut.kind = Annotation::Kind::Rectangle;
  croppedOut.start = {120, 20};
  croppedOut.end = {150, 60};
  croppedOut.color = QColor(QStringLiteral("#ff00ff"));
  croppedOut.size = 4;
  const QImage clippedExport =
      renderCapture(clippingCapture, QRectF(0, 0, 100, 100), {croppedOut},
                    BackgroundStyle::Aurora);
  const QRect exportedImageBounds(64, 64, 100, 100);
  for (int y = 0; y < clippedExport.height(); ++y) {
    for (int x = 0; x < clippedExport.width(); ++x) {
      if (exportedImageBounds.contains(x, y))
        continue;
      const QColor pixel = clippedExport.pixelColor(x, y);
      if (pixel.red() > 240 && pixel.green() < 32 && pixel.blue() > 240)
        return 65;
    }
  }

  Annotation edgeSpotlight;
  edgeSpotlight.kind = Annotation::Kind::Spotlight;
  edgeSpotlight.start = {0, 0};
  edgeSpotlight.end = {40, 40};
  edgeSpotlight.color = QColor(QStringLiteral("#ff00ff"));
  edgeSpotlight.size = 4;
  edgeSpotlight.magnification = 2;
  edgeSpotlight.spotlightShape = SpotlightShape::Rectangle;
  const QImage roundedBaseline =
      renderCapture(clippingCapture, QRectF(0, 0, 100, 100), {},
                    BackgroundStyle::Aurora);
  const QImage roundedSpotlight = renderCapture(
      clippingCapture, QRectF(0, 0, 100, 100), {edgeSpotlight},
      BackgroundStyle::Aurora);
  if (roundedSpotlight.pixelColor(66, 66) !=
      roundedBaseline.pixelColor(66, 66))
    return 89;

  CaptureData highDpiCapture = capture;
  highDpiCapture.monitor.scale = 2.0;
  highDpiCapture.monitor.pixelSize = {1600, 1200};
  highDpiCapture.source = capture.source.scaled(
      1500, 1125, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  const QImage highDpiRendered = renderCapture(
      highDpiCapture, QRectF(100, 100, 500, 300), {}, BackgroundStyle::None);
  if (highDpiRendered.size() != QSize(1000, 600))
    return 13;
  const QImage fullHighDpi = renderCapture(
      highDpiCapture, QRectF(0, 0, 800, 600), {}, BackgroundStyle::None);
  if (fullHighDpi.size() != QSize(1600, 1200) ||
      !fullHighDpi.save(outputRoot + QStringLiteral("-fullscreen-hidpi.png"),
                        "PNG"))
    return 15;

  QImage ocrImage(1000, 260, QImage::Format_RGB32);
  ocrImage.fill(Qt::white);
  {
    QPainter painter(&ocrImage);
    painter.setPen(Qt::black);
    painter.setFont(QFont(QStringLiteral("Noto Sans"), 64, QFont::Bold));
    painter.drawText(ocrImage.rect(), Qt::AlignCenter,
                     QStringLiteral("OCR smoke test 42"));
  }
  QString ocrError;
  if (!recognizeText(ocrImage, ocrError)
           .contains(QStringLiteral("OCR smoke test 42")))
    return 5;
  const QByteArray savedOmasnapLangs = qgetenv("OMASNAP_OCR_LANGS");
  const QByteArray savedOmarchyLangs = qgetenv("OMARCHY_OCR_LANGS");
  qputenv("OMASNAP_OCR_LANGS", "");
  qputenv("OMARCHY_OCR_LANGS", "eng");
  const QString fallbackOcr = recognizeText(ocrImage, ocrError);
  if (savedOmasnapLangs.isEmpty())
    qunsetenv("OMASNAP_OCR_LANGS");
  else
    qputenv("OMASNAP_OCR_LANGS", savedOmasnapLangs);
  if (savedOmarchyLangs.isEmpty())
    qunsetenv("OMARCHY_OCR_LANGS");
  else
    qputenv("OMARCHY_OCR_LANGS", savedOmarchyLangs);
  if (!fallbackOcr.contains(QStringLiteral("OCR smoke test 42")))
    return 65;
  QTest::mouseMove(&editor, QPoint(400, 300), 20);
  QTest::keyClick(&editor, Qt::Key_T);
  QTest::keyClick(&editor, Qt::Key_Escape);
  application.processEvents();
  if (!editor.isVisible() || editor.cursor().shape() != Qt::ArrowCursor)
    return 24;
  QTest::keyClick(&editor, Qt::Key_Escape);
  application.processEvents();
  if (editor.isVisible())
    return 25;

  QString savedPath;
  {
    CaptureEditor finishEditor(capture);
    finishEditor.resize(800, 600);
    finishEditor.show();
    application.processEvents();
    QTest::mousePress(&finishEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(100, 100));
    QTest::mouseMove(&finishEditor, QPoint(650, 470), 20);
    QTest::mouseRelease(&finishEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(650, 470));
    QTest::keyClick(&finishEditor, Qt::Key_C);
    QTest::mouseClick(&finishEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(470, 300));
    application.processEvents();
    const QImage snapshotBeforeSave(snapshotPath);
    if (snapshotBeforeSave.isNull())
      return 59;
    QTest::keyClick(&finishEditor, Qt::Key_S, Qt::ControlModifier);
    application.processEvents();
    const QStringList savedFiles =
        QDir(QDir(outputRoot).filePath(QStringLiteral("saved")))
            .entryList({QStringLiteral("*.png")}, QDir::Files);
    if (finishEditor.isVisible() || savedFiles.size() != 1)
      return 60;
    savedPath = QDir(QDir(outputRoot).filePath(QStringLiteral("saved")))
                    .filePath(savedFiles.constFirst());
    if (QImage(savedPath) != snapshotBeforeSave || QFile::exists(snapshotPath))
      return 61;
  }
  if (!QFile::exists(savedPath))
    return 70;
  if (qEnvironmentVariableIsSet("OMASNAP_SMOKE_COPY")) {
    QString clipboardError;
    if (!copyPngFileToClipboard(savedPath, clipboardError)) {
      qWarning().noquote() << clipboardError;
      return 64;
    }
  }

  QString transformError;
  if (!runTransformSmoke(transformError)) {
    qWarning().noquote() << transformError;
    return 67;
  }
  return 0;
}
