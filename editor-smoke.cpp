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
#include <QTemporaryDir>
#include <QWheelEvent>
#include <QtTest/QTest>

#include <algorithm>

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

bool runSecureRedactionChecks(QString &error) {
  error = QStringLiteral("Secure redaction rendering check failed");
  CaptureData capture;
  capture.monitor.scale = 1.0;
  capture.monitor.pixelSize = {96, 72};
  capture.source = QImage(96, 72, QImage::Format_RGB32);
  for (int y = 0; y < capture.source.height(); ++y) {
    for (int x = 0; x < capture.source.width(); ++x) {
      capture.source.setPixelColor(
          x, y, QColor((x * 3) % 256, (y * 5) % 256, ((x + y) * 7) % 256));
    }
  }
  capture.preview = capture.source;

  Annotation pixelate;
  pixelate.kind = Annotation::Kind::Redaction;
  pixelate.start = {12, 12};
  pixelate.end = {84, 60};
  pixelate.redactionStyle = RedactionStyle::Pixelate;
  pixelate.redactionSeed = 0x1234abcdU;
  const QRect redactionBounds(12, 12, 72, 48);
  const QImage first = renderCapture(capture, QRectF(0, 0, 96, 72), {pixelate},
                                     BackgroundStyle::None);
  const QImage repeated = renderCapture(capture, QRectF(0, 0, 96, 72),
                                        {pixelate}, BackgroundStyle::None);
  if (first.isNull() || first != repeated ||
      first.pixelColor(2, 2) != capture.source.pixelColor(2, 2))
    return false;

  // Every exported mosaic cell is a flat synthetic color. No fine-grained
  // source pixels survive inside a cell.
  constexpr int blockSize = 12;
  for (int top = redactionBounds.top(); top <= redactionBounds.bottom();
       top += blockSize) {
    for (int left = redactionBounds.left(); left <= redactionBounds.right();
         left += blockSize) {
      const QColor cell = first.pixelColor(left, top);
      if (cell.alpha() != 255)
        return false;
      const int bottom =
          std::min(top + blockSize - 1, redactionBounds.bottom());
      const int right = std::min(left + blockSize - 1, redactionBounds.right());
      for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
          if (first.pixelColor(x, y) != cell)
            return false;
        }
      }
    }
  }

  // Mirroring the source preserves the region's aggregate palette but changes
  // every spatial relationship. Identical redaction output proves the mosaic
  // does not retain source-block positions.
  CaptureData mirroredCapture = capture;
  for (int y = 0; y < capture.source.height(); ++y) {
    for (int x = 0; x < capture.source.width(); ++x) {
      mirroredCapture.source.setPixelColor(
          x, y, capture.source.pixelColor(capture.source.width() - x - 1, y));
    }
  }
  mirroredCapture.preview = mirroredCapture.source;
  const QImage mirrored = renderCapture(mirroredCapture, QRectF(0, 0, 96, 72),
                                        {pixelate}, BackgroundStyle::None);
  if (first.copy(redactionBounds) != mirrored.copy(redactionBounds))
    return false;

  Annotation differentSeed = pixelate;
  ++differentSeed.redactionSeed;
  const QImage reseeded = renderCapture(capture, QRectF(0, 0, 96, 72),
                                        {differentSeed}, BackgroundStyle::None);
  if (first.copy(redactionBounds) == reseeded.copy(redactionBounds))
    return false;

  Annotation solid = pixelate;
  solid.redactionStyle = RedactionStyle::Solid;
  const QImage solidOutput = renderCapture(capture, QRectF(0, 0, 96, 72),
                                           {solid}, BackgroundStyle::None);
  const QColor solidColor(QStringLiteral("#121216"));
  for (int y = redactionBounds.top(); y <= redactionBounds.bottom(); ++y) {
    for (int x = redactionBounds.left(); x <= redactionBounds.right(); ++x) {
      if (solidOutput.pixelColor(x, y) != solidColor)
        return false;
    }
  }

  Annotation line;
  line.kind = Annotation::Kind::Line;
  line.start = {12, 36};
  line.end = {83, 36};
  line.color = Qt::white;
  line.size = 4;
  const QImage annotated = renderCapture(capture, QRectF(0, 0, 96, 72),
                                         {solid, line}, BackgroundStyle::None);
  if (annotated.pixelColor(48, 36) != QColor(Qt::white))
    return false;

  Annotation clipped = solid;
  clipped.start = {-10, -10};
  clipped.end = {20, 20};
  const QImage clippedOutput = renderCapture(capture, QRectF(0, 0, 96, 72),
                                             {clipped}, BackgroundStyle::None);
  if (clippedOutput.pixelColor(0, 0) != solidColor ||
      clippedOutput.pixelColor(21, 21) != capture.source.pixelColor(21, 21))
    return false;

  CaptureData highDpi = capture;
  highDpi.monitor.scale = 2.0;
  highDpi.monitor.pixelSize = {192, 144};
  highDpi.source = capture.source.scaled(192, 144, Qt::IgnoreAspectRatio,
                                         Qt::SmoothTransformation);
  highDpi.preview = capture.source;
  Annotation highDpiSolid = solid;
  highDpiSolid.start = {10, 10};
  highDpiSolid.end = {30, 30};
  const QImage highDpiOutput = renderCapture(
      highDpi, QRectF(0, 0, 96, 72), {highDpiSolid}, BackgroundStyle::None);
  if (highDpiOutput.size() != QSize(192, 144) ||
      highDpiOutput.pixelColor(20, 20) != solidColor ||
      highDpiOutput.pixelColor(59, 59) != solidColor ||
      highDpiOutput.pixelColor(61, 61) == solidColor)
    return false;

  // Fractional crop rounding can force a one-pixel smooth resize. Redact the
  // native source first so protected colors cannot bleed just outside the
  // final mask through the resampling kernel.
  CaptureData fractional;
  fractional.monitor.scale = 1.25;
  fractional.preview = QImage(100, 100, QImage::Format_RGB32);
  fractional.preview.fill(Qt::white);
  fractional.source = QImage(125, 125, QImage::Format_RGB32);
  fractional.source.fill(Qt::white);
  for (int y = 13; y <= 38; ++y) {
    for (int x = 13; x <= 38; ++x)
      fractional.source.setPixelColor(x, y, Qt::red);
  }
  Annotation fractionalSolid;
  fractionalSolid.kind = Annotation::Kind::Redaction;
  fractionalSolid.redactionStyle = RedactionStyle::Solid;
  fractionalSolid.start = {10, 10};
  fractionalSolid.end = {30, 30};
  const QImage fractionalOutput =
      renderCapture(fractional, QRectF(1, 1, 81, 81), {fractionalSolid},
                    BackgroundStyle::None);
  const QColor outsideHorizontal = fractionalOutput.pixelColor(12, 11);
  const QColor outsideVertical = fractionalOutput.pixelColor(11, 12);
  if (fractionalOutput.size() != QSize(101, 101) ||
      outsideHorizontal.red() != outsideHorizontal.green() ||
      outsideHorizontal.green() != outsideHorizontal.blue() ||
      outsideVertical.red() != outsideVertical.green() ||
      outsideVertical.green() != outsideVertical.blue() ||
      fractionalOutput.pixelColor(12, 12) != solidColor)
    return false;

  CaptureData matchingFractional;
  matchingFractional.monitor.scale = 1.25;
  matchingFractional.preview = QImage(160, 160, QImage::Format_RGB32);
  matchingFractional.preview.fill(Qt::white);
  matchingFractional.source = QImage(200, 200, QImage::Format_RGB32);
  matchingFractional.source.fill(Qt::white);
  for (int y = 13; y <= 26; ++y) {
    for (int x = 13; x <= 26; ++x)
      matchingFractional.source.setPixelColor(x, y, Qt::red);
  }
  Annotation matchingSolid;
  matchingSolid.kind = Annotation::Kind::Redaction;
  matchingSolid.redactionStyle = RedactionStyle::Solid;
  matchingSolid.start = {10.75, 10.75};
  matchingSolid.end = {20.75, 20.75};
  const QImage matchingOutput =
      renderCapture(matchingFractional, QRectF(0.08, 0.08, 79.68, 79.68),
                    {matchingSolid}, BackgroundStyle::None);
  if (matchingOutput.size() != QSize(100, 100) ||
      matchingOutput.pixelColor(26, 26) != solidColor)
    return false;

  CaptureData nonIntegralSourceScale;
  nonIntegralSourceScale.monitor.scale = 1.5;
  nonIntegralSourceScale.preview = QImage(1707, 100, QImage::Format_RGB32);
  nonIntegralSourceScale.preview.fill(Qt::white);
  nonIntegralSourceScale.source = QImage(2561, 150, QImage::Format_RGB32);
  nonIntegralSourceScale.source.fill(Qt::white);
  for (int y = 0; y < nonIntegralSourceScale.source.height(); ++y)
    nonIntegralSourceScale.source.setPixelColor(1500, y, Qt::red);
  Annotation sourceScaleSolid;
  sourceScaleSolid.kind = Annotation::Kind::Redaction;
  sourceScaleSolid.redactionStyle = RedactionStyle::Solid;
  sourceScaleSolid.start = {0, 0};
  sourceScaleSolid.end = {1000, 100};
  const QImage sourceScaleOutput =
      renderCapture(nonIntegralSourceScale, QRectF(0, 0, 1707, 100),
                    {sourceScaleSolid}, BackgroundStyle::None);
  if (sourceScaleOutput.pixelColor(1500, 50) != solidColor)
    return false;
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
  if (!runSecureRedactionChecks(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 71;
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
  // Palette toolbar button (index 8 after Highlighter was inserted).
  QTest::mouseMove(&editor, QPoint(398, 92), 20);
  application.processEvents();
  if (!editor.grab().save(outputRoot + QStringLiteral("-palette.png"), "PNG"))
    return 14;
  // Custom color control in the open palette strip.
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(470, 134));
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(320, 200));
  // Freehand toolbar button.
  QTest::mouseMove(&editor, QPoint(198, 92), 20);
  application.processEvents();
  if (editor.cursor().shape() != Qt::PointingHandCursor)
    return 12;
  if (!editor.grab().save(outputRoot + QStringLiteral("-ui.png"), "PNG") ||
      !hoverUi.save(outputRoot + QStringLiteral("-window-hover.png"), "PNG") ||
      !keyboardWindowUi.save(
          outputRoot + QStringLiteral("-window-keyboard.png"), "PNG"))
    return 2;

  {
    CaptureEditor redactionEditor(capture);
    redactionEditor.resize(800, 600);
    redactionEditor.show();
    application.processEvents();
    QTest::mousePress(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(100, 100));
    QTest::mouseMove(&redactionEditor, QPoint(650, 470), 20);
    QTest::mouseRelease(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(650, 470));
    const QImage beforeRedaction(snapshotPath);
    QTest::keyClick(&redactionEditor, Qt::Key_D);
    QTest::mousePress(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(300, 200));
    QTest::mouseMove(&redactionEditor, QPoint(420, 200), 20);
    QTest::mouseRelease(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(420, 200));
    application.processEvents();
    if (QImage(snapshotPath) != beforeRedaction)
      return 72;
    QTest::mousePress(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(300, 200));
    QTest::mouseMove(&redactionEditor, QPoint(420, 260), 20);
    QTest::mouseRelease(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(420, 260));
    application.processEvents();
    const QImage pixelatedRedaction(snapshotPath);
    if (pixelatedRedaction.isNull() || pixelatedRedaction == beforeRedaction)
      return 73;

    QTest::mouseClick(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(360, 230));
    QTest::keyClick(&redactionEditor, Qt::Key_D);
    application.processEvents();
    const QImage solidRedaction(snapshotPath);
    if (solidRedaction.isNull() || solidRedaction == pixelatedRedaction)
      return 74;
    QTest::keyClick(&redactionEditor, Qt::Key_Z, Qt::ControlModifier);
    application.processEvents();
    if (QImage(snapshotPath) != pixelatedRedaction)
      return 75;
    QTest::keyClick(&redactionEditor, Qt::Key_Y, Qt::ControlModifier);
    application.processEvents();
    if (QImage(snapshotPath) != solidRedaction)
      return 76;

    QTest::mousePress(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(360, 230));
    QTest::mouseMove(&redactionEditor, QPoint(380, 245), 20);
    QTest::mouseRelease(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(380, 245));
    application.processEvents();
    const QImage movedRedaction(snapshotPath);
    if (movedRedaction.isNull() || movedRedaction == solidRedaction)
      return 77;
    QTest::keyClick(&redactionEditor, Qt::Key_Z, Qt::ControlModifier);
    application.processEvents();
    if (QImage(snapshotPath) != solidRedaction)
      return 78;

    QTest::mousePress(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(300, 200));
    QTest::mouseMove(&redactionEditor, QPoint(280, 190), 20);
    QTest::mouseRelease(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(280, 190));
    application.processEvents();
    if (QImage(snapshotPath) == solidRedaction ||
        !redactionEditor.grab().save(
            outputRoot + QStringLiteral("-secure-redaction.png"), "PNG"))
      return 79;
    QTest::mousePress(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(280, 190));
    QTest::mouseMove(&redactionEditor, QPoint(420, 190), 20);
    QTest::mouseRelease(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(420, 190));
    application.processEvents();
    if (QImage(snapshotPath) == beforeRedaction)
      return 81;
  }

  {
    CaptureEditor overlapEditor(capture);
    overlapEditor.resize(800, 600);
    overlapEditor.show();
    application.processEvents();
    QTest::mousePress(&overlapEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(100, 100));
    QTest::mouseMove(&overlapEditor, QPoint(650, 470), 20);
    QTest::mouseRelease(&overlapEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(650, 470));
    QTest::keyClick(&overlapEditor, Qt::Key_L);
    QTest::mousePress(&overlapEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(300, 300));
    QTest::mouseMove(&overlapEditor, QPoint(500, 300), 20);
    QTest::mouseRelease(&overlapEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(500, 300));
    QTest::keyClick(&overlapEditor, Qt::Key_D);
    QTest::mousePress(&overlapEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(350, 270));
    QTest::mouseMove(&overlapEditor, QPoint(450, 330), 20);
    QTest::mouseRelease(&overlapEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(450, 330));
    QTest::mouseClick(&overlapEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(400, 300));
    application.processEvents();
    const QImage overlapUi = overlapEditor.grab().toImage();
    if (overlapUi.pixelColor(300, 300) != QColor(QStringLiteral("#0a84ff")) ||
        overlapUi.pixelColor(500, 300) != QColor(QStringLiteral("#0a84ff")) ||
        !overlapUi.save(outputRoot + QStringLiteral("-redaction-overlap.png"),
                        "PNG"))
      return 80;

    QTest::keyClick(&overlapEditor, Qt::Key_R);
    QTest::mousePress(&overlapEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(330, 250));
    QTest::mouseMove(&overlapEditor, QPoint(470, 350), 20);
    QTest::mouseRelease(&overlapEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(470, 350));
    QTest::mouseClick(&overlapEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(400, 285));
    application.processEvents();
    const QImage enclosedRedactionUi = overlapEditor.grab().toImage();
    if (enclosedRedactionUi.pixelColor(350, 270) !=
            QColor(QStringLiteral("#0a84ff")) ||
        enclosedRedactionUi.pixelColor(450, 330) !=
            QColor(QStringLiteral("#0a84ff")))
      return 84;
  }

  CaptureEditor fullscreenEditor(capture,
                                 CaptureEditor::CaptureMode::Fullscreen);
  fullscreenEditor.resize(800, 600);
  fullscreenEditor.show();
  application.processEvents();
  CaptureEditor compactToolbarEditor(capture,
                                     CaptureEditor::CaptureMode::Fullscreen);
  compactToolbarEditor.resize(720, 600);
  compactToolbarEditor.show();
  application.processEvents();
  QTest::mouseMove(&compactToolbarEditor, QPoint(20, 45), 20);
  application.processEvents();
  QTest::mouseMove(&compactToolbarEditor, QPoint(700, 45), 20);
  application.processEvents();
  const QImage compactToolbarUi = compactToolbarEditor.grab().toImage();
  if (compactToolbarUi.pixelColor(20, 45).alpha() < 240 ||
      compactToolbarUi.pixelColor(700, 45).alpha() < 240 ||
      !compactToolbarUi.save(
          outputRoot + QStringLiteral("-compact-toolbar.png"), "PNG"))
    return 83;
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
