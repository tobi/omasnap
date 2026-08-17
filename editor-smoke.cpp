/** @fileoverview Exercises capture editor behavior without a live compositor.
 */
#include "capture.hpp"
#include "cli-path.hpp"
#include "editor.hpp"
#include "instance-lock-smoke.hpp"
#include "pin-layout-smoke.hpp"
#include "pin-lifecycle-smoke.hpp"
#include "transform-smoke.hpp"
#include "eyedropper.hpp"

#include <QApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QPainter>
#include <QPixmap>
#include <QTemporaryDir>
#include <QUrl>
#include <QWheelEvent>
#include <QtTest/QTest>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <csignal>
#include <sys/resource.h>

namespace {
// Comfortably longer than the editor's snapshot debounce window.
constexpr int kSnapshotSettleMs = 400;

/** Sends one wheel notch to the editor, like a mouse scroll. */
void sendWheelNotch(QWidget &editor, int direction) {
  QWheelEvent wheel(QPointF(400, 300), QPointF(400, 300), {},
                    {0, 120 * direction}, Qt::NoButton, Qt::NoModifier,
                    Qt::NoScrollPhase, false);
  QApplication::sendEvent(&editor, &wheel);
  QApplication::processEvents();
}

/** Selects a region, draws a line and selects that line for scaling. */
void prepareSelectedLine(QWidget &editor) {
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
  QTest::mouseMove(&editor, QPoint(650, 470), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(650, 470));
  QTest::keyClick(&editor, Qt::Key_L);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(260, 210));
  QTest::mouseMove(&editor, QPoint(520, 265), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(520, 265));
  // Annotation tools stay active, so return to Select before picking the line.
  QTest::keyClick(&editor, Qt::Key_V);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(390, 238));
  QApplication::processEvents();
}

/** The cached backdrop must still show the capture inside the selection and
 *  the dimmed capture outside it. */
bool runBackdropCacheRenderingCheck(const CaptureData &capture,
                                    QString &error) {
  if (capture.source.size() != QSize(800, 600) ||
      capture.previewSize != capture.source.size()) {
    error =
        QStringLiteral("Backdrop check expects an unscaled 800x600 capture");
    return false;
  }
  CaptureEditor editor(capture);
  editor.resize(800, 600);
  editor.show();
  QApplication::processEvents();
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(150, 120));
  QTest::mouseMove(&editor, QPoint(600, 430), 20);
  QApplication::processEvents();
  const QImage ui = editor.grab().toImage();

  for (const QPoint &inside : {QPoint(300, 250), QPoint(500, 400)}) {
    if (ui.pixelColor(inside) != capture.source.pixelColor(inside)) {
      error = QStringLiteral("Selection preview no longer matches the capture");
      return false;
    }
  }
  // Outside the selection the capture stays visible under a 143/255 black
  // wash, exactly as the per-frame resample used to draw it.
  for (const QPoint &outside : {QPoint(40, 40), QPoint(720, 540)}) {
    const QColor source = capture.source.pixelColor(outside);
    const QColor dimmed = ui.pixelColor(outside);
    const auto matches = [](int actual, int expected) {
      return std::abs(actual - expected) <= 2;
    };
    if (!matches(dimmed.red(), source.red() * (255 - 143) / 255) ||
        !matches(dimmed.green(), source.green() * (255 - 143) / 255) ||
        !matches(dimmed.blue(), source.blue() * (255 - 143) / 255)) {
      error = QStringLiteral("Dimmed backdrop brightness changed");
      return false;
    }
  }
  editor.close();
  return true;
}

/** Rapid edits must coalesce into one snapshot write that still lands, and
 *  saving must output the final state. */
bool runSnapshotDebounceCheck(const CaptureData &capture,
                              const QString &snapshotPath,
                              const QString &savedRoot, QString &error) {
  QFile::remove(snapshotPath);
  CaptureEditor editor(capture);
  editor.resize(800, 600);
  editor.show();
  QApplication::processEvents();
  prepareSelectedLine(editor);
  if (!QFileInfo::exists(snapshotPath)) {
    error = QStringLiteral("Selecting an area did not write a snapshot");
    return false;
  }
  QTest::qWait(kSnapshotSettleMs);
  const QImage beforeSingle(snapshotPath);
  if (beforeSingle.isNull()) {
    error = QStringLiteral("Working snapshot was unreadable");
    return false;
  }

  sendWheelNotch(editor, 1);
  if (QImage(snapshotPath) != beforeSingle) {
    error = QStringLiteral("Wheel scaling wrote its snapshot synchronously");
    return false;
  }
  QTest::qWait(kSnapshotSettleMs);
  const QImage afterSingle(snapshotPath);
  if (afterSingle.isNull() || afterSingle == beforeSingle) {
    error = QStringLiteral("Debounced snapshot never landed");
    return false;
  }

  for (int notch = 0; notch < 5; ++notch)
    sendWheelNotch(editor, 1);
  if (QImage(snapshotPath) != afterSingle) {
    error = QStringLiteral("Burst of edits was not coalesced");
    return false;
  }
  QTest::qWait(kSnapshotSettleMs);
  const QImage afterBurst(snapshotPath);
  if (afterBurst.isNull() || afterBurst == afterSingle) {
    error = QStringLiteral("Coalesced snapshot never landed");
    return false;
  }

  const QStringList before =
      QDir(savedRoot).entryList({QStringLiteral("*.png")}, QDir::Files);
  QTest::keyClick(&editor, Qt::Key_S, Qt::ControlModifier);
  QApplication::processEvents();
  QStringList saved =
      QDir(savedRoot).entryList({QStringLiteral("*.png")}, QDir::Files);
  for (const QString &existing : before)
    saved.removeAll(existing);
  if (editor.isVisible() || saved.size() != 1) {
    error = QStringLiteral("Saving after a burst of edits produced no file");
    return false;
  }
  if (QImage(QDir(savedRoot).filePath(saved.constFirst())) != afterBurst) {
    error = QStringLiteral("Saved screenshot did not match the final state");
    return false;
  }
  return true;
}

/** The cached canvas crop must switch to the redacted preview the moment a
 *  redaction exists, and keep showing untouched pixels elsewhere. */
bool runRedactedCropCacheCheck(QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#182030")));
  {
    QPainter painter(&capture.source);
    painter.fillRect(QRect(200, 200, 100, 60),
                     QColor(QStringLiteral("#ff0000")));
  }
  capture.previewSize = capture.source.size();

  CaptureEditor editor(capture);
  editor.resize(800, 600);
  editor.show();
  QApplication::processEvents();
  // Selecting 100,100 to 650,470 draws the capture unscaled at 125,120, so the
  // secret patch covers 225,220 to 325,280 in widget coordinates.
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
  QTest::mouseMove(&editor, QPoint(650, 470), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(650, 470));
  QApplication::processEvents();

  const auto isSecret = [](const QColor &color) {
    return color.red() > 180 && color.green() < 70 && color.blue() < 70;
  };
  if (!isSecret(editor.grab().toImage().pixelColor(QPoint(275, 250)))) {
    error = QStringLiteral("Cached canvas crop did not show the capture");
    return false;
  }

  // D selects the redact tool, D again switches it to solid.
  QTest::keyClick(&editor, Qt::Key_D);
  QTest::keyClick(&editor, Qt::Key_D);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(215, 210));
  QTest::mouseMove(&editor, QPoint(335, 290), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(335, 290));
  QApplication::processEvents();
  const QImage redactedUi = editor.grab().toImage();
  if (isSecret(redactedUi.pixelColor(QPoint(275, 250)))) {
    error = QStringLiteral("Cached canvas crop leaked redacted pixels");
    return false;
  }
  if (redactedUi.pixelColor(QPoint(500, 400)) !=
      QColor(QStringLiteral("#182030"))) {
    error = QStringLiteral("Redacted crop lost the surrounding capture");
    return false;
  }
  editor.close();
  return true;
}

/** Reports repaint and working-snapshot cost on a 5K capture. Enabled with
 *  OMASNAP_SMOKE_BENCH=1; uses only public API so the same harness builds
 *  against any revision of the editor. */
void runEditorBenchmark(const QString &snapshotPath) {
  constexpr QSize nativeSize(5120, 2880);
  constexpr QSize logicalSize(2560, 1440);
  constexpr int frames = 20;

  CaptureData capture;
  capture.monitor.name = QStringLiteral("BENCH");
  capture.monitor.geometry = {0, 0, logicalSize.width(), logicalSize.height()};
  capture.monitor.pixelSize = nativeSize;
  capture.monitor.scale = 2.0;
  capture.source = QImage(nativeSize, QImage::Format_ARGB32_Premultiplied);
  {
    QPainter painter(&capture.source);
    QLinearGradient gradient(0, 0, nativeSize.width(), nativeSize.height());
    gradient.setColorAt(0, QColor(QStringLiteral("#172033")));
    gradient.setColorAt(1, QColor(QStringLiteral("#5278b5")));
    painter.fillRect(capture.source.rect(), gradient);
  }
  capture.previewSize = logicalSize;

  CaptureEditor editor(capture);
  editor.resize(logicalSize);
  editor.show();
  QApplication::processEvents();

  QPixmap target(logicalSize);
  const auto renderFrames = [&editor, &target] {
    QElapsedTimer timer;
    timer.start();
    for (int frame = 0; frame < frames; ++frame)
      editor.render(&target);
    return timer.nsecsElapsed() / 1e6 / frames;
  };

  // Select phase, mid-drag: dimmed backdrop plus the bright selection.
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(200, 200));
  QTest::mouseMove(&editor, QPoint(2300, 1300), 5);
  QApplication::processEvents();
  const double selectMs = renderFrames();

  QElapsedTimer singleWrite;
  singleWrite.start();
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(2300, 1300));
  QApplication::processEvents();
  const double enterEditMs = singleWrite.nsecsElapsed() / 1e6;

  // Edit phase: the frozen crop plus annotation layers.
  QTest::keyClick(&editor, Qt::Key_L);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(600, 500));
  QTest::mouseMove(&editor, QPoint(1400, 900), 5);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(1400, 900));
  QTest::keyClick(&editor, Qt::Key_V);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(1000, 700));
  QApplication::processEvents();
  const double editMs = renderFrames();

  // Wheel-scaling burst: how many snapshot writes land, and at what cost.
  const auto snapshotStamp = [&snapshotPath] {
    const QFileInfo info(snapshotPath);
    return QStringLiteral("%1/%2")
        .arg(info.lastModified().toMSecsSinceEpoch())
        .arg(info.size());
  };
  QString stamp = snapshotStamp();
  int burstWrites = 0;
  QElapsedTimer burst;
  burst.start();
  for (int notch = 0; notch < 10; ++notch) {
    sendWheelNotch(editor, 1);
    const QString current = snapshotStamp();
    if (current != stamp) {
      ++burstWrites;
      stamp = current;
    }
  }
  const double burstMs = burst.nsecsElapsed() / 1e6;
  QTest::qWait(kSnapshotSettleMs);
  const int settledWrites = snapshotStamp() != stamp ? 1 : 0;

  std::printf("bench.paint.select.ms_per_frame=%.2f\n", selectMs);
  std::printf("bench.paint.edit.ms_per_frame=%.2f\n", editMs);
  std::printf("bench.snapshot.single_write_ms=%.2f\n", enterEditMs);
  std::printf("bench.snapshot.burst_writes=%d\n", burstWrites);
  std::printf("bench.snapshot.burst_ms=%.2f\n", burstMs);
  std::printf("bench.snapshot.writes_after_settle=%d\n", settledWrites);
  std::fflush(stdout);
}

/** Checks that positional local image targets are recognized. */
bool runPositionalImageTargetCheck(QString &error) {
  QTemporaryDir directory;
  if (!directory.isValid()) {
    error = QStringLiteral("Could not create positional target directory");
    return false;
  }
  const QString path = QDir(directory.path())
                           .filePath(QStringLiteral("image with # marker.png"));
  QImage image(8, 8, QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::white);
  const QString url = QUrl::fromLocalFile(path).toString();
  const QString previousDirectory = QDir::currentPath();
  const QString colonName = QStringLiteral("image:one.png");
  const QString colonPath = QDir(directory.path()).filePath(colonName);
  const QString remoteLookalikeName =
      QStringLiteral("https:/example.com/image.png");
  const bool savedColonImage = image.save(colonPath, "PNG");
  const bool changedDirectory = QDir::setCurrent(directory.path());
  const QString resolvedColonPath = resolveLocalImagePath(colonName);
  const bool createdRemoteLookalike =
      changedDirectory && QDir().mkpath(QStringLiteral("https:/example.com")) &&
      image.save(remoteLookalikeName, "PNG");
  const QString resolvedRemoteUrl = createdRemoteLookalike
                                        ? resolveLocalImagePath(QStringLiteral(
                                              "https://example.com/image.png"))
                                        : QStringLiteral("setup failed");
  QDir::setCurrent(previousDirectory);
  if (!image.save(path, "PNG") || resolveLocalImagePath(path) != path ||
      resolveLocalImagePath(url) != path || !changedDirectory ||
      resolvedColonPath != colonPath || !savedColonImage ||
      !createdRemoteLookalike || !resolvedRemoteUrl.isEmpty() ||
      !resolveLocalImagePath(
           QDir(directory.path()).filePath(QStringLiteral("missing.png")))
           .isEmpty()) {
    error =
        QStringLiteral("Local file URL was not recognized as an image target");
    return false;
  }
  return true;
}

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

  QFile baselineFile(path);
  if (!baselineFile.open(QIODevice::ReadOnly)) {
    error = QStringLiteral("Could not read the valid snapshot baseline");
    return false;
  }
  const QByteArray baseline = baselineFile.readAll();
  baselineFile.close();

  QImage largeImage(512, 512, QImage::Format_ARGB32_Premultiplied);
  quint32 noise = 0x12345678U;
  for (int y = 0; y < largeImage.height(); ++y) {
    for (int x = 0; x < largeImage.width(); ++x) {
      noise ^= noise << 13;
      noise ^= noise >> 17;
      noise ^= noise << 5;
      largeImage.setPixel(x, y, 0xff000000U | (noise & 0x00ffffffU));
    }
  }

  struct rlimit originalLimit{};
  struct sigaction originalSignal{};
  struct sigaction ignoredSignal{};
  ignoredSignal.sa_handler = SIG_IGN;
  sigemptyset(&ignoredSignal.sa_mask);
  if (::getrlimit(RLIMIT_FSIZE, &originalLimit) != 0 ||
      ::sigaction(SIGXFSZ, &ignoredSignal, &originalSignal) != 0) {
    error = QStringLiteral("Could not prepare the interrupted-write check");
    return false;
  }
  struct rlimit limited = originalLimit;
  limited.rlim_cur = std::min<rlim_t>(originalLimit.rlim_cur, 1024);
  const bool limitSet = ::setrlimit(RLIMIT_FSIZE, &limited) == 0;
  QString writeError;
  const bool interruptedSave =
      limitSet && saveTemporarySnapshot(largeImage, path, writeError);
  const bool limitRestored = ::setrlimit(RLIMIT_FSIZE, &originalLimit) == 0;
  const bool signalRestored =
      ::sigaction(SIGXFSZ, &originalSignal, nullptr) == 0;

  QFile preservedFile(path);
  const bool preservedOpened = preservedFile.open(QIODevice::ReadOnly);
  const QByteArray preserved = preservedFile.readAll();
  if (!limitSet || interruptedSave || !limitRestored || !signalRestored ||
      !preservedOpened || preserved != baseline) {
    error = QStringLiteral(
        "A failed snapshot write did not preserve the previous PNG");
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
  capture.previewSize = capture.source.size();

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
  capture.previewSize = capture.source.size();

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
  mirroredCapture.previewSize = mirroredCapture.source.size();
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
  highDpi.previewSize = capture.source.size();
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
  fractional.previewSize = QSize(100, 100);
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
  matchingFractional.previewSize = QSize(160, 160);
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
  nonIntegralSourceScale.previewSize = QSize(1707, 100);
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

bool runCreationConstraintCheck(QString &error) {
  const QPointF start(10, 10);
  const QPointF square = constrainedCreationEndpoint(
      CaptureEditor::Tool::Rectangle, start, QPointF(40, 30));
  if (square != QPointF(40, 40)) {
    error = QStringLiteral(
        "Rectangle creation constraint did not preserve a 1:1 bounding box");
    return false;
  }

  for (const CaptureEditor::Tool tool :
       {CaptureEditor::Tool::Line, CaptureEditor::Tool::Arrow}) {
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

bool runQuickOutputChecks(QString &error) {
  QImage image(32, 24, QImage::Format_ARGB32_Premultiplied);
  image.fill(QColor(QStringLiteral("#345678")));
  QString outputError = QStringLiteral("unchanged");
  if (quickOutput({}, QuickOutputMode::Save, outputError) ||
      outputError == QStringLiteral("unchanged") ||
      quickOutput(image, QuickOutputMode::None, outputError)) {
    error = QStringLiteral("quickOutput accepted an invalid request");
    return false;
  }

  QTemporaryDir directory;
  if (!directory.isValid()) {
    error = QStringLiteral("Could not create quick-output directory");
    return false;
  }
  const QByteArray previousDir = qgetenv("OMASNAP_SCREENSHOT_DIR");
  qputenv("OMASNAP_SCREENSHOT_DIR", directory.path().toUtf8());
  outputError.clear();
  const bool saved = quickOutput(image, QuickOutputMode::Save, outputError);
  const QStringList files =
      QDir(directory.path()).entryList({QStringLiteral("*.png")}, QDir::Files);
  if (previousDir.isEmpty())
    qunsetenv("OMASNAP_SCREENSHOT_DIR");
  else
    qputenv("OMASNAP_SCREENSHOT_DIR", previousDir);
  if (!saved || !outputError.isEmpty() || files.size() != 1 ||
      QImage(QDir(directory.path()).filePath(files.constFirst())).isNull() ||
      QFile::exists(temporarySnapshotPath())) {
    error = QStringLiteral("quickOutput did not save exactly one PNG");
    return false;
  }
  return true;
}

bool runSpotlightAndSampleChecks(QString &error) {
  CaptureData capture;
  capture.monitor.scale = 1.0;
  capture.monitor.pixelSize = {80, 40};
  capture.source = QImage(80, 40, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#204080")));
  capture.source.setPixelColor(10, 20, QColor(QStringLiteral("#ff0000")));
  capture.previewSize = capture.source.size();

  Annotation line;
  line.kind = Annotation::Kind::Line;
  line.start = {5, 20};
  line.end = {75, 20};
  line.color = QColor(QStringLiteral("#00ff00"));
  line.size = 4;
  Annotation lens;
  lens.kind = Annotation::Kind::Spotlight;
  lens.start = {0, 0};
  lens.end = {80, 40};
  lens.magnification = 2.0;
  lens.color = Qt::white;
  const QImage rendered =
      renderCapture(capture, QRectF(0, 0, 80, 40), {line, lens},
                    BackgroundStyle::None);
  if (rendered.isNull() || rendered.pixelColor(40, 2).alpha() < 100) {
    error = QStringLiteral("Spotlight did not dim the surrounding image");
    return false;
  }

  QImage checker(8, 4, QImage::Format_ARGB32);
  checker.fill(QColor(255, 0, 0));
  checker.setPixelColor(0, 0, QColor(0, 255, 0));
  checker.setPixelColor(7, 0, QColor(0, 0, 255));
  checker.setPixelColor(3, 1, QColor(10, 20, 30, 80));
  const QColor mid = sampleSourceColor(checker, QSizeF(8, 4), QRectF(0, 0, 8, 4),
                                       QRectF(0, 0, 80, 40), QPointF(35, 15));
  const QColor right =
      sampleSourceColor(checker, QSizeF(8, 4), QRectF(1.5, 0, 5, 4),
                        QRectF(0, 0, 80, 40), QPointF(79, 5));
  const QColor faded =
      sampleSourceColor(checker, QSizeF(8, 4), QRectF(0, 0, 8, 4),
                        QRectF(0, 0, 80, 40), QPointF(35, 15));
  if (!mid.isValid() || right == QColor(0, 0, 255) || faded.alpha() != 255) {
    error = QStringLiteral("Eyedropper sampled outside the crop or kept alpha");
    return false;
  }
  return true;
}

/** Checks that the live preview magnifies redacted pixels, never the source. */
bool runSpotlightRedactionPreviewCheck(QApplication &application,
                                       QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#182030")));
  {
    QPainter painter(&capture.source);
    painter.fillRect(QRect(200, 200, 100, 60),
                     QColor(QStringLiteral("#ff0000")));
  }
  capture.previewSize = capture.source.size();

  // Selecting 100,100 to 650,470 draws the capture unscaled at 125,120, so the
  // secret patch covers 225,220 to 325,280 in widget coordinates.
  const auto spotlightCenterColor = [&](bool redact) {
    CaptureEditor editor(capture);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
    QTest::mouseMove(&editor, QPoint(650, 470), 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(650, 470));
    application.processEvents();
    if (redact) {
      QTest::keyClick(&editor, Qt::Key_D);
      QTest::keyClick(&editor, Qt::Key_D);
      QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(215, 210));
      QTest::mouseMove(&editor, QPoint(335, 290), 20);
      QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                          QPoint(335, 290));
      application.processEvents();
    }
    QTest::keyClick(&editor, Qt::Key_S);
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(245, 230));
    QTest::mouseMove(&editor, QPoint(305, 270), 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(305, 270));
    application.processEvents();
    const QColor color = editor.grab().toImage().pixelColor(QPoint(275, 250));
    editor.close();
    return color;
  };

  const auto showsSecret = [](const QColor &color) {
    return color.red() > 180 && color.green() < 70 && color.blue() < 70;
  };
  if (!showsSecret(spotlightCenterColor(false))) {
    error = QStringLiteral("Spotlight preview did not magnify the capture");
    return false;
  }
  if (showsSecret(spotlightCenterColor(true))) {
    error = QStringLiteral("Spotlight preview magnified un-redacted pixels");
    return false;
  }
  return true;
}

/** Checks that clicking away commits in-progress text instead of losing it. */
bool runTextClickAwayCommitCheck(QApplication &application, QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#182030")));
  capture.previewSize = capture.source.size();

  const QString snapshotPath = temporarySnapshotPath();
  const QRectF selection(100, 100, 550, 370);
  const qreal ascent = QFontMetricsF(annotationTextFont(5.0)).ascent();
  const auto textAnnotation = [&ascent](const QPointF &origin,
                                        const QString &value) {
    Annotation text;
    text.kind = Annotation::Kind::Text;
    text.start = origin + QPointF(0, ascent);
    text.text = value;
    text.color = QColor(QStringLiteral("#ff375f"));
    text.size = 5.0;
    return text;
  };
  const auto snapshotMatches = [&snapshotPath](const QImage &expected) {
    return !expected.isNull() &&
           QImage(snapshotPath).convertToFormat(expected.format()) == expected;
  };

  CaptureEditor editor(capture);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
  QTest::mouseMove(&editor, QPoint(650, 470), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(650, 470));
  application.processEvents();

  QTest::keyClick(&editor, Qt::Key_T);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(300, 220));
  application.processEvents();
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(250, 400));
  application.processEvents();
  if (!snapshotMatches(
          renderCapture(capture, selection, {}, BackgroundStyle::None))) {
    error = QStringLiteral("Clicking away from empty text added an annotation");
    return false;
  }

  const Annotation clicked =
      textAnnotation({125, 280}, QStringLiteral("Clicked away"));
  QTest::keyClicks(QApplication::focusWidget(), clicked.text);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(450, 300));
  application.processEvents();
  if (!snapshotMatches(
          renderCapture(capture, selection, {clicked}, BackgroundStyle::None))) {
    error = QStringLiteral("Clicking the canvas discarded in-progress text");
    return false;
  }

  const Annotation toolbar =
      textAnnotation({325, 180}, QStringLiteral("Toolbar"));
  // 118,92 is the arrow tool button: the toolbar row sits above the capture.
  QTest::keyClicks(QApplication::focusWidget(), toolbar.text);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(118, 92));
  QTest::mouseMove(&editor, QPoint(400, 300), 10);
  application.processEvents();
  if (!snapshotMatches(renderCapture(capture, selection, {clicked, toolbar},
                                     BackgroundStyle::None)) ||
      editor.cursor().shape() != Qt::CrossCursor) {
    error = QStringLiteral("Clicking the toolbar discarded in-progress text");
    return false;
  }
  editor.close();
  return true;
}

bool runContinuousAnnotationToolsSmoke(QApplication &application,
                                       QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#182030")));
  capture.previewSize = capture.source.size();

  CaptureEditor editor(capture);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();

  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
  QTest::mouseMove(&editor, QPoint(700, 500), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(700, 500));
  application.processEvents();

  QTest::keyClick(&editor, Qt::Key_A);
  application.processEvents();
  if (editor.cursor().shape() != Qt::CrossCursor) {
    error = QStringLiteral("Arrow tool did not set cross cursor");
    return false;
  }

  for (const auto &[startX, startY, endX, endY] : {
           std::tuple{150, 150, 250, 200},
           std::tuple{300, 150, 400, 200},
           std::tuple{450, 150, 550, 200},
       }) {
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(startX, startY));
    QTest::mouseMove(&editor, QPoint(endX, endY), 10);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(endX, endY));
    application.processEvents();
    if (editor.cursor().shape() != Qt::CrossCursor) {
      error = QStringLiteral("Arrow tool did not remain active after drawing");
      return false;
    }
  }

  QTest::mouseClick(&editor, Qt::RightButton, Qt::NoModifier, QPoint(100, 100));
  application.processEvents();
  if (editor.cursor().shape() != Qt::ArrowCursor) {
    error = QStringLiteral("Right-click did not return to Select tool");
    return false;
  }

  QTest::keyClick(&editor, Qt::Key_M);
  application.processEvents();
  if (editor.cursor().shape() != Qt::PointingHandCursor) {
    error = QStringLiteral("Marker tool did not set pointing hand cursor");
    return false;
  }

  for (const auto &[x, y] :
       {std::pair{200, 300}, std::pair{250, 300}, std::pair{300, 300}}) {
    QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(x, y));
    application.processEvents();
    if (editor.cursor().shape() != Qt::PointingHandCursor) {
      error = QStringLiteral("Marker tool did not remain active after click");
      return false;
    }
  }

  QTest::keyClick(&editor, Qt::Key_Escape);
  application.processEvents();
  if (editor.cursor().shape() != Qt::ArrowCursor) {
    error = QStringLiteral("Escape did not return to Select tool");
    return false;
  }

  QTest::keyClick(&editor, Qt::Key_R);
  application.processEvents();
  if (editor.cursor().shape() != Qt::CrossCursor) {
    error = QStringLiteral("Rectangle tool did not set cross cursor");
    return false;
  }

  for (const auto &[startX, startY, endX, endY] : {
           std::tuple{150, 350, 250, 420},
           std::tuple{300, 350, 400, 420},
       }) {
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(startX, startY));
    QTest::mouseMove(&editor, QPoint(endX, endY), 10);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(endX, endY));
    application.processEvents();
    if (editor.cursor().shape() != Qt::CrossCursor) {
      error =
          QStringLiteral("Rectangle tool did not remain active after drawing");
      return false;
    }
  }

  QTest::keyClick(&editor, Qt::Key_V);
  application.processEvents();
  if (editor.cursor().shape() != Qt::ArrowCursor) {
    error = QStringLiteral("V key did not return to Select tool");
    return false;
  }

  QTest::keyClick(&editor, Qt::Key_T);
  application.processEvents();
  if (editor.cursor().shape() != Qt::IBeamCursor) {
    error = QStringLiteral("Text tool did not set IBeam cursor");
    return false;
  }

  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(200, 450));
  application.processEvents();
  QTest::keyClicks(QApplication::focusWidget(), QStringLiteral("Text 1"));
  QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
  application.processEvents();
  if (editor.cursor().shape() != Qt::IBeamCursor) {
    error = QStringLiteral(
        "Text tool did not remain active after submitting new text");
    return false;
  }

  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(350, 450));
  application.processEvents();
  QTest::keyClicks(QApplication::focusWidget(), QStringLiteral("Text 2"));
  QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
  application.processEvents();
  if (editor.cursor().shape() != Qt::IBeamCursor) {
    error = QStringLiteral(
        "Text tool did not remain active after second text commit");
    return false;
  }

  QTest::mouseClick(&editor, Qt::RightButton, Qt::NoModifier, QPoint(100, 100));
  application.processEvents();
  if (editor.cursor().shape() != Qt::ArrowCursor) {
    error = QStringLiteral(
        "Right-click did not return from Text tool to Select tool");
    return false;
  }

  editor.close();
  return true;
}
} // namespace
/** Runs the interaction and rendering smoke checks. */
int main(int argc, char **argv) {
  // Re-executed by the instance-lock checks as the process holding the lock.
  const QString heldLockPath =
      qEnvironmentVariable(kInstanceLockHolderVariable);
  if (!heldLockPath.isEmpty())
    return runInstanceLockHolder(heldLockPath);

  QApplication application(argc, argv);
  if (!loadCaptureFonts())
    return 17;
  QString snapshotError;
  if (!runPositionalImageTargetCheck(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 70;
  }
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
  if (!runCreationConstraintCheck(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 90;
  }
  if (!runQuickOutputChecks(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 73;
  }
  if (!runPinLayoutSmoke(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 77;
  }
  if (!runPinLifecycleSmoke(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 78;
  }
  if (!runSpotlightAndSampleChecks(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 79;
  }
  if (!runSpotlightRedactionPreviewCheck(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 86;
  }
  if (!runTextClickAwayCommitCheck(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 82;
  }
  if (!runContinuousAnnotationToolsSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 80;
  }
  const QString outputRoot =
      argc > 1 ? QString::fromLocal8Bit(argv[1])
               : QDir(QDir::tempPath())
                     .filePath(QStringLiteral("omasnap-native-smoke"));
  const QString snapshotPath = temporarySnapshotPath();
  QFile::remove(snapshotPath);
  if (qEnvironmentVariableIsSet("OMASNAP_SMOKE_BENCH")) {
    runEditorBenchmark(snapshotPath);
    QFile::remove(snapshotPath);
    return 0;
  }
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
  capture.previewSize = capture.source.size();
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
    CaptureEditor quickEditor(capture, CaptureEditor::CaptureMode::Region,
                              QuickOutputMode::Save);
    quickEditor.resize(800, 600);
    quickEditor.show();
    application.processEvents();
    QTest::mousePress(&quickEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(100, 100));
    QTest::mouseMove(&quickEditor, QPoint(650, 470), 20);
    QTest::mouseRelease(&quickEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(650, 470));
    application.processEvents();
    const QStringList files =
        QDir(savedRoot).entryList({QStringLiteral("*.png")}, QDir::Files);
    if (quickEditor.isVisible() || files.size() != 1 ||
        QImage(QDir(savedRoot).filePath(files.constFirst())).isNull())
      return 74;
  }
  QDir(savedRoot).removeRecursively();

  {
    CaptureData cropCapture;
    cropCapture.monitor.geometry = {0, 0, 800, 600};
    cropCapture.monitor.pixelSize = {64, 64};
    cropCapture.source = QImage(64, 64, QImage::Format_ARGB32_Premultiplied);
    cropCapture.source.fill(QColor(QStringLiteral("#112233")));
    cropCapture.previewSize = cropCapture.source.size();
    CaptureEditor cropEditor(cropCapture, CaptureEditor::CaptureMode::File);
    cropEditor.resize(800, 600);
    cropEditor.show();
    application.processEvents();
    const QRectF available(30, 68, 740, 474);
    const qreal scale = std::min<qreal>(
        {1.0, available.width() / 64.0, available.height() / 64.0});
    const QSizeF shown(64.0 * scale, 64.0 * scale);
    const QRectF image(available.center().x() - shown.width() / 2.0,
                       available.center().y() - shown.height() / 2.0,
                       shown.width(), shown.height());
    const QPoint leftHandle(qRound(image.left() - 7.0),
                            qRound(image.center().y()));
    QTest::mousePress(&cropEditor, Qt::LeftButton, Qt::NoModifier, leftHandle);
    QTest::mouseMove(&cropEditor, QPoint(0, leftHandle.y()), 20);
    QTest::mouseRelease(&cropEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(0, leftHandle.y()));
    application.processEvents();
    const QImage cropped(snapshotPath);
    if (cropped.isNull() || cropped.width() < 16 || cropped.width() > 64 ||
        cropped.height() != 64)
      return 93;
    cropEditor.close();
    QFile::remove(snapshotPath);
  }


  CaptureEditor editor(capture);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();
  QTest::keyClick(&editor, Qt::Key_Space);
  QTest::mouseMove(&editor, QPoint(200, 160), 20);
  application.processEvents();
  const QImage hoverUi = editor.grab().toImage();
  if (hoverUi.pixelColor(200, 160) != capture.source.pixelColor(200, 160))
    return 7;
  QTest::keyClick(&editor, Qt::Key_Right, Qt::MetaModifier);
  application.processEvents();
  const QImage keyboardWindowUi = editor.grab().toImage();
  if (keyboardWindowUi.pixelColor(500, 200) !=
          capture.source.pixelColor(500, 200) ||
      keyboardWindowUi.pixelColor(200, 160) ==
          capture.source.pixelColor(200, 160))
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
  if (editor.cursor().shape() != Qt::CrossCursor)
    return 26;
  const QImage arrowSnapshot(snapshotPath);
  if (arrowSnapshot.isNull() || arrowSnapshot == initialSnapshot)
    return 38;
  QTest::mouseClick(&editor, Qt::RightButton, Qt::NoModifier, QPoint(100, 100));
  application.processEvents();
  if (editor.cursor().shape() != Qt::ArrowCursor)
    return 26;
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
  if (editor.cursor().shape() != Qt::CrossCursor)
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

  QTest::keyClick(&editor, Qt::Key_V);
  application.processEvents();
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
  // Wheel scaling debounces its working-snapshot write.
  QTest::qWait(kSnapshotSettleMs);
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
  if (editor.cursor().shape() != Qt::CrossCursor ||
      QImage(snapshotPath) == beforeFreehandSnapshot)
    return 28;
  const QImage beforeMarkerSnapshot(snapshotPath);
  QTest::keyClick(&editor, Qt::Key_2);
  QTest::keyClick(&editor, Qt::Key_C);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(470, 300));
  application.processEvents();
  if (editor.cursor().shape() != Qt::PointingHandCursor ||
      QImage(snapshotPath) == beforeMarkerSnapshot)
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
  // Backdrop cycling debounces its working-snapshot write.
  QTest::qWait(kSnapshotSettleMs);
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
  // Color picking debounces its write; land it before the next editor opens.
  QTest::qWait(kSnapshotSettleMs);
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

    QTest::keyClick(&redactionEditor, Qt::Key_V);
    QTest::mouseClick(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(360, 230));
    QTest::keyClick(&redactionEditor, Qt::Key_D);
    // Redaction style toggling debounces its working-snapshot write.
    QTest::qWait(kSnapshotSettleMs);
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

    QTest::keyClick(&redactionEditor, Qt::Key_D);
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
    QTest::keyClick(&overlapEditor, Qt::Key_V);
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
    QTest::keyClick(&overlapEditor, Qt::Key_V);
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
  nativePreviewCapture.previewSize = QSize(800, 600);
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
  fileData.previewSize = fileData.source.size();
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
  windowSurfaceCapture.previewSize = windowSurfaceCapture.source.size();
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
  QTest::keyClick(&cropEditor, Qt::Key_V);
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
  // Backdrop cycling above debounced a write, and this editor stays open for
  // the rest of the run: land it now instead of during a later check, since
  // every editor in this process shares one working-snapshot path.
  QTest::qWait(kSnapshotSettleMs);
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
  clippingCapture.previewSize = clippingCapture.source.size();
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

  QString previewError;
  if (!runBackdropCacheRenderingCheck(capture, previewError)) {
    qWarning().noquote() << previewError;
    return 87;
  }
  if (!runRedactedCropCacheCheck(previewError)) {
    qWarning().noquote() << previewError;
    return 88;
  }
  if (!runSnapshotDebounceCheck(capture, snapshotPath, savedRoot,
                                previewError)) {
    qWarning().noquote() << previewError;
    return 89;
  }

  QString transformError;
  if (!runTransformSmoke(transformError)) {
    qWarning().noquote() << transformError;
    return 67;
  }
  QString instanceError;
  if (!runInstanceLockSmoke(instanceError)) {
    qWarning().noquote() << instanceError;
    return 85;
  }
  return 0;
}
