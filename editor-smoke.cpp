#include "capture.hpp"
#include "editor.hpp"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QWheelEvent>
#include <QtTest/QTest>

int main(int argc, char **argv) {
  QApplication application(argc, argv);
  if (!loadCaptureFonts())
    return 17;
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
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(320, 133));
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
  QTest::mouseMove(&editor, QPoint(378, 92), 20);
  application.processEvents();
  if (!editor.grab().save(outputRoot + QStringLiteral("-palette.png"), "PNG"))
    return 14;
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(460, 134));
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(320, 200));
  QTest::mouseMove(&editor, QPoint(178, 92), 20);
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
  const QString savedPath =
      QDir(QDir(outputRoot).filePath(QStringLiteral("saved")))
          .filePath(savedFiles.constFirst());
  if (QImage(savedPath) != snapshotBeforeSave || QFile::exists(snapshotPath))
    return 61;
  if (qEnvironmentVariableIsSet("OMASNAP_SMOKE_COPY")) {
    QString clipboardError;
    if (!copyPngFileToClipboard(savedPath, clipboardError)) {
      qWarning().noquote() << clipboardError;
      return 64;
    }
  }
  return 0;
}
