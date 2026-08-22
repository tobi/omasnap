/** @fileoverview Exercises capture editor behavior without a live compositor.
 */
#include "capture.hpp"
#include "cli-path.hpp"
#include "clipboard-smoke.hpp"
#include "cut-smoke.hpp"
#include "editor.hpp"
#include "instance-lock-smoke.hpp"
#include "palette-config-smoke.hpp"
#include "pin-layout-smoke.hpp"
#include "stitch-smoke.hpp"
#include "stitch.hpp"
#include "pin-lifecycle-smoke.hpp"
#include "transform-smoke.hpp"
#include "eyedropper.hpp"

#include <QApplication>
#include <QBuffer>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QThread>
#include <QFile>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QUrl>
#include <QWheelEvent>
#include <QtTest/QTest>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <csignal>
#include <sys/resource.h>

namespace {
/**
 * The current output is a memory-only composite until an explicit save/copy.
 */
template <typename Editor>
QImage flushedSnapshot(Editor &editor, const QString &) {
  return editor.renderCurrentOutput();
}
/** Cached select-phase painting preserves bright and dimmed capture pixels. */
bool runBackdropCacheRenderingCheck(QString &error) {
  CaptureData capture;
  capture.monitor.geometry = QRect(0, 0, 800, 600);
  capture.monitor.pixelSize = QSize(800, 600);
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#6480a0")));
  capture.previewSize = capture.source.size();

  CaptureEditor editor(capture);
  editor.resize(800, 600);
  editor.show();
  QApplication::processEvents();
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(150, 120));
  QTest::mouseMove(&editor, QPoint(600, 430), 20);
  QApplication::processEvents();
  const QImage ui = editor.grab().toImage();

  for (const QPoint inside : {QPoint(300, 250), QPoint(500, 400)}) {
    if (ui.pixelColor(inside) != capture.source.pixelColor(inside)) {
      error = QStringLiteral("Cached selection backdrop changed source pixels");
      return false;
    }
  }
  for (const QPoint outside : {QPoint(40, 200), QPoint(720, 200)}) {
    const QColor source = capture.source.pixelColor(outside);
    const QColor dimmed = ui.pixelColor(outside);
    const auto matches = [](int actual, int expected) {
      return std::abs(actual - expected) <= 2;
    };
    if (!matches(dimmed.red(), source.red() * (255 - 143) / 255) ||
        !matches(dimmed.green(), source.green() * (255 - 143) / 255) ||
        !matches(dimmed.blue(), source.blue() * (255 - 143) / 255)) {
      error = QStringLiteral("Cached selection backdrop changed dimming");
      return false;
    }
  }
  return true;
}

constexpr int kSelectUndim = 143;
const QColor kSelectHoleMarker(QStringLiteral("#ff40c0"));
const QColor kSelectBackdropFill(QStringLiteral("#1a4a7a"));

QColor dimmedSelectColor(const QColor &source) {
  return QColor(source.red() * (255 - kSelectUndim) / 255,
                source.green() * (255 - kSelectUndim) / 255,
                source.blue() * (255 - kSelectUndim) / 255);
}

bool colorNear(const QColor &actual, const QColor &wanted, int slop = 20) {
  return std::abs(actual.red() - wanted.red()) <= slop &&
         std::abs(actual.green() - wanted.green()) <= slop &&
         std::abs(actual.blue() - wanted.blue()) <= slop;
}

QRectF mappedRect(const QRectF &rect, const QSize &from, const QSize &to) {
  if (from.isEmpty() || to.isEmpty() || from == to)
    return rect;
  return {rect.x() * to.width() / static_cast<qreal>(from.width()),
          rect.y() * to.height() / static_cast<qreal>(from.height()),
          rect.width() * to.width() / static_cast<qreal>(from.width()),
          rect.height() * to.height() / static_cast<qreal>(from.height())};
}

QRectF nativeSourceRect(const QSize &source, const QSize &preview,
                        const QRectF &logical) {
  if (preview.isEmpty())
    return {};
  return {logical.x() * source.width() / static_cast<qreal>(preview.width()),
          logical.y() * source.height() / static_cast<qreal>(preview.height()),
          logical.width() * source.width() / static_cast<qreal>(preview.width()),
          logical.height() * source.height() /
              static_cast<qreal>(preview.height())};
}

CaptureData selectHoleCapture(const QSize &preview, const QSize &sourceSize,
                              qreal scale, const QRectF &previewHole,
                              const QVector<WindowTarget> &windows = {}) {
  CaptureData capture;
  capture.monitor.geometry = QRect(QPoint(), preview);
  capture.monitor.pixelSize = sourceSize;
  capture.monitor.scale = scale;
  capture.source = QImage(sourceSize, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(kSelectBackdropFill);
  capture.previewSize = preview;
  capture.windows = windows;
  QPainter painter(&capture.source);
  painter.setCompositionMode(QPainter::CompositionMode_Source);
  painter.fillRect(nativeSourceRect(sourceSize, preview, previewHole),
                   kSelectHoleMarker);
  return capture;
}

void prepareSelectEditor(CaptureEditor &editor, const QSize &widget) {
  editor.setSuppressSnapshots(true);
  editor.resize(widget);
  editor.show();
  QApplication::processEvents();
}

QColor grabLogicalPixel(const QImage &ui, const QWidget &editor,
                        const QPointF &logical) {
  const qreal scaleX = ui.width() / static_cast<qreal>(std::max(1, editor.width()));
  const qreal scaleY =
      ui.height() / static_cast<qreal>(std::max(1, editor.height()));
  const int x = std::clamp(qRound(logical.x() * scaleX), 0, ui.width() - 1);
  const int y = std::clamp(qRound(logical.y() * scaleY), 0, ui.height() - 1);
  return ui.pixelColor(x, y);
}

/**
 * Reconstructs the old clip + full-widget backdrop blit. The clip is in the
 * unmapped space the previous punch used (widget selection, or preview
 * window.rect). Sampling destHole on this image is how the suite proves a
 * case actually fails on that path.
 */
QImage renderOldClipBlit(const QImage &source, const QSize &widget, qreal dpr,
                         const QRectF &oldClip) {
  const QSize deviceSize = (QSizeF(widget) * dpr).toSize();
  QPixmap backdrop(deviceSize);
  backdrop.setDevicePixelRatio(dpr);
  backdrop.fill(Qt::transparent);
  {
    QPainter cache(&backdrop);
    cache.setRenderHint(QPainter::SmoothPixmapTransform, true);
    cache.drawImage(QRectF(QPointF(), QSizeF(deviceSize) / dpr), source);
  }
  QPixmap dimmed = backdrop.copy();
  {
    QPainter cache(&dimmed);
    cache.fillRect(QRectF(QPointF(), QSizeF(deviceSize) / dpr),
                   QColor(0, 0, 0, kSelectUndim));
  }
  QImage out(deviceSize, QImage::Format_ARGB32_Premultiplied);
  out.setDevicePixelRatio(dpr);
  out.fill(Qt::transparent);
  QPainter painter(&out);
  painter.setRenderHints(QPainter::Antialiasing |
                         QPainter::SmoothPixmapTransform);
  // Device canvas, logical clip/dest: the space mix that left the hole dim.
  painter.drawPixmap(QRect(QPoint(), widget), dimmed);
  painter.save();
  painter.setClipRect(oldClip);
  painter.drawPixmap(QRect(QPoint(), widget), backdrop);
  painter.restore();
  return out;
}

QColor oldClipBlitPixel(const QImage &source, const QSize &widget, qreal dpr,
                        const QRectF &oldClip, const QPointF &logical) {
  const QImage ui = renderOldClipBlit(source, widget, dpr, oldClip);
  const qreal scaleX = ui.width() / static_cast<qreal>(std::max(1, widget.width()));
  const qreal scaleY =
      ui.height() / static_cast<qreal>(std::max(1, widget.height()));
  const int x = std::clamp(qRound(logical.x() * scaleX), 0, ui.width() - 1);
  const int y = std::clamp(qRound(logical.y() * scaleY), 0, ui.height() - 1);
  return ui.pixelColor(x, y);
}

bool expectUndimmedHole(const QImage &ui, const CaptureEditor &editor,
                        const QImage &source, const QSize &widget, qreal dpr,
                        const QRectF &oldClip, const QPointF &inside,
                        const QPointF &outside, const QString &what,
                        QString &error) {
  const QColor hole = grabLogicalPixel(ui, editor, inside);
  const QColor chrome = grabLogicalPixel(ui, editor, outside);
  const QColor oldHole = oldClipBlitPixel(source, widget, dpr, oldClip, inside);
  if (!colorNear(hole, kSelectHoleMarker)) {
    error = QStringLiteral("%1 hole at %2,%3 was %4, expected undimmed sourceRect")
                .arg(what)
                .arg(inside.x(), 0, 'f', 1)
                .arg(inside.y(), 0, 'f', 1)
                .arg(hole.name());
    return false;
  }
  // When the old clip lives in a different space than destHole, the mapped
  // interior stays dim (or shows the wrong source). Identity clips can match.
  if (!oldClip.contains(inside) && colorNear(oldHole, kSelectHoleMarker)) {
    error = QStringLiteral("%1 old clip+blit at %2,%3 already matched the hole")
                .arg(what)
                .arg(inside.x(), 0, 'f', 1)
                .arg(inside.y(), 0, 'f', 1);
    return false;
  }
  if (colorNear(chrome, kSelectHoleMarker) ||
      !colorNear(chrome, dimmedSelectColor(kSelectBackdropFill), 24)) {
    error = QStringLiteral("%1 chrome at %2,%3 was %4, expected dimmed backdrop")
                .arg(what)
                .arg(outside.x(), 0, 'f', 1)
                .arg(outside.y(), 0, 'f', 1)
                .arg(chrome.name());
    return false;
  }
  return true;
}

/**
 * The select-phase undim hole must be a destHole/sourceRect blit, not clip +
 * full-widget cache. 1x widget==preview cannot catch fractional scale, 90/270
 * transform, or window.rect preview space.
 */
bool runSelectUndimHoleCheck(QString &error) {
  const auto dragRegion = [](CaptureEditor &editor, const QPoint &a,
                             const QPoint &b) {
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, a);
    QTest::mouseMove(&editor, b, 20);
    QApplication::processEvents();
  };

  // Scale 2.0 grab, widget == preview. Native sourceRect is 2x the drag.
  {
    const QSize preview(800, 600);
    const QSize sourceSize(1600, 1200);
    const QSize widget(800, 600);
    const QRectF selection(160, 200, 280, 200);
    CaptureData capture = selectHoleCapture(preview, sourceSize, 2.0, selection);
    CaptureEditor editor(capture);
    prepareSelectEditor(editor, widget);
    dragRegion(editor, QPoint(160, 200), QPoint(440, 400));
    const QImage ui = editor.grab().toImage();
    if (!expectUndimmedHole(ui, editor, capture.source, widget, 2.0, selection,
                            QPointF(280, 300), QPointF(40, 300),
                            QStringLiteral("Scale 2.0 region"), error))
      return false;
  }

  // Region drag when widget != preview: dest is the drag, source is the mapped
  // preview hole. Old clip+blit using the preview-space rect punches elsewhere.
  {
    const QSize preview(800, 600);
    const QSize sourceSize(1600, 1200);
    const QSize widget(400, 300);
    const QRectF selection(60, 140, 140, 100);
    const QRectF previewHole = mappedRect(selection, widget, preview);
    CaptureData capture =
        selectHoleCapture(preview, sourceSize, 2.0, previewHole);
    CaptureEditor editor(capture);
    prepareSelectEditor(editor, widget);
    dragRegion(editor, QPoint(60, 140), QPoint(200, 240));
    const QImage ui = editor.grab().toImage();
    if (!expectUndimmedHole(ui, editor, capture.source, widget, 1.0, previewHole,
                            QPointF(100, 180), QPointF(20, 200),
                            QStringLiteral("Mismatched region"), error))
      return false;
  }

  // Window hover: window.rect is preview space. destHole is mapped to widget.
  // Old clip uses window.rect on the widget, so the mapped interior stays dim.
  {
    const QSize preview(800, 600);
    const QSize sourceSize(800, 600);
    const QSize widget(400, 300);
    const QRect windowRect(160, 120, 400, 320);
    CaptureData capture = selectHoleCapture(
        preview, sourceSize, 1.0, QRectF(windowRect),
        {{{windowRect}, QStringLiteral("w1"), QStringLiteral("one")}});
    CaptureEditor editor(capture);
    prepareSelectEditor(editor, widget);
    QTest::keyClick(&editor, Qt::Key_Space);
    QTest::mouseMove(&editor, QPoint(100, 90), 20);
    QApplication::processEvents();
    const QImage ui = editor.grab().toImage();
    if (!expectUndimmedHole(ui, editor, capture.source, widget, 1.0,
                            QRectF(windowRect), QPointF(100, 90), QPointF(20, 200),
                            QStringLiteral("Mismatched window"), error))
      return false;
  }

  // 90/270-style aspect swap: widget landscape, preview portrait.
  {
    const QSize preview(600, 800);
    const QSize sourceSize(600, 800);
    const QSize widget(800, 600);
    const QRect windowRect(80, 200, 200, 280);
    CaptureData capture = selectHoleCapture(
        preview, sourceSize, 1.0, QRectF(windowRect),
        {{{windowRect}, QStringLiteral("w1"), QStringLiteral("rotated")}});
    CaptureEditor editor(capture);
    prepareSelectEditor(editor, widget);
    QTest::keyClick(&editor, Qt::Key_Space);
    QTest::mouseMove(&editor, QPoint(320, 180), 20);
    QApplication::processEvents();
    const QImage ui = editor.grab().toImage();
    if (!expectUndimmedHole(ui, editor, capture.source, widget, 1.0,
                            QRectF(windowRect), QPointF(320, 180),
                            QPointF(40, 400),
                            QStringLiteral("Transform window"), error))
      return false;
  }

  {
    const QSize preview(600, 800);
    const QSize sourceSize(600, 800);
    const QSize widget(800, 600);
    const QRectF selection(80, 200, 200, 160);
    const QRectF previewHole = mappedRect(selection, widget, preview);
    CaptureData capture = selectHoleCapture(preview, sourceSize, 1.0, previewHole);
    CaptureEditor editor(capture);
    prepareSelectEditor(editor, widget);
    dragRegion(editor, QPoint(80, 200), QPoint(280, 360));
    const QImage ui = editor.grab().toImage();
    if (!expectUndimmedHole(ui, editor, capture.source, widget, 1.0, previewHole,
                            QPointF(120, 220), QPointF(30, 400),
                            QStringLiteral("Transform region"), error))
      return false;
  }

  // Combined mandatory grab: scale 2.0 and widget != preview, window + region.
  {
    const QSize preview(800, 600);
    const QSize sourceSize(1600, 1200);
    const QSize widget(400, 300);
    const QRectF selection(60, 140, 140, 100);
    const QRectF previewHole = mappedRect(selection, widget, preview);
    CaptureData capture =
        selectHoleCapture(preview, sourceSize, 2.0, previewHole);
    CaptureEditor editor(capture);
    prepareSelectEditor(editor, widget);
    dragRegion(editor, QPoint(60, 140), QPoint(200, 240));
    const QImage ui = editor.grab().toImage();
    if (!expectUndimmedHole(ui, editor, capture.source, widget, 2.0, previewHole,
                            QPointF(100, 180), QPointF(20, 200),
                            QStringLiteral("Scale 2.0 mismatched region"), error))
      return false;
  }

  {
    const QSize preview(800, 600);
    const QSize sourceSize(1600, 1200);
    const QSize widget(400, 300);
    const QRect windowRect(160, 120, 400, 320);
    CaptureData capture = selectHoleCapture(
        preview, sourceSize, 2.0, QRectF(windowRect),
        {{{windowRect}, QStringLiteral("w1"), QStringLiteral("hidpi")}});
    CaptureEditor editor(capture);
    prepareSelectEditor(editor, widget);
    QTest::keyClick(&editor, Qt::Key_Space);
    QTest::mouseMove(&editor, QPoint(100, 90), 20);
    QApplication::processEvents();
    const QImage ui = editor.grab().toImage();
    if (!expectUndimmedHole(ui, editor, capture.source, widget, 2.0,
                            QRectF(windowRect), QPointF(100, 90), QPointF(20, 200),
                            QStringLiteral("Scale 2.0 mismatched window"),
                            error))
      return false;
  }

  return true;
}

/**
 * The pointer readout reports native export pixels, not the logical units the
 * pointer travels through. On a 2x monitor a 450x310 drag exports 900x620, and
 * quoting the logical number would make the overlay useless as a ruler.
 */
bool runMeasurementReadoutCheck(QString &error) {
  CaptureData capture;
  capture.monitor.geometry = QRect(0, 0, 800, 600);
  capture.monitor.pixelSize = QSize(1600, 1200);
  capture.monitor.scale = 2.0;
  capture.source = QImage(1600, 1200, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#6480a0")));
  capture.previewSize = QSize(800, 600);
  capture.windows = {
      {{80, 80, 300, 220}, QStringLiteral("1"), QStringLiteral("first")}};

  CaptureEditor editor(capture);
  // The readout is overlay-only, so this check never needs the working
  // snapshot. Writing one would race the shared runtime path with the checks
  // that follow.
  editor.setSuppressSnapshots(true);
  editor.resize(800, 600);
  editor.show();
  QApplication::processEvents();

  const auto expect = [&editor, &error](const QString &wanted,
                                        const QString &what) {
    const QString actual = editor.measurementText();
    if (actual == wanted)
      return true;
    error = QStringLiteral("%1 readout was \"%2\", expected \"%3\"")
                .arg(what, actual, wanted);
    return false;
  };

  QTest::mouseMove(&editor, QPoint(150, 120), 20);
  QApplication::processEvents();
  if (!expect(QStringLiteral("300, 240"), QStringLiteral("Idle pointer")))
    return false;

  QTest::keyClick(&editor, Qt::Key_Space);
  QTest::mouseMove(&editor, QPoint(200, 150), 20);
  QApplication::processEvents();
  if (!expect(QStringLiteral("600 × 440"), QStringLiteral("Hovered window")))
    return false;
  QTest::keyClick(&editor, Qt::Key_Space);
  QApplication::processEvents();

  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(150, 120));
  QApplication::processEvents();
  if (!expect(QStringLiteral("0 × 0"), QStringLiteral("Fresh drag")))
    return false;
  QTest::mouseMove(&editor, QPoint(600, 430), 20);
  QApplication::processEvents();
  if (!expect(QStringLiteral("900 × 620"), QStringLiteral("Live drag")))
    return false;

  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(600, 430));
  QApplication::processEvents();
  // QTest carries one pointer position across every check in this binary and
  // drops a move that repeats it, so this check parks the pointer somewhere no
  // later check moves to first.
  QTest::mouseMove(&editor, QPoint(512, 337), 20);
  QApplication::processEvents();
  // Annotating is not measuring: the badge must not shadow the canvas once the
  // frame is settled.
  if (!expect(QString(), QStringLiteral("Settled edit phase")))
    return false;
  editor.close();
  return true;
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


  // Alt mirrors the end point through the press point so the shape is
  // centered there; composed with Shift it yields a centered square/circle.
  for (const CaptureEditor::Tool tool :
       {CaptureEditor::Tool::Rectangle, CaptureEditor::Tool::Ellipse,
        CaptureEditor::Tool::Spotlight}) {
    if (centeredCreationStart(tool, start, QPointF(40, 30)) !=
        QPointF(-20, -10)) {
      error = QStringLiteral("Centered creation did not mirror the start");
      return false;
    }
    if (centeredCreationStart(tool, start, square) != QPointF(-20, -20)) {
      error = QStringLiteral(
          "Centered creation did not compose with the 1:1 constraint");
      return false;
    }
  }
  for (const CaptureEditor::Tool tool :
       {CaptureEditor::Tool::Arrow, CaptureEditor::Tool::Line,
        CaptureEditor::Tool::Redact, CaptureEditor::Tool::Freehand}) {
    if (centeredCreationStart(tool, start, QPointF(40, 30)) != start) {
      error = QStringLiteral("Centered creation changed an unrelated tool");
      return false;
    }
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

/**
 * Crash recovery: the original source plus op-log JSON must reach disk while
 * editing, save still writes one flattened PNG, and quit removes the working
 * document.
 */
bool runCrashSnapshotChecks(const CaptureData &capture, QString &error) {
  QTemporaryDir directory;
  if (!directory.isValid()) {
    error = QStringLiteral("Could not create crash-snapshot directory");
    return false;
  }
  const QByteArray previousDir = qgetenv("OMASNAP_SCREENSHOT_DIR");
  qputenv("OMASNAP_SCREENSHOT_DIR", directory.path().toUtf8());
  const auto restoreDir = qScopeGuard(
      [&previousDir] { qputenv("OMASNAP_SCREENSHOT_DIR", previousDir); });
  const QString snapshotPath = temporarySnapshotPath();
  QFile::remove(snapshotPath);
  const auto settleUntilWritten = [&snapshotPath] {
    QElapsedTimer settle;
    settle.start();
    while (!QFile::exists(snapshotPath) && settle.elapsed() < 5000) {
      QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
      QThread::msleep(2);
    }
    return QFile::exists(snapshotPath);
  };
  const auto annotate = [](CaptureEditor &editor) {
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(100, 100));
    QTest::mouseMove(&editor, QPoint(650, 470), 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(650, 470));
    QTest::keyClick(&editor, Qt::Key_R);
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(300, 220));
    QTest::mouseMove(&editor, QPoint(420, 320), 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(420, 320));
    QCoreApplication::processEvents();
  };

  {
    CaptureEditor editor(capture);
    editor.resize(800, 600);
    editor.show();
    annotate(editor);
    if (!settleUntilWritten()) {
      error = QStringLiteral("No working snapshot was written while editing");
      return false;
    }
    editor.waitForSnapshot();
    const QImage current = editor.renderCurrentOutput();
    const QString logPath = operationLogPath(snapshotPath);
    OperationLog log;
    QString logError;
    if (QImage(snapshotPath).convertToFormat(QImage::Format_ARGB32) !=
            capture.source.convertToFormat(QImage::Format_ARGB32) ||
        !loadOperationLog(logPath, log, logError) || log.ops.isEmpty() ||
        log.index <= 0) {
      error = QStringLiteral(
          "Working document did not keep the source image and op log");
      return false;
    }
    QByteArray reference;
    QBuffer referenceBuffer(&reference);
    referenceBuffer.open(QIODevice::WriteOnly);
    current.save(&referenceBuffer, "PNG");
    QTest::keyClick(&editor, Qt::Key_S, Qt::ControlModifier);
    QCoreApplication::processEvents();
    const QStringList files =
        QDir(directory.path())
            .entryList({QStringLiteral("*.png")}, QDir::Files);
    if (files.size() != 1 || QFile::exists(snapshotPath) ||
        QFile::exists(logPath) ||
        QFileInfo(QDir(directory.path()).filePath(files.constFirst())).size() >
            reference.size() * 3 / 2) {
      error = QStringLiteral(
          "Save did not leave exactly one default-encoded screenshot");
      return false;
    }
  }

  {
    CaptureEditor quitEditor(capture);
    quitEditor.resize(800, 600);
    quitEditor.show();
    annotate(quitEditor);
    QTest::keyClick(&quitEditor, Qt::Key_Escape);
    QTest::keyClick(&quitEditor, Qt::Key_Escape);
    QCoreApplication::processEvents();
    if (!settleUntilWritten() || quitEditor.isVisible()) {
      error = QStringLiteral("Editing before a quit left no snapshot to clean");
      return false;
    }
  }
  if (QFile::exists(snapshotPath)) {
    error = QStringLiteral("Quitting left the working snapshot behind");
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
  CaptureEditor editor(capture);
  const auto snapshotMatches = [&editor, &snapshotPath](const QImage &expected) {
    return !expected.isNull() &&
           flushedSnapshot(editor, snapshotPath)
                   .convertToFormat(expected.format()) == expected;
  };
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
      textAnnotation({125, 280}, QStringLiteral("Clicked\naway"));
  QWidget *inlineEditor = QApplication::focusWidget();
  QTest::keyClicks(inlineEditor, QStringLiteral("Clicked"));
  QTest::keyClick(inlineEditor, Qt::Key_Return);
  application.processEvents();
  if (QApplication::focusWidget() != inlineEditor ||
      editor.annotationCountForTest() != 0) {
    error = QStringLiteral("Enter committed text instead of starting a new line");
    return false;
  }
  const auto *multilineEditor = qobject_cast<QPlainTextEdit *>(inlineEditor);
  if (!multilineEditor ||
      multilineEditor->toPlainText() != QStringLiteral("Clicked\n") ||
      multilineEditor->verticalScrollBar()->value() != 0 ||
      multilineEditor->viewport()->height() <
          multilineEditor->fontMetrics().lineSpacing() * 2) {
    error = QStringLiteral("First Enter hid the existing line or clipped the editor");
    return false;
  }
  QTest::keyClicks(inlineEditor, QStringLiteral("away"));
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(450, 300));
  application.processEvents();
  if (!snapshotMatches(
          renderCapture(capture, selection, {clicked}, BackgroundStyle::None))) {
    error = QStringLiteral("Clicking the canvas discarded multiline text");
    return false;
  }

  const Annotation toolbar =
      textAnnotation({325, 180}, QStringLiteral("Toolbar"));
  // The arrow button sits in the spaced toolbar above the capture.
  QTest::keyClicks(QApplication::focusWidget(), toolbar.text);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(60, 43));
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

bool showsSecretRed(const QColor &color) {
  return color.red() > 180 && color.green() < 70 && color.blue() < 70;
}

/** Redaction sits under every other tool in both preview and export. */
bool runAnnotationLayerChecks(QApplication &application, QString &error) {
  const QColor solid(QStringLiteral("#121216"));
  const QColor secret(QStringLiteral("#ff0000"));
  const QColor backdrop(QStringLiteral("#182030"));

  if (annotationLayer(Annotation::Kind::Redaction) !=
          AnnotationLayer::Redaction ||
      annotationLayer(Annotation::Kind::Arrow) != AnnotationLayer::Default ||
      annotationLayer(Annotation::Kind::Spotlight) !=
          AnnotationLayer::Default ||
      annotationLayer(Annotation::Kind::Text) != AnnotationLayer::Default) {
    error = QStringLiteral("Annotation kinds did not map onto two layers");
    return false;
  }

  // Selection-relative redactions must stay put when the crop is not at the
  // origin. The old applyRedactionsScaled offset subtracted selection.topLeft()
  // and slid the block off the secret.
  QImage scaledBase(100, 80, QImage::Format_ARGB32_Premultiplied);
  scaledBase.fill(secret);
  Annotation scaledRedaction;
  scaledRedaction.kind = Annotation::Kind::Redaction;
  scaledRedaction.start = {10, 10};
  scaledRedaction.end = {40, 40};
  scaledRedaction.redactionStyle = RedactionStyle::Solid;
  const QImage scaled = applyRedactionsScaled(
      scaledBase, {scaledRedaction}, QRectF(200, 150, 100, 80), QSizeF(100, 80));
  if (scaled.pixelColor(20, 20) != solid ||
      scaled.pixelColor(2, 2) != secret) {
    error = QStringLiteral(
        "Scaled redaction layer used capture coordinates instead of "
        "selection-relative ones");
    return false;
  }

  CaptureData capture;
  capture.monitor.scale = 1.0;
  capture.monitor.pixelSize = {80, 40};
  capture.source = QImage(80, 40, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(backdrop);
  for (int y = 10; y < 30; ++y) {
    for (int x = 20; x < 40; ++x)
      capture.source.setPixelColor(x, y, secret);
  }
  capture.previewSize = capture.source.size();

  Annotation redaction;
  redaction.kind = Annotation::Kind::Redaction;
  redaction.start = {20, 10};
  redaction.end = {40, 30};
  redaction.redactionStyle = RedactionStyle::Solid;

  Annotation lens;
  lens.kind = Annotation::Kind::Spotlight;
  lens.start = {16, 6};
  lens.end = {44, 34};
  lens.magnification = 2.0;
  lens.color = Qt::white;
  lens.size = 2;

  const QImage spotlightExport =
      renderCapture(capture, QRectF(0, 0, 80, 40), {redaction, lens},
                    BackgroundStyle::None);
  if (spotlightExport.isNull() ||
      showsSecretRed(spotlightExport.pixelColor(30, 20)) ||
      spotlightExport.pixelColor(30, 20) != solid) {
    error = QStringLiteral("Export spotlight sampled un-redacted source pixels");
    return false;
  }

  Annotation arrow;
  arrow.kind = Annotation::Kind::Arrow;
  arrow.start = {22, 20};
  arrow.end = {38, 20};
  arrow.color = QColor(QStringLiteral("#0a84ff"));
  arrow.size = 4;
  Annotation label;
  label.kind = Annotation::Kind::Text;
  label.start = {24, 26};
  label.text = QStringLiteral("X");
  label.color = Qt::white;
  label.size = 8;
  // Plain, so this stays a layer-order check: a readability pill would cover
  // the arrow beneath it by design, which runTextPillSmoke covers instead.
  label.textBackground = TextBackground::Plain;
  const QImage redactionOnly =
      renderCapture(capture, QRectF(0, 0, 80, 40), {redaction},
                    BackgroundStyle::None);
  const QImage overlayExport =
      renderCapture(capture, QRectF(0, 0, 80, 40), {redaction, arrow, label},
                    BackgroundStyle::None);
  const QColor overlayFill = overlayExport.pixelColor(22, 12);
  const QColor overlayStroke = overlayExport.pixelColor(26, 20);
  if (overlayExport.isNull() || redactionOnly.isNull() ||
      redactionOnly.pixelColor(22, 12) != solid || showsSecretRed(overlayFill) ||
      overlayStroke.blue() <= overlayStroke.red() + 20) {
    error = QStringLiteral(
        "Export did not keep redaction under arrow and text");
    return false;
  }

  CaptureData editorCapture;
  editorCapture.monitor.name = QStringLiteral("TEST");
  editorCapture.monitor.geometry = {0, 0, 800, 600};
  editorCapture.monitor.pixelSize = {800, 600};
  editorCapture.monitor.scale = 1.0;
  editorCapture.source =
      QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  editorCapture.source.fill(backdrop);
  {
    QPainter painter(&editorCapture.source);
    painter.fillRect(QRect(200, 200, 100, 60), secret);
  }
  editorCapture.previewSize = editorCapture.source.size();

  // Fullscreen draws the 800x600 capture at 84,68 scaled by 0.79. The secret
  // then covers 242,226 to 321,273, centered on 281,250. A loupe smaller than
  // the redaction can only show redacted pixels if it samples the redaction
  // layer.
  const auto spotlightPreviewColor = [&](bool redact) {
    CaptureEditor editor(editorCapture, CaptureEditor::CaptureMode::Fullscreen);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    if (redact) {
      QTest::keyClick(&editor, Qt::Key_D);
      QTest::keyClick(&editor, Qt::Key_D);
      QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(230, 214));
      QTest::mouseMove(&editor, QPoint(333, 285), 20);
      QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                          QPoint(333, 285));
      application.processEvents();
    }
    QTest::keyClick(&editor, Qt::Key_S);
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(251, 230));
    QTest::mouseMove(&editor, QPoint(311, 270), 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(311, 270));
    application.processEvents();
    const QColor color = editor.grab().toImage().pixelColor(QPoint(281, 250));
    editor.close();
    return color;
  };
  if (!showsSecretRed(spotlightPreviewColor(false))) {
    error = QStringLiteral("Spotlight preview did not magnify the capture");
    return false;
  }
  if (showsSecretRed(spotlightPreviewColor(true))) {
    error = QStringLiteral("Spotlight preview magnified un-redacted pixels");
    return false;
  }

  {
    CaptureEditor editor(editorCapture, CaptureEditor::CaptureMode::Fullscreen);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    QTest::keyClick(&editor, Qt::Key_D);
    QTest::keyClick(&editor, Qt::Key_D);
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(230, 214));
    QTest::mouseMove(&editor, QPoint(333, 285), 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(333, 285));
    QTest::keyClick(&editor, Qt::Key_5);
    QTest::keyClick(&editor, Qt::Key_A);
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(250, 250));
    QTest::mouseMove(&editor, QPoint(312, 250), 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(312, 250));
    application.processEvents();
    const QImage preview = editor.grab().toImage();
    const QImage exported = editor.renderCurrentOutput();
    editor.close();
    const QColor arrowColor(QStringLiteral("#0a84ff"));
    if (preview.pixelColor(250, 226) != solid ||
        showsSecretRed(preview.pixelColor(250, 226)) ||
        preview.pixelColor(260, 250) != arrowColor) {
      error = QStringLiteral("Preview did not keep redaction under the arrow");
      return false;
    }
    if (exported.pixelColor(210, 202) != solid ||
        showsSecretRed(exported.pixelColor(210, 202)) ||
        exported.pixelColor(220, 230) != arrowColor) {
      error = QStringLiteral("Export did not keep redaction under the arrow");
      return false;
    }
  }

  {
    CaptureData offsetCapture = editorCapture;
    {
      QPainter painter(&offsetCapture.source);
      painter.fillRect(QRect(450, 350, 80, 60), secret);
    }
    offsetCapture.previewSize = offsetCapture.source.size();
    CaptureEditor editor(offsetCapture);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();

    // Region 200,150 400x300 draws at 200,155 with edit scale 1. The secret
    // is then at 450,355 to 530,415. A preview that subtracts the selection
    // origin lands the block around 250,205 instead.
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(200, 150));
    QTest::mouseMove(&editor, QPoint(600, 450), 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(600, 450));
    application.processEvents();
    const QPoint secretCenter(490, 385);
    const QPoint displacedCenter(250, 205);
    if (!showsSecretRed(editor.grab().toImage().pixelColor(secretCenter))) {
      error = QStringLiteral("Editor did not show the secret before redacting");
      return false;
    }

    QTest::keyClick(&editor, Qt::Key_D);
    QTest::keyClick(&editor, Qt::Key_D);
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(450, 355));
    QTest::mouseMove(&editor, QPoint(530, 415), 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(530, 415));
    application.processEvents();
    const QImage preview = editor.grab().toImage();
    const QImage exported = editor.renderCurrentOutput();
    editor.close();
    if (showsSecretRed(preview.pixelColor(secretCenter)) ||
        preview.pixelColor(secretCenter) != solid) {
      error = QStringLiteral("Redaction preview left the secret visible");
      return false;
    }
    if (preview.pixelColor(displacedCenter) == solid) {
      error = QStringLiteral(
          "Redaction preview drew the block at the selection offset");
      return false;
    }
    if (exported.size() != QSize(400, 300) ||
        exported.pixelColor(290, 230) != solid ||
        showsSecretRed(exported.pixelColor(290, 230)) ||
        exported.pixelColor(10, 10) != backdrop) {
      error = QStringLiteral(
          "Export redaction did not stay on the off-origin secret");
      return false;
    }
  }
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
    if (editor.armedToolForTest() != CaptureEditor::Tool::Arrow) {
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

  QTest::keyClick(&editor, Qt::Key_V);
  const QImage beforeMarquee = editor.grab().toImage();
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(120, 120));
  QTest::mouseMove(&editor, QPoint(560, 260), 20);
  application.processEvents();
  const QImage activeMarquee = editor.grab().toImage();
  bool foundBlueMarqueeEdge = false;
  for (int y = 117; y <= 123 && !foundBlueMarqueeEdge; ++y) {
    for (int x = 336; x <= 344; ++x) {
      const QColor pixel = activeMarquee.pixelColor(x, y);
      if (pixel.blue() > 180 && pixel.green() > 80 && pixel.red() < 80) {
        foundBlueMarqueeEdge = true;
        break;
      }
    }
  }
  if (!foundBlueMarqueeEdge ||
      activeMarquee.pixelColor(340, 130) ==
          beforeMarquee.pixelColor(340, 130)) {
    error = QStringLiteral("Select drag did not paint a visible marquee box");
    return false;
  }
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(560, 260));
  QTest::keyClick(&editor, Qt::Key_Delete);
  application.processEvents();
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
    if (editor.armedToolForTest() != CaptureEditor::Tool::Marker) {
      error = QStringLiteral("Marker tool did not remain active after click");
      return false;
    }
  }

  QTest::keyClick(&editor, Qt::Key_Escape);
  application.processEvents();
  if (editor.armedToolForTest() != CaptureEditor::Tool::Select) {
    error = QStringLiteral("Escape did not return to Select tool");
    return false;
  }

  QTest::keyClick(&editor, Qt::Key_R);
  // Over empty canvas: a drawing tool draws, so it shows the crosshair. (Over
  // a layer's edge it shows the move cursor, checked separately below.)
  QTest::mouseMove(&editor, QPoint(680, 480), 10);
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
    if (editor.armedToolForTest() != CaptureEditor::Tool::Rectangle) {
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
  if (editor.armedToolForTest() != CaptureEditor::Tool::Text) {
    error = QStringLiteral("Text tool did not set IBeam cursor");
    return false;
  }

  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(200, 450));
  application.processEvents();
  QTest::keyClicks(QApplication::focusWidget(), QStringLiteral("Text 1"));
  QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return, Qt::ControlModifier);
  application.processEvents();
  if (editor.armedToolForTest() != CaptureEditor::Tool::Text) {
    error = QStringLiteral(
        "Text tool did not remain active after submitting new text");
    return false;
  }

  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(350, 450));
  application.processEvents();
  QTest::keyClicks(QApplication::focusWidget(), QStringLiteral("Text 2"));
  QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return, Qt::ControlModifier);
  application.processEvents();
  if (editor.armedToolForTest() != CaptureEditor::Tool::Text) {
    error = QStringLiteral(
        "Text tool did not remain active after second text commit");
    return false;
  }

  QTest::mouseClick(&editor, Qt::RightButton, Qt::NoModifier, QPoint(100, 100));
  application.processEvents();
  if (editor.armedToolForTest() != CaptureEditor::Tool::Select) {
    error = QStringLiteral(
        "Right-click did not return from Text tool to Select tool");
    return false;
  }

  editor.close();
  return true;
}

/**
 * Exercises the cut tool end to end: tool selection/cursor, a plain click
 * being a no-op, a vertical drag removing a horizontal band (height
 * shrinks), a horizontal drag removing a vertical band (width shrinks), and
 * undo/redo restoring/reapplying both cuts. The synthetic capture is 1:1
 * (source size == previewSize), so annotation-space, logical, and native
 * source px all coincide and the editor's display scale is exactly 1.0 for
 * every selection size used here (see editImageRect()'s available-rect math:
 * a 600-wide, <=400-tall selection always fits under scale 1.0 in an 800x600
 * widget), which keeps the expected band sizes exact integers.
 */
bool runCutToolSmoke(QApplication &application, QString &error) {
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

  const QImage originalOutput = editor.renderCurrentOutput();
  if (originalOutput.isNull()) {
    error = QStringLiteral("Cut smoke: initial selection produced no output");
    return false;
  }
  const int originalWidth = originalOutput.width();
  const int originalHeight = originalOutput.height();

  QTest::keyClick(&editor, Qt::Key_X);
  application.processEvents();
  if (editor.cursor().shape() != Qt::CrossCursor) {
    error = QStringLiteral("Cut tool did not set cross cursor");
    return false;
  }

  // A plain click never crosses the drag-activation threshold, so it must
  // be a no-op.
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(300, 200));
  application.processEvents();
  if (editor.renderCurrentOutput() != originalOutput) {
    error = QStringLiteral("Plain click with the cut tool changed the output");
    return false;
  }

  // Dominant-vertical delta locks a horizontal band (rows removed): the
  // image collapses vertically, so height shrinks by the band size.
  constexpr int band1 = 60;
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(150, 200));
  QTest::mouseMove(&editor, QPoint(150, 200 + band1), 10);
  application.processEvents();
  if (editor.renderCurrentOutput() != originalOutput) {
    error = QStringLiteral("Cut preview changed the image before release");
    return false;
  }
  const QImage horizontalPreview = editor.grab().toImage();
  const QColor horizontalBand = horizontalPreview.pixelColor(450, 230);
  const QColor horizontalOutside = horizontalPreview.pixelColor(450, 180);
  if (horizontalBand == horizontalOutside ||
      horizontalBand.red() <= horizontalOutside.red()) {
    error = QStringLiteral("Horizontal cut preview did not shade its removal band");
    return false;
  }
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(150, 200 + band1));
  application.processEvents();

  const QImage afterCut1 = editor.renderCurrentOutput();
  if (afterCut1.width() != originalWidth ||
      afterCut1.height() != originalHeight - band1) {
    error = QStringLiteral(
                "Vertical drag did not shrink height by the band size: "
                "expected %1x%2, got %3x%4")
                .arg(originalWidth)
                .arg(originalHeight - band1)
                .arg(afterCut1.width())
                .arg(afterCut1.height());
    return false;
  }

  // Dominant-horizontal delta locks a vertical band (columns removed):
  // width shrinks by the band size, height stays as cut1 left it.
  constexpr int band2 = 80;
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(300, 200));
  QTest::mouseMove(&editor, QPoint(300 + band2, 200), 10);
  application.processEvents();
  if (editor.renderCurrentOutput() != afterCut1) {
    error = QStringLiteral("Second cut preview changed the image before release");
    return false;
  }
  const QImage verticalPreview = editor.grab().toImage();
  const QColor verticalBand = verticalPreview.pixelColor(340, 300);
  const QColor verticalOutside = verticalPreview.pixelColor(420, 300);
  if (verticalBand == verticalOutside ||
      verticalBand.red() <= verticalOutside.red()) {
    error = QStringLiteral("Vertical cut preview did not shade its removal band");
    return false;
  }
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(300 + band2, 200));
  application.processEvents();

  const QImage afterCut2 = editor.renderCurrentOutput();
  if (afterCut2.width() != originalWidth - band2 ||
      afterCut2.height() != originalHeight - band1) {
    error = QStringLiteral(
                "Horizontal drag did not shrink width by the band size: "
                "expected %1x%2, got %3x%4")
                .arg(originalWidth - band2)
                .arg(originalHeight - band1)
                .arg(afterCut2.width())
                .arg(afterCut2.height());
    return false;
  }

  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  const QImage afterUndo = editor.renderCurrentOutput();
  if (afterUndo.width() != originalWidth || afterUndo.height() != originalHeight) {
    error = QStringLiteral(
                "Two undoEdit calls did not restore the original size: got %1x%2")
                .arg(afterUndo.width())
                .arg(afterUndo.height());
    return false;
  }

  QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
  application.processEvents();
  QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
  application.processEvents();
  const QImage afterRedo = editor.renderCurrentOutput();
  if (afterRedo.width() != afterCut2.width() ||
      afterRedo.height() != afterCut2.height()) {
    error = QStringLiteral(
                "Two redoEdit calls did not reapply both cuts: got %1x%2")
                .arg(afterRedo.width())
                .arg(afterRedo.height());
    return false;
  }

  editor.close();
  return true;
}

bool runAsyncCaptureRegionSmoke(QApplication &application, QString &error) {
  QTemporaryDir commands;
  if (!commands.isValid()) {
    error = QStringLiteral("Could not create async capture command directory");
    return false;
  }

  const auto writeExecutable = [](const QString &path,
                                  const QByteArray &contents) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size())
      return false;
    file.close();
    return QFile::setPermissions(
        path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                  QFileDevice::ExeOwner);
  };
  const QString fakeHyprctl = QDir(commands.path()).filePath(QStringLiteral("hyprctl"));
  const QByteArray hyprctlScript =
      QByteArrayLiteral("#!/usr/bin/env bash\n"
                        "if [[ \"$1 $2\" == \"monitors -j\" ]]; then\n"
                        "  printf '%s\\n' "
                        "'[{\"focused\":true,\"scale\":1,\"width\":320,"
                        "\"height\":240,\"transform\":0,\"name\":\"TEST\","
                        "\"x\":0,\"y\":0,\"activeWorkspace\":{\"id\":7}}]'\n"
                        "else\n"
                        "  printf '[]\\n'\n"
                        "fi\n");
  if (!writeExecutable(fakeHyprctl, hyprctlScript)) {
    error = QStringLiteral("Could not create async capture commands");
    return false;
  }

  QImage source(320, 240, QImage::Format_RGB32);
  source.fill(QColor(QStringLiteral("#28405c")));
  const QString sourcePath =
      QDir(commands.path()).filePath(QStringLiteral("source.png"));
  if (!source.save(sourcePath, "PNG")) {
    error = QStringLiteral("Could not create async capture source");
    return false;
  }

  const QByteArray oldPath = qgetenv("PATH");
  const QByteArray oldCapture = qgetenv("OMASNAP_TEST_CAPTURE");
  qputenv("PATH", commands.path().toUtf8() + ':' + oldPath);
  qputenv("OMASNAP_TEST_CAPTURE", sourcePath.toUtf8());

  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 320, 240};
  capture.monitor.pixelSize = {320, 240};
  CaptureEditor editor(capture);
  editor.resize(320, 240);
  editor.show();
  application.processEvents();

  bool ready = false;
  QString captureError;
  QObject::connect(&editor, &CaptureEditor::captureReady, &editor,
                   [&ready, &captureError](bool ok, const QString &message) {
                     ready = ok;
                     captureError = message;
                   });
  editor.startCapture(CaptureEditor::CaptureMode::Region, true);

  QElapsedTimer timer;
  timer.start();
  while (!ready && timer.elapsed() < 5000) {
    application.processEvents();
    QThread::msleep(1);
  }
  if (!ready) {
    error = captureError.isEmpty()
                ? QStringLiteral("Async monitor capture did not complete")
                : captureError;
    editor.close();
    qputenv("PATH", oldPath);
    if (oldCapture.isEmpty())
      qunsetenv("OMASNAP_TEST_CAPTURE");
    else
      qputenv("OMASNAP_TEST_CAPTURE", oldCapture);
    return false;
  }

  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(40, 30));
  QTest::mouseMove(&editor, QPoint(200, 180), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(200, 180));
  application.processEvents();
  const QImage selected = editor.renderCurrentOutput();
  editor.close();
  qputenv("PATH", oldPath);
  if (oldCapture.isEmpty())
    qunsetenv("OMASNAP_TEST_CAPTURE");
  else
    qputenv("OMASNAP_TEST_CAPTURE", oldCapture);

  if (selected.size() != QSize(160, 150)) {
    error = QStringLiteral("Async capture region selection produced %1x%2")
                .arg(selected.width())
                .arg(selected.height());
    return false;
  }
  return true;
}

/** Checks that the wheel retargets the spotlight under the cursor. */
bool runSpotlightWheelSmoke(QApplication &application, QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  // Fine banding so a magnification change moves the sampled loupe pixels.
  for (int y = 0; y < capture.source.height(); ++y) {
    for (int x = 0; x < capture.source.width(); ++x) {
      const int band = ((x / 3) + (y / 5)) % 3;
      capture.source.setPixelColor(
          x, y, QColor(40 + band * 70, 60 + band * 50, 90 + band * 40));
    }
  }
  capture.previewSize = capture.source.size();

  const QString snapshotPath = temporarySnapshotPath();
  QFile::remove(snapshotPath);

  const auto armSpotlightTool = [&application](CaptureEditor &editor) {
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
    QTest::mouseMove(&editor, QPoint(700, 500), 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(700, 500));
    QTest::keyClick(&editor, Qt::Key_S);
    application.processEvents();
  };
  const auto drawSpotlight = [&application](CaptureEditor &editor) {
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(300, 250));
    QTest::mouseMove(&editor, QPoint(500, 400), 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(500, 400));
    application.processEvents();
  };
  const auto sendWheel = [&application](CaptureEditor &editor,
                                        const QPointF &position, int notches) {
    for (int notch = 0; notch < notches; ++notch) {
      QWheelEvent wheel(position, position, {}, {0, 120}, Qt::NoButton,
                        Qt::NoModifier, Qt::NoScrollPhase, false);
      QApplication::sendEvent(&editor, &wheel);
    }
    application.processEvents();
  };
  // Widget center of the spotlight drawn above, and a point clear of it.
  const QPointF overSpotlight(400, 325);
  const QPointF awayFromSpotlight(160, 160);

  CaptureEditor editor(capture);
  armSpotlightTool(editor);
  drawSpotlight(editor);
  if (editor.cursor().shape() != Qt::CrossCursor) {
    error = QStringLiteral("Spotlight tool did not stay armed after placement");
    return false;
  }
  const QImage placed = flushedSnapshot(editor, snapshotPath);
  if (placed.isNull()) {
    error = QStringLiteral("Spotlight placement did not render a snapshot");
    return false;
  }

  sendWheel(editor, overSpotlight, 4);
  const QImage adjusted = flushedSnapshot(editor, snapshotPath);
  if (adjusted.isNull() || adjusted == placed) {
    error = QStringLiteral(
        "Wheel over a placed spotlight did not change its magnification");
    return false;
  }
  for (int notch = 0; notch < 4; ++notch)
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath) != placed) {
    error = QStringLiteral("Undo did not restore the spotlight magnification");
    return false;
  }
  for (int notch = 0; notch < 4; ++notch)
    QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath) != adjusted) {
    error = QStringLiteral("Redo did not restore the adjusted magnification");
    return false;
  }

  sendWheel(editor, awayFromSpotlight, 4);
  if (flushedSnapshot(editor, snapshotPath) != adjusted) {
    error = QStringLiteral(
        "Wheel away from a spotlight changed the placed spotlight");
    return false;
  }
  editor.close();

  // Away from any spotlight the wheel still moves the default that the next
  // spotlight inherits.
  CaptureEditor defaultEditor(capture);
  armSpotlightTool(defaultEditor);
  sendWheel(defaultEditor, awayFromSpotlight, 4);
  drawSpotlight(defaultEditor);
  const QImage inherited = flushedSnapshot(defaultEditor, snapshotPath);
  if (inherited.isNull() || inherited == placed) {
    error = QStringLiteral(
        "Wheel away from any spotlight did not move the default magnification");
    return false;
  }
  defaultEditor.close();
  QFile::remove(snapshotPath);
  return true;
}

/** Window crop, undo/redo replay, persist+reload, and redaction order. */
bool runOpLogSmoke(QApplication &application, QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  const QColor backdrop(QStringLiteral("#204060"));
  const QColor secret(QStringLiteral("#ff2040"));
  capture.source.fill(backdrop);
  {
    QPainter painter(&capture.source);
    painter.fillRect(QRect(200, 200, 100, 60), secret);
  }
  capture.previewSize = capture.source.size();
  capture.windows = {
      {{80, 80, 300, 220}, QStringLiteral("w1"), QStringLiteral("first")}};

  {
    const QImage original = capture.source.copy();
    CaptureEditor editor(capture, CaptureEditor::CaptureMode::Window);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(200, 160));
    application.processEvents();
    if (editor.captureData().source != original ||
        editor.currentSelection() != QRectF(80, 80, 300, 220) ||
        editor.operationIndex() < 1) {
      error = QStringLiteral("Window pick recaptured or did not crop the source");
      return false;
    }
    editor.close();
  }

  CaptureEditor editor(capture, CaptureEditor::CaptureMode::Fullscreen);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();
  QTest::keyClick(&editor, Qt::Key_D);
  QTest::keyClick(&editor, Qt::Key_D);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(230, 214));
  QTest::mouseMove(&editor, QPoint(333, 285), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(333, 285));
  QTest::keyClick(&editor, Qt::Key_5);
  QTest::keyClick(&editor, Qt::Key_A);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(250, 250));
  QTest::mouseMove(&editor, QPoint(312, 250), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(312, 250));
  application.processEvents();
  if (!editor.waitForSnapshot()) {
    error = QStringLiteral("Op-log working document did not persist");
    return false;
  }
  const QImage annotated = editor.renderCurrentOutput();
  const int annotatedIndex = editor.operationIndex();
  if (annotatedIndex < 2) {
    error = QStringLiteral("Annotation tools did not append to the op log");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  const QImage undone = editor.renderCurrentOutput();
  if (editor.operationIndex() != annotatedIndex - 1 || undone == annotated) {
    error = QStringLiteral("Undo did not replay the op log");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
  application.processEvents();
  if (editor.operationIndex() != annotatedIndex ||
      editor.renderCurrentOutput() != annotated) {
    error = QStringLiteral("Redo did not replay the op log");
    return false;
  }

  QTemporaryDir directory;
  if (!directory.isValid()) {
    error = QStringLiteral("Could not create op-log reload directory");
    return false;
  }
  const QString sourceCopy =
      QDir(directory.path()).filePath(QStringLiteral("working.png"));
  const QString logCopy = operationLogPath(sourceCopy);
  if (!QFile::copy(editor.workingSourcePath(), sourceCopy) ||
      !QFile::copy(editor.workingLogPath(), logCopy)) {
    error = QStringLiteral("Could not copy the working document");
    return false;
  }
  editor.close();

  QImage restoredSource;
  if (!restoredSource.load(sourceCopy)) {
    error = QStringLiteral("Could not reload the persisted source image");
    return false;
  }
  OperationLog log;
  if (!loadOperationLog(logCopy, log, error))
    return false;
  CaptureData restored;
  restored.source = restoredSource;
  restored.previewSize = restoredSource.size();
  restored.monitor.scale = 1.0;
  restored.monitor.pixelSize = restoredSource.size();
  restored.monitor.geometry = QRect(QPoint(), restoredSource.size());
  CaptureEditor reopened(restored, CaptureEditor::CaptureMode::File,
                         QuickOutputMode::None, log);
  reopened.resize(800, 600);
  reopened.show();
  application.processEvents();
  const QImage reloaded = reopened.renderCurrentOutput();
  if (reopened.operationIndex() != annotatedIndex ||
      reloaded.convertToFormat(QImage::Format_ARGB32) !=
          annotated.convertToFormat(QImage::Format_ARGB32)) {
    error = QStringLiteral("Reloading the op log did not restore the view");
    return false;
  }
  QTest::keyClick(&reopened, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (reopened.renderCurrentOutput().convertToFormat(QImage::Format_ARGB32) !=
      undone.convertToFormat(QImage::Format_ARGB32)) {
    error = QStringLiteral("Reloaded undo did not keep working");
    return false;
  }
  QTest::keyClick(&reopened, Qt::Key_Y, Qt::ControlModifier);
  application.processEvents();

  const QImage replayed = reopened.renderCurrentOutput().convertToFormat(
      QImage::Format_ARGB32);
  const QColor covered = replayed.pixelColor(220, 210);
  const QColor overlay = replayed.pixelColor(260, 230);
  if (covered == secret || overlay == secret) {
    error = QStringLiteral("Replay leaked source pixels through redaction");
    return false;
  }
  if (covered != QColor(QStringLiteral("#121216"))) {
    error = QStringLiteral("Replay did not keep the redaction layer (%1)")
                .arg(covered.name());
    return false;
  }
  if (overlay != QColor(QStringLiteral("#0a84ff"))) {
    error = QStringLiteral(
                "Replay did not keep default-layer annotations above "
                "redaction (%1)")
                .arg(overlay.name());
    return false;
  }
  reopened.close();
  return true;
}

/** Quotes the same way sendCaptureNotification builds --exec. */
bool runShellQuoteCheck(QString &error) {
  if (shellQuote(QStringLiteral("omasnap")) != QStringLiteral("'omasnap'")) {
    error = QStringLiteral("shellQuote did not wrap a simple token");
    return false;
  }
  if (shellQuote(QStringLiteral("omasnap /tmp/a.png")) !=
      QStringLiteral("'omasnap /tmp/a.png'")) {
    error = QStringLiteral("shellQuote did not keep spaces inside quotes");
    return false;
  }
  if (shellQuote(QStringLiteral("it's")) != QStringLiteral("'it'\"'\"'s'")) {
    error = QStringLiteral("shellQuote did not escape a single quote (%1)")
                .arg(shellQuote(QStringLiteral("it's")));
    return false;
  }
  sendCaptureNotification(QStringLiteral("smoke"));
  return true;
}

/**
 * Filling the 100-op cap used to drop the leading Crop, so replay started at
 * the full monitor while later annotations stayed in cropped space.
 */
bool runOpLogCapKeepsLeadingCrop(QApplication &application, QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  const QColor outside(QStringLiteral("#102030"));
  const QColor inside(QStringLiteral("#e8c040"));
  capture.source.fill(outside);
  {
    QPainter painter(&capture.source);
    painter.fillRect(QRect(80, 80, 300, 220), inside);
  }
  capture.previewSize = capture.source.size();

  const QRectF crop(80, 80, 300, 220);
  OperationLog log;
  Operation cropOp;
  cropOp.type = Operation::Type::Crop;
  cropOp.crop = crop;
  log.ops.push_back(cropOp);
  for (int n = 1; n <= 99; ++n) {
    Annotation marker;
    marker.kind = Annotation::Kind::Marker;
    marker.start = QPointF(40, 40);
    marker.number = n;
    marker.color = QColor(QStringLiteral("#ff375f"));
    marker.size = 4.0;
    marker.id = static_cast<quint64>(n);
    Operation annotate;
    annotate.type = Operation::Type::Annotate;
    annotate.annotations = {marker};
    log.ops.push_back(std::move(annotate));
  }
  log.index = log.ops.size();
  log.nextId = 100;
  log.nextMarker = 100;

  CaptureEditor editor(capture, CaptureEditor::CaptureMode::File,
                       QuickOutputMode::None, log);
  editor.setSuppressSnapshots(true);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();
  if (editor.currentSelection() != crop || editor.operationLog().isEmpty() ||
      editor.operationLog().constFirst().type != Operation::Type::Crop) {
    error = QStringLiteral("Loaded log did not keep the leading crop");
    return false;
  }

  QTest::keyClick(&editor, Qt::Key_C);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(400, 300));
  application.processEvents();

  if (editor.operationLog().size() != 100 ||
      editor.operationLog().constFirst().type != Operation::Type::Crop ||
      editor.currentSelection() != crop) {
    error = QStringLiteral("Op-log cap dropped the initial crop");
    return false;
  }

  const QImage exported = editor.renderCurrentOutput();
  if (exported.size() != QSize(300, 220)) {
    error = QStringLiteral("Capped replay left the full monitor (%1x%2)")
                .arg(exported.width())
                .arg(exported.height());
    return false;
  }
  if (exported.pixelColor(10, 10) != inside) {
    error = QStringLiteral("Capped replay did not stay in cropped space");
    return false;
  }

  QTemporaryDir directory;
  if (!directory.isValid()) {
    error = QStringLiteral("Could not create op-log cap directory");
    return false;
  }
  const QString logPath =
      QDir(directory.path()).filePath(QStringLiteral("capped.json"));
  OperationLog persisted;
  persisted.ops = editor.operationLog();
  persisted.index = editor.operationIndex();
  persisted.nextId = 101;
  persisted.nextMarker = 101;
  if (!saveOperationLog(logPath, persisted, error))
    return false;
  OperationLog reloaded;
  if (!loadOperationLog(logPath, reloaded, error))
    return false;
  if (reloaded.ops.isEmpty() ||
      reloaded.ops.constFirst().type != Operation::Type::Crop ||
      reloaded.ops.constFirst().crop != crop) {
    error = QStringLiteral("Persisted capped log lost the leading crop");
    return false;
  }
  editor.close();
  return true;
}

} // namespace
/** Runs the interaction and rendering smoke checks. */
/** Runs the interaction and rendering smoke checks. */
/** Runs the interaction and rendering smoke checks. */
/** Runs the interaction and rendering smoke checks. */
/** Runs the interaction and rendering smoke checks. */
/** Runs the interaction and rendering smoke checks. */
/** Runs the interaction and rendering smoke checks. */
/** Runs the interaction and rendering smoke checks. */
/** Checks that a modifier left over from the launch binding is not believed.
 *  A binding with a modifier in it, such as the README's ALT + SHIFT + 4,
 *  leaves that modifier held as the overlay takes keyboard focus. Its release
 *  goes to the compositor's binding rather than to us, so Qt keeps reporting
 *  it held, and every arrow snaps to 45° until some later key event refreshes
 *  the state. Nothing here presses a key before the check, because that is
 *  exactly what a fresh capture looks like. */
bool runStuckModifierSmoke(QApplication &application, QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#182030")));
  capture.previewSize = capture.source.size();
  const QString snapshotPath = temporarySnapshotPath();

  CaptureEditor editor(capture);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();
  // Mouse only from here: the region, then the arrow tool from the toolbar
  // above the submenu gutter.
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
  QTest::mouseMove(&editor, QPoint(700, 500), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(700, 500));
  application.processEvents();
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(60, 43));
  application.processEvents();

  // A shallow drag, with the stale Shift the compositor still reports. It must
  // draw where it was dragged: snapped to 45°, this arrow would come out flat.
  const QImage before = flushedSnapshot(editor, snapshotPath);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::ShiftModifier, QPoint(200, 400));
  QTest::mouseMove(&editor, QPoint(400, 370), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::ShiftModifier,
                      QPoint(400, 370));
  application.processEvents();
  const QImage drawn = flushedSnapshot(editor, snapshotPath);
  if (drawn == before) {
    error = QStringLiteral("Stuck-modifier smoke: nothing was drawn");
    return false;
  }
  // Annotation coords: the shaft runs from (100,295) to (300,265). At x 200 it
  // sits at y 280; a 45° snap would have flattened it to 295.
  const auto ink = [&drawn](int x, int y) {
    for (int dy = -3; dy <= 3; ++dy) {
      for (int dx = -3; dx <= 3; ++dx) {
        if (drawn.pixelColor(x + dx, y + dy) !=
            QColor(QStringLiteral("#182030")))
          return true;
      }
    }
    return false;
  };
  if (!ink(200, 280) || ink(200, 295)) {
    error = QStringLiteral("A modifier left over from the launch chord was "
                           "believed: the first stroke snapped on its own");
    return false;
  }
  // Once a key has been pressed the reported state can be trusted again, so
  // Shift means Shift: the same drag now snaps flat.
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  QTest::mousePress(&editor, Qt::LeftButton, Qt::ShiftModifier, QPoint(200, 400));
  QTest::mouseMove(&editor, QPoint(400, 370), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::ShiftModifier,
                      QPoint(400, 370));
  application.processEvents();
  const QImage snapped = flushedSnapshot(editor, snapshotPath);
  const auto snappedInk = [&snapped](int x, int y) {
    for (int dy = -3; dy <= 3; ++dy) {
      for (int dx = -3; dx <= 3; ++dx) {
        if (snapped.pixelColor(x + dx, y + dy) !=
            QColor(QStringLiteral("#182030")))
          return true;
      }
    }
    return false;
  };
  if (!snappedInk(200, 295)) {
    error = QStringLiteral("Shift stopped meaning Shift after a key event");
    return false;
  }
  editor.close();
  QFile::remove(snapshotPath);
  return true;
}

/** Checks that a spotlight resizes by its corner handle. The handle sits on
 *  the layer's bounds, which for an elliptical spotlight is outside the shape
 *  the hit test uses, so a press there used to read as empty canvas and start
 *  a marquee. */
bool runSpotlightHandleSmoke(QApplication &application, QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  // Fine banding, so a spotlight's lens is visible against the surround.
  for (int y = 0; y < capture.source.height(); ++y) {
    for (int x = 0; x < capture.source.width(); ++x) {
      const int band = ((x / 3) + (y / 5)) % 3;
      capture.source.setPixelColor(
          x, y, QColor(40 + band * 70, 60 + band * 50, 90 + band * 40));
    }
  }
  capture.previewSize = capture.source.size();
  const QString snapshotPath = temporarySnapshotPath();

  CaptureEditor editor(capture);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
  QTest::mouseMove(&editor, QPoint(700, 500), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(700, 500));
  application.processEvents();

  // A spotlight from (300,250) to (500,400), then selected by its middle.
  QTest::keyClick(&editor, Qt::Key_S);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(300, 250));
  QTest::mouseMove(&editor, QPoint(500, 400), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(500, 400));
  application.processEvents();
  QTest::keyClick(&editor, Qt::Key_V);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(400, 325));
  application.processEvents();

  const QImage placed = flushedSnapshot(editor, snapshotPath);
  if (placed.isNull()) {
    error = QStringLiteral("Spotlight handle smoke: nothing was rendered");
    return false;
  }
  // Its bottom-right handle is at widget (500,400), on the bounds and outside
  // the ellipse. Dragging it in must resize the spotlight.
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(500, 400));
  QTest::mouseMove(&editor, QPoint(430, 340), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(430, 340));
  application.processEvents();
  const QImage resized = flushedSnapshot(editor, snapshotPath);
  if (resized == placed) {
    error = QStringLiteral("Dragging a spotlight's handle did nothing");
    return false;
  }
  // A resize keeps the opposite corner: moving it by the same delta would
  // have produced a different picture.
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(400, 325));
  QTest::mouseMove(&editor, QPoint(330, 265), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(330, 265));
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath) == resized) {
    error = QStringLiteral("Dragging a spotlight's handle moved it instead of "
                           "resizing it");
    return false;
  }
  editor.close();
  QFile::remove(snapshotPath);
  return true;
}

/** Runs the interaction and rendering smoke checks. */
bool runSelectOutsideCanvasSmoke(QApplication &application, QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#182030")));
  capture.previewSize = capture.source.size();
  const QString snapshotPath = temporarySnapshotPath();

  CaptureEditor editor(capture);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
  QTest::mouseMove(&editor, QPoint(700, 500), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(700, 500));
  application.processEvents();
  const QRectF selection(100, 100, 600, 400);
  const auto expected = [&](const QVector<Annotation> &annotations) {
    return renderCapture(capture, selection, annotations,
                         BackgroundStyle::None);
  };
  const auto snapshotMatches = [&](const QImage &image) {
    return flushedSnapshot(editor, snapshotPath)
                   .convertToFormat(image.format()) == image;
  };
  const auto drag = [&](const QPoint &from, const QPoint &to) {
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, from);
    QTest::mouseMove(&editor, to, 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, to);
    application.processEvents();
  };

  // A rectangle at annotation (400,195)-(550,345); the widget offset is
  // (100,105). Drag it 100 px right so its right side leaves the canvas.
  QTest::keyClick(&editor, Qt::Key_R);
  drag(QPoint(500, 300), QPoint(650, 450));
  Annotation rectangle;
  rectangle.kind = Annotation::Kind::Rectangle;
  rectangle.start = {400, 195};
  rectangle.end = {550, 345};
  rectangle.color = QColor(QStringLiteral("#ff375f"));
  rectangle.size = 4;
  if (!snapshotMatches(expected({rectangle}))) {
    error = QStringLiteral("Outside-canvas smoke: rectangle did not render");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_V);
  // Press the top edge, clear of the corner and mid-side handles (eight on
  // a box), so this is a move.
  drag(QPoint(540, 300), QPoint(640, 300));
  Annotation shifted = rectangle;
  shifted.start.rx() += 100;
  shifted.end.rx() += 100;
  if (!snapshotMatches(expected({shifted}))) {
    error = QStringLiteral("Outside-canvas smoke: rectangle did not move");
    return false;
  }

  // Its bottom-right handle now sits outside the canvas (widget (750,450));
  // dragging it back in resizes the layer.
  drag(QPoint(750, 450), QPoint(650, 400));
  Annotation resized = shifted;
  resized.end = {550, 295};
  if (!snapshotMatches(expected({resized}))) {
    error = QStringLiteral(
        "Dragging a handle that lies outside the canvas did not resize");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (!snapshotMatches(expected({shifted}))) {
    error = QStringLiteral("Undo did not restore the outside-handle resize");
    return false;
  }

  // Its right edge is outside the canvas too (widget x 750); grab off the
  // mid-side handle so this is a move, not a one-axis resize.
  drag(QPoint(750, 340), QPoint(650, 340));
  if (!snapshotMatches(expected({rectangle}))) {
    error = QStringLiteral(
        "Grabbing a selected layer outside the canvas did not move it");
    return false;
  }

  // Outside the canvas, anything but the selected layer stays inert: a
  // click on the surround neither deselects nor changes anything, so
  // Delete still removes the layer.
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(760, 150));
  application.processEvents();
  if (!snapshotMatches(expected({rectangle}))) {
    error = QStringLiteral("A click outside the canvas changed the capture");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Delete);
  application.processEvents();
  if (!snapshotMatches(expected({}))) {
    error = QStringLiteral("A click outside the canvas deselected the layer");
    return false;
  }
  editor.close();
  QFile::remove(snapshotPath);
  return true;
}

/** Runs the interaction and rendering smoke checks. */
/** Checks that sampling a color is not a change of tool: the eyedropper
 *  hands back whatever was in hand, and recolors the layer that was selected
 *  rather than dropping the selection with it. */
bool runEyedropperSmoke(QApplication &application, QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#182030")));
  // A patch of a color nothing else uses, to sample from.
  QPainter painter(&capture.source);
  painter.fillRect(QRect(560, 380, 120, 120), QColor(QStringLiteral("#12b886")));
  painter.fillRect(QRect(560, 140, 120, 120), QColor(QStringLiteral("#f59f00")));
  painter.end();
  capture.previewSize = capture.source.size();
  const QString snapshotPath = temporarySnapshotPath();

  CaptureEditor editor(capture);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
  QTest::mouseMove(&editor, QPoint(700, 500), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(700, 500));
  application.processEvents();

  // An arrow, selected, then a color sampled from the patch.
  QTest::keyClick(&editor, Qt::Key_A);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(200, 200));
  QTest::mouseMove(&editor, QPoint(360, 300), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(360, 300));
  application.processEvents();
  QTest::keyClick(&editor, Qt::Key_V);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(280, 250));
  application.processEvents();
  const QImage before = flushedSnapshot(editor, snapshotPath);
  QTest::keyClick(&editor, Qt::Key_A); // back to the arrow tool
  QTest::keyClick(&editor, Qt::Key_I); // eyedropper
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(620, 440));
  application.processEvents();
  const QImage recolored = flushedSnapshot(editor, snapshotPath);
  if (recolored == before) {
    error = QStringLiteral("Sampling a color did not recolor the selected "
                           "layer");
    return false;
  }
  // The layer is still the selected one, so a second color lands on it too.
  // That is the half a cleared selection would break: the recolor reads the
  // selected index, and dropping it makes every later sample a no-op.
  QTest::keyClick(&editor, Qt::Key_I);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(620, 200));
  application.processEvents();
  const QImage twice = flushedSnapshot(editor, snapshotPath);
  if (twice == recolored) {
    error = QStringLiteral("Sampling a color dropped the selection");
    return false;
  }
  // The tool that was in hand is still in hand: this drag draws a second
  // arrow. With the tool dropped for the select tool it would only have
  // marqueed, leaving the capture as it was.
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(200, 420));
  QTest::mouseMove(&editor, QPoint(340, 470), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(340, 470));
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath) == twice) {
    error = QStringLiteral("Sampling a color took the tool away");
    return false;
  }
  editor.close();
  QFile::remove(snapshotPath);
  return true;
}

/** Diagonal submenu approaches must not dismiss an already-open popover. */
bool runSubmenuSelectionTriangleSmoke(QApplication &application, QString &error) {
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

  QTest::mouseMove(&editor, QPoint(435, 43), 10);
  application.processEvents();
  if (!editor.colorPaletteOpenForTest()) {
    error = QStringLiteral("Hovering the palette button did not open its submenu");
    return false;
  }

  QTest::mouseMove(&editor, QPoint(417, 53), 10);
  application.processEvents();
  if (!editor.colorPaletteOpenForTest()) {
    error = QStringLiteral("Diagonal movement inside the submenu triangle closed the palette");
    return false;
  }

  QTest::mouseMove(&editor, QPoint(350, 78), 10);
  application.processEvents();
  if (!editor.colorPaletteOpenForTest()) {
    error = QStringLiteral("Moving from the submenu triangle into the palette closed it");
    return false;
  }

  QTest::mouseMove(&editor, QPoint(180, 43), 10);
  application.processEvents();
  if (editor.colorPaletteOpenForTest()) {
    error = QStringLiteral("Movement away from the submenu triangle kept the palette open");
    return false;
  }

  editor.close();
  return true;
}

/** Runs the interaction and rendering smoke checks. */
bool runEllipseRenderingCheck(QString &error) {
  error = QStringLiteral("Ellipse rendering check failed");
  CaptureData capture;
  capture.monitor.scale = 1.0;
  capture.monitor.pixelSize = {200, 100};
  capture.source = QImage(200, 100, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(Qt::transparent);
  capture.previewSize = capture.source.size();

  Annotation ellipse;
  ellipse.kind = Annotation::Kind::Ellipse;
  ellipse.color = QColor(QStringLiteral("#ff375f"));
  ellipse.size = 4;
  ellipse.start = {20, 20};
  ellipse.end = {180, 80};
  const QImage rendered = renderCapture(capture, QRectF(0, 0, 200, 100),
                                        {ellipse}, BackgroundStyle::None);
  if (rendered.isNull())
    return false;
  // Stroke lands on the ellipse (bounding-box edge midpoints)...
  for (const QPoint &onStroke :
       {QPoint(100, 20), QPoint(100, 80), QPoint(20, 50), QPoint(180, 50)}) {
    if (rendered.pixelColor(onStroke).alpha() < 200) {
      error = QStringLiteral("Ellipse stroke missing at bounding-box edge");
      return false;
    }
  }
  // ...but not on the bounding-box corners (a rectangle would paint these)...
  for (const QPoint &corner :
       {QPoint(22, 22), QPoint(178, 22), QPoint(22, 78), QPoint(178, 78)}) {
    if (rendered.pixelColor(corner).alpha() != 0) {
      error = QStringLiteral("Ellipse painted its bounding-box corner");
      return false;
    }
  }
  // ...and the interior stays hollow.
  if (rendered.pixelColor(100, 50).alpha() != 0) {
    error = QStringLiteral("Ellipse interior was not hollow");
    return false;
  }
  return true;
}

bool runEllipseToolSmoke(QApplication &application, QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#182030")));
  capture.previewSize = capture.source.size();
  const QString snapshotPath = temporarySnapshotPath();

  CaptureEditor editor(capture);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();

  // 600x400 selection shown 1:1 at widget offset (100, 105).
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
  QTest::mouseMove(&editor, QPoint(700, 500), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(700, 500));
  application.processEvents();
  const QRectF selection(100, 100, 600, 400);
  const auto expected = [&](const QVector<Annotation> &annotations) {
    return renderCapture(capture, selection, annotations,
                         BackgroundStyle::None);
  };
  const auto snapshotMatches = [&](const QImage &image) {
    return flushedSnapshot(editor, snapshotPath)
                   .convertToFormat(image.format()) == image;
  };
  const auto ellipseAnnotation = [](const QPointF &start, const QPointF &end) {
    Annotation ellipse;
    ellipse.kind = Annotation::Kind::Ellipse;
    ellipse.start = start;
    ellipse.end = end;
    ellipse.color = QColor(QStringLiteral("#ff375f"));
    ellipse.size = 4;
    return ellipse;
  };

  QTest::keyClick(&editor, Qt::Key_E);
  application.processEvents();
  if (editor.cursor().shape() != Qt::CrossCursor) {
    error = QStringLiteral("E did not select the ellipse tool");
    return false;
  }
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(200, 200));
  QTest::mouseMove(&editor, QPoint(400, 300), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(400, 300));
  application.processEvents();
  const Annotation ellipse = ellipseAnnotation({100, 95}, {300, 195});
  if (!snapshotMatches(expected({ellipse}))) {
    error = QStringLiteral("Dragged ellipse did not render as an ellipse");
    return false;
  }
  if (editor.cursor().shape() != Qt::CrossCursor) {
    error = QStringLiteral("Ellipse tool did not stay active after drawing");
    return false;
  }

  // Shift keeps the bounding box 1:1 (a circle), extending the short axis.
  QTest::mousePress(&editor, Qt::LeftButton, Qt::ShiftModifier,
                    QPoint(500, 200));
  QTest::mouseMove(&editor, QPoint(560, 300), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::ShiftModifier,
                      QPoint(560, 300));
  application.processEvents();
  const Annotation circle = ellipseAnnotation({400, 95}, {500, 195});
  if (!snapshotMatches(expected({ellipse, circle}))) {
    error = QStringLiteral("Shift-drag did not create a circle");
    return false;
  }

  // Hollow hit-test: the bounding-box corner and the interior miss, the
  // stroke hits (Delete only removes a selected layer).
  QTest::keyClick(&editor, Qt::Key_V);
  application.processEvents();
  for (const QPoint &miss : {QPoint(203, 203), QPoint(300, 250)}) {
    QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, miss);
    QTest::keyClick(&editor, Qt::Key_Delete);
    application.processEvents();
    if (!snapshotMatches(expected({ellipse, circle}))) {
      error = QStringLiteral("Ellipse hit-test selected outside its stroke");
      return false;
    }
  }
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(250, 200));
  QTest::keyClick(&editor, Qt::Key_Delete);
  application.processEvents();
  if (!snapshotMatches(expected({circle}))) {
    error = QStringLiteral("Ellipse stroke click did not select the layer");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (!snapshotMatches(expected({ellipse, circle}))) {
    error = QStringLiteral("Undo did not restore the deleted ellipse");
    return false;
  }

  // Selected ellipse moves with its whole (hollow) body like other layers.
  // Off the mid-side handle (300,200) so this is a move, not a resize.
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(250, 200));
  QTest::mouseMove(&editor, QPoint(270, 230), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(270, 230));
  application.processEvents();
  if (!snapshotMatches(
          expected({ellipseAnnotation({120, 125}, {320, 225}), circle}))) {
    error = QStringLiteral("Selected ellipse did not move");
    return false;
  }

  // The grouped shape submenu arms the ellipse tool without a separate
  // top-level toolbar slot.
  QTest::keyClick(&editor, Qt::Key_Escape);
  application.processEvents();
  if (editor.cursor().shape() != Qt::ArrowCursor) {
    error = QStringLiteral("Escape did not return to Select");
    return false;
  }
  constexpr qreal toolbarWidth = 840.0;
  const qreal scale = std::min<qreal>(1.0, (800.0 - 16.0) / toolbarWidth);
  const qreal toolbarX = (800.0 - toolbarWidth * scale) / 2.0;
  const QPointF button(toolbarX + (6 * 40 + 18) * scale,
                       105 - 36 * scale - 46 + 18 * scale);
  QTest::mouseMove(&editor, button.toPoint(), 10);
  application.processEvents();
  if (!editor.shapeMenuOpenForTest()) {
    error = QStringLiteral("Hovering the shape button did not open its submenu");
    return false;
  }
  const QPoint ellipseButton(qRound(button.x() - 2), qRound(button.y() + 36));
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, ellipseButton);
  QTest::mouseMove(&editor, QPoint(300, 300), 20);
  application.processEvents();
  if (editor.cursor().shape() != Qt::CrossCursor) {
    error = QStringLiteral("Ellipse submenu button did not arm the tool");
    return false;
  }

  editor.close();
  QFile::remove(snapshotPath);
  return true;
}

bool runShapeFillRenderingCheck(QString &error) {
  error = QStringLiteral("Shape fill rendering check failed");
  CaptureData capture;
  capture.monitor.scale = 1.0;
  capture.monitor.pixelSize = {200, 100};
  capture.source = QImage(200, 100, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(Qt::transparent);
  capture.previewSize = capture.source.size();
  const auto render = [&](Annotation shape) {
    shape.color = QColor(QStringLiteral("#ff375f"));
    shape.size = 4;
    shape.start = {20, 20};
    shape.end = {180, 80};
    return renderCapture(capture, QRectF(0, 0, 200, 100), {shape},
                         BackgroundStyle::None);
  };

  Annotation filledRectangle;
  filledRectangle.kind = Annotation::Kind::Rectangle;
  filledRectangle.filled = true;
  const QImage filled = render(filledRectangle);
  if (filled.pixelColor(100, 50).alpha() != 255 ||
      filled.pixelColor(21, 21).alpha() != 255 ||
      filled.pixelColor(10, 50).alpha() != 0) {
    error = QStringLiteral("Filled rectangle did not paint a flat silhouette");
    return false;
  }

  Annotation roundedRectangle;
  roundedRectangle.kind = Annotation::Kind::Rectangle;
  roundedRectangle.cornerRadius = 12;
  const QImage rounded = render(roundedRectangle);
  if (rounded.pixelColor(21, 21).alpha() != 0 ||
      rounded.pixelColor(100, 20).alpha() < 200 ||
      rounded.pixelColor(20, 50).alpha() < 200 ||
      rounded.pixelColor(100, 50).alpha() != 0) {
    error = QStringLiteral("Rounded rectangle did not clip its corners");
    return false;
  }

  Annotation filledEllipse;
  filledEllipse.kind = Annotation::Kind::Ellipse;
  filledEllipse.filled = true;
  const QImage ellipse = render(filledEllipse);
  if (ellipse.pixelColor(100, 50).alpha() != 255 ||
      ellipse.pixelColor(22, 22).alpha() != 0) {
    error = QStringLiteral("Filled ellipse did not fill its silhouette only");
    return false;
  }

  // The selection box follows the corners, so the radius can be seen while it
  // is being set rather than judged against a square frame around it.
  Annotation roundedBox;
  roundedBox.kind = Annotation::Kind::Rectangle;
  roundedBox.cornerRadius = 8.0;
  if (selectionBoundsRadius(roundedBox, 4.0) != 12.0) {
    error = QStringLiteral("Selection box did not stay concentric with a "
                           "rounded rectangle");
    return false;
  }
  Annotation squareBox;
  squareBox.kind = Annotation::Kind::Rectangle;
  if (selectionBoundsRadius(squareBox, 4.0) != 0.0) {
    error = QStringLiteral("Selection box rounded a square rectangle");
    return false;
  }
  Annotation ellipseBox;
  ellipseBox.kind = Annotation::Kind::Ellipse;
  ellipseBox.cornerRadius = 8.0;
  if (selectionBoundsRadius(ellipseBox, 4.0) != 0.0) {
    error = QStringLiteral("Selection box rounded a kind that has no corners");
    return false;
  }
  return true;
}

bool runShapeFillToolSmoke(QApplication &application, QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#182030")));
  capture.previewSize = capture.source.size();
  const QString snapshotPath = temporarySnapshotPath();

  CaptureEditor editor(capture);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
  QTest::mouseMove(&editor, QPoint(700, 500), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(700, 500));
  application.processEvents();
  const QRectF selection(100, 100, 600, 400);
  const auto expected = [&](const QVector<Annotation> &annotations) {
    return renderCapture(capture, selection, annotations,
                         BackgroundStyle::None);
  };
  const auto snapshotMatches = [&](const QImage &image) {
    return flushedSnapshot(editor, snapshotPath)
                   .convertToFormat(image.format()) == image;
  };
  const auto shape = [](Annotation::Kind kind, const QPointF &start,
                        const QPointF &end, bool filled, qreal radius = 0) {
    Annotation annotation;
    annotation.kind = kind;
    annotation.start = start;
    annotation.end = end;
    annotation.filled = filled;
    annotation.cornerRadius = radius;
    annotation.color = QColor(QStringLiteral("#ff375f"));
    annotation.size = 4;
    return annotation;
  };
  const auto drag = [&](const QPoint &from, const QPoint &to) {
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, from);
    QTest::mouseMove(&editor, to, 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, to);
    application.processEvents();
  };
  const auto wheel = [&](int steps,
                         Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    for (int step = 0; step < std::abs(steps); ++step) {
      QWheelEvent event(QPointF(400, 300), QPointF(400, 300), {},
                        {0, steps > 0 ? 120 : -120}, Qt::NoButton, modifiers,
                        Qt::NoScrollPhase, false);
      QApplication::sendEvent(&editor, &event);
    }
    application.processEvents();
  };
  const auto shapeSized = [&shape](Annotation::Kind kind, const QPointF &start,
                                   const QPointF &end, bool filled,
                                   qreal radius, qreal size) {
    Annotation annotation = shape(kind, start, end, filled, radius);
    annotation.size = size;
    return annotation;
  };

  // R arms a hollow rectangle; the wheel sets its stroke size like every
  // other tool; R again toggles fill for the next shapes.
  QTest::keyClick(&editor, Qt::Key_R);
  wheel(1);
  drag(QPoint(200, 200), QPoint(400, 300));
  const Annotation hollow = shapeSized(Annotation::Kind::Rectangle, {100, 95},
                                       {300, 195}, false, 0, 5);
  if (!snapshotMatches(expected({hollow}))) {
    error = QStringLiteral("Rectangle did not start hollow at wheel size 5");
    return false;
  }
  wheel(-1);
  QTest::keyClick(&editor, Qt::Key_R);
  drag(QPoint(200, 350), QPoint(400, 450));
  const Annotation filled =
      shape(Annotation::Kind::Rectangle, {100, 245}, {300, 345}, true);
  if (!snapshotMatches(expected({hollow, filled}))) {
    error = QStringLiteral("R again did not fill the next rectangle");
    return false;
  }

  // Alt+wheel rounds the corners of new rectangles (2 px per notch, 0–24);
  // the plain wheel keeps meaning stroke size, as for every other tool.
  wheel(6, Qt::AltModifier);
  drag(QPoint(450, 200), QPoint(650, 300));
  const Annotation rounded =
      shape(Annotation::Kind::Rectangle, {350, 95}, {550, 195}, true, 12);
  if (!snapshotMatches(expected({hollow, filled, rounded}))) {
    error = QStringLiteral("Alt+wheel did not round the next rectangle");
    return false;
  }
  wheel(-20, Qt::AltModifier);
  drag(QPoint(450, 350), QPoint(650, 450));
  const Annotation squareAgain =
      shape(Annotation::Kind::Rectangle, {350, 245}, {550, 345}, true, 0);
  if (!snapshotMatches(expected({hollow, filled, rounded, squareAgain}))) {
    error = QStringLiteral("Alt+wheel did not clamp the corner radius to 0");
    return false;
  }

  // Ellipses share the fill flag; E again toggles it back to hollow.
  QTest::keyClick(&editor, Qt::Key_E);
  QTest::keyClick(&editor, Qt::Key_E);
  drag(QPoint(200, 480), QPoint(300, 500));
  const Annotation ellipse =
      shape(Annotation::Kind::Ellipse, {100, 375}, {200, 395}, false);
  if (!snapshotMatches(
          expected({hollow, filled, rounded, squareAgain, ellipse}))) {
    error = QStringLiteral("E again did not toggle the shared fill off");
    return false;
  }

  // Filled shapes hit anywhere inside; hollow ones still only on the band.
  QTest::keyClick(&editor, Qt::Key_V);
  application.processEvents();
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(300, 250));
  QTest::keyClick(&editor, Qt::Key_Delete);
  application.processEvents();
  if (!snapshotMatches(
          expected({hollow, filled, rounded, squareAgain, ellipse}))) {
    error = QStringLiteral("Hollow rectangle interior selected a layer");
    return false;
  }
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(300, 400));
  QTest::keyClick(&editor, Qt::Key_Delete);
  application.processEvents();
  if (!snapshotMatches(expected({hollow, rounded, squareAgain, ellipse}))) {
    error = QStringLiteral("Filled rectangle interior did not select it");
    return false;
  }

  // R with a selected rectangle toggles that layer's fill (undoable).
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(300, 200));
  QTest::keyClick(&editor, Qt::Key_R);
  application.processEvents();
  Annotation hollowNowFilled = hollow;
  hollowNowFilled.filled = true;
  if (!snapshotMatches(
          expected({hollowNowFilled, rounded, squareAgain, ellipse}))) {
    error = QStringLiteral("R did not fill the selected rectangle");
    return false;
  }
  // Off any handle: with eight box handles the press point may be a resize
  // cursor even though the tool is still Select.
  QTest::mouseMove(&editor, QPoint(160, 480));
  application.processEvents();
  if (editor.cursor().shape() != Qt::ArrowCursor) {
    error = QStringLiteral("Toggling a selected rectangle switched tools");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (!snapshotMatches(expected({hollow, rounded, squareAgain, ellipse}))) {
    error = QStringLiteral("Undo did not restore the hollow rectangle");
    return false;
  }

  // Clicking the armed rectangle's toolbar button toggles fill as well
  // (deselect first: R with a rectangle selected targets that layer).
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(680, 480));
  QTest::keyClick(&editor, Qt::Key_R);
  application.processEvents();
  constexpr qreal toolbarWidth = 840.0; // kToolbarWidth in editor.cpp
  const qreal scale = std::min<qreal>(1.0, (800.0 - 16.0) / toolbarWidth);
  const qreal toolbarX = (800.0 - toolbarWidth * scale) / 2.0;
  const QPointF button(toolbarX + (6 * 40 + 18) * scale,
                       105 - 36 * scale - 46 + 18 * scale);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, button.toPoint());
  application.processEvents();
  drag(QPoint(500, 480), QPoint(600, 500));
  const Annotation viaButton =
      shape(Annotation::Kind::Rectangle, {400, 375}, {500, 395}, true);
  if (!snapshotMatches(
          expected({hollow, rounded, squareAgain, ellipse, viaButton}))) {
    error = QStringLiteral("Toolbar re-click did not toggle fill");
    return false;
  }

  editor.close();
  QFile::remove(snapshotPath);
  return true;
}

bool runCenteredCreationSmoke(QApplication &application, QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#182030")));
  capture.previewSize = capture.source.size();
  const QString snapshotPath = temporarySnapshotPath();

  CaptureEditor editor(capture);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
  QTest::mouseMove(&editor, QPoint(700, 500), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(700, 500));
  application.processEvents();
  const QRectF selection(100, 100, 600, 400);
  const auto expected = [&](const QVector<Annotation> &annotations) {
    return renderCapture(capture, selection, annotations,
                         BackgroundStyle::None);
  };
  const auto snapshotMatches = [&](const QImage &image) {
    return flushedSnapshot(editor, snapshotPath)
                   .convertToFormat(image.format()) == image;
  };
  const auto shape = [](Annotation::Kind kind, const QPointF &start,
                        const QPointF &end) {
    Annotation annotation;
    annotation.kind = kind;
    annotation.start = start;
    annotation.end = end;
    annotation.color = QColor(QStringLiteral("#ff375f"));
    annotation.size = 4;
    return annotation;
  };

  // Alt held from the press: the ellipse is centered on the press point
  // (widget (400,300) = annotation (300,195)) with the cursor on its corner.
  QTest::keyClick(&editor, Qt::Key_E);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::AltModifier, QPoint(400, 300));
  QTest::mouseMove(&editor, QPoint(500, 350), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::AltModifier,
                      QPoint(500, 350));
  application.processEvents();
  const Annotation centeredEllipse =
      shape(Annotation::Kind::Ellipse, {200, 145}, {400, 245});
  if (!snapshotMatches(expected({centeredEllipse}))) {
    error = QStringLiteral("Alt-drag did not center the ellipse");
    return false;
  }

  // Alt+Shift: centered square whose half-extent is the longer drag axis.
  QTest::keyClick(&editor, Qt::Key_R);
  QTest::mousePress(&editor, Qt::LeftButton,
                    Qt::AltModifier | Qt::ShiftModifier, QPoint(400, 300));
  QTest::mouseMove(&editor, QPoint(460, 340), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton,
                      Qt::AltModifier | Qt::ShiftModifier, QPoint(460, 340));
  application.processEvents();
  const Annotation centeredSquare =
      shape(Annotation::Kind::Rectangle, {240, 135}, {360, 255});
  if (!snapshotMatches(expected({centeredEllipse, centeredSquare}))) {
    error = QStringLiteral("Alt+Shift-drag did not create a centered square");
    return false;
  }

  // Alt pressed mid-drag applies; released before the mouse it does not.
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(200, 200));
  QTest::mouseMove(&editor, QPoint(250, 230), 20);
  QTest::keyPress(&editor, Qt::Key_Alt);
  application.processEvents();
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::AltModifier,
                      QPoint(250, 230));
  QTest::keyRelease(&editor, Qt::Key_Alt);
  application.processEvents();
  const Annotation midDrag =
      shape(Annotation::Kind::Rectangle, {50, 65}, {150, 125});
  if (!snapshotMatches(expected({centeredEllipse, centeredSquare, midDrag}))) {
    error = QStringLiteral("Alt pressed mid-drag did not center the shape");
    return false;
  }
  QTest::mousePress(&editor, Qt::LeftButton, Qt::AltModifier, QPoint(600, 200));
  QTest::mouseMove(&editor, QPoint(650, 230), 20);
  QTest::keyRelease(&editor, Qt::Key_Alt);
  application.processEvents();
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(650, 230));
  application.processEvents();
  const Annotation released =
      shape(Annotation::Kind::Rectangle, {500, 95}, {550, 125});
  if (!snapshotMatches(
          expected({centeredEllipse, centeredSquare, midDrag, released}))) {
    error = QStringLiteral("Alt released before the mouse still centered");
    return false;
  }

  // Spotlight (Omasnap's own drag-rectangle shape) centers the same way.
  QTest::keyClick(&editor, Qt::Key_S);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::AltModifier, QPoint(600, 400));
  QTest::mouseMove(&editor, QPoint(660, 440), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::AltModifier,
                      QPoint(660, 440));
  application.processEvents();
  Annotation spotlight =
      shape(Annotation::Kind::Spotlight, {440, 255}, {560, 335});
  spotlight.magnification = 2.0;
  spotlight.spotlightShape = SpotlightShape::Ellipse;
  if (!snapshotMatches(expected(
          {centeredEllipse, centeredSquare, midDrag, released, spotlight}))) {
    error = QStringLiteral("Alt-drag did not center the spotlight");
    return false;
  }

  // Alt leaves lines alone.
  QTest::keyClick(&editor, Qt::Key_L);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::AltModifier, QPoint(200, 400));
  QTest::mouseMove(&editor, QPoint(300, 450), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::AltModifier,
                      QPoint(300, 450));
  application.processEvents();
  const Annotation line = shape(Annotation::Kind::Line, {100, 295}, {200, 345});
  if (!snapshotMatches(expected({centeredEllipse, centeredSquare, midDrag,
                                 released, spotlight, line}))) {
    error = QStringLiteral("Alt changed a line's start point");
    return false;
  }

  editor.close();
  QFile::remove(snapshotPath);
  return true;
}

/** Runs the interaction and rendering smoke checks. */
bool runTextPillRenderingCheck(QString &error) {
  error = QStringLiteral("Text pill rendering check failed");
  CaptureData capture;
  capture.monitor.scale = 1.0;
  capture.monitor.pixelSize = {300, 100};
  capture.source = QImage(300, 100, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(Qt::transparent);
  capture.previewSize = capture.source.size();
  Annotation text;
  text.kind = Annotation::Kind::Text;
  text.text = QStringLiteral("Readable");
  text.color = QColor(QStringLiteral("#ff375f"));
  text.size = 5;
  text.start = {40, 60};
  const QRectF pill = annotationTextBounds(text);
  const QImage withPill = renderCapture(capture, QRectF(0, 0, 300, 100), {text},
                                        BackgroundStyle::None);
  // Cream just left of the first glyph and just above the ascent, inside
  // the pill but away from any ink.
  const QPoint beside(qRound(pill.left() + 2), 55);
  const QPoint above(60, qRound(pill.top() + 2));
  for (const QPoint &probe : {beside, above}) {
    if (withPill.pixelColor(probe) != QColor(248, 245, 235)) {
      error = QStringLiteral("Text pill was not painted around the glyphs");
      return false;
    }
  }
  if (withPill.pixelColor(pill.left() - 3, 55).alpha() != 0) {
    error = QStringLiteral("Text pill spilled outside its bounds");
    return false;
  }
  text.textBackground = TextBackground::Plain;
  const QImage plain = renderCapture(capture, QRectF(0, 0, 300, 100), {text},
                                     BackgroundStyle::None);
  if (plain.pixelColor(beside).alpha() != 0 ||
      plain.pixelColor(above).alpha() != 0) {
    error = QStringLiteral("Plain text still painted a pill");
    return false;
  }

  Annotation multiline = text;
  multiline.text = QStringLiteral("Readable\nsecond line");
  multiline.textBackground = TextBackground::Pill;
  const QRectF multilineBounds = annotationTextBounds(multiline);
  if (multilineBounds.height() <= pill.height() ||
      multilineBounds.width() <= pill.width()) {
    error = QStringLiteral("Multiline text bounds did not include every line");
    return false;
  }
  const QImage multilineImage = renderCapture(
      capture, QRectF(0, 0, 300, 100), {multiline}, BackgroundStyle::None);
  const QPoint secondLinePill(qRound(multilineBounds.left() + 2),
                              qRound(text.start.y() +
                                     QFontMetricsF(annotationTextFont(text.size))
                                         .lineSpacing()));
  if (multilineImage.pixelColor(secondLinePill) != QColor(248, 245, 235)) {
    error = QStringLiteral("Multiline text pill did not cover the second line");
    return false;
  }

  // The selection box follows the pill, so a rounded background does not sit
  // inside a square dashed frame.
  Annotation pilled = text;
  pilled.textBackground = TextBackground::Pill;
  const qreal pillHeight = annotationTextBounds(pilled).height();
  const qreal want = std::min(pillHeight / 4.0, 6.0) + 4.0;
  if (selectionBoundsRadius(pilled, 4.0) != want) {
    error = QStringLiteral("Selection box did not follow the text pill");
    return false;
  }
  Annotation bare = text;
  bare.textBackground = TextBackground::Plain;
  if (selectionBoundsRadius(bare, 4.0) != 0.0) {
    error = QStringLiteral("Selection box rounded text that has no pill");
    return false;
  }
  Annotation rect;
  rect.kind = Annotation::Kind::Rectangle;
  if (selectionBoundsRadius(rect, 4.0) != 0.0) {
    error = QStringLiteral("Selection box rounded a kind with no pill");
    return false;
  }
  return true;
}

bool runTextPillSmoke(QApplication &application, QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#182030")));
  capture.previewSize = capture.source.size();
  const QString snapshotPath = temporarySnapshotPath();

  CaptureEditor editor(capture);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
  QTest::mouseMove(&editor, QPoint(700, 500), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(700, 500));
  application.processEvents();
  const QRectF selection(100, 100, 600, 400);
  const auto expected = [&](const QVector<Annotation> &annotations) {
    return renderCapture(capture, selection, annotations,
                         BackgroundStyle::None);
  };
  const auto snapshotMatches = [&](const QImage &image) {
    return flushedSnapshot(editor, snapshotPath)
                   .convertToFormat(image.format()) == image;
  };
  const qreal ascent = QFontMetricsF(annotationTextFont(5.0)).ascent();
  const auto text = [&](const QPointF &clicked, const QString &content,
                        TextBackground background) {
    Annotation annotation;
    annotation.kind = Annotation::Kind::Text;
    annotation.start = clicked + QPointF(0, ascent);
    annotation.text = content;
    annotation.color = QColor(QStringLiteral("#ff375f"));
    annotation.size = 5;
    annotation.textBackground = background;
    return annotation;
  };
  const auto typeText = [&](const QPoint &at, const QString &content) {
    QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, at);
    application.processEvents();
    QTest::keyClicks(QApplication::focusWidget(), content);
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return, Qt::ControlModifier);
    application.processEvents();
  };

  // New text gets the pill by default.
  QTest::keyClick(&editor, Qt::Key_T);
  typeText(QPoint(300, 300), QStringLiteral("Pill"));
  const Annotation pill =
      text({200, 195}, QStringLiteral("Pill"), TextBackground::Pill);
  if (!snapshotMatches(expected({pill}))) {
    error = QStringLiteral("New text did not get a readability pill");
    return false;
  }

  // T again (tool still armed) switches the next text to plain.
  QTest::keyClick(&editor, Qt::Key_T);
  typeText(QPoint(300, 400), QStringLiteral("Plain"));
  const Annotation plain =
      text({200, 295}, QStringLiteral("Plain"), TextBackground::Plain);
  if (!snapshotMatches(expected({pill, plain}))) {
    error = QStringLiteral("T again did not switch new text to plain");
    return false;
  }

  // T with a text layer selected toggles that layer, undoably.
  QTest::keyClick(&editor, Qt::Key_V);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(310, 290));
  QTest::keyClick(&editor, Qt::Key_T);
  application.processEvents();
  Annotation pillNowPlain = pill;
  pillNowPlain.textBackground = TextBackground::Plain;
  if (!snapshotMatches(expected({pillNowPlain, plain}))) {
    error = QStringLiteral("T did not toggle the selected text's pill");
    return false;
  }
  if (editor.cursor().shape() != Qt::ArrowCursor) {
    error = QStringLiteral("Toggling a selected text switched tools");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (!snapshotMatches(expected({pill, plain}))) {
    error = QStringLiteral("Undo did not restore the text pill");
    return false;
  }

  // Re-editing keeps the layer's own background.
  QTest::mouseDClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(310, 410));
  application.processEvents();
  QTest::keyClick(QApplication::focusWidget(), Qt::Key_End); // text is selected
  QTest::keyClicks(QApplication::focusWidget(), QStringLiteral(" text"));
  QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return, Qt::ControlModifier);
  application.processEvents();
  const Annotation plainEdited =
      text({200, 295}, QStringLiteral("Plain text"), TextBackground::Plain);
  if (!snapshotMatches(expected({pill, plainEdited}))) {
    error = QStringLiteral("Re-editing changed the text's background");
    return false;
  }

  editor.close();
  QFile::remove(snapshotPath);
  return true;
}

/** Runs the interaction and rendering smoke checks. */
bool runSelectAllDeleteSmoke(QApplication &application, QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#182030")));
  capture.previewSize = capture.source.size();
  const QString snapshotPath = temporarySnapshotPath();

  CaptureEditor editor(capture);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
  QTest::mouseMove(&editor, QPoint(700, 500), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(700, 500));
  application.processEvents();
  const QRectF selection(100, 100, 600, 400);
  const auto expected = [&](const QVector<Annotation> &annotations) {
    return renderCapture(capture, selection, annotations,
                         BackgroundStyle::None);
  };
  const auto snapshotMatches = [&](const QImage &image) {
    // Snapshots are written off the UI thread, so flush before reading.
    return flushedSnapshot(editor, snapshotPath).convertToFormat(image.format()) ==
           image;
  };
  const auto drag = [&](const QPoint &from, const QPoint &to) {
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, from);
    QTest::mouseMove(&editor, to, 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, to);
    application.processEvents();
  };
  const auto layer = [](Annotation::Kind kind, const QPointF &start,
                        const QPointF &end, int number = 0) {
    Annotation annotation;
    annotation.kind = kind;
    annotation.start = start;
    annotation.end = end;
    annotation.number = number;
    annotation.color = QColor(QStringLiteral("#ff375f"));
    annotation.size = 4;
    return annotation;
  };

  // A rectangle, an arrow and two markers.
  QTest::keyClick(&editor, Qt::Key_R);
  drag(QPoint(200, 200), QPoint(400, 300));
  QTest::keyClick(&editor, Qt::Key_A);
  drag(QPoint(450, 200), QPoint(650, 300));
  QTest::keyClick(&editor, Qt::Key_M);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(250, 400));
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(350, 400));
  application.processEvents();
  const QVector<Annotation> all = {
      layer(Annotation::Kind::Rectangle, {100, 95}, {300, 195}),
      layer(Annotation::Kind::Arrow, {350, 95}, {550, 195}),
      layer(Annotation::Kind::Marker, {150, 295}, {}, 1),
      layer(Annotation::Kind::Marker, {250, 295}, {}, 2)};
  if (!snapshotMatches(expected(all))) {
    error = QStringLiteral("Select-all smoke could not draw its four layers");
    return false;
  }

  // Delete with nothing selected leaves everything alone.
  QTest::keyClick(&editor, Qt::Key_V);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(680, 480));
  QTest::keyClick(&editor, Qt::Key_Delete);
  application.processEvents();
  if (!snapshotMatches(expected(all))) {
    error = QStringLiteral("Delete with nothing selected removed layers");
    return false;
  }

  // Every member of a select-all is outlined, including a flat arrow whose
  // bounds have no height: the group must look selected, not just delete.
  QTest::keyClick(&editor, Qt::Key_A, Qt::ControlModifier);
  application.processEvents();
  const QImage grouped = editor.grab().toImage();
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(680, 480));
  application.processEvents();
  if (grouped == editor.grab().toImage()) {
    error = QStringLiteral("Select-all drew no selection indicator");
    return false;
  }

  // Clicking empty canvas drops the group selection again, so a following
  // Delete is a no-op.
  QTest::keyClick(&editor, Qt::Key_A, Qt::ControlModifier);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(680, 480));
  QTest::keyClick(&editor, Qt::Key_Backspace);
  application.processEvents();
  if (!snapshotMatches(expected(all))) {
    error = QStringLiteral("Delete after leaving select-all removed layers");
    return false;
  }

  // Ctrl+A then Delete removes every layer as one undo step, even with a
  // single layer selected beforehand.
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(300, 200));
  QTest::keyClick(&editor, Qt::Key_A, Qt::ControlModifier);
  QTest::keyClick(&editor, Qt::Key_Delete);
  application.processEvents();
  if (!snapshotMatches(expected({}))) {
    error = QStringLiteral("Ctrl+A then Delete did not remove every layer");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (!snapshotMatches(expected(all))) {
    error = QStringLiteral("Undo did not restore all deleted layers");
    return false;
  }

  // After clearing, marker numbering restarts at 1.
  QTest::keyClick(&editor, Qt::Key_A, Qt::ControlModifier);
  QTest::keyClick(&editor, Qt::Key_Backspace);
  QTest::keyClick(&editor, Qt::Key_M);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(300, 300));
  application.processEvents();
  if (!snapshotMatches(
          expected({layer(Annotation::Kind::Marker, {200, 195}, {}, 1)}))) {
    error = QStringLiteral("Marker numbering did not restart after clearing");
    return false;
  }

  editor.close();
  QFile::remove(snapshotPath);
  return true;
}

/** Runs the interaction and rendering smoke checks. */
bool runDuplicateLayerSmoke(QApplication &application, QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#182030")));
  capture.previewSize = capture.source.size();
  const QString snapshotPath = temporarySnapshotPath();

  CaptureEditor editor(capture);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();

  // 600x400 selection shown 1:1 at widget offset (100, 105).
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
  QTest::mouseMove(&editor, QPoint(700, 500), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(700, 500));
  application.processEvents();
  const QRectF selection(100, 100, 600, 400);
  const auto expected = [&](const QVector<Annotation> &annotations) {
    return renderCapture(capture, selection, annotations,
                         BackgroundStyle::None);
  };
  const auto snapshotMatches = [&](const QImage &image) {
    return flushedSnapshot(editor, snapshotPath)
                   .convertToFormat(image.format()) == image;
  };
  const auto rectangle = [](const QPointF &start, const QPointF &end) {
    Annotation annotation;
    annotation.kind = Annotation::Kind::Rectangle;
    annotation.start = start;
    annotation.end = end;
    annotation.color = QColor(QStringLiteral("#ff375f"));
    annotation.size = 4;
    return annotation;
  };
  const auto marker = [](const QPointF &at, int number) {
    Annotation annotation;
    annotation.kind = Annotation::Kind::Marker;
    annotation.start = at;
    annotation.number = number;
    annotation.color = QColor(QStringLiteral("#ff375f"));
    annotation.size = 4;
    return annotation;
  };
  const auto drag = [&](const QPoint &from, const QPoint &to) {
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, from);
    QTest::mouseMove(&editor, to, 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, to);
    application.processEvents();
  };

  // A rectangle hugging the left edge: the default down-left offset would
  // leave the canvas, so the copy goes down-right instead.
  QTest::keyClick(&editor, Qt::Key_R);
  drag(QPoint(120, 200), QPoint(220, 260));
  const Annotation left = rectangle({20, 95}, {120, 155});
  QTest::keyClick(&editor, Qt::Key_V);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(170, 200));
  QTest::keyClick(&editor, Qt::Key_D, Qt::AltModifier);
  application.processEvents();
  const Annotation leftCopy = rectangle({120, 195}, {220, 255});
  if (!snapshotMatches(expected({left, leftCopy}))) {
    error = QStringLiteral("Alt+D did not flip the offset away from the edge");
    return false;
  }

  // Mid-canvas: down-left, and chained Alt+D keeps stepping from the copy.
  // R with a rectangle selected toggles that layer's fill (#56), so drop the
  // selection before arming the tool.
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(150, 450));
  application.processEvents();
  QTest::keyClick(&editor, Qt::Key_R);
  drag(QPoint(400, 250), QPoint(500, 300));
  const Annotation middle = rectangle({300, 145}, {400, 195});
  QTest::keyClick(&editor, Qt::Key_V);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(450, 250));
  QTest::keyClick(&editor, Qt::Key_D, Qt::AltModifier);
  QTest::keyClick(&editor, Qt::Key_D, Qt::AltModifier);
  application.processEvents();
  const Annotation middleCopy = rectangle({200, 245}, {300, 295});
  const Annotation middleCopyCopy = rectangle({100, 345}, {200, 395});
  if (!snapshotMatches(
          expected({left, leftCopy, middle, middleCopy, middleCopyCopy}))) {
    error = QStringLiteral("Chained Alt+D did not offset down-left twice");
    return false;
  }

  // A duplicated marker takes the next number.
  // Alt+D leaves the copy selected; a Marker press on empty canvas first
  // puts that layer down (#65) and would not stamp, so drop the selection.
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(150, 450));
  application.processEvents();
  QTest::keyClick(&editor, Qt::Key_C);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(600, 200));
  QTest::keyClick(&editor, Qt::Key_V);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(600, 200));
  QTest::keyClick(&editor, Qt::Key_D, Qt::AltModifier);
  application.processEvents();
  const Annotation first = marker({500, 95}, 1);
  const Annotation second = marker({400, 195}, 2);
  const QVector<Annotation> withMarkers = {
      left, leftCopy, middle, middleCopy, middleCopyCopy, first, second};
  if (!snapshotMatches(expected(withMarkers))) {
    error = QStringLiteral("Duplicated marker did not take the next number");
    return false;
  }

  // Delete removes the selected copy; undo brings it back.
  QTest::keyClick(&editor, Qt::Key_Delete);
  application.processEvents();
  if (!snapshotMatches(expected(
          {left, leftCopy, middle, middleCopy, middleCopyCopy, first}))) {
    error = QStringLiteral("Delete did not remove the selected copy");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (!snapshotMatches(expected(withMarkers))) {
    error = QStringLiteral("Undo did not restore the deleted layer");
    return false;
  }

  // Plain D still arms the Redact tool.
  QTest::keyClick(&editor, Qt::Key_D);
  application.processEvents();
  if (editor.armedToolForTest() != CaptureEditor::Tool::Redact) {
    error = QStringLiteral("Plain D no longer arms the Redact tool");
    return false;
  }
  if (!snapshotMatches(expected(withMarkers))) {
    error = QStringLiteral("Plain D changed the layers");
    return false;
  }

  editor.close();
  QFile::remove(snapshotPath);
  return true;
}

/** Runs the interaction and rendering smoke checks. */
bool runKeyboardNudgeSmoke(QApplication &application, QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#182030")));
  capture.previewSize = capture.source.size();
  const QString snapshotPath = temporarySnapshotPath();

  CaptureEditor editor(capture);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();
  // 600x400 selection shown 1:1 at widget offset (100, 105).
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
  QTest::mouseMove(&editor, QPoint(700, 500), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(700, 500));
  application.processEvents();
  const QRectF selection(100, 100, 600, 400);
  const auto rectangle = [](const QPointF &start, const QPointF &end) {
    Annotation annotation;
    annotation.kind = Annotation::Kind::Rectangle;
    annotation.start = start;
    annotation.end = end;
    annotation.color = QColor(QStringLiteral("#ff375f"));
    annotation.size = 4;
    return annotation;
  };
  const auto snapshotMatches = [&](const Annotation &expected) {
    const QImage image =
        renderCapture(capture, selection, {expected}, BackgroundStyle::None);
    return flushedSnapshot(editor, snapshotPath)
                   .convertToFormat(image.format()) == image;
  };
  // A run of nudges writes its snapshot once, ~100 ms after the last key.
  const auto settle = [&] {
    QTest::qWait(150);
    application.processEvents();
  };

  QTest::keyClick(&editor, Qt::Key_R);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(200, 200));
  QTest::mouseMove(&editor, QPoint(400, 300), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(400, 300));
  application.processEvents();
  const Annotation drawn = rectangle({100, 95}, {300, 195});
  if (!snapshotMatches(drawn)) {
    error = QStringLiteral("Nudge smoke: rectangle did not render");
    return false;
  }

  // Arrow keys with nothing selected leave the layer alone.
  QTest::keyClick(&editor, Qt::Key_V);
  QTest::keyClick(&editor, Qt::Key_Right);
  application.processEvents();
  if (!snapshotMatches(drawn)) {
    error = QStringLiteral("Arrow key moved a layer that was not selected");
    return false;
  }

  // Select the rectangle on its stroke, then nudge: quick presses coalesce
  // into one undo entry.
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(300, 200));
  application.processEvents();
  QTest::keyClick(&editor, Qt::Key_Right);
  QTest::keyClick(&editor, Qt::Key_Right);
  QTest::keyClick(&editor, Qt::Key_Down);
  QTest::keyClick(&editor, Qt::Key_Left, Qt::ShiftModifier);
  settle();
  if (!snapshotMatches(rectangle({92, 96}, {292, 196}))) {
    error = QStringLiteral("Arrow nudges did not move the selected layer");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (!snapshotMatches(drawn)) {
    error = QStringLiteral("Quick nudges did not coalesce into one undo step");
    return false;
  }

  // A pause between presses starts a new undo entry.
  QTest::keyClick(&editor, Qt::Key_Down, Qt::ShiftModifier);
  settle();
  QTest::keyClick(&editor, Qt::Key_Down, Qt::ShiftModifier);
  settle();
  if (!snapshotMatches(rectangle({100, 115}, {300, 215}))) {
    error = QStringLiteral("Shift nudges did not move by 10 px");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (!snapshotMatches(rectangle({100, 105}, {300, 205}))) {
    error = QStringLiteral("Separated nudges did not get separate undo steps");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (!snapshotMatches(drawn)) {
    error = QStringLiteral("Second undo did not restore the drawn rectangle");
    return false;
  }

  // Arrow keys typed into the inline text editor edit text, not layers:
  // with the rectangle still selected, open a text field and press Down.
  QTest::keyClick(&editor, Qt::Key_T);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(500, 450));
  application.processEvents();
  QTest::keyClick(QApplication::focusWidget(), Qt::Key_Down);
  QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
  settle();
  if (!snapshotMatches(drawn)) {
    error = QStringLiteral("Arrow key inside the text editor nudged a layer");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_V);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(300, 200));
  application.processEvents();

  // Shift while dragging the end handle keeps the original 2:1 aspect ratio;
  // the axis scaled more (x: 200 -> 300) wins, so y follows to 150.
  QTest::mousePress(&editor, Qt::LeftButton, Qt::ShiftModifier,
                    QPoint(400, 300));
  QTest::mouseMove(&editor, QPoint(500, 320), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::ShiftModifier,
                      QPoint(500, 320));
  application.processEvents();
  if (!snapshotMatches(rectangle({100, 95}, {400, 245}))) {
    error = QStringLiteral("Shift resize did not keep the aspect ratio");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (!snapshotMatches(drawn)) {
    error = QStringLiteral("Undo did not revert the constrained resize");
    return false;
  }
  // Without Shift the same drag lands on the raw point.
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(400, 300));
  QTest::mouseMove(&editor, QPoint(500, 320), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(500, 320));
  application.processEvents();
  if (!snapshotMatches(rectangle({100, 95}, {400, 215}))) {
    error = QStringLiteral("Plain resize was constrained without Shift");
    return false;
  }

  editor.close();
  QFile::remove(snapshotPath);
  return true;
}

/** Checks the handle set: a box offers eight, a counter four, a line two.
 *  Side handles stretch one axis, corners take Shift for the proportions, and
 *  a box dragged through itself comes out the other side. */
bool runLayerHandlesSmoke(QApplication &application, QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#182030")));
  capture.previewSize = capture.source.size();
  const QString snapshotPath = temporarySnapshotPath();

  CaptureEditor editor(capture);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();
  // 600x400 selection shown 1:1 at widget offset (100, 105).
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
  QTest::mouseMove(&editor, QPoint(700, 500), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(700, 500));
  application.processEvents();
  const auto drag = [&](const QPoint &from, const QPoint &to,
                        Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QTest::mousePress(&editor, Qt::LeftButton, modifiers, from);
    QTest::mouseMove(&editor, to, 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, modifiers, to);
    application.processEvents();
  };
  const auto inkBounds = [](const QImage &image) {
    QRect bounds;
    for (int y = 0; y < image.height(); ++y) {
      for (int x = 0; x < image.width(); ++x) {
        if (image.pixelColor(x, y) != QColor(QStringLiteral("#182030")))
          bounds = bounds.isNull() ? QRect(x, y, 1, 1)
                                   : bounds.united(QRect(x, y, 1, 1));
      }
    }
    return bounds;
  };

  // A rectangle at annotation (100,95)-(300,195).
  QTest::keyClick(&editor, Qt::Key_R);
  drag(QPoint(200, 200), QPoint(400, 300));
  QTest::keyClick(&editor, Qt::Key_V);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(200, 250));
  application.processEvents();

  // Its right edge midpoint is at widget (400,250): a side handle stretches
  // that axis and leaves the other alone, however far the pointer wanders.
  QTest::mouseMove(&editor, QPoint(400, 250), 10);
  application.processEvents();
  if (editor.cursor().shape() != Qt::SizeHorCursor) {
    error = QStringLiteral("A box offered no handle on its right edge");
    return false;
  }
  drag(QPoint(400, 250), QPoint(460, 320));
  {
    const QRect after = inkBounds(flushedSnapshot(editor, snapshotPath));
    if (std::abs(after.right() - 360) > 4) {
      error = QStringLiteral("The right handle did not move that edge");
      return false;
    }
    if (std::abs(after.top() - 95) > 4 || std::abs(after.bottom() - 195) > 4) {
      error = QStringLiteral("A side handle changed the other axis too");
      return false;
    }
  }

  // Dragged through itself, the box comes out the other side rather than
  // collapsing against the edge it crossed.
  drag(QPoint(460, 250), QPoint(150, 250));
  {
    const QRect flipped = inkBounds(flushedSnapshot(editor, snapshotPath));
    if (flipped.width() < 20) {
      error = QStringLiteral("Dragging an edge through the shape collapsed it");
      return false;
    }
    if (std::abs(flipped.right() - 100) > 6) {
      error = QStringLiteral("The crossed edge did not become the far side");
      return false;
    }
  }
  editor.close();
  QFile::remove(snapshotPath);
  return true;
}


/**
 * Selecting a line near mid-canvas must leave both endpoint handles visible.
 * The help card flips away from the pointer; after Cut added a row it was
 * tall enough that this click parked the card on the start handle.
 */
bool runLineHandleLegendSmoke(QApplication &application, QString &error) {
  CaptureData capture;
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#182030")));
  capture.previewSize = capture.source.size();

  CaptureEditor editor(capture);
  editor.setSuppressSnapshots(true);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
  QTest::mouseMove(&editor, QPoint(650, 470), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(650, 470));
  application.processEvents();
  QTest::keyClick(&editor, Qt::Key_L);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(260, 210));
  QTest::mouseMove(&editor, QPoint(520, 265), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(520, 265));
  application.processEvents();
  QTest::keyClick(&editor, Qt::Key_V);
  application.processEvents();
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(390, 238));
  application.processEvents();
  const QImage ui = editor.grab().toImage();
  const QColor handle(QStringLiteral("#0a84ff"));
  if (ui.pixelColor(260, 210) != handle || ui.pixelColor(520, 265) != handle) {
    error = QStringLiteral(
        "Selected line start handle %1 end handle %2 (want #0a84ff)")
                .arg(ui.pixelColor(260, 210).name(QColor::HexArgb),
                     ui.pixelColor(520, 265).name(QColor::HexArgb));
    return false;
  }
  editor.close();
  return true;
}

/** Checks that a drawing tool moves the layer under its edge without losing
 *  the tool: adjust what is there, then keep drawing. */
bool runHoverMoveSmoke(QApplication &application, QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#182030")));
  capture.previewSize = capture.source.size();
  const QString snapshotPath = temporarySnapshotPath();
  CaptureEditor editor(capture);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
  QTest::mouseMove(&editor, QPoint(700, 500), 10);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(700, 500));
  application.processEvents();

  // A hollow rectangle, then the arrow tool armed.
  QTest::keyClick(&editor, Qt::Key_R);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(300, 250));
  QTest::mouseMove(&editor, QPoint(500, 400), 10);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(500, 400));
  application.processEvents();
  QTest::keyClick(&editor, Qt::Key_A);
  application.processEvents();

  // Inside the outline is canvas: the arrow tool draws there.
  const QImage beforeDraw = flushedSnapshot(editor, snapshotPath);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(380, 320));
  QTest::mouseMove(&editor, QPoint(450, 360), 10);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(450, 360));
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath) == beforeDraw) {
    error = QStringLiteral("Drawing inside a hollow shape did not draw");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();

  // On the outline it grabs and moves, and the arrow tool survives.
  const QImage beforeMove = flushedSnapshot(editor, snapshotPath);
  const int layersBeforeMove = editor.annotationCountForTest();
  QTest::mouseMove(&editor, QPoint(300, 250), 10);
  application.processEvents();
  if (editor.cursor().shape() != Qt::SizeAllCursor) {
    error = QStringLiteral("An edge under a drawing tool showed no move cursor");
    return false;
  }
  // The border is wide enough to hit without aiming: 10 px outside the stroke
  // still grabs, which is what makes a hollow shape draggable in practice.
  QTest::mouseMove(&editor, QPoint(400, 240), 10);
  application.processEvents();
  if (editor.cursor().shape() != Qt::SizeAllCursor) {
    error = QStringLiteral("The edge grab band was too narrow to hit");
    return false;
  }
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(300, 250));
  QTest::mouseMove(&editor, QPoint(340, 290), 10);
  application.processEvents();
  {
    // Mid-grab there must be no half-drawn annotation following the pointer:
    // the on-screen frame should differ from a live arrow being dragged out.
    const QImage midGrab = editor.grab().toImage();
    QTest::mouseMove(&editor, QPoint(420, 370), 10);
    application.processEvents();
    const QImage laterGrab = editor.grab().toImage();
    if (midGrab == laterGrab) {
      error = QStringLiteral("Grab did not track the pointer");
      return false;
    }
    // With the arrow tool armed, a grab must not also draw an arrow from the
    // grab point to the pointer. Halfway along that line is empty canvas once
    // the rectangle has moved away, so it must still read as background.
    const QColor midLine = laterGrab.pixelColor(360, 310);
    if (midLine != QColor(QStringLiteral("#182030"))) {
      error = QStringLiteral("A grab also drew the armed tool (%1 at 360,310)")
                  .arg(midLine.name());
      return false;
    }
  }
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(420, 370));
  application.processEvents();
  if (editor.annotationCountForTest() != layersBeforeMove) {
    error = QStringLiteral("A grab left a stray annotation behind (%1 -> %2)")
                .arg(layersBeforeMove)
                .arg(editor.annotationCountForTest());
    return false;
  }
  if (flushedSnapshot(editor, snapshotPath) == beforeMove) {
    error = QStringLiteral("Grabbing an edge did not move the layer");
    return false;
  }
  // The distinction that matters: it moved the layer rather than drawing a
  // new one, which would also have changed the snapshot.
  if (editor.annotationCountForTest() != layersBeforeMove) {
    error = QStringLiteral("Grabbing an edge drew instead of moving (%1 -> %2)")
                .arg(layersBeforeMove)
                .arg(editor.annotationCountForTest());
    return false;
  }

  // That move is an edit like any other, so one undo puts it back and a
  // second one is needed to reach whatever came before it.
  {
    const QImage moved = flushedSnapshot(editor, snapshotPath);
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    application.processEvents();
    const QImage undone = flushedSnapshot(editor, snapshotPath);
    if (undone == moved) {
      error = QStringLiteral("Undo did not put the moved layer back");
      return false;
    }
    if (editor.annotationCountForTest() != layersBeforeMove) {
      error = QStringLiteral("Undoing a move removed the layer instead");
      return false;
    }
    QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
    application.processEvents();
    if (flushedSnapshot(editor, snapshotPath) != moved) {
      error = QStringLiteral("Redo did not restore the move");
      return false;
    }
  }

  // Still the arrow tool. The moved layer is selected, so one click on empty
  // canvas puts it down without drawing anything...
  const QImage beforeDismiss = flushedSnapshot(editor, snapshotPath);
  const int layersBeforeDismiss = editor.annotationCountForTest();
  if (editor.selectedCountForTest() == 0) {
    error = QStringLiteral("Moving a layer did not leave it selected");
    return false;
  }
  QTest::mouseMove(&editor, QPoint(620, 200), 10);
  application.processEvents();
  if (editor.cursor().shape() != Qt::CrossCursor) {
    error = QStringLiteral("Moving a layer left the drawing tool behind");
    return false;
  }
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(620, 200));
  application.processEvents();
  if (editor.annotationCountForTest() != layersBeforeDismiss ||
      flushedSnapshot(editor, snapshotPath) != beforeDismiss) {
    error = QStringLiteral("Clicking off a selected layer drew something");
    return false;
  }
  // ...and it really is deselected.
  if (editor.selectedCountForTest() != 0) {
    error = QStringLiteral("Clicking off a selected layer left it selected");
    return false;
  }

  // A selected layer's handle resizes it rather than being grabbed as a move,
  // with the tool still armed. The text tool's wrap handle depends on this.
  {
    QTest::keyClick(&editor, Qt::Key_R);
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(200, 200));
    QTest::mouseMove(&editor, QPoint(350, 300), 10);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(350, 300));
    application.processEvents();
    QTest::keyClick(&editor, Qt::Key_V);
    QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(275, 200));
    application.processEvents();
    if (editor.selectedCountForTest() == 0) {
      error = QStringLiteral("Could not select the rectangle to resize it");
      return false;
    }
    QTest::keyClick(&editor, Qt::Key_A); // a drawing tool, not Select
    application.processEvents();
    const QImage beforeResize = flushedSnapshot(editor, snapshotPath);
    const int layersBeforeResize = editor.annotationCountForTest();
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(350, 300));
    QTest::mouseMove(&editor, QPoint(420, 360), 10);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(420, 360));
    application.processEvents();
    if (editor.annotationCountForTest() != layersBeforeResize) {
      error = QStringLiteral("Dragging a handle drew instead of resizing");
      return false;
    }
    if (flushedSnapshot(editor, snapshotPath) == beforeResize) {
      error = QStringLiteral("Dragging a handle did not resize the layer");
      return false;
    }
    // The distinction that matters: a resize leaves the opposite corner where
    // it was, a move would have taken the whole rectangle with it.
    const QImage resized = editor.grab().toImage();
    if (resized.pixelColor(202, 202) == QColor(QStringLiteral("#182030"))) {
      error = QStringLiteral("Dragging a handle moved the layer instead of "
                             "resizing it");
      return false;
    }
  }

  // A counter is stamped on top of the work, so it goes down over an existing
  // layer instead of grabbing it, including on the edge that every other tool
  // would move.
  {
    QTest::keyClick(&editor, Qt::Key_C);
    application.processEvents();
    QTest::mouseMove(&editor, QPoint(420, 370), 10);
    application.processEvents();
    if (editor.cursor().shape() != Qt::PointingHandCursor) {
      error = QStringLiteral("The counter offered to move the layer under it");
      return false;
    }
    const int layersBeforeMarker = editor.annotationCountForTest();
    // The resized layer is still selected, so this click only puts it down.
    QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(420, 370));
    application.processEvents();
    if (editor.annotationCountForTest() != layersBeforeMarker ||
        editor.selectedCountForTest() != 0) {
      error = QStringLiteral("Dismissing a selection also placed a counter");
      return false;
    }
    QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(420, 370));
    application.processEvents();
    if (editor.annotationCountForTest() != layersBeforeMarker + 1) {
      error = QStringLiteral("A counter on top of a layer did not go down");
      return false;
    }
    if (editor.selectedCountForTest() != 0) {
      error = QStringLiteral("Placing a counter grabbed the layer under it");
      return false;
    }
    // Its own counters it does pick up: one just placed has to be movable
    // without changing tools, and two that must overlap are placed apart and
    // dragged together.
    QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(420, 370));
    application.processEvents();
    if (editor.annotationCountForTest() != layersBeforeMarker + 1) {
      error = QStringLiteral("Clicking a counter placed another on top of it");
      return false;
    }
    if (editor.selectedCountForTest() != 1) {
      error = QStringLiteral("Clicking a counter did not pick it up");
      return false;
    }
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    application.processEvents();
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    application.processEvents();
    QTest::keyClick(&editor, Qt::Key_A);
    application.processEvents();
  }

  // ...and the drag after that draws an arrow.
  const QImage beforeSecond = flushedSnapshot(editor, snapshotPath);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(620, 200));
  QTest::mouseMove(&editor, QPoint(660, 260), 10);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(660, 260));
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath) == beforeSecond) {
    error = QStringLiteral("Could not draw after dismissing the selection");
    return false;
  }
  editor.close();
  QFile::remove(snapshotPath);
  return true;
}

/** Checks the stacking: the last layer picked up sits on top of the ones it
 *  overlaps, and text and counters stay above whatever the order. */
bool runLayerOrderSmoke(QApplication &application, QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#182030")));
  capture.previewSize = capture.source.size();
  const QString snapshotPath = temporarySnapshotPath();

  CaptureEditor editor(capture);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
  QTest::mouseMove(&editor, QPoint(700, 500), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(700, 500));
  application.processEvents();

  // Two overlapping strokes in different colors: the second is on top.
  QTest::keyClick(&editor, Qt::Key_L);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(200, 300));
  QTest::mouseMove(&editor, QPoint(500, 300), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(500, 300));
  application.processEvents();
  QTest::keyClick(&editor, Qt::Key_4);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(350, 200));
  QTest::mouseMove(&editor, QPoint(350, 400), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(350, 400));
  application.processEvents();
  const QPoint crossing(250, 195); // annotation coords, where they meet
  const QImage drawn = flushedSnapshot(editor, snapshotPath);
  const QColor second = drawn.pixelColor(crossing);
  if (second == QColor(QStringLiteral("#182030"))) {
    error = QStringLiteral("Layer order smoke: the strokes did not cross");
    return false;
  }

  // Picking the first one up puts it back on top.
  QTest::keyClick(&editor, Qt::Key_V);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(220, 300));
  application.processEvents();
  const QColor raised =
      flushedSnapshot(editor, snapshotPath).pixelColor(crossing);
  if (raised == second) {
    error = QStringLiteral("Clicking a layer did not bring it to the top");
    return false;
  }
  // And that reorder is an edit like any other.
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath).pixelColor(crossing) != second) {
    error = QStringLiteral("Undo did not put the order back");
    return false;
  }

  // A counter keeps the top even when something is drawn over it afterwards.
  // The undo left a layer selected, so the first click puts it down and the
  // second places the counter.
  QTest::keyClick(&editor, Qt::Key_C);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(620, 200));
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(620, 200));
  application.processEvents();
  const QPoint counterCenter(520, 95);
  const QColor counterInk =
      flushedSnapshot(editor, snapshotPath).pixelColor(counterCenter);
  if (counterInk == QColor(QStringLiteral("#182030"))) {
    error = QStringLiteral("Layer order smoke: the counter did not go down");
    return false;
  }
  // A line straight through it, drawn afterwards and painted in the same pass:
  // in plain order it would take the middle of the counter.
  QTest::keyClick(&editor, Qt::Key_L);
  QTest::keyClick(&editor, Qt::Key_4); // a color the counter is not
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(560, 200));
  QTest::mouseMove(&editor, QPoint(690, 200), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(690, 200));
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath).pixelColor(counterCenter) !=
      counterInk) {
    error = QStringLiteral("A layer drawn later covered the counter");
    return false;
  }
  editor.close();
  QFile::remove(snapshotPath);
  return true;
}

/** Runs the interaction and rendering smoke checks. */
bool runViewportZoomSmoke(QApplication &application, QString &error) {
  // A capture much taller than the window: fit shows it all (tiny); zoom in
  // and pan to navigate it.
  const QSize size(600, 6000);
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = QRect(QPoint(), size);
  capture.monitor.pixelSize = size;
  capture.monitor.scale = 1.0;
  capture.source = QImage(size, QImage::Format_ARGB32);
  for (int y = 0; y < size.height(); ++y) {
    QRgb *row = reinterpret_cast<QRgb *>(capture.source.scanLine(y));
    for (int x = 0; x < size.width(); ++x)
      row[x] = qRgb((x * 7) & 0xff, (y * 3) & 0xff, ((x + y) * 5) & 0xff);
  }
  capture.previewSize = capture.source.size();

  CaptureEditor editor(capture);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();
  // Whole-monitor selection so the tall image fills the edit surface.
  QTest::keyClick(&editor, Qt::Key_Space);
  QTest::keyClick(&editor, Qt::Key_Space); // window mode toggles off if on
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(40, 80));
  QTest::mouseMove(&editor, QPoint(760, 560), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(760, 560));
  application.processEvents();

  // Compare only the content band (exclude the toolbar row and the status
  // line, which carry zoom text that is not part of the view).
  const auto contentBand = [](const QImage &image) {
    return image.copy(0, 80, image.width(), image.height() - 140);
  };
  const QImage fitView = contentBand(editor.grab().toImage());
  const auto wheel = [&](int deltaY, Qt::KeyboardModifiers modifiers) {
    QWheelEvent event(QPointF(400, 300), QPointF(400, 300), {}, {0, deltaY},
                      Qt::NoButton, modifiers, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&editor, &event);
    application.processEvents();
  };
  // Scrolling must never change the zoom: at fit, plain wheel and Shift+wheel
  // are inert.
  wheel(-120, Qt::NoModifier);
  wheel(-120, Qt::ShiftModifier);
  if (contentBand(editor.grab().toImage()) != fitView) {
    error = QStringLiteral("Scrolling at fit changed the view");
    return false;
  }
  // Ctrl+wheel is the zoom gesture.
  wheel(360, Qt::ControlModifier);
  const QImage zoomedView = contentBand(editor.grab().toImage());
  if (zoomedView == fitView) {
    error = QStringLiteral("Ctrl+wheel did not zoom the view");
    return false;
  }
  // Plain wheel scrolls the zoomed view vertically.
  wheel(-120, Qt::NoModifier);
  const QImage pannedView = contentBand(editor.grab().toImage());
  if (pannedView == zoomedView) {
    error = QStringLiteral("Plain wheel did not scroll the zoomed view");
    return false;
  }
  // Deep zoom gives horizontal slack; Shift+wheel then scrolls sideways.
  for (int i = 0; i < 13; ++i)
    wheel(120, Qt::ControlModifier);
  const QImage deepView = contentBand(editor.grab().toImage());
  wheel(-120, Qt::ShiftModifier);
  if (contentBand(editor.grab().toImage()) == deepView) {
    error = QStringLiteral("Shift+wheel did not scroll sideways when zoomed");
    return false;
  }
  // Arrows pan the zoomed view when nothing is selected, which is how a long
  // capture is walked without a middle mouse button.
  {
    const QImage beforeArrow = contentBand(editor.grab().toImage());
    QTest::keyClick(&editor, Qt::Key_Down);
    application.processEvents();
    if (contentBand(editor.grab().toImage()) == beforeArrow) {
      error = QStringLiteral("Arrow keys did not pan the zoomed view");
      return false;
    }
    QTest::keyClick(&editor, Qt::Key_Up);
    application.processEvents();
    if (contentBand(editor.grab().toImage()) != beforeArrow) {
      error = QStringLiteral("Arrow panning did not come back");
      return false;
    }
  }

  // Middle-drag also pans.
  const QImage beforeDrag = contentBand(editor.grab().toImage());
  QTest::mousePress(&editor, Qt::MiddleButton, Qt::NoModifier, QPoint(400, 300));
  QTest::mouseMove(&editor, QPoint(400, 380), 20);
  QTest::mouseRelease(&editor, Qt::MiddleButton, Qt::NoModifier, QPoint(400, 380));
  application.processEvents();
  if (contentBand(editor.grab().toImage()) == beforeDrag) {
    error = QStringLiteral("Middle-drag did not pan the view");
    return false;
  }
  // Ctrl+0 restores fit exactly.
  QTest::keyClick(&editor, Qt::Key_0, Qt::ControlModifier);
  application.processEvents();
  if (contentBand(editor.grab().toImage()) != fitView) {
    error = QStringLiteral("Ctrl+0 did not restore the fitted view");
    return false;
  }
  // Placing an annotation while zoomed lands at the intended image point:
  // zoom back in, draw a rectangle, fit, and confirm the layer persisted.
  wheel(360, Qt::ControlModifier);
  QTest::keyClick(&editor, Qt::Key_R);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(300, 250));
  QTest::mouseMove(&editor, QPoint(450, 400), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(450, 400));
  application.processEvents();
  QTest::keyClick(&editor, Qt::Key_0, Qt::ControlModifier);
  application.processEvents();
  if (contentBand(editor.grab().toImage()) == fitView) {
    error = QStringLiteral("A layer drawn while zoomed did not persist to fit");
    return false;
  }
  editor.close();

  // A wide capture whose full height fits: the plain vertical wheel scrolls
  // the only overflowing axis (horizontal) instead of doing nothing.
  {
    const QSize wideSize(6000, 600);
    CaptureData wide;
    wide.monitor.name = QStringLiteral("TEST");
    wide.monitor.geometry = QRect(QPoint(), wideSize);
    wide.monitor.pixelSize = wideSize;
    wide.monitor.scale = 1.0;
    wide.source = QImage(wideSize, QImage::Format_ARGB32);
    for (int y = 0; y < wideSize.height(); ++y) {
      QRgb *row = reinterpret_cast<QRgb *>(wide.source.scanLine(y));
      for (int x = 0; x < wideSize.width(); ++x)
        row[x] = qRgb((x * 3) & 0xff, (y * 7) & 0xff, ((x ^ y) * 5) & 0xff);
    }
    wide.previewSize = wide.source.size();
    CaptureEditor wideEditor(wide);
    wideEditor.resize(800, 600);
    wideEditor.show();
    application.processEvents();
    QTest::keyClick(&wideEditor, Qt::Key_Space);
    QTest::keyClick(&wideEditor, Qt::Key_Space);
    QTest::mousePress(&wideEditor, Qt::LeftButton, Qt::NoModifier, QPoint(40, 80));
    QTest::mouseMove(&wideEditor, QPoint(760, 560), 20);
    QTest::mouseRelease(&wideEditor, Qt::LeftButton, Qt::NoModifier, QPoint(760, 560));
    application.processEvents();
    const auto wideWheel = [&](int deltaY, Qt::KeyboardModifiers modifiers) {
      QWheelEvent event(QPointF(400, 300), QPointF(400, 300), {}, {0, deltaY},
                        Qt::NoButton, modifiers, Qt::NoScrollPhase, false);
      QApplication::sendEvent(&wideEditor, &event);
      application.processEvents();
    };
    for (int i = 0; i < 6; ++i)
      wideWheel(120, Qt::ControlModifier);
    const QImage zoomedWide = contentBand(wideEditor.grab().toImage());
    wideWheel(-120, Qt::NoModifier);
    if (contentBand(wideEditor.grab().toImage()) == zoomedWide) {
      error = QStringLiteral(
          "Plain wheel did not scroll a wide capture sideways");
      return false;
    }
    // A horizontal touchpad swipe (x-axis delta) scrolls sideways too.
    const QImage beforeSwipe = contentBand(wideEditor.grab().toImage());
    QWheelEvent swipe(QPointF(400, 300), QPointF(400, 300), {}, {120, 0},
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&wideEditor, &swipe);
    application.processEvents();
    if (contentBand(wideEditor.grab().toImage()) == beforeSwipe) {
      error = QStringLiteral(
          "A horizontal wheel delta did not scroll the wide capture");
      return false;
    }
    wideEditor.close();
  }
  return true;
}

/** Checks that the wheel over a selected layer changes its weight, not its
 *  extent: a stroke gets heavier where it already is, and resizing stays with
 *  the corner handle. */
bool runLayerWeightSmoke(QApplication &application, QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#182030")));
  capture.previewSize = capture.source.size();
  const QString snapshotPath = temporarySnapshotPath();

  CaptureEditor editor(capture);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();
  // 600x400 selection shown 1:1 at widget offset (100, 105).
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
  QTest::mouseMove(&editor, QPoint(700, 500), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(700, 500));
  application.processEvents();
  const auto wheel = [&](int notches) {
    for (int notch = 0; notch < std::abs(notches); ++notch) {
      QWheelEvent event(QPointF(400, 300), QPointF(400, 300), {},
                        {0, notches > 0 ? 120 : -120}, Qt::NoButton,
                        Qt::NoModifier, Qt::NoScrollPhase, false);
      QApplication::sendEvent(&editor, &event);
    }
    application.processEvents();
  };
  const auto ink = [](const QImage &image, int x, int y) {
    return image.pixelColor(x, y) != QColor(QStringLiteral("#182030"));
  };

  // A rectangle at annotation (100,95)-(300,195), stroke 4.
  QTest::keyClick(&editor, Qt::Key_R);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(200, 200));
  QTest::mouseMove(&editor, QPoint(400, 300), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(400, 300));
  application.processEvents();
  QTest::keyClick(&editor, Qt::Key_V);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(200, 250));
  application.processEvents();
  {
    const QImage drawn = flushedSnapshot(editor, snapshotPath);
    if (!ink(drawn, 100, 145) || ink(drawn, 105, 145)) {
      error = QStringLiteral("Weight smoke: the stroke is not where expected");
      return false;
    }
  }
  // Four notches take the stroke from 4 to 8: it thickens about the same
  // outline, so the left edge stays at x 100 and now reaches x 103. Scaling
  // instead would have carried that edge out to x 54.
  wheel(4);
  {
    const QImage heavier = flushedSnapshot(editor, snapshotPath);
    if (!ink(heavier, 100, 145) || !ink(heavier, 103, 145)) {
      error = QStringLiteral("The wheel did not thicken the selected layer");
      return false;
    }
    if (ink(heavier, 54, 145)) {
      error = QStringLiteral("The wheel resized the layer instead of "
                             "thickening it");
      return false;
    }
  }
  wheel(-4);
  {
    const QImage lighter = flushedSnapshot(editor, snapshotPath);
    if (!ink(lighter, 100, 145) || ink(lighter, 105, 145)) {
      error = QStringLiteral("The wheel did not thin the layer back down");
      return false;
    }
  }
  // The chrome steps back while the wheel is turning, because the handles sit
  // where the change shows, and comes back once it settles.
  wheel(2);
  if (!editor.selectionFadedForTest()) {
    error = QStringLiteral("Selection chrome did not step back for the wheel");
    return false;
  }
  QTest::qWait(600);
  application.processEvents();
  if (editor.selectionFadedForTest()) {
    error = QStringLiteral("Selection chrome stayed faint after the wheel "
                           "settled");
    return false;
  }
  editor.close();
  QFile::remove(snapshotPath);
  return true;
}

int main(int argc, char **argv) {
  // Re-executed by the instance-lock checks as the process holding the lock.
  const QString heldLockPath =
      qEnvironmentVariable(kInstanceLockHolderVariable);
  if (!heldLockPath.isEmpty())
    return runInstanceLockHolder(heldLockPath);

  QApplication application(argc, argv);
  if (!loadCaptureFonts())
    return 17;

  // Live output capture against a real compositor (the smoke's own Wayland
  // connection; Qt's platform does not matter): open a session on the named
  // output and grab several frames through the same buffer, timing them,
  // since scroll capture needs many per second.
  const QString liveOutputName = qEnvironmentVariable("OMASNAP_SMOKE_OUTPUT");
  if (!liveOutputName.isEmpty()) {
    OutputCapture output;
    QString outputError;
    if (!output.open(liveOutputName, outputError)) {
      qWarning().noquote() << outputError;
      return 105;
    }
    QImage frame;
    QElapsedTimer stopwatch;
    for (int index = 0; index < 5; ++index) {
      stopwatch.start();
      if (!output.grab(frame, outputError)) {
        qWarning().noquote() << outputError;
        return 105;
      }
      if (frame.isNull() || frame.size() != output.bufferSize()) {
        qWarning().noquote() << QStringLiteral(
            "Output frame size does not match the announced buffer");
        return 105;
      }
      qInfo().noquote() << QStringLiteral("output %1 frame %2: %3x%4 in %5 ms")
                               .arg(liveOutputName)
                               .arg(index + 1)
                               .arg(frame.width())
                               .arg(frame.height())
                               .arg(stopwatch.elapsed());
    }
    const QString liveRoot =
        argc > 1 ? QString::fromLocal8Bit(argv[1])
                 : QDir(QDir::tempPath())
                       .filePath(QStringLiteral("omasnap-native-smoke"));
    if (!frame.save(liveRoot + QStringLiteral("-native-output.png"), "PNG"))
      return 105;
    return 0;
  }
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
  if (!runBackdropCacheRenderingCheck(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 88;
  }
  if (!runSelectUndimHoleCheck(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 96;
  }
  if (!runCreationConstraintCheck(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 90;
  }
  if (!runMeasurementReadoutCheck(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 95;
  }
  if (!runQuickOutputChecks(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 73;
  }
  if (!runStuckModifierSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 97;
  }
  if (!runSpotlightHandleSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 98;
  }
  if (!runEyedropperSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 107;
  }
  if (!runSubmenuSelectionTriangleSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 124;
  }
  if (!runLayerHandlesSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 120;
  }
  if (!runHoverMoveSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 114;
  }
  if (!runLayerOrderSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 122;
  }
  {
    // A notch that arrives on the horizontal axis (Alt+wheel does, on some
    // setups) must step both ways. Reading only the vertical delta left every
    // Alt-adjusted setting able to rise and never fall.
    CaptureData capture;
    capture.monitor.name = QStringLiteral("TEST");
    capture.monitor.geometry = {0, 0, 800, 600};
    capture.monitor.pixelSize = {800, 600};
    capture.monitor.scale = 1.0;
    capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
    capture.source.fill(QColor(QStringLiteral("#182030")));
    capture.previewSize = capture.source.size();
    CaptureEditor editor(capture, CaptureEditor::CaptureMode::Fullscreen);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    QTest::keyClick(&editor, Qt::Key_A); // a tool whose wheel sets its size
    application.processEvents();
    const auto sideways = [&](int deltaX) {
      QWheelEvent event(QPointF(400, 300), QPointF(400, 300), {}, {deltaX, 0},
                        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
      QApplication::sendEvent(&editor, &event);
      application.processEvents();
      return editor.statusForTest();
    };
    const QString up = sideways(120);
    const QString down = sideways(-120);
    // One notch each way must land back where it started, not climb twice.
    if (!up.contains(QStringLiteral("Size 5")) ||
        !down.contains(QStringLiteral("Size 4"))) {
      qWarning().noquote()
          << QStringLiteral("A horizontal notch stepped only one way: up=") +
                 up + QStringLiteral(" down=") + down;
      return 116;
    }
    editor.close();
  }
  {
    // A capture past the advisory edge still opens and edits; the only
    // difference is that it says so.
    CaptureData tall;
    tall.monitor.name = QStringLiteral("TEST");
    tall.monitor.scale = 1.0;
    tall.monitor.pixelSize = {8, stitch::kWidelyOpenableEdge + 2};
    tall.source = QImage(8, stitch::kWidelyOpenableEdge + 2,
                         QImage::Format_ARGB32_Premultiplied);
    tall.source.fill(QColor(QStringLiteral("#203040")));
    tall.previewSize = tall.source.size();
    CaptureEditor editor(std::move(tall), CaptureEditor::CaptureMode::File);
    editor.resize(400, 300);
    editor.show();
    application.processEvents();
    if (!editor.statusForTest().contains(QStringLiteral("Very long capture")) ||
        !editor.statusForTest().contains(QStringLiteral("edits and saves"))) {
      qWarning().noquote()
          << QStringLiteral("Oversized capture did not explain itself: ")
          << editor.statusForTest();
      return 112;
    }
    editor.close();
  }
  if (!runViewportZoomSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 123;
  }
  if (!runLayerWeightSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 118;
  }
  if (!runAsyncCaptureRegionSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 82;
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
  if (!runTextClickAwayCommitCheck(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 87;
  }
  if (!runAnnotationLayerChecks(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 86;
  }
  if (!runContinuousAnnotationToolsSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 80;
  }
  if (!runSelectOutsideCanvasSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 106;
  }
  if (!runCenteredCreationSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 112;
  }
  if (!runShapeFillRenderingCheck(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 110;
  }
  if (!runShapeFillToolSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 111;
  }
  if (!runEllipseRenderingCheck(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 108;
  }
  if (!runEllipseToolSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 109;
  }
  if (!runTextPillRenderingCheck(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 102;
  }
  if (!runTextPillSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 103;
  }
  if (!runSelectAllDeleteSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 101;
  }
  if (!runDuplicateLayerSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 100;
  }
  if (!runKeyboardNudgeSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 117;
  }
  {
    // 1x is a named state ("no zoom"), not the bottom of a magnification
    // range, and the line names the whole spotlight, so the two settings you
    // are not touching never have to be remembered.
    const QString plain =
        spotlightStatusForTest(SpotlightShape::Ellipse, 1.0, 4.0);
    const QString magnified =
        spotlightStatusForTest(SpotlightShape::Rectangle, 2.0, 0.0);
    if (!plain.contains(QStringLiteral("no zoom")) ||
        !plain.contains(QStringLiteral("ellipse")) ||
        !plain.contains(QStringLiteral("border 4")) ||
        !magnified.contains(QStringLiteral("2.0×")) ||
        !magnified.contains(QStringLiteral("rectangle")) ||
        !magnified.contains(QStringLiteral("no border"))) {
      qWarning().noquote()
          << QStringLiteral("Spotlight status did not name the no-zoom state");
      return 113;
    }
  }
  {
    // Arming a tool says what it is set to; the spotlight names its shape,
    // zoom and border, which is the whole point of showing it.
    CaptureData capture;
    capture.monitor.name = QStringLiteral("TEST");
    capture.monitor.geometry = {0, 0, 800, 600};
    capture.monitor.pixelSize = {800, 600};
    capture.monitor.scale = 1.0;
    capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
    capture.source.fill(QColor(QStringLiteral("#182030")));
    capture.previewSize = capture.source.size();
    CaptureEditor editor(capture, CaptureEditor::CaptureMode::Fullscreen);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    QTest::keyClick(&editor, Qt::Key_S);
    application.processEvents();
    const QString armed = editor.statusForTest();
    if (!armed.contains(QStringLiteral("Spotlight")) ||
        !armed.contains(QStringLiteral("ellipse")) ||
        !armed.contains(QStringLiteral("2.0×")) ||
        !armed.contains(QStringLiteral("border"))) {
      qWarning().noquote()
          << QStringLiteral("Arming the spotlight did not report it: ") + armed;
      return 115;
    }
    // Adjusting one setting still reports all three: the shape and the ring
    // stay on screen while the wheel moves the zoom, and the zoom while
    // Alt+wheel moves the ring.
    {
      const auto spotlightWheel = [&](Qt::KeyboardModifiers modifiers) {
        QWheelEvent event(QPointF(400, 300), QPointF(400, 300), {}, {0, 120},
                          Qt::NoButton, modifiers, Qt::NoScrollPhase, false);
        QApplication::sendEvent(&editor, &event);
        application.processEvents();
      };
      spotlightWheel(Qt::NoModifier);
      if (editor.statusForTest() !=
          spotlightStatusForTest(SpotlightShape::Ellipse, 2.25, 4.0)) {
        qWarning().noquote()
            << QStringLiteral("Wheel reported only the zoom: ") +
                   editor.statusForTest();
        return 115;
      }
      spotlightWheel(Qt::AltModifier);
      if (editor.statusForTest() !=
          spotlightStatusForTest(SpotlightShape::Ellipse, 2.25, 6.0)) {
        qWarning().noquote()
            << QStringLiteral("Alt+wheel reported only the ring: ") +
                   editor.statusForTest();
        return 115;
      }
    }
    // S again cycles the shape and says so.
    QTest::keyClick(&editor, Qt::Key_S);
    application.processEvents();
    if (!editor.statusForTest().contains(QStringLiteral("rectangle"))) {
      qWarning().noquote() << QStringLiteral("Cycling the shape was silent: ") +
                                  editor.statusForTest();
      return 115;
    }
    QTest::keyClick(&editor, Qt::Key_A);
    application.processEvents();
    if (!editor.statusForTest().contains(QStringLiteral("Arrow"))) {
      qWarning().noquote() << QStringLiteral("Arming the arrow was silent: ") +
                                  editor.statusForTest();
      return 115;
    }
    editor.close();
  }
  if (!runStitchChecks()) {
    qWarning().noquote() << QStringLiteral("Stitcher checks failed");
    return 104;
  }
  if (!runSpotlightWheelSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 94;
  }
  if (!runCutToolSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 95;
  }
  if (!runLineHandleLegendSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 89;
  }
  if (!runOpLogSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 98;
  }
  if (!runShellQuoteCheck(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 83;
  }
  if (!runOpLogCapKeepsLeadingCrop(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 84;
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

  if (!runCrashSnapshotChecks(capture, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 99;
  }

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
    // Arm once: pressing R again would toggle the fill instead.
    QTest::keyClick(&constraintEditor, Qt::Key_R);
    const auto dragRectangle = [&](bool releaseShiftBeforeMouse) {
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
    if (flushedSnapshot(constraintEditor, snapshotPath).convertToFormat(freeExpected.format()) !=
        freeExpected)
      return 91;
    QTest::keyClick(&constraintEditor, Qt::Key_Z, Qt::ControlModifier);
    application.processEvents();

    dragRectangle(false);
    const QImage constrainedExpected = expectedRectangle(QPointF(295, 220));
    if (flushedSnapshot(constraintEditor, snapshotPath).convertToFormat(constrainedExpected.format()) !=
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
    const QImage cropped = flushedSnapshot(cropEditor, snapshotPath);
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
  const QImage initialSnapshot = flushedSnapshot(editor, snapshotPath);
  if (initialSnapshot.isNull())
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
  const QImage arrowSnapshot = flushedSnapshot(editor, snapshotPath);
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
  if (flushedSnapshot(editor, snapshotPath) != initialSnapshot)
    return 56;
  QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath) != arrowSnapshot)
    return 57;
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(140, 140));
  application.processEvents();
  QTest::keyClick(&editor, Qt::Key_Delete);
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath) != arrowSnapshot)
    return 39;
  QTest::keyClick(&editor, Qt::Key_L);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(260, 210));
  QTest::mouseMove(&editor, QPoint(520, 265), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(520, 265));
  application.processEvents();
  if (editor.cursor().shape() != Qt::CrossCursor)
    return 27;
  const QImage lineSnapshot = flushedSnapshot(editor, snapshotPath);
  if (lineSnapshot.isNull() || lineSnapshot == arrowSnapshot)
    return 40;
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath) != arrowSnapshot)
    return 41;
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath) != lineSnapshot)
    return 42;

  QTest::keyClick(&editor, Qt::Key_V);
  application.processEvents();
  const QImage lineUnselectedUi = editor.grab().toImage();
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(390, 238));
  application.processEvents();
  const QImage lineSelectedUi = editor.grab().toImage();
  // Handles stay visible even when this click would flip the help card.
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
  const QImage scaledLineSnapshot = flushedSnapshot(editor, snapshotPath);
  if (scaledLineSnapshot.isNull() || scaledLineSnapshot == lineSnapshot)
    return 45;
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath) != lineSnapshot)
    return 46;
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath) != scaledLineSnapshot)
    return 47;
  const QImage beforeFreehandSnapshot = flushedSnapshot(editor, snapshotPath);
  QTest::keyClick(&editor, Qt::Key_F);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(270, 390));
  QTest::mouseMove(&editor, QPoint(330, 360), 10);
  QTest::mouseMove(&editor, QPoint(390, 410), 10);
  QTest::mouseMove(&editor, QPoint(460, 370), 10);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(530, 420));
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath) == beforeFreehandSnapshot)
    return 28;
  // The pointer is left on the stroke it just drew, where a press would move
  // it; the crosshair belongs to empty canvas.
  QTest::mouseMove(&editor, QPoint(680, 470), 10);
  application.processEvents();
  if (editor.cursor().shape() != Qt::CrossCursor)
    return 28;
  const QImage beforeMarkerSnapshot = flushedSnapshot(editor, snapshotPath);
  QTest::keyClick(&editor, Qt::Key_2);
  QTest::keyClick(&editor, Qt::Key_C);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(470, 300));
  application.processEvents();
  if (editor.armedToolForTest() != CaptureEditor::Tool::Marker ||
      flushedSnapshot(editor, snapshotPath) == beforeMarkerSnapshot)
    return 48;
  const QImage beforeTextSnapshot = flushedSnapshot(editor, snapshotPath);
  QTest::keyClick(&editor, Qt::Key_T);
  // Text size options appear when the text button is hovered.
  QTest::mouseMove(&editor, QPoint(398, 43), 10);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(359, 78));
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
  QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return, Qt::ControlModifier);
  application.processEvents();
  if (!editor.grab().save(outputRoot + QStringLiteral("-text-committed.png"),
                          "PNG") ||
      flushedSnapshot(editor, snapshotPath) == beforeTextSnapshot)
    return 20;
  const QImage beforeMoveSnapshot = flushedSnapshot(editor, snapshotPath);
  QTest::keyClick(&editor, Qt::Key_V);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(400, 300));
  QTest::mouseMove(&editor, QPoint(420, 315), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(420, 315));
  application.processEvents();
  const QImage movedSnapshot = flushedSnapshot(editor, snapshotPath);
  if (movedSnapshot.isNull() || movedSnapshot == beforeMoveSnapshot)
    return 49;
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath) != beforeMoveSnapshot)
    return 50;
  QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath) != movedSnapshot)
    return 51;
  const QImage beforeEndpointSnapshot = flushedSnapshot(editor, snapshotPath);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(590, 365));
  QTest::mouseMove(&editor, QPoint(620, 380), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(620, 380));
  application.processEvents();
  const QImage resizedSnapshot = flushedSnapshot(editor, snapshotPath);
  if (resizedSnapshot.isNull() || resizedSnapshot == beforeEndpointSnapshot)
    return 52;
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath) != beforeEndpointSnapshot)
    return 53;
  QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath) != resizedSnapshot)
    return 54;
  if (!editor.grab().save(outputRoot + QStringLiteral("-vector-selected.png"),
                          "PNG"))
    return 21;
  QTest::mouseDClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(380, 330));
  if (QApplication::focusWidget() != nullptr &&
      QApplication::focusWidget() != &editor) {
    QTest::keyClicks(QApplication::focusWidget(),
                     QStringLiteral("Edited Neucha"));
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return, Qt::ControlModifier);
  } else {
    return 22;
  }
  const QImage beforeBackdropSnapshot = flushedSnapshot(editor, snapshotPath);
  QTest::keyClick(&editor, Qt::Key_B);
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath) == beforeBackdropSnapshot)
    return 55;
  // Palette toolbar button after the grouped shape controls.
  QTest::mouseMove(&editor, QPoint(435, 43), 20);
  application.processEvents();
  if (!editor.grab().save(outputRoot + QStringLiteral("-palette.png"), "PNG"))
    return 14;
  // Custom color control in the open palette strip.
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(531, 81));
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(320, 200));
  // Freehand toolbar button.
  QTest::mouseMove(&editor, QPoint(130, 43), 20);
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
    const QImage beforeRedaction = flushedSnapshot(redactionEditor, snapshotPath);
    const QImage beforeRedactionUi = redactionEditor.grab().toImage();
    QTest::keyClick(&redactionEditor, Qt::Key_D);
    QTest::mousePress(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(300, 200));
    QTest::mouseMove(&redactionEditor, QPoint(420, 200), 20);
    QTest::mouseRelease(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(420, 200));
    application.processEvents();
    if (flushedSnapshot(redactionEditor, snapshotPath) != beforeRedaction)
      return 72;
    QTest::mousePress(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(300, 200));
    QTest::mouseMove(&redactionEditor, QPoint(420, 260), 20);
    QTest::mouseRelease(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(420, 260));
    application.processEvents();
    const QImage pixelatedRedactionUi = redactionEditor.grab().toImage();
    if (pixelatedRedactionUi == beforeRedactionUi)
      return 83;
    const QImage pixelatedRedaction = flushedSnapshot(redactionEditor, snapshotPath);
    if (pixelatedRedaction.isNull() || pixelatedRedaction == beforeRedaction)
      return 73;

    QTest::keyClick(&redactionEditor, Qt::Key_V);
    QTest::mouseClick(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(360, 230));
    QTest::keyClick(&redactionEditor, Qt::Key_D);
    application.processEvents();
    const QImage solidRedaction = flushedSnapshot(redactionEditor, snapshotPath);
    if (solidRedaction.isNull() || solidRedaction == pixelatedRedaction)
      return 74;
    QTest::keyClick(&redactionEditor, Qt::Key_Z, Qt::ControlModifier);
    application.processEvents();
    if (flushedSnapshot(redactionEditor, snapshotPath) != pixelatedRedaction)
      return 75;
    QTest::keyClick(&redactionEditor, Qt::Key_Y, Qt::ControlModifier);
    application.processEvents();
    if (flushedSnapshot(redactionEditor, snapshotPath) != solidRedaction)
      return 76;

    QTest::mousePress(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(360, 230));
    QTest::mouseMove(&redactionEditor, QPoint(380, 245), 20);
    QTest::mouseRelease(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(380, 245));
    application.processEvents();
    const QImage movedRedaction = flushedSnapshot(redactionEditor, snapshotPath);
    if (movedRedaction.isNull() || movedRedaction == solidRedaction)
      return 77;
    QTest::keyClick(&redactionEditor, Qt::Key_Z, Qt::ControlModifier);
    application.processEvents();
    if (flushedSnapshot(redactionEditor, snapshotPath) != solidRedaction)
      return 78;

    QTest::keyClick(&redactionEditor, Qt::Key_D);
    QTest::mousePress(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(300, 200));
    QTest::mouseMove(&redactionEditor, QPoint(280, 190), 20);
    QTest::mouseRelease(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(280, 190));
    application.processEvents();
    if (flushedSnapshot(redactionEditor, snapshotPath) == solidRedaction ||
        !redactionEditor.grab().save(
            outputRoot + QStringLiteral("-secure-redaction.png"), "PNG"))
      return 79;
    QTest::mousePress(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(280, 190));
    QTest::mouseMove(&redactionEditor, QPoint(420, 190), 20);
    QTest::mouseRelease(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(420, 190));
    application.processEvents();
    if (flushedSnapshot(redactionEditor, snapshotPath) == beforeRedaction)
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
  QTest::mouseMove(&compactToolbarEditor, QPoint(20, 25), 20);
  application.processEvents();
  QTest::mouseMove(&compactToolbarEditor, QPoint(700, 25), 20);
  application.processEvents();
  const QImage compactToolbarUi = compactToolbarEditor.grab().toImage();
  if (compactToolbarUi.pixelColor(20, 25).alpha() < 240 ||
      compactToolbarUi.pixelColor(700, 25).alpha() < 240 ||
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

  CaptureData windowPickCapture = capture;
  const QImage originalSource = windowPickCapture.source.copy();
  CaptureEditor windowPickEditor(windowPickCapture,
                                 CaptureEditor::CaptureMode::Window);
  windowPickEditor.resize(800, 600);
  windowPickEditor.show();
  application.processEvents();
  QTest::mouseClick(&windowPickEditor, Qt::LeftButton, Qt::NoModifier,
                    QPoint(200, 160));
  application.processEvents();
  const QImage windowCrop = windowPickEditor.renderCurrentOutput();
  const QImage expectedCrop = renderCapture(
      capture, QRectF(80, 80, 300, 220), {}, BackgroundStyle::None);
  if (windowPickEditor.captureData().source != originalSource ||
      windowPickEditor.currentSelection() != QRectF(80, 80, 300, 220) ||
      windowCrop.convertToFormat(QImage::Format_ARGB32) !=
          expectedCrop.convertToFormat(QImage::Format_ARGB32) ||
      !windowPickEditor.grab().save(
          outputRoot + QStringLiteral("-window-pick-crop.png"), "PNG"))
    return 63;
  windowPickEditor.close();

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
    const QImage snapshotBeforeSave =
        flushedSnapshot(finishEditor, snapshotPath)
            .convertToFormat(QImage::Format_ARGB32);
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
    if (QImage(savedPath).convertToFormat(QImage::Format_ARGB32) !=
            snapshotBeforeSave ||
        QFile::exists(snapshotPath))
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

  QString clipboardError;
  if (!runClipboardSmoke(clipboardError)) {
    qWarning().noquote() << clipboardError;
    return 88;
  }

  QString transformError;
  if (!runTransformSmoke(transformError)) {
    qWarning().noquote() << transformError;
    return 67;
  }

  QString cutError;
  if (!runCutSmoke(cutError)) {
    qWarning().noquote() << "cut smoke failed:" << cutError;
    return EXIT_FAILURE;
  }

  QString paletteError;
  if (!runPaletteConfigSmoke(paletteError)) {
    qWarning().noquote() << "palette config smoke failed:" << paletteError;
    return EXIT_FAILURE;
  }

  QString instanceError;
  if (!runInstanceLockSmoke(instanceError)) {
    qWarning().noquote() << instanceError;
    return 85;
  }
  return 0;
}
