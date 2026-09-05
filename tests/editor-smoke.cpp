/** @fileoverview Exercises capture editor behavior without a live compositor.
 */
#include "capture.hpp"
#include "output-config.hpp"
#include "cli-path.hpp"
#include "clipboard-smoke.hpp"
#include "cut-mapping-smoke.hpp"
#include "cut-smoke.hpp"
#include "editor.hpp"
#include "overlay-chrome.hpp"
#include "recent-snaps.hpp"
#include "instance-lock-smoke.hpp"
#include "palette-config-smoke.hpp"
#include "pin-layout-smoke.hpp"
#include "stitch-smoke.hpp"
#include "stitch.hpp"
#include "pin-lifecycle-smoke.hpp"
#include "text-band.hpp"
#include "transform-smoke.hpp"
#include "eyedropper.hpp"

#include <QApplication>
#include <QBuffer>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QThread>
#include <QFile>
#include <QFileInfo>
#include <QFontInfo>
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
#include <array>
#include <cmath>
#include <csignal>
#include <limits>
#include <numbers>
#include <optional>
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
    QTest::keyClick(&editor, Qt::Key_Space); // Region -> Window
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
    QTest::keyClick(&editor, Qt::Key_Space); // Region -> Window
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
    QTest::keyClick(&editor, Qt::Key_Space); // Region -> Window
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

  QTest::keyClick(&editor, Qt::Key_Space); // Region -> Window
  QTest::mouseMove(&editor, QPoint(200, 150), 20);
  QApplication::processEvents();
  if (!expect(QStringLiteral("600 × 440"), QStringLiteral("Hovered window")))
    return false;
  QTest::keyClick(&editor, Qt::Key_Space); // Window -> Scroll
  QTest::keyClick(&editor, Qt::Key_Space); // Scroll -> Region
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

void paintTextLikeBand(QImage &image, const QRectF &logicalBand, qreal scale,
                       const QColor &ink) {
  QPainter painter(&image);
  painter.setRenderHint(QPainter::Antialiasing, false);
  painter.setPen(Qt::NoPen);
  painter.setBrush(ink);
  for (int glyph = 0; glyph < 11; ++glyph) {
    const qreal topInset = glyph % 4 == 0 ? 0.0 : 2.0;
    const qreal bottomInset = glyph % 3 == 0 ? 0.0 : 1.0;
    const QRectF stem(logicalBand.left() + glyph * 13.0,
                      logicalBand.top() + topInset, 6.0,
                      logicalBand.height() - topInset - bottomInset);
    painter.drawRect(QRectF(stem.topLeft() * scale, stem.size() * scale));
  }
}

/** Local edge scan finds text height across themes and native image scales. */
// The chrome font is pinned in code (the platform theme is disabled at
// startup); the general face must resolve to a real family and the mono face
// to a fixed-pitch one.
bool runChromeFontCheck(QString &error) {
  const QFont general = chromeFont(11, true);
  if (general.pixelSize() != 11 || !general.bold() ||
      QFontInfo(general).family().isEmpty()) {
    error = QStringLiteral("chromeFont(11, true) did not resolve: %1")
                .arg(general.toString());
    return false;
  }
  const QFont mono = chromeMonoFont(12);
  const QFont appFont = QApplication::font();
  if (appFont.pointSize() != 11 || QFontInfo(appFont).family().isEmpty() ||
      appFont.families() != chromeDefaultFont().families()) {
    error = QStringLiteral("application default font is %1, expected "
                           "chromeDefaultFont()")
                .arg(appFont.toString());
    return false;
  }
  const QFontInfo monoInfo(mono);
  if (mono.pixelSize() != 12 || mono.bold() || monoInfo.family().isEmpty() ||
      !monoInfo.fixedPitch()) {
    error = QStringLiteral("chromeMonoFont(12) resolved to %1, expected a "
                           "fixed-pitch family")
                .arg(monoInfo.family());
    return false;
  }
  return true;
}

bool runTextBandDetectionCheck(QString &error) {
  for (const qreal scale : {1.0, 2.0}) {
    const QSize logicalSize(360, 180);
    QImage image((QSizeF(logicalSize) * scale).toSize(), QImage::Format_RGB32);
    image.fill(QColor(QStringLiteral("#515862")));
    {
      // Two surfaces in the same scan row exercise the edge-density approach:
      // there is deliberately no single row-wide background color.
      QPainter painter(&image);
      painter.fillRect(
          QRectF(QPointF(180 * scale, 0), QSizeF(180, 180) * scale),
          QColor(QStringLiteral("#606772")));
    }
    const QRectF expected(82, 66, 149, 16);
    paintTextLikeBand(image, expected, scale,
                      QColor(QStringLiteral("#737b86")));

    // The pointer may sit just under the glyphs: a highlighter should still
    // choose the nearby row, but report its native-pixel extent.
    const auto band =
        detectTextBand(image, QPointF(130, 89) * scale, QSizeF(scale, scale));
    if (!band ||
        std::abs(band->center() / scale - expected.center().y()) > 3.0 ||
        band->height() / scale < 10.0 || band->height() / scale > 20.0) {
      error = QStringLiteral("Text-band detector missed a %1x mixed-background "
                             "text row")
                  .arg(scale);
      return false;
    }

    // Bowl-shaped glyphs can produce distinct upper/lower edge runs with a
    // quiet five-row middle. They are one text line, not two undersized bands.
    QImage split((QSizeF(logicalSize) * scale).toSize(), QImage::Format_RGB32);
    split.fill(QColor(QStringLiteral("#252a31")));
    {
      QPainter painter(&split);
      painter.setRenderHint(QPainter::Antialiasing, false);
      painter.setPen(Qt::NoPen);
      painter.setBrush(QColor(QStringLiteral("#d8dbe2")));
      for (int glyph = 0; glyph < 11; ++glyph) {
        const qreal x = (82.0 + glyph * 13.0) * scale;
        painter.drawRect(QRectF(x, 60.0 * scale, 6.0 * scale, 4.0 * scale));
        painter.drawRect(QRectF(x, 69.0 * scale, 6.0 * scale, 5.0 * scale));
      }
    }
    const auto splitBand =
        detectTextBand(split, QPointF(130, 72) * scale, QSizeF(scale, scale));
    if (!splitBand || splitBand->height() / scale < 12.0 ||
        std::abs(splitBand->center() / scale - 67.0) > 3.0) {
      error = QStringLiteral(
                  "Text-band detector fragmented split glyphs at %1x")
                  .arg(scale);
      return false;
    }
  }

  QImage flat(320, 160, QImage::Format_RGB32);
  flat.fill(QColor(QStringLiteral("#e6e4dd")));
  if (detectTextBand(flat, QPointF(120, 80)) ||
      detectTextBand(flat, QPointF(319.9, 159.9))) {
    error = QStringLiteral("Text-band detector snapped on a flat background");
    return false;
  }
  return true;
}

/** The editor locks a detected text highlight and preserves freehand fallback. */
bool runTextAwareHighlighterEditorCheck(QApplication &application,
                                        QString &error) {
  constexpr qreal sourceScale = 2.0;
  const QSize logicalSize(400, 240);
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = QRect(QPoint(), logicalSize);
  capture.monitor.pixelSize = (QSizeF(logicalSize) * sourceScale).toSize();
  capture.monitor.scale = sourceScale;
  capture.previewSize = logicalSize;
  capture.source = QImage(capture.monitor.pixelSize, QImage::Format_RGB32);
  capture.source.fill(QColor(QStringLiteral("#20242c")));
  const QRectF textBand(80, 96, 149, 16);
  paintTextLikeBand(capture.source, textBand, sourceScale,
                    QColor(QStringLiteral("#d8dbe2")));

  CaptureEditor editor(capture, CaptureEditor::CaptureMode::Fullscreen);
  editor.setSuppressSnapshots(true);
  // baseImageRect is exactly 400x240 at (30,135), making test gestures map
  // 1:1 to annotation coordinates while leaving the toolbar clear of the
  // capture tabs. The source itself remains 2x HiDPI.
  editor.resize(460, 500);
  editor.show();
  application.processEvents();
  const auto widgetPoint = [&editor](qreal x, qreal y) {
    return editor.toScreenPointForTest(QPointF(x, y)).toPoint();
  };
  const auto waitForPreview = [&](bool present) {
    QElapsedTimer timeout;
    timeout.start();
    while (editor.highlighterPreviewRectForTest().isEmpty() == present &&
           timeout.elapsed() < 2000) {
      application.processEvents(QEventLoop::ExcludeUserInputEvents);
      QThread::yieldCurrentThread();
    }
    return editor.highlighterPreviewRectForTest().isEmpty() != present;
  };

  QTest::keyClick(&editor, Qt::Key_H);
  application.processEvents();
  if (!editor.highlighterSnapModeForTest() ||
      !editor.statusForTest().contains(QStringLiteral("Snap")) ||
      !editor.statusForTest().contains(QStringLiteral("automatic"))) {
    error = QStringLiteral("Highlighter did not start in disclosed Snap mode");
    return false;
  }
  // Hovering a detected row replaces the fixed crosshair with a measured
  // I-beam. The detected row supplies only its height: the beam itself follows
  // the pointer fluidly until mouse-down commits to a row.
  QTest::mouseMove(&editor, widgetPoint(105, 114), 10);
  application.processEvents();
  if (!waitForPreview(true)) {
    error = QStringLiteral("Highlighter text probe did not finish");
    return false;
  }
  const QRectF firstBeam = editor.highlighterPreviewRectForTest();
  QTest::mouseMove(&editor, widgetPoint(105, 120), 10);
  application.processEvents();
  const QRectF beam = editor.highlighterPreviewRectForTest();
  const QImage hovered = editor.grab().toImage();
  const QPoint beamCenter = widgetPoint(beam.center().x(), beam.center().y());
  if (firstBeam.isEmpty() || beam.isEmpty() ||
      editor.cursor().shape() != Qt::BlankCursor ||
      std::abs(firstBeam.center().y() - 114.0) > 0.6 ||
      std::abs(beam.center().y() - 120.0) > 0.6 ||
      std::abs(beam.height() - firstBeam.height()) > 0.01 ||
      beam.height() < 12.0 || beam.height() > 22.0 ||
      !hovered.rect().contains(beamCenter) ||
      hovered.pixelColor(beamCenter).lightness() < 180) {
    error = QStringLiteral(
        "Snap highlighter preview did not follow the mouse at detected height");
    return false;
  }
  QTest::mouseMove(&editor, widgetPoint(70, 180), 10);
  application.processEvents();
  if (!waitForPreview(false) ||
      !editor.highlighterPreviewRectForTest().isEmpty() ||
      editor.cursor().shape() != Qt::CrossCursor) {
    error = QStringLiteral("Highlighter I-beam did not fall back off text");
    return false;
  }
  QTest::mouseMove(&editor, widgetPoint(105, 118), 10);
  application.processEvents();
  if (!waitForPreview(true)) {
    error = QStringLiteral("Highlighter text probe did not return before drag");
    return false;
  }

  // Start just below the glyph box, then deliberately wobble far above and
  // below it. Every recorded point should remain on the detected centerline.
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier,
                    widgetPoint(105, 118));
  QTest::mouseMove(&editor, widgetPoint(190, 145), 10);
  QTest::mouseMove(&editor, widgetPoint(270, 75), 10);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      widgetPoint(325, 135));
  application.processEvents();
  if (editor.operationLog().isEmpty() ||
      editor.operationLog().constLast().annotations.size() != 1) {
    error = QStringLiteral("Text-aware highlighter did not commit a layer");
    return false;
  }
  const Annotation locked =
      editor.operationLog().constLast().annotations.constFirst();
  qreal minimumY = std::numeric_limits<qreal>::max();
  qreal maximumY = std::numeric_limits<qreal>::lowest();
  for (const QPointF &point : locked.points) {
    minimumY = std::min(minimumY, point.y());
    maximumY = std::max(maximumY, point.y());
  }
  const qreal lockedWidth = std::max<qreal>(6.0, locked.size * 3.0);
  if (locked.kind != Annotation::Kind::Highlighter ||
      locked.points.size() < 3 || maximumY - minimumY > 0.01 ||
      std::abs(minimumY - textBand.center().y()) > 3.0 || lockedWidth < 12.0 ||
      lockedWidth > 22.0) {
    error = QStringLiteral("Detected highlight did not stay straight at the "
                           "text height");
    return false;
  }

  // The same geometry must reach the native-resolution renderer: ink covers
  // the measured row, but not background several pixels beyond its edge.
  const QImage rendered = editor.renderCurrentOutput();
  const int nativeX = qRound(260 * sourceScale);
  const int nativeCenter = qRound(minimumY * sourceScale);
  const int nativeOutside =
      qRound((minimumY + lockedWidth / 2.0 + 4.0) * sourceScale);
  if (rendered.isNull() ||
      rendered.pixelColor(nativeX, nativeCenter) ==
          capture.source.pixelColor(nativeX, nativeCenter) ||
      rendered.pixelColor(nativeX, nativeOutside) !=
          capture.source.pixelColor(nativeX, nativeOutside)) {
    error = QStringLiteral("Text-height highlight did not render at its locked "
                           "native-pixel width");
    return false;
  }

  // Repeating H switches the active tool to Normal: text scanning and its
  // I-beam are both gone, and Alt+wheel changes the freehand thickness.
  QTest::keyClick(&editor, Qt::Key_H);
  QTest::mouseMove(&editor, widgetPoint(105, 118), 10);
  application.processEvents();
  if (editor.highlighterSnapModeForTest() ||
      !editor.statusForTest().contains(QStringLiteral("Normal")) ||
      !editor.statusForTest().contains(QStringLiteral("Alt+wheel")) ||
      !editor.highlighterPreviewRectForTest().isEmpty() ||
      editor.cursor().shape() != Qt::CrossCursor) {
    error = QStringLiteral("Repeated H did not arm disclosed Normal mode");
    return false;
  }
  {
    const QPointF wheelPoint(widgetPoint(105, 118));
    QWheelEvent wheel(wheelPoint, wheelPoint, {}, {0, 120}, Qt::NoButton,
                      Qt::AltModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&editor, &wheel);
    application.processEvents();
  }
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier,
                    widgetPoint(45, 118));
  QTest::mouseMove(&editor, widgetPoint(55, 145), 10);
  QTest::mouseMove(&editor, widgetPoint(65, 75), 10);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      widgetPoint(75, 135));
  application.processEvents();
  const Annotation normal =
      editor.operationLog().constLast().annotations.constFirst();
  minimumY = std::numeric_limits<qreal>::max();
  maximumY = std::numeric_limits<qreal>::lowest();
  for (const QPointF &point : normal.points) {
    minimumY = std::min(minimumY, point.y());
    maximumY = std::max(maximumY, point.y());
  }
  if (normal.kind != Annotation::Kind::Highlighter ||
      maximumY - minimumY < 40.0 || !qFuzzyCompare(normal.size, 5.0) ||
      !editor.statusForTest().contains(QStringLiteral("Normal"))) {
    error = QStringLiteral(
        "Normal highlighter detected a row or ignored Alt+wheel thickness");
    return false;
  }

  // Clicking the already-active toolbar tool follows the same repeated-tool
  // convention and returns to Snap.
  const QRectF highlighterButton = editor.highlighterToolbarRectForTest();
  if (highlighterButton.isEmpty()) {
    error = QStringLiteral("Highlighter toolbar button was unavailable");
    return false;
  }
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                    highlighterButton.center().toPoint());
  application.processEvents();
  if (!editor.highlighterSnapModeForTest() ||
      !editor.statusForTest().contains(QStringLiteral("Snap"))) {
    error = QStringLiteral("Active toolbar click did not return H to Snap");
    return false;
  }

  // Snap still preserves its freehand fallback away from detected text. The
  // manual size changed in Normal is retained for such an off-text stroke.
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier,
                    widgetPoint(70, 180));
  QTest::mouseMove(&editor, widgetPoint(140, 211), 10);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      widgetPoint(205, 170));
  application.processEvents();
  const Annotation fallback =
      editor.operationLog().constLast().annotations.constFirst();
  minimumY = std::numeric_limits<qreal>::max();
  maximumY = std::numeric_limits<qreal>::lowest();
  for (const QPointF &point : fallback.points) {
    minimumY = std::min(minimumY, point.y());
    maximumY = std::max(maximumY, point.y());
  }
  if (fallback.kind != Annotation::Kind::Highlighter ||
      maximumY - minimumY < 20.0 || !qFuzzyCompare(fallback.size, 5.0)) {
    error = QStringLiteral("Highlighter lost its freehand no-text fallback");
    return false;
  }
  editor.close();
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

/** Pointer-only chrome damages narrow strips/local badges, never a full 6K
 *  overlay. QWidget's backing store preserves every pixel outside this region. */
bool runPointerDamageRegionCheck(QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 6016, 3384};
  capture.monitor.pixelSize = {6016, 3384};
  capture.monitor.scale = 1.0;
  capture.previewSize = capture.monitor.pixelSize;
  capture.source = QImage(32, 18, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#182030")));

  CaptureEditor editor(capture);
  editor.resize(capture.previewSize);
  const QRegion motion =
      editor.pointerMotionRegionForTest(QPointF(1000, 900)) |
      editor.pointerMotionRegionForTest(QPointF(5000, 2600));
  qint64 damagedPixels = 0;
  for (const QRect &rect : motion)
    damagedPixels += static_cast<qint64>(rect.width()) * rect.height();
  const qint64 screenPixels =
      static_cast<qint64>(capture.previewSize.width()) *
      capture.previewSize.height();
  if (motion.isEmpty() || damagedPixels >= screenPixels / 20) {
    error = QStringLiteral(
        "Pointer chrome damaged %1 of %2 6K pixels instead of narrow regions")
                .arg(damagedPixels)
                .arg(screenPixels);
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

/**
 * Saved filenames lead with the date (so the folder sorts chronologically)
 * and end with the slug of the app under the selection when one is known.
 */
bool runScreenshotFilenameChecks(QString &error) {
  const struct {
    const char *appClass;
    const char *slug;
  } slugCases[] = {
      {"firefox", "firefox"},
      {"Alacritty", "alacritty"},
      {"org.gnome.Nautilus", "nautilus"},
      {"Code - OSS", "code-oss"},
      {"a.very.long.segment-name-that-keeps-going-on",
       "segment-name-that-keeps"},
      {"---", ""},
      {"", ""},
  };
  for (const auto &[appClass, slug] : slugCases) {
    const QString actual = appFilenameSlug(QString::fromUtf8(appClass));
    if (actual != QString::fromUtf8(slug)) {
      error = QStringLiteral("appFilenameSlug(%1) = %2, expected %3")
                  .arg(QString::fromUtf8(appClass), actual,
                       QString::fromUtf8(slug));
      return false;
    }
  }

  const QVector<WindowTarget> windows = {
      {QRect(0, 0, 400, 300), QStringLiteral("w1"), QStringLiteral("Browser"),
       QStringLiteral("firefox")},
      {QRect(400, 0, 400, 300), QStringLiteral("w2"), QStringLiteral("Shell"),
       QStringLiteral("Alacritty")},
  };
  if (dominantAppClass(windows, QRectF(250, 50, 200, 100)) !=
      QStringLiteral("firefox")) {
    error = QStringLiteral("dominantAppClass did not pick the larger overlap");
    return false;
  }
  if (dominantAppClass(windows, QRectF(350, 50, 200, 100)) !=
      QStringLiteral("Alacritty")) {
    error = QStringLiteral("dominantAppClass did not pick the larger overlap");
    return false;
  }
  if (!dominantAppClass(windows, QRectF(0, 400, 100, 100)).isEmpty()) {
    error = QStringLiteral("dominantAppClass named an app with no overlap");
    return false;
  }

  const QDateTime when(QDate(2026, 8, 23), QTime(14, 5, 9));
  const struct {
    const char *pattern;
    const char *app;
    const char *expected;
  } patternCases[] = {
      {"screenshot-{date}_{time}-{app}", "firefox",
       "screenshot-2026-08-23_14-05-09-firefox.png"},
      {"screenshot-{date}_{time}-{app}", "",
       "screenshot-2026-08-23_14-05-09.png"},
      {"{app}_{date}", "", "2026-08-23.png"},
      {"shot {date}.png", "", "shot 2026-08-23.png"},
      {"../{date}", "", "2026-08-23.png"},
      {"", "", "screenshot-2026-08-23_14-05-09.png"},
  };
  for (const auto &[pattern, app, expected] : patternCases) {
    const QString actual = formatScreenshotFilename(
        QString::fromUtf8(pattern), when, QString::fromUtf8(app));
    if (actual != QString::fromUtf8(expected)) {
      error = QStringLiteral("formatScreenshotFilename(%1, %2) = %3, expected %4")
                  .arg(QString::fromUtf8(pattern), QString::fromUtf8(app),
                       actual, QString::fromUtf8(expected));
      return false;
    }
  }

  QTemporaryDir directory;
  if (!directory.isValid()) {
    error = QStringLiteral("Could not create filename-check directory");
    return false;
  }
  {
    const QString configPath =
        QDir(directory.path()).filePath(QStringLiteral("omasnap.conf"));
    QFile configFile(configPath);
    if (!configFile.open(QIODevice::WriteOnly | QIODevice::Text) ||
        configFile.write("[output]\ndirectory = ~/Captures\n"
                         "filename = {date} {app}\n") < 0) {
      error = QStringLiteral("Could not write filename-check config");
      return false;
    }
    configFile.close();
    const OutputConfig loaded = loadOutputConfig(configPath);
    if (loaded.directory != QDir::homePath() + QStringLiteral("/Captures") ||
        loaded.filename != QStringLiteral("{date} {app}")) {
      error = QStringLiteral("loadOutputConfig read %1 / %2")
                  .arg(loaded.directory, loaded.filename);
      return false;
    }
    const OutputConfig defaults = loadOutputConfig(
        QDir(directory.path()).filePath(QStringLiteral("missing.conf")));
    if (!defaults.directory.isEmpty() ||
        defaults.filename != QStringLiteral("screenshot-{date}_{time}-{app}")) {
      error = QStringLiteral("loadOutputConfig changed defaults for a missing file");
      return false;
    }
  }
  const QByteArray previousDir = qgetenv("OMASNAP_SCREENSHOT_DIR");
  qputenv("OMASNAP_SCREENSHOT_DIR", directory.path().toUtf8());
  const auto restoreDir = qScopeGuard([&previousDir] {
    if (previousDir.isEmpty())
      qunsetenv("OMASNAP_SCREENSHOT_DIR");
    else
      qputenv("OMASNAP_SCREENSHOT_DIR", previousDir);
  });
  QImage image(4, 4, QImage::Format_RGB32);
  image.fill(Qt::red);
  const QString source = QDir(directory.path()).filePath(
      QStringLiteral("source.png"));
  if (!image.save(source)) {
    error = QStringLiteral("Could not write filename-check source");
    return false;
  }
  // The move reads the real config path; keep the developer's own
  // [output] filename out of this check.
  QStandardPaths::setTestModeEnabled(true);
  const auto restoreTestMode =
      qScopeGuard([] { QStandardPaths::setTestModeEnabled(false); });
  QString moveError;
  const QString saved = moveSnapshotToScreenshots(
      source, moveError, appFilenameSlug(QStringLiteral("firefox")));
  const QString name = QFileInfo(saved).fileName();
  static const QRegularExpression pattern(QStringLiteral(
      "^screenshot-\\d{4}-\\d{2}-\\d{2}_\\d{2}-\\d{2}-\\d{2}-firefox\\.png$"));
  if (saved.isEmpty() || !moveError.isEmpty() || !pattern.match(name).hasMatch()) {
    error = QStringLiteral("Saved filename did not match <date>-<app>: %1 %2")
                .arg(name, moveError);
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
    editor.waitForExport();
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

/** Ctrl+wheel down zooms out below fit, to a 10% floor, and Ctrl+0 comes
 *  back to fit. */
bool runZoomOutCheck(QApplication &application, QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(255, 220, 40));
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

  const auto ctrlWheel = [&](int deltaY) {
    QWheelEvent wheel(QPointF(400, 300), QPointF(400, 300), {}, {0, deltaY},
                      Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase,
                      false);
    QApplication::sendEvent(&editor, &wheel);
    application.processEvents();
  };
  // A point just inside the fitted image's corner is bright yellow at fit
  // and stops being image once the view zooms out. Probed at the fit rect
  // measured before any zoom, so the spot is geometry-independent.
  const QPoint fitCorner =
      (editor.editImageRectForTest().topLeft() + QPointF(5, 5)).toPoint();
  const auto cornerIsImage = [&] {
    const QColor color = editor.grab().toImage().pixelColor(fitCorner);
    return color.red() > 200 && color.green() > 150 && color.blue() < 120;
  };
  if (!cornerIsImage()) {
    error = QStringLiteral("Zoom fixture corner was not image at fit");
    return false;
  }
  ctrlWheel(-120);
  if (cornerIsImage()) {
    error = QStringLiteral("Ctrl+wheel down did not zoom out below fit");
    return false;
  }
  for (int step = 0; step < 30; ++step)
    ctrlWheel(-120);
  if (cornerIsImage()) {
    error = QStringLiteral("Deep zoom out lost the floor");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_0, Qt::ControlModifier);
  application.processEvents();
  if (!cornerIsImage()) {
    error = QStringLiteral("Ctrl+0 did not return to fit from a zoom out");
    return false;
  }
  return true;
}

/** Outlined text renders a white halo around colored glyphs and survives
 *  the op log; plain text renders no halo. */
bool runTextOutlineCheck(QString &error) {
  CaptureData capture;
  capture.monitor.scale = 1.0;
  capture.monitor.pixelSize = {400, 200};
  capture.source = QImage(400, 200, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#182030")));
  capture.previewSize = capture.source.size();

  Annotation text;
  text.kind = Annotation::Kind::Text;
  text.start = {30, 50};
  text.color = QColor(QStringLiteral("#ff375f"));
  text.size = 5;
  text.text = QStringLiteral("Ok");
  text.textBackground = TextBackground::Outline;

  const QRect band =
      annotationTextBounds(text).toAlignedRect().adjusted(-6, -6, 6, 6);
  const auto count = [&band](const QImage &frame, auto match) {
    int hits = 0;
    for (int y = band.top(); y <= band.bottom(); ++y)
      for (int x = band.left(); x <= band.right(); ++x)
        if (frame.rect().contains(x, y) && match(frame.pixelColor(x, y)))
          ++hits;
    return hits;
  };
  const auto halo = [](const QColor &c) {
    return c.red() >= 225 && c.green() >= 225 && c.blue() >= 225;
  };
  const auto glyph = [](const QColor &c) {
    return c.red() > 200 && c.green() < 120 && c.blue() < 140;
  };

  const QImage outlined = renderCapture(capture, QRectF(0, 0, 400, 200), {text},
                                        BackgroundStyle::None);
  if (count(outlined, halo) < 20) {
    error = QStringLiteral("Outlined text drew no white halo");
    return false;
  }
  if (count(outlined, glyph) < 20) {
    error = QStringLiteral("Outlined text lost its glyph color");
    return false;
  }
  text.textBackground = TextBackground::Plain;
  const QImage plain = renderCapture(capture, QRectF(0, 0, 400, 200), {text},
                                     BackgroundStyle::None);
  if (count(plain, halo) != 0) {
    error = QStringLiteral("Plain text grew a halo");
    return false;
  }

  // The style survives a save and load of the op log, so reopening a capture
  // does not quietly turn outlined text back into a pill.
  text.textBackground = TextBackground::Outline;
  text.id = 1;
  OperationLog log;
  Operation annotate;
  annotate.type = Operation::Type::Annotate;
  annotate.annotations = {text};
  log.ops.push_back(std::move(annotate));
  log.index = log.ops.size();
  log.nextId = 2;
  const QString logPath =
      QDir(QDir::tempPath()).filePath(QStringLiteral("outline-oplog.json"));
  OperationLog loaded;
  QString logError;
  if (!saveOperationLog(logPath, log, logError) ||
      !loadOperationLog(logPath, loaded, logError)) {
    QFile::remove(logPath);
    error = QStringLiteral("Op log with outlined text failed to round-trip: %1")
                .arg(logError);
    return false;
  }
  QFile::remove(logPath);
  if (loaded.ops.size() != 1 || loaded.ops.constFirst().annotations.isEmpty() ||
      loaded.ops.constFirst().annotations.constFirst().textBackground !=
          TextBackground::Outline) {
    error = QStringLiteral("Outlined text came back as a different style");
    return false;
  }
  return true;
}

/** The draft's native caret is hidden; QPlainTextEdit actually reads cursorWidth. */
bool runNativeCaretHiddenCheck(QApplication &application, QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(300, 200, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(Qt::white);
  capture.previewSize = capture.source.size();

  CaptureEditor editor(capture, CaptureEditor::CaptureMode::File);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();
  QTest::keyClick(&editor, Qt::Key_T);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                    editor.toScreenPointForTest(QPointF(80, 80)).toPoint());
  application.processEvents();
  auto *draft = qobject_cast<QPlainTextEdit *>(QApplication::focusWidget());
  if (!draft) {
    error = QStringLiteral("Text draft did not open for the caret check");
    return false;
  }
  if (draft->cursorWidth() != 0) {
    error = QStringLiteral("Native caret width is %1, expected 0")
                .arg(draft->cursorWidth());
    return false;
  }
  editor.close();
  return true;
}

/** The view holds still while a text draft is open. */
bool runDraftViewLockCheck(QApplication &application, QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  {
    QPainter painter(&capture.source);
    for (int y = 0; y < 600; y += 20)
      for (int x = 0; x < 800; x += 20)
        painter.fillRect(QRect(x, y, 20, 20), ((x + y) / 20) % 2 == 0
                                                  ? QColor(24, 32, 48)
                                                  : QColor(96, 128, 176));
  }
  capture.previewSize = capture.source.size();

  CaptureEditor editor(capture, CaptureEditor::CaptureMode::File);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();

  const auto ctrlWheel = [&] {
    QWheelEvent wheel(QPointF(400, 300), QPointF(400, 300), {}, {0, 120},
                      Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase,
                      false);
    QApplication::sendEvent(&editor, &wheel);
    application.processEvents();
  };
  const QRectF image = editor.editImageRectForTest();
  const QRect sample(qRound(image.left() + 20), qRound(image.bottom() - 80),
                     200, 40);
  const auto grab = [&] { return editor.grab().toImage().copy(sample); };

  ctrlWheel();
  ctrlWheel();
  ctrlWheel();
  QTest::keyClick(&editor, Qt::Key_T);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                    editor.toScreenPointForTest(QPointF(400, 200)).toPoint());
  application.processEvents();
  QWidget *draft = QApplication::focusWidget();
  if (!draft) {
    error = QStringLiteral("Text draft did not open for the view-lock check");
    return false;
  }
  QTest::keyClicks(draft, QStringLiteral("hold"));
  application.processEvents();

  const QImage before = grab();
  ctrlWheel();
  if (grab() != before) {
    error = QStringLiteral("Zooming during a text draft moved the view");
    return false;
  }
  QTest::mousePress(&editor, Qt::MiddleButton, Qt::NoModifier, QPoint(400, 300));
  QTest::mouseMove(&editor, QPoint(460, 350), 20);
  QTest::mouseRelease(&editor, Qt::MiddleButton, Qt::NoModifier,
                      QPoint(460, 350));
  application.processEvents();
  if (grab() != before) {
    error = QStringLiteral("Panning during a text draft moved the view");
    return false;
  }

  const QImage committed = grab();
  ctrlWheel();
  if (grab() == committed) {
    error = QStringLiteral("The view stayed locked after the text committed");
    return false;
  }
  editor.close();
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
  // Screen points computed from the real image geometry, not hand-picked
  // literals: robust to the toolbar/tab strip's own layout, which this click
  // has to land through regardless of how tall it currently is.
  const QPointF imageOrigin = editor.editImageRectForTest().topLeft();
  const qreal editScale = editor.editScaleForTest();
  const auto toScreen = [&](const QPointF &annotationPoint) {
    return (imageOrigin + annotationPoint * editScale).toPoint();
  };

  QTest::keyClick(&editor, Qt::Key_T);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, toScreen({175, 100}));
  application.processEvents();
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, toScreen({125, 280}));
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
  QTest::keyClick(inlineEditor, Qt::Key_Return, Qt::ShiftModifier);
  application.processEvents();
  if (QApplication::focusWidget() != inlineEditor ||
      editor.annotationCountForTest() != 0) {
    error = QStringLiteral(
        "Shift+Enter committed text instead of starting a new line");
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
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, toScreen({325, 180}));
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
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                    editor.toolbarButtonCenterForTest(QStringLiteral("tool-arrow")));
  QTest::mouseMove(&editor, toScreen({275, 120}), 10);
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

/**
 * Enter commits a one-line label, reopens a selected one, and walks the lines
 * of a dragged box before committing on its last; Esc commits but keeps the
 * label selected so Backspace removes it.
 */
bool runTextEnterSemanticsCheck(QApplication &application, QString &error) {
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
  QTest::mouseMove(&editor, QPoint(650, 470), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(650, 470));
  application.processEvents();

  const auto inlineEditor = [] {
    return qobject_cast<QPlainTextEdit *>(QApplication::focusWidget());
  };

  // A click places a one-line label: Enter commits it and the tool stays.
  QTest::keyClick(&editor, Qt::Key_T);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(300, 220));
  application.processEvents();
  if (!inlineEditor()) {
    error = QStringLiteral("Text click did not open the inline editor");
    return false;
  }
  QTest::keyClicks(inlineEditor(), QStringLiteral("One"));
  QTest::keyClick(inlineEditor(), Qt::Key_Return);
  application.processEvents();
  if (inlineEditor() || editor.annotationCountForTest() != 1 ||
      editor.armedToolForTest() != CaptureEditor::Tool::Text ||
      editor.selectedCountForTest() != 0) {
    error = QStringLiteral("Enter did not commit a one-line label");
    return false;
  }

  // Esc commits too, but leaves the label selected so Backspace removes it.
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(300, 320));
  application.processEvents();
  if (!inlineEditor()) {
    error = QStringLiteral("Second text click did not open the inline editor");
    return false;
  }
  QTest::keyClicks(inlineEditor(), QStringLiteral("Stray"));
  QTest::keyClick(inlineEditor(), Qt::Key_Escape);
  application.processEvents();
  if (inlineEditor() || editor.annotationCountForTest() != 2 ||
      editor.selectedCountForTest() != 1 ||
      editor.armedToolForTest() != CaptureEditor::Tool::Select) {
    error = QStringLiteral("Esc did not commit the label and keep it selected");
    return false;
  }
  // Enter on the selected label reopens it rather than finishing the capture.
  QTest::keyClick(&editor, Qt::Key_Return);
  application.processEvents();
  if (!inlineEditor() ||
      inlineEditor()->toPlainText() != QStringLiteral("Stray")) {
    error = QStringLiteral("Enter on a selected label did not reopen it");
    return false;
  }
  QTest::keyClick(inlineEditor(), Qt::Key_Escape);
  application.processEvents();
  QTest::keyClick(&editor, Qt::Key_Backspace);
  application.processEvents();
  if (editor.annotationCountForTest() != 1 ||
      editor.selectedCountForTest() != 0) {
    error = QStringLiteral("Backspace did not remove the Esc-kept label");
    return false;
  }

  // A dragged box holds as many lines as fit: Enter walks them and commits
  // on the last; Shift+Enter adds one more.
  const qreal lineHeight = QFontMetricsF(annotationTextFont(5.0)).lineSpacing();
  const int boxHeight = qRound(lineHeight * 3 + 2);
  QTest::keyClick(&editor, Qt::Key_T);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(200, 250));
  QTest::mouseMove(&editor, QPoint(400, 250 + boxHeight), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(400, 250 + boxHeight));
  application.processEvents();
  if (!inlineEditor()) {
    error = QStringLiteral("Text drag did not open the inline editor");
    return false;
  }
  QTest::keyClicks(inlineEditor(), QStringLiteral("a"));
  QTest::keyClick(inlineEditor(), Qt::Key_Return);
  QTest::keyClicks(inlineEditor(), QStringLiteral("b"));
  QTest::keyClick(inlineEditor(), Qt::Key_Return);
  application.processEvents();
  if (!inlineEditor() ||
      inlineEditor()->toPlainText() != QStringLiteral("a\nb\n")) {
    error = QStringLiteral("Enter did not walk the lines of a dragged text box");
    return false;
  }
  QTest::keyClicks(inlineEditor(), QStringLiteral("c"));
  QTest::keyClick(inlineEditor(), Qt::Key_Return, Qt::ShiftModifier);
  QTest::keyClicks(inlineEditor(), QStringLiteral("d"));
  application.processEvents();
  if (!inlineEditor() ||
      inlineEditor()->toPlainText() != QStringLiteral("a\nb\nc\nd")) {
    error = QStringLiteral("Shift+Enter did not add a line past the box");
    return false;
  }
  QTest::keyClick(inlineEditor(), Qt::Key_Return);
  application.processEvents();
  if (inlineEditor() || editor.annotationCountForTest() != 2) {
    error = QStringLiteral("Enter on the last line did not commit the box");
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
  const QRectF overlayCanvas =
      captureCanvasRect(QSizeF(80, 40), {redaction, arrow, label});
  const QPoint overlayOrigin = (-overlayCanvas.topLeft()).toPoint();
  const QColor overlayFill =
      overlayExport.pixelColor(overlayOrigin + QPoint(22, 12));
  const QColor overlayStroke =
      overlayExport.pixelColor(overlayOrigin + QPoint(26, 20));
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

  // Fullscreen draws the 800x600 capture scaled to fit under the toolbar; the
  // secret sits at annotation (200,200)-(300,260), centered on (250,230). A
  // loupe smaller than the redaction can only show redacted pixels if it
  // samples the redaction layer. Screen points are computed from the live
  // image geometry (toScreen), not hand-picked, since the toolbar/tab chrome
  // above the image changes that geometry.
  const auto spotlightPreviewColor = [&](bool redact) {
    CaptureEditor editor(editorCapture, CaptureEditor::CaptureMode::Fullscreen);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    const QPointF origin = editor.editImageRectForTest().topLeft();
    const qreal scale = editor.editScaleForTest();
    const auto toScreen = [&](const QPointF &point) {
      return (origin + point * scale).toPoint();
    };
    if (redact) {
      QTest::keyClick(&editor, Qt::Key_D);
      QTest::keyClick(&editor, Qt::Key_D);
      QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier,
                        toScreen({180, 180}));
      QTest::mouseMove(&editor, toScreen({320, 280}), 20);
      QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                          toScreen({320, 280}));
      application.processEvents();
    }
    QTest::keyClick(&editor, Qt::Key_S);
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier,
                      toScreen({210, 205}));
    QTest::mouseMove(&editor, toScreen({290, 255}), 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        toScreen({290, 255}));
    application.processEvents();
    const QColor color =
        editor.grab().toImage().pixelColor(toScreen({250, 230}));
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
    const QPointF origin = editor.editImageRectForTest().topLeft();
    const qreal scale = editor.editScaleForTest();
    const auto toScreen = [&](const QPointF &point) {
      return (origin + point * scale).toPoint();
    };
    QTest::keyClick(&editor, Qt::Key_D);
    QTest::keyClick(&editor, Qt::Key_D);
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier,
                      toScreen({180, 180}));
    QTest::mouseMove(&editor, toScreen({320, 280}), 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        toScreen({320, 280}));
    QTest::keyClick(&editor, Qt::Key_5);
    QTest::keyClick(&editor, Qt::Key_A);
    // A horizontal arrow across the secret's vertical center (230).
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier,
                      toScreen({210, 230}));
    QTest::mouseMove(&editor, toScreen({289, 230}), 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        toScreen({289, 230}));
    // Park the pointer well below the key guide before sampling: the card
    // sizes itself to its text now, and a pointer left at the arrow's end
    // flips it left, over the pixels this check reads.
    QTest::mouseMove(&editor, QPoint(400, 550), 20);
    application.processEvents();
    const QImage preview = editor.grab().toImage();
    const QImage exported = editor.renderCurrentOutput();
    editor.close();
    const QColor arrowColor(QStringLiteral("#0a84ff"));
    const QPoint cornerScreen = toScreen({210, 202});
    const QPoint onArrowScreen = toScreen({220, 230});
    if (preview.pixelColor(cornerScreen) != solid ||
        showsSecretRed(preview.pixelColor(cornerScreen)) ||
        preview.pixelColor(onArrowScreen) != arrowColor) {
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

std::optional<QPointF> changedCenter(const QImage &before, const QImage &after,
                                     const QRect &search) {
  if (before.size() != after.size())
    return std::nullopt;
  const QRect area = search.intersected(before.rect());
  QRect changed;
  for (int y = area.top(); y <= area.bottom(); ++y) {
    for (int x = area.left(); x <= area.right(); ++x) {
      const QColor first = before.pixelColor(x, y);
      const QColor second = after.pixelColor(x, y);
      const int delta = std::max({std::abs(first.red() - second.red()),
                                  std::abs(first.green() - second.green()),
                                  std::abs(first.blue() - second.blue())});
      if (delta > 12)
        changed |= QRect(x, y, 1, 1);
    }
  }
  if (changed.isEmpty())
    return std::nullopt;
  return QRectF(changed).center();
}

/** The counter preview and committed counter share a nine-pixel up-left
 *  pointer lead at every zoom. */
bool runMarkerPointerOffsetSmoke(QApplication &application, QString &error) {
  const auto exercise = [&](bool zoomed) {
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

    if (zoomed) {
      QWheelEvent wheel(QPointF(400, 300), QPointF(400, 300), {}, {0, 120},
                        Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase,
                        false);
      QApplication::sendEvent(&editor, &wheel);
      application.processEvents();
    }

    const QPoint pointer = zoomed ? QPoint(510, 350) : QPoint(350, 280);
    QTest::mouseMove(&editor, pointer, 10);
    application.processEvents();
    const QImage withoutMarker = editor.grab().toImage();

    QTest::keyClick(&editor, Qt::Key_C);
    QTest::mouseMove(&editor, QPoint(0, 0), 10);
    application.processEvents();
    const QImage markerToolAtRest = editor.grab().toImage();
    QTest::mouseMove(&editor, pointer, 10);
    application.processEvents();
    const QImage ghost = editor.grab().toImage();
    const qreal dpr = ghost.devicePixelRatio();
    const QRect search(qFloor((pointer.x() - 36) * dpr),
                       qFloor((pointer.y() - 36) * dpr), qCeil(72 * dpr),
                       qCeil(72 * dpr));
    const std::optional<QPointF> ghostCenter =
        changedCenter(withoutMarker, ghost, search);
    const QPointF expectedCenter = (QPointF(pointer) - QPointF(9, 9)) * dpr;
    if (!ghostCenter || QLineF(*ghostCenter, expectedCenter).length() >
                            std::max<qreal>(1.5, dpr * 1.5)) {
      error = QStringLiteral("Counter ghost did not stay 9 px ahead of the "
                             "pointer at %1")
                  .arg(zoomed ? QStringLiteral("zoom") : QStringLiteral("fit"));
      return false;
    }

    QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, pointer);
    application.processEvents();
    const QVector<Operation> &ops = editor.operationLog();
    if (ops.isEmpty() || ops.constLast().type != Operation::Type::Annotate ||
        ops.constLast().annotations.size() != 1 ||
        ops.constLast().annotations.constFirst().kind !=
            Annotation::Kind::Marker) {
      error = QStringLiteral("Counter pointer-offset click did not commit a "
                             "marker at %1")
                  .arg(zoomed ? QStringLiteral("zoom") : QStringLiteral("fit"));
      return false;
    }

    // Move the still-armed tool away so the next-number ghost cannot cover
    // the marker we just committed.
    QTest::mouseMove(&editor, QPoint(0, 0), 10);
    application.processEvents();
    const QImage committed = editor.grab().toImage();
    const std::optional<QPointF> committedCenter =
        changedCenter(markerToolAtRest, committed, search);
    if (!committedCenter || QLineF(*committedCenter, *ghostCenter).length() >
                                std::max<qreal>(1.0, dpr)) {
      error = QStringLiteral(
                  "Counter jumped from its ghost on click at %1 (%2,%3 -> "
                  "%4,%5)")
                  .arg(zoomed ? QStringLiteral("zoom") : QStringLiteral("fit"))
                  .arg(ghostCenter ? ghostCenter->x() : -1)
                  .arg(ghostCenter ? ghostCenter->y() : -1)
                  .arg(committedCenter ? committedCenter->x() : -1)
                  .arg(committedCenter ? committedCenter->y() : -1);
      return false;
    }
    editor.close();
    application.processEvents();
    return true;
  };

  return exercise(false) && exercise(true);
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

  // y=60 clears the capture-kind tabs, which on a 320 px wide test surface
  // reach almost edge to edge.
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(40, 60));
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

  if (selected.size() != QSize(160, 120)) {
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
  // Screen points computed from the live image geometry, not hand-picked:
  // Fullscreen scales the 800x600 capture to fit under the toolbar/tab
  // chrome, and that scale depends on their current height.
  const QPointF origin = editor.editImageRectForTest().topLeft();
  const qreal scale = editor.editScaleForTest();
  const auto toScreen = [&](const QPointF &point) {
    return (origin + point * scale).toPoint();
  };
  QTest::keyClick(&editor, Qt::Key_D);
  QTest::keyClick(&editor, Qt::Key_D);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, toScreen({180, 180}));
  QTest::mouseMove(&editor, toScreen({320, 280}), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, toScreen({320, 280}));
  QTest::keyClick(&editor, Qt::Key_5);
  QTest::keyClick(&editor, Qt::Key_A);
  // A horizontal arrow across the secret's vertical center (230).
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, toScreen({210, 230}));
  QTest::mouseMove(&editor, toScreen({289, 230}), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, toScreen({289, 230}));
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

/**
 * The recents shelf: finishing a capture moves its working document onto the
 * shelf, the next select overlay shows it as a small card on the right that
 * fans out under the pointer, and clicking the card asks main() to reopen it
 * with its layers still editable. Reopening and finishing again replaces the
 * entry rather than adding a twin, and the shelf never grows past its limit.
 */
bool runRecentsShelfSmoke(QApplication &application, QString &error) {
  QTemporaryDir shelf;
  QTemporaryDir screenshots;
  if (!shelf.isValid() || !screenshots.isValid()) {
    error = QStringLiteral("Could not create recents shelf directories");
    return false;
  }
  const QByteArray previousShelf = qgetenv("OMASNAP_RECENT_DIR");
  const QByteArray previousDir = qgetenv("OMASNAP_SCREENSHOT_DIR");
  qputenv("OMASNAP_RECENT_DIR", shelf.path().toUtf8());
  qputenv("OMASNAP_SCREENSHOT_DIR", screenshots.path().toUtf8());
  const auto restoreEnv = qScopeGuard([&] {
    qputenv("OMASNAP_RECENT_DIR", previousShelf);
    if (previousDir.isEmpty())
      qunsetenv("OMASNAP_SCREENSHOT_DIR");
    else
      qputenv("OMASNAP_SCREENSHOT_DIR", previousDir);
  });

  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#204060")));
  {
    QPainter painter(&capture.source);
    painter.fillRect(QRect(100, 100, 200, 120), QColor(QStringLiteral("#ffaa00")));
  }
  capture.previewSize = capture.source.size();

  // An empty shelf draws nothing and claims no pointer.
  {
    CaptureEditor editor(capture, CaptureEditor::CaptureMode::Region);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    if (editor.waitForRecents() || editor.recentCountForTest() != 0 ||
        !editor.recentCardRectForTest(0).isNull()) {
      error = QStringLiteral("Empty shelf reported cards");
      return false;
    }
    editor.close();
  }

  // Take a capture with a layer on it and save: the working document should
  // land on the shelf with a thumbnail and a log that knows its preview size.
  QImage firstOutput;
  {
    CaptureEditor editor(capture, CaptureEditor::CaptureMode::Fullscreen);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    QTest::keyClick(&editor, Qt::Key_A);
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(200, 200));
    QTest::mouseMove(&editor, QPoint(400, 320), 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(400, 320));
    application.processEvents();
    if (editor.annotationCountForTest() != 1) {
      error = QStringLiteral("Recents smoke could not draw an arrow");
      return false;
    }
    firstOutput = editor.renderCurrentOutput();
    QTest::keyClick(&editor, Qt::Key_S, Qt::ControlModifier);
    editor.waitForExport();
  }
  QVector<RecentSnap> shelved = listRecentSnaps(true);
  if (shelved.size() != 1 || shelved.constFirst().thumbnail.isNull() ||
      shelved.constFirst().logPath.isEmpty() ||
      !QFile::exists(shelved.constFirst().sourcePath)) {
    error = QStringLiteral("Saving did not shelve the working document (%1)")
                .arg(shelved.size());
    return false;
  }
  if (shelved.constFirst().thumbnail.width() > kRecentThumbEdge ||
      shelved.constFirst().thumbnail.height() > kRecentThumbEdge) {
    error = QStringLiteral("Shelf thumbnail was not shrunk");
    return false;
  }
  {
    OperationLog log;
    if (!loadOperationLog(shelved.constFirst().logPath, log, error))
      return false;
    if (log.previewSize != QSize(800, 600)) {
      error = QStringLiteral("Shelved log did not record its preview size");
      return false;
    }
  }
  const QString firstSource = shelved.constFirst().sourcePath;

  // The next select overlay shows the card; hovering fans it out; a click
  // asks to reopen it without capturing.
  {
    CaptureEditor editor(capture, CaptureEditor::CaptureMode::Region);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    if (!editor.waitForRecents() || editor.recentCountForTest() != 1) {
      error = QStringLiteral("Select overlay did not list the shelved capture");
      return false;
    }
    if (editor.recentsOpenForTest()) {
      error = QStringLiteral("Shelf opened before the pointer reached it");
      return false;
    }
    const QRectF stacked = editor.recentCardRectForTest(0);
    if (stacked.isNull() || stacked.right() > 800 || stacked.left() < 600) {
      error = QStringLiteral("Shelf card is not along the right edge (%1,%2)")
                  .arg(stacked.left())
                  .arg(stacked.right());
      return false;
    }
    QTest::mouseMove(&editor, QPoint(100, 300), 20);
    application.processEvents();
    if (editor.measurementText().isEmpty()) {
      error = QStringLiteral("Selection readout disappeared outside the shelf");
      return false;
    }
    QTest::mouseMove(&editor, stacked.center().toPoint(), 20);
    application.processEvents();
    if (!editor.recentsOpenForTest()) {
      error = QStringLiteral("Hovering the stack did not fan the shelf out");
      return false;
    }
    if (!editor.measurementText().isEmpty()) {
      error = QStringLiteral("Selection readout overlapped the open shelf");
      return false;
    }
    const QRectF fanned = editor.recentCardRectForTest(0);
    // Dragging from inside the open shelf must not start a region.
    QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(qRound(fanned.left()) - 8,
                             qRound(fanned.bottom()) + 4));
    application.processEvents();
    if (!editor.selectingForTest() || !editor.currentSelection().isEmpty()) {
      error = QStringLiteral("A click in the shelf margin started a selection");
      return false;
    }
    QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                      fanned.center().toPoint());
    editor.waitForReopen();
    // Reopened in place: same surface, same picture, layer still undoable.
    if (editor.selectingForTest() || !editor.isVisible() ||
        editor.annotationCountForTest() != 1 ||
        editor.renderCurrentOutput().convertToFormat(QImage::Format_ARGB32) !=
            firstOutput.convertToFormat(QImage::Format_ARGB32)) {
      error = QStringLiteral("Clicking the card did not reopen it in place");
      return false;
    }
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    application.processEvents();
    if (editor.annotationCountForTest() != 0) {
      error = QStringLiteral("Reopened capture lost its undo history");
      return false;
    }
    // Finishing again replaces the shelf entry instead of shelving a twin.
    QTest::keyClick(&editor, Qt::Key_S, Qt::ControlModifier);
    editor.waitForExport();

    // Far from the shelf, it folds back.
    CaptureEditor again(capture, CaptureEditor::CaptureMode::Region);
    again.resize(800, 600);
    again.show();
    application.processEvents();
    again.waitForRecents();
    QTest::mouseMove(&again, again.recentCardRectForTest(0).center().toPoint(),
                     20);
    application.processEvents();
    QTest::mouseMove(&again, QPoint(100, 300), 20);
    application.processEvents();
    if (again.recentsOpenForTest()) {
      error = QStringLiteral("Shelf stayed open after the pointer left");
      return false;
    }
    again.close();
  }
  shelved = listRecentSnaps(false);
  if (shelved.size() != 1 || shelved.constFirst().sourcePath == firstSource ||
      QFile::exists(firstSource)) {
    error = QStringLiteral("Finishing a reopened capture did not replace its "
                           "shelf entry (%1 entries)")
                .arg(shelved.size());
    return false;
  }

  // The shelf holds the newest five and no more.
  for (int index = 0; index < kRecentSnapLimit + 2; ++index) {
    const QString source = temporarySnapshotPath();
    QString saveError;
    if (!saveTemporarySnapshot(capture.source, source, saveError, 100) ||
        !recordRecentSnap(source, {}, capture.source, saveError)) {
      error = QStringLiteral("Could not fill the shelf: %1").arg(saveError);
      return false;
    }
    QThread::msleep(2); // distinct millisecond stamps
  }
  const QVector<RecentSnap> full = listRecentSnaps(false);
  if (full.size() != kRecentSnapLimit) {
    error = QStringLiteral("Shelf kept %1 entries, wanted %2")
                .arg(full.size())
                .arg(kRecentSnapLimit);
    return false;
  }
  for (int index = 1; index < full.size(); ++index) {
    if (full.at(index - 1).stampMs < full.at(index).stampMs) {
      error = QStringLiteral("Shelf is not newest first");
      return false;
    }
  }
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

/** A recrop moves the frame, never the ink: annotations stay over the
 *  pixels they were drawn on when the top/left crop handles move the
 *  selection origin. */
bool runCropKeepsAnnotationsAnchored(QApplication &application,
                                     QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 400, 300};
  capture.monitor.pixelSize = {400, 300};
  capture.monitor.scale = 1.0;
  capture.source = QImage(400, 300, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#112233")));
  capture.previewSize = capture.source.size();

  const auto lineAt = [](const QImage &image, int x, int y) {
    const QColor pixel = image.pixelColor(x, y);
    return pixel.red() > 200 && pixel.green() < 100;
  };

  CaptureEditor editor(capture, CaptureEditor::CaptureMode::File);
  editor.setSuppressSnapshots(true);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();
  QTest::keyClick(&editor, Qt::Key_L);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier,
                    editor.toScreenPointForTest(QPointF(100, 100)).toPoint());
  QTest::mouseMove(&editor, editor.toScreenPointForTest(QPointF(300, 100)).toPoint(),
                   20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      editor.toScreenPointForTest(QPointF(300, 100)).toPoint());
  application.processEvents();
  if (!lineAt(editor.renderCurrentOutput(), 150, 100)) {
    error = QStringLiteral("Anchoring check could not draw its line");
    return false;
  }

  QTest::keyClick(&editor, Qt::Key_V);
  application.processEvents();
  const QRectF image = editor.editImageRectForTest();
  const QPoint handle = (image.topLeft() + QPointF(-7, -7)).toPoint();
  const QPoint inward = editor.toScreenPointForTest(QPointF(43, 43)).toPoint();
  QTest::mouseMove(&editor, handle, 20);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, handle);
  QTest::mouseMove(&editor, inward, 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, inward);
  application.processEvents();
  if (editor.currentSelection() != QRectF(43, 43, 357, 257)) {
    error = QStringLiteral("Top-left crop drag did not land on (43,43): %1,%2 %3x%4")
                .arg(editor.currentSelection().x())
                .arg(editor.currentSelection().y())
                .arg(editor.currentSelection().width())
                .arg(editor.currentSelection().height());
    return false;
  }
  QImage cropped = editor.renderCurrentOutput();
  if (!lineAt(cropped, 150, 57) || !lineAt(cropped, 70, 57) ||
      lineAt(cropped, 280, 57) || lineAt(cropped, 150, 100)) {
    error = QStringLiteral(
        "Crop from the top-left moved the line off its content");
    return false;
  }

  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  const QImage uncropped = editor.renderCurrentOutput();
  if (!lineAt(uncropped, 150, 100) || !lineAt(uncropped, 280, 100) ||
      lineAt(uncropped, 70, 57)) {
    error = QStringLiteral("Undoing the crop did not restore the line");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
  application.processEvents();
  cropped = editor.renderCurrentOutput();
  if (!lineAt(cropped, 150, 57) || !lineAt(cropped, 70, 57) ||
      lineAt(cropped, 280, 57) || lineAt(cropped, 150, 100)) {
    error = QStringLiteral("Redoing the crop lost the content anchoring");
    return false;
  }

  QTemporaryDir directory;
  if (!directory.isValid()) {
    error = QStringLiteral("Could not create crop-anchor directory");
    return false;
  }
  const QString logPath =
      QDir(directory.path()).filePath(QStringLiteral("anchored.json"));
  OperationLog persisted;
  persisted.ops = editor.operationLog();
  persisted.index = editor.operationIndex();
  if (!saveOperationLog(logPath, persisted, error))
    return false;
  OperationLog reloaded;
  if (!loadOperationLog(logPath, reloaded, error))
    return false;
  editor.close();

  CaptureEditor replayed(capture, CaptureEditor::CaptureMode::File,
                         QuickOutputMode::None, reloaded);
  replayed.setSuppressSnapshots(true);
  replayed.resize(800, 600);
  replayed.show();
  application.processEvents();
  const QImage restored = replayed.renderCurrentOutput();
  if (replayed.currentSelection() != QRectF(43, 43, 357, 257) ||
      !lineAt(restored, 150, 57) || !lineAt(restored, 70, 57) ||
      lineAt(restored, 280, 57) || lineAt(restored, 150, 100)) {
    error = QStringLiteral("Reloaded op log lost the crop anchoring");
    return false;
  }
  replayed.close();
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
  QTest::mouseClick(
      &editor, Qt::LeftButton, Qt::NoModifier,
      editor.toolbarButtonCenterForTest(QStringLiteral("tool-arrow")));
  application.processEvents();

  // A shallow drag, with the stale Shift the compositor still reports. It must
  // draw where it was dragged: snapped to 45°, this arrow would come out flat.
  const QImage before = flushedSnapshot(editor, snapshotPath);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::ShiftModifier, QPoint(200, 411));
  QTest::mouseMove(&editor, QPoint(400, 381), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::ShiftModifier,
                      QPoint(400, 381));
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
  QTest::mousePress(&editor, Qt::LeftButton, Qt::ShiftModifier, QPoint(200, 411));
  QTest::mouseMove(&editor, QPoint(400, 381), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::ShiftModifier,
                      QPoint(400, 381));
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

/** Canvas boundary policies clip the presentation, never the stored layer. */
bool runCanvasBoundaryModeSmoke(QApplication &application, QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 100, 100};
  capture.monitor.pixelSize = {100, 100};
  capture.monitor.scale = 1.0;
  capture.source = QImage(100, 100, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#182030")));
  capture.previewSize = capture.source.size();

  Annotation outside;
  outside.kind = Annotation::Kind::Rectangle;
  outside.start = {50, 40};
  outside.end = {260, 80};
  outside.color = QColor(QStringLiteral("#ff375f"));
  outside.size = 4.0;
  outside.id = 1;

  Operation annotate;
  annotate.type = Operation::Type::Annotate;
  annotate.annotations = {outside};
  OperationLog log;
  log.ops = {annotate};
  log.index = 1;
  log.nextId = 2;
  log.previewSize = capture.previewSize;

  const QRectF sourceFrame(QPointF(), QSizeF(capture.previewSize));
  const QRectF framedCanvas = captureCanvasRect(
      sourceFrame.size(), {outside}, CanvasBoundaryMode::Framed);
  const QRectF overflowCanvas = captureCanvasRect(
      sourceFrame.size(), {outside}, CanvasBoundaryMode::Overflow);
  const QRectF imageCanvas = captureCanvasRect(
      sourceFrame.size(), {outside}, CanvasBoundaryMode::Image);
  if (captureCanvasRect(sourceFrame.size(), {outside}) != framedCanvas ||
      framedCanvas != QRectF(-64, -64, 327, 228) ||
      overflowCanvas != QRectF(0, 0, 263, 100) ||
      imageCanvas != sourceFrame ||
      captureCanvasRect(sourceFrame.size(), {},
                        CanvasBoundaryMode::Overflow) != sourceFrame) {
    error = QStringLiteral("Canvas boundary geometry reached the wrong bounds");
    return false;
  }

  const auto expectedOutput = [&](CanvasBoundaryMode mode) {
    return renderCapture(capture, sourceFrame, {outside},
                         BackgroundStyle::None, true, mode);
  };
  const QImage framedOutput = expectedOutput(CanvasBoundaryMode::Framed);
  const QImage overflowOutput = expectedOutput(CanvasBoundaryMode::Overflow);
  const QImage imageOutput = expectedOutput(CanvasBoundaryMode::Image);
  const QImage framedOff =
      renderCapture(capture, sourceFrame, {outside}, BackgroundStyle::Off, true,
                    CanvasBoundaryMode::Framed);
  const QImage overflowColor = renderCapture(
      capture, sourceFrame, {outside}, BackgroundStyle::Aurora, true,
      CanvasBoundaryMode::Overflow);
  const QImage imageColor = renderCapture(capture, sourceFrame, {outside},
                                          BackgroundStyle::Aurora, true,
                                          CanvasBoundaryMode::Image);
  QImage customBackdrop(20, 20, QImage::Format_ARGB32_Premultiplied);
  const QColor customGreen(QStringLiteral("#18a558"));
  customBackdrop.fill(customGreen);
  const QImage framedCustom =
      renderCapture(capture, sourceFrame, {outside}, BackgroundStyle::Custom,
                    true, CanvasBoundaryMode::Framed, customBackdrop);
  const QImage overflowCustom =
      renderCapture(capture, sourceFrame, {outside}, BackgroundStyle::Custom,
                    true, CanvasBoundaryMode::Overflow, customBackdrop);
  const QImage imageCustom =
      renderCapture(capture, sourceFrame, {outside}, BackgroundStyle::Custom,
                    true, CanvasBoundaryMode::Image, customBackdrop);
  BackgroundStyle parsedCustom = BackgroundStyle::None;
  if (framedOutput.size() != framedCanvas.size().toSize() ||
      overflowOutput.size() != overflowCanvas.size().toSize() ||
      imageOutput.size() != imageCanvas.size().toSize() ||
      !(framedOutput.width() > overflowOutput.width() &&
        overflowOutput.width() > imageOutput.width()) ||
      framedOutput.pixelColor(0, 0).alpha() != 255 ||
      framedOff.size() != framedOutput.size() ||
      framedOff.pixelColor(0, 0).alpha() != 0 ||
      overflowOutput.pixelColor(200, 10).alpha() != 0 ||
      overflowColor.size() != overflowOutput.size() ||
      overflowColor.pixelColor(200, 10).alpha() != 255 ||
      imageColor != imageOutput ||
      framedCustom.size() != framedOutput.size() ||
      framedCustom.pixelColor(0, 0) != customGreen ||
      overflowCustom.size() != overflowOutput.size() ||
      overflowCustom.pixelColor(200, 10) != customGreen ||
      imageCustom != imageOutput ||
      backgroundStyleName(BackgroundStyle::Custom) !=
          QStringLiteral("custom") ||
      !backgroundStyleFromName(QStringLiteral("custom"), parsedCustom) ||
      parsedCustom != BackgroundStyle::Custom) {
    error = QStringLiteral("Canvas boundary exports were not clipped in order");
    return false;
  }

  CaptureEditor editor(capture, CaptureEditor::CaptureMode::File,
                       QuickOutputMode::None, log);
  editor.resize(640, 480);
  editor.show();
  application.processEvents();
  if (editor.currentCanvasBoundaryForTest() != CanvasBoundaryMode::Framed ||
      editor.currentCanvasForTest() != framedCanvas ||
      editor.renderCurrentOutput() != framedOutput) {
    error = QStringLiteral("Canvas did not default to Framed");
    return false;
  }

  QTest::keyClick(&editor, Qt::Key_G);
  application.processEvents();
  if (editor.currentCanvasBoundaryForTest() != CanvasBoundaryMode::Overflow ||
      editor.currentCanvasForTest() != overflowCanvas ||
      editor.currentAnnotationsForTest() != QVector<Annotation>{outside} ||
      editor.renderCurrentOutput() != overflowOutput ||
      !editor.statusForTest().contains(QStringLiteral("Canvas: Overflow")) ||
      editor.operationLog().constLast().type !=
          Operation::Type::CanvasBoundary ||
      editor.operationLog().constLast().canvasBoundary !=
          CanvasBoundaryMode::Overflow) {
    error = QStringLiteral("G did not switch non-destructively to Overflow");
    return false;
  }

  if (!editor.waitForSnapshot() || editor.workingLogPath().isEmpty()) {
    error = QStringLiteral("Overflow boundary was not persisted");
    return false;
  }
  CaptureEditor restored(capture, CaptureEditor::CaptureMode::File);
  QString restoreError;
  if (!restored.restoreOperationLog(editor.workingLogPath(), restoreError) ||
      restored.currentCanvasBoundaryForTest() !=
          CanvasBoundaryMode::Overflow ||
      restored.currentCanvasForTest() != overflowCanvas ||
      restored.currentAnnotationsForTest() != QVector<Annotation>{outside} ||
      restored.renderCurrentOutput() != overflowOutput) {
    error =
        QStringLiteral("Restoring Overflow changed its layers or clipping: %1")
            .arg(restoreError);
    return false;
  }
  restored.close();

  QTest::keyClick(&editor, Qt::Key_G);
  application.processEvents();
  if (editor.currentCanvasBoundaryForTest() != CanvasBoundaryMode::Image ||
      editor.currentCanvasForTest() != imageCanvas ||
      editor.currentAnnotationsForTest() != QVector<Annotation>{outside} ||
      editor.renderCurrentOutput() != imageOutput ||
      !editor.statusForTest().contains(QStringLiteral("Canvas: Image"))) {
    error = QStringLiteral("G did not switch non-destructively to Image");
    return false;
  }

  QTest::keyClick(&editor, Qt::Key_G, Qt::ShiftModifier);
  QTest::keyClick(&editor, Qt::Key_G, Qt::ShiftModifier);
  application.processEvents();
  if (editor.currentCanvasBoundaryForTest() != CanvasBoundaryMode::Framed ||
      editor.currentCanvasForTest() != framedCanvas ||
      editor.currentAnnotationsForTest() != QVector<Annotation>{outside} ||
      editor.renderCurrentOutput() != framedOutput) {
    error = QStringLiteral("Shift+G did not cycle backward to Framed");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (editor.currentCanvasBoundaryForTest() !=
          CanvasBoundaryMode::Overflow ||
      editor.renderCurrentOutput() != overflowOutput) {
    error = QStringLiteral("Undo did not restore the prior canvas boundary");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
  application.processEvents();
  if (editor.currentCanvasBoundaryForTest() != CanvasBoundaryMode::Framed ||
      editor.renderCurrentOutput() != framedOutput) {
    error = QStringLiteral("Redo did not restore the Framed boundary");
    return false;
  }

  editor.close();
  return true;
}

/** Fullscreen edit chrome is a workspace around the current canvas: a layer
 *  may begin there and grow the document, while source tools, chrome, and the
 *  strict Image boundary remain inert. */
bool runOffCanvasCreationSmoke(QApplication &application, QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 400, 300};
  capture.monitor.pixelSize = {400, 300};
  capture.monitor.scale = 1.0;
  capture.source = QImage(400, 300, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#182030")));
  capture.previewSize = capture.source.size();

  CaptureEditor editor(capture, CaptureEditor::CaptureMode::Fullscreen);
  editor.setSuppressSnapshots(true);
  editor.resize(1200, 700);
  editor.show();
  application.processEvents();

  const QRectF sourceCanvas(QPointF(), capture.previewSize);
  const QRectF initialImage = editor.editImageRectForTest();
  const QPoint outside(qRound(initialImage.left() - 180.0),
                       qRound(initialImage.center().y()));
  const QPoint inside(qRound(initialImage.left() + 120.0), outside.y());
  if (outside.x() < 1 || initialImage.contains(outside) ||
      !initialImage.contains(inside)) {
    error = QStringLiteral("Off-canvas fixture had no fullscreen workspace");
    return false;
  }
  const auto drag = [&](const QPoint &from, const QPoint &to) {
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, from);
    QTest::mouseMove(&editor, to, 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, to);
    application.processEvents();
  };

  // The start is in unused fullscreen space and the head is over the source.
  QTest::keyClick(&editor, Qt::Key_A);
  QTest::mouseMove(&editor, outside, 20);
  application.processEvents();
  if (editor.cursor().shape() != Qt::CrossCursor) {
    error = QStringLiteral("Arrow did not advertise the off-canvas workspace");
    return false;
  }
  drag(outside, inside);
  if (editor.currentAnnotationsForTest().size() != 1 ||
      editor.currentAnnotationsForTest().constFirst().kind !=
          Annotation::Kind::Arrow ||
      editor.currentAnnotationsForTest().constFirst().start.x() >= -100.0 ||
      editor.currentAnnotationsForTest().constFirst().end.x() <= 0.0 ||
      editor.currentCanvasForTest().left() >= 0.0 ||
      editor.operationLog().constLast().type != Operation::Type::Annotate) {
    error =
        QStringLiteral("Arrow could not begin outside and point into canvas");
    return false;
  }
  const Annotation arrow = editor.currentAnnotationsForTest().constFirst();

  // Once the arrow grows the canvas, its outer tail is a normal place to add
  // a label. This is the intended callout workflow, not a special arrow/text
  // relationship: both remain ordinary undoable vector operations.
  QTest::keyClick(&editor, Qt::Key_T);
  const QPointF labelPoint = arrow.start + QPointF(8, -34);
  QTest::mouseClick(
      &editor, Qt::LeftButton, Qt::NoModifier,
      editor.annotationPointToWidgetForTest(labelPoint).toPoint());
  application.processEvents();
  auto *textEditor =
      qobject_cast<QPlainTextEdit *>(QApplication::focusWidget());
  if (textEditor == nullptr) {
    error = QStringLiteral("Arrow tail did not accept a text annotation");
    return false;
  }
  QTest::keyClicks(textEditor, QStringLiteral("Outside note"));
  QTest::keyClick(textEditor, Qt::Key_Return, Qt::ControlModifier);
  application.processEvents();
  if (editor.currentAnnotationsForTest().size() != 2 ||
      editor.currentAnnotationsForTest().constLast().kind !=
          Annotation::Kind::Text ||
      annotationTextBounds(editor.currentAnnotationsForTest().constLast())
              .left() >= 0.0 ||
      editor.currentCanvasForTest() !=
          captureCanvasRect(sourceCanvas.size(),
                            editor.currentAnnotationsForTest()) ||
      editor.renderCurrentOutput().size() !=
          editor.currentCanvasForTest().size().toSize()) {
    error = QStringLiteral("Off-canvas arrow label did not enter the output");
    return false;
  }

  // The grown strip is canvas for vector paint, but it is not screenshot
  // data. Redaction cannot begin there even after another layer made it part
  // of the document.
  QTest::keyClick(&editor, Qt::Key_D);
  const QPoint sourceOutside = editor
                                   .annotationPointToWidgetForTest(QPointF(
                                       arrow.start.x(), arrow.start.y() + 80.0))
                                   .toPoint();
  const QPoint sourceInside =
      editor
          .annotationPointToWidgetForTest(QPointF(80.0, arrow.start.y() + 80.0))
          .toPoint();
  drag(sourceOutside, sourceInside);
  if (editor.currentAnnotationsForTest().size() != 2) {
    error = QStringLiteral("Redaction began in a grown background strip");
    return false;
  }

  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (!editor.currentAnnotationsForTest().isEmpty() ||
      editor.currentCanvasForTest() != sourceCanvas) {
    error = QStringLiteral("Undo did not contract the off-canvas workflow");
    return false;
  }

  // Press-to-place tools need an unclamped point too; otherwise every marker
  // stamped in the surround would pile up on the old canvas edge.
  QTest::keyClick(&editor, Qt::Key_C);
  QTest::mouseMove(&editor, outside + QPoint(7, 0), 20);
  QTest::mouseMove(&editor, outside, 20);
  application.processEvents();
  const QImage markerHover = editor.grab().toImage();
  bool markerGhost = false;
  for (int y = std::max(0, outside.y() - 30);
       y <= std::min(markerHover.height() - 1, outside.y() + 20) &&
       !markerGhost;
       ++y) {
    for (int x = std::max(0, outside.x() - 30);
         x <= std::min(markerHover.width() - 1, outside.x() + 20); ++x) {
      const QColor pixel = markerHover.pixelColor(x, y);
      if (pixel.red() > 120 && pixel.red() > pixel.green() + 40 &&
          pixel.red() > pixel.blue() + 20) {
        markerGhost = true;
        break;
      }
    }
  }
  if (editor.cursor().shape() != Qt::PointingHandCursor || !markerGhost) {
    error = QStringLiteral("Off-canvas marker preview was not visible");
    return false;
  }
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, outside);
  application.processEvents();
  if (editor.currentAnnotationsForTest().size() != 1 ||
      editor.currentAnnotationsForTest().constFirst().kind !=
          Annotation::Kind::Marker ||
      editor.currentAnnotationsForTest().constFirst().start.x() >= -100.0 ||
      editor.currentCanvasForTest().left() >= 0.0) {
    error = QStringLiteral("Marker placement clamped to the old canvas edge");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();

  // Text can also be the first layer that grows a side, independently of an
  // arrow having already made that position part of the canvas.
  QTest::keyClick(&editor, Qt::Key_T);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, outside);
  application.processEvents();
  textEditor = qobject_cast<QPlainTextEdit *>(QApplication::focusWidget());
  if (textEditor == nullptr) {
    error = QStringLiteral("Text could not begin outside the current canvas");
    return false;
  }
  QTest::keyClicks(textEditor, QStringLiteral("Standalone note"));
  QTest::keyClick(textEditor, Qt::Key_Return, Qt::ControlModifier);
  application.processEvents();
  if (editor.currentAnnotationsForTest().size() != 1 ||
      editor.currentAnnotationsForTest().constFirst().kind !=
          Annotation::Kind::Text ||
      annotationTextBounds(editor.currentAnnotationsForTest().constFirst())
              .left() >= 0.0 ||
      editor.currentCanvasForTest().left() >= 0.0) {
    error = QStringLiteral("Standalone text did not grow the canvas");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();

  // Screenshot-derived operations have nothing meaningful to do in the
  // surround, and the narrow band occupied by editor chrome is never canvas.
  QTest::keyClick(&editor, Qt::Key_D);
  drag(outside, inside);
  QTest::keyClick(&editor, Qt::Key_A);
  const QRectF arrowButton =
      editor.toolbarButtonRectForTest(QStringLiteral("tool-arrow"));
  const QPoint chromeGap(outside.x(), qRound(arrowButton.bottom() + 2.0));
  drag(chromeGap, inside);
  if (!editor.currentAnnotationsForTest().isEmpty()) {
    error = QStringLiteral("Source tool or editor chrome grew the canvas");
    return false;
  }

  // Image is the explicit strict boundary: starting outside it must stay a
  // no-op even for a tool that grows Framed and Overflow canvases.
  QTest::keyClick(&editor, Qt::Key_G, Qt::ShiftModifier);
  application.processEvents();
  if (editor.currentCanvasBoundaryForTest() != CanvasBoundaryMode::Image) {
    error = QStringLiteral("Off-canvas fixture did not enter Image boundary");
    return false;
  }
  drag(outside, inside);
  if (!editor.currentAnnotationsForTest().isEmpty() ||
      editor.currentCanvasForTest() != sourceCanvas) {
    error = QStringLiteral("Image boundary accepted an off-canvas start");
    return false;
  }
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
  const QRectF sourceCanvas(QPointF(), selection.size());
  const auto widgetPoint = [&](const QPointF &point) {
    const QPointF mapped = editor.annotationPointToWidgetForTest(point);
    return QPoint(qRound(mapped.x()), qRound(mapped.y()));
  };
  const auto currentOutput = [&] {
    return flushedSnapshot(editor, snapshotPath);
  };
  const auto expectedCurrent = [&](BackgroundStyle background,
                                   bool imageShadow = true) {
    return renderCapture(capture, selection,
                         editor.currentAnnotationsForTest(), background,
                         imageShadow);
  };
  const auto canvasIsDerived = [&] {
    return editor.currentCanvasForTest() ==
           captureCanvasRect(selection.size(),
                             editor.currentAnnotationsForTest());
  };
  const QColor slate(QStringLiteral("#242424"));
  const auto isSlateShadow = [&](const QColor &pixel) {
    return pixel.alpha() == 255 && pixel.red() < slate.red() &&
           pixel.green() < slate.green() && pixel.blue() < slate.blue();
  };
  const auto drag = [&](const QPoint &from, const QPoint &to) {
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, from);
    QTest::mouseMove(&editor, to, 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, to);
    application.processEvents();
  };

  // Start with a layer wholly inside the source frame.
  QTest::keyClick(&editor, Qt::Key_R);
  drag(widgetPoint({300, 140}), widgetPoint({450, 260}));
  if (editor.currentAnnotationsForTest().size() != 1 ||
      editor.currentAnnotationsForTest().constFirst().kind !=
          Annotation::Kind::Rectangle ||
      editor.currentCanvasForTest() != sourceCanvas || !canvasIsDerived()) {
    error = QStringLiteral("Outside-canvas smoke: rectangle did not render");
    return false;
  }

  const QImage inside = currentOutput();
  if (inside != expectedCurrent(BackgroundStyle::None)) {
    error = QStringLiteral("Inside rectangle output did not match its layers");
    return false;
  }

  // Carry it across the right edge. Canvas mapping stays fixed for the whole
  // drag and refits only after the patch commits.
  QTest::keyClick(&editor, Qt::Key_V);
  const Annotation initial = editor.currentAnnotationsForTest().constFirst();
  const QPointF initialMovePoint(
      initial.start.x() + (initial.end.x() - initial.start.x()) * 0.3,
      initial.start.y());
  const QPoint moveStart = widgetPoint(initialMovePoint);
  drag(moveStart, moveStart + QPoint(220, 0));
  if (editor.currentAnnotationsForTest().size() != 1 || !canvasIsDerived() ||
      editor.currentCanvasForTest().right() <= sourceCanvas.right()) {
    error = QStringLiteral("Outside-canvas smoke: rectangle did not move");
    return false;
  }
  const Annotation shifted = editor.currentAnnotationsForTest().constFirst();
  const QRectF shiftedCanvas = editor.currentCanvasForTest();
  const QImage shiftedOutput = currentOutput();
  const QPoint shiftedSourceOrigin(qRound(-shiftedCanvas.left()),
                                   qRound(-shiftedCanvas.top()));
  if (shiftedOutput != expectedCurrent(BackgroundStyle::None) ||
      shiftedOutput.size() != shiftedCanvas.size().toSize() ||
      !isSlateShadow(shiftedOutput.pixelColor(shiftedSourceOrigin +
                                              QPoint(610, 350))) ||
      shiftedOutput.pixelColor(shiftedSourceOrigin + QPoint(650, 350)) !=
          slate ||
      shiftedOutput.pixelColor(shiftedSourceOrigin + QPoint(599, 350)) !=
          QColor(QStringLiteral("#182030"))) {
    error = QStringLiteral(
        "Right-side growth did not shadow only the source card");
    return false;
  }

  // Resize its exposed bottom-right handle farther out, then verify that undo
  // and redo derive the exact prior/new canvases instead of accumulating a
  // copied extension.
  const QPoint resizeStart = widgetPoint(shifted.end);
  drag(resizeStart, widgetPoint(shifted.end + QPointF(50, 170)));
  const Annotation resized = editor.currentAnnotationsForTest().constFirst();
  const QRectF resizedCanvas = editor.currentCanvasForTest();
  const QImage resizedOutput = currentOutput();
  if (!canvasIsDerived() || resizedCanvas.right() <= shiftedCanvas.right() ||
      resizedCanvas.bottom() <= sourceCanvas.bottom() ||
      resizedOutput != expectedCurrent(BackgroundStyle::None)) {
    error = QStringLiteral(
        "Dragging an exposed handle did not grow the canvas again");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (editor.currentAnnotationsForTest().constFirst() != shifted ||
      editor.currentCanvasForTest() != shiftedCanvas ||
      currentOutput() != shiftedOutput) {
    error = QStringLiteral("Undo did not restore the outside-handle resize");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
  application.processEvents();
  if (editor.currentAnnotationsForTest().constFirst() != resized ||
      editor.currentCanvasForTest() != resizedCanvas ||
      currentOutput() != resizedOutput) {
    error = QStringLiteral("Redo did not replay the grown-handle resize");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();

  // Carry the layer through the top-left. This forces a non-zero source
  // offset and verifies that the shadow stays anchored to the old frame.
  const QPointF shiftedMovePoint(
      shifted.start.x() + (shifted.end.x() - shifted.start.x()) * 0.3,
      shifted.start.y());
  const QPoint shiftedStart = widgetPoint(shiftedMovePoint);
  drag(shiftedStart, shiftedStart + QPoint(-600, -190));
  const QRectF offsetCanvas = editor.currentCanvasForTest();
  const QImage slateOutput = currentOutput();
  if (!canvasIsDerived() || offsetCanvas.left() >= 0 ||
      offsetCanvas.top() >= 0 ||
      slateOutput != expectedCurrent(BackgroundStyle::None)) {
    error = QStringLiteral(
        "Top-left drag did not grow a stable offset canvas");
    return false;
  }
  const QPoint sourceOrigin(qRound(-offsetCanvas.left()),
                            qRound(-offsetCanvas.top()));
  if (slateOutput.pixelColor(sourceOrigin + QPoint(300, 300)) !=
          QColor(QStringLiteral("#182030")) ||
      !isSlateShadow(
          slateOutput.pixelColor(sourceOrigin + QPoint(-5, 300))) ||
      slateOutput.pixelColor(0, sourceOrigin.y() + 300) != slate ||
      slateOutput.pixelColor(sourceOrigin + QPoint(0, 300)) !=
          QColor(QStringLiteral("#182030")) ||
      slateOutput != currentOutput()) {
    error = QStringLiteral(
        "Grown output shifted the source or misplaced its shadow");
    return false;
  }

  // Shift+B is a document edit, not another backdrop step: it removes only
  // the source card's shadow, can be undone/redone, and survives sidecar
  // persistence with the implicit Slate backdrop still implicit.
  QTest::keyClick(&editor, Qt::Key_B, Qt::ShiftModifier);
  application.processEvents();
  const QImage noShadowOutput = currentOutput();
  if (noShadowOutput == slateOutput ||
      noShadowOutput != expectedCurrent(BackgroundStyle::None, false) ||
      noShadowOutput.pixelColor(sourceOrigin + QPoint(-5, 300)) != slate ||
      editor.operationLog().isEmpty() ||
      editor.operationLog().constLast().type != Operation::Type::Background ||
      editor.operationLog().constLast().background != BackgroundStyle::None ||
      editor.operationLog().constLast().imageShadow) {
    error = QStringLiteral("Shift+B did not disable only the drop shadow");
    return false;
  }
  if (!editor.waitForSnapshot() || editor.workingLogPath().isEmpty()) {
    error = QStringLiteral("Disabled shadow operation was not persisted");
    return false;
  }
  CaptureEditor shadowRestored(capture);
  QString shadowRestoreError;
  if (!shadowRestored.restoreOperationLog(editor.workingLogPath(),
                                          shadowRestoreError) ||
      shadowRestored.renderCurrentOutput() != noShadowOutput) {
    error = QStringLiteral("Restoring disabled shadow changed output: %1")
                .arg(shadowRestoreError);
    return false;
  }
  shadowRestored.close();
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (currentOutput() != slateOutput) {
    error = QStringLiteral("Undo did not restore the default drop shadow");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
  application.processEvents();
  if (currentOutput() != noShadowOutput) {
    error = QStringLiteral("Redo did not disable the drop shadow again");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_B, Qt::ShiftModifier);
  application.processEvents();
  if (currentOutput() != slateOutput) {
    error = QStringLiteral("Shift+B did not toggle the drop shadow back on");
    return false;
  }

  // Automatic growth starts on an implicit shadowed-gray mat but remains in
  // fullscreen mode: B runs through every shadowed color, then shadowed gray,
  // flat gray, and explicit Off before wrapping back to blue.
  struct BackdropStep {
    BackgroundStyle style;
    bool shadow;
  };
  const std::array<BackdropStep, 7> grownCycle{{
      {BackgroundStyle::Aurora, true},
      {BackgroundStyle::Sunset, true},
      {BackgroundStyle::Lagoon, true},
      {BackgroundStyle::Violet, true},
      {BackgroundStyle::Slate, true},
      {BackgroundStyle::Slate, false},
      {BackgroundStyle::Off, true},
  }};
  for (const BackdropStep step : grownCycle) {
    QTest::keyClick(&editor, Qt::Key_B);
    application.processEvents();
    const Operation &operation = editor.operationLog().constLast();
    if (operation.type != Operation::Type::Background ||
        operation.background != step.style ||
        operation.imageShadow != step.shadow ||
        currentOutput() != expectedCurrent(step.style, step.shadow) ||
        editor.currentCanvasForTest() != offsetCanvas) {
      error = QStringLiteral("Grown backdrop cycle reached the wrong state");
      return false;
    }
  }
  // Leave this larger fixture on shadowed gray for the movement, contraction,
  // and operation-log checks below.
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (currentOutput() != slateOutput) {
    error = QStringLiteral(
        "Undoing the grown backdrop wrap did not restore shadowed Slate");
    return false;
  }

  // Moving the layer wholly back over the source contracts the canvas. Undo
  // grows it from vector geometry again; delete contracts it for the same
  // reason, with no special raster-resize operation to replay.
  const Annotation offset = editor.currentAnnotationsForTest().constFirst();
  const QPointF offsetMovePoint(
      offset.start.x() + (offset.end.x() - offset.start.x()) * 0.3,
      offset.start.y());
  const QPoint offsetStart = widgetPoint(offsetMovePoint);
  drag(offsetStart, offsetStart + QPoint(300, 150));
  if (editor.currentCanvasForTest() != sourceCanvas || !canvasIsDerived()) {
    error = QStringLiteral("Moving a layer back in did not contract canvas");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (editor.currentCanvasForTest() != offsetCanvas ||
      currentOutput() != slateOutput) {
    error = QStringLiteral("Undo did not regrow the offset canvas");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Delete);
  application.processEvents();
  if (!editor.currentAnnotationsForTest().isEmpty() ||
      editor.currentCanvasForTest() != sourceCanvas) {
    error = QStringLiteral("Deleting the outside layer did not contract canvas");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (editor.currentCanvasForTest() != offsetCanvas ||
      currentOutput() != slateOutput) {
    error = QStringLiteral("Undo delete did not regrow canvas");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
  application.processEvents();

  // Typing across the frame grows by the committed glyph/pill bounds. Undo,
  // redo, and operation-log restore all derive the same canvas and pixels.
  QTest::keyClick(&editor, Qt::Key_T);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                    widgetPoint({570, 100}));
  application.processEvents();
  auto *inlineEditor =
      qobject_cast<QPlainTextEdit *>(QApplication::focusWidget());
  if (inlineEditor == nullptr) {
    error = QStringLiteral("Text growth did not open the inline editor");
    return false;
  }
  QTest::keyClicks(inlineEditor,
                   QStringLiteral("Canvas grows for typed labels"));
  QTest::keyClick(inlineEditor, Qt::Key_Return, Qt::ControlModifier);
  application.processEvents();
  if (editor.currentAnnotationsForTest().size() != 1 ||
      editor.currentAnnotationsForTest().constFirst().kind !=
          Annotation::Kind::Text ||
      editor.currentCanvasForTest().right() <= sourceCanvas.right() ||
      !canvasIsDerived()) {
    error = QStringLiteral("Committed text did not grow past the frame");
    return false;
  }
  const QVector<Annotation> textAnnotations =
      editor.currentAnnotationsForTest();
  const QRectF textCanvas = editor.currentCanvasForTest();
  const QImage textOutput = currentOutput();
  const QPoint textSourceOrigin(qRound(-textCanvas.left()),
                                qRound(-textCanvas.top()));
  if (textOutput != expectedCurrent(BackgroundStyle::None) ||
      !isSlateShadow(textOutput.pixelColor(textSourceOrigin +
                                           QPoint(610, 350))) ||
      textOutput.pixelColor(textSourceOrigin + QPoint(650, 350)) != slate) {
    error = QStringLiteral(
        "Text growth did not keep the source shadow on default Slate");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (!editor.currentAnnotationsForTest().isEmpty() ||
      editor.currentCanvasForTest() != sourceCanvas) {
    error = QStringLiteral("Undo text did not contract canvas");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
  application.processEvents();
  if (editor.currentAnnotationsForTest() != textAnnotations ||
      editor.currentCanvasForTest() != textCanvas ||
      currentOutput() != textOutput) {
    error = QStringLiteral("Redo text did not replay canvas growth");
    return false;
  }

  // Crop handles still belong to the source frame inside the wider canvas.
  // Moving its left edge keeps the text at the same absolute capture point,
  // including after Crop replay.
  const QPointF absoluteTextStart =
      editor.currentSelection().topLeft() + textAnnotations.constFirst().start;
  QTest::keyClick(&editor, Qt::Key_V);
  const QRectF sourceFrame = editor.sourceFrameWidgetRectForTest();
  const QPoint cropLeft(qRound(sourceFrame.left() - 7),
                        qRound(sourceFrame.center().y()));
  drag(cropLeft, cropLeft + QPoint(20, 0));
  const QRectF croppedSelection = editor.currentSelection();
  const QVector<Annotation> croppedAnnotations =
      editor.currentAnnotationsForTest();
  const QRectF croppedCanvas = editor.currentCanvasForTest();
  const QImage croppedOutput = currentOutput();
  const QPointF replayedAbsoluteStart =
      croppedSelection.topLeft() + croppedAnnotations.constFirst().start;
  if (croppedSelection.left() <= selection.left() || !canvasIsDerived() ||
      QLineF(absoluteTextStart, replayedAbsoluteStart).length() > 0.01) {
    error = QStringLiteral("Source recrop shifted grown text");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (editor.currentSelection() != selection ||
      editor.currentAnnotationsForTest() != textAnnotations ||
      editor.currentCanvasForTest() != textCanvas ||
      currentOutput() != textOutput) {
    error = QStringLiteral("Undo recrop changed grown text coordinates");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
  application.processEvents();
  if (editor.currentSelection() != croppedSelection ||
      editor.currentAnnotationsForTest() != croppedAnnotations ||
      editor.currentCanvasForTest() != croppedCanvas ||
      currentOutput() != croppedOutput) {
    error = QStringLiteral("Redo recrop did not replay grown coordinates");
    return false;
  }

  if (!editor.waitForSnapshot() || editor.workingLogPath().isEmpty()) {
    error = QStringLiteral("Text growth operation log was not persisted");
    return false;
  }
  CaptureEditor restored(capture);
  QString restoreError;
  if (!restored.restoreOperationLog(editor.workingLogPath(), restoreError) ||
      restored.currentSelection() != croppedSelection ||
      restored.currentAnnotationsForTest() != croppedAnnotations ||
      restored.currentCanvasForTest() != croppedCanvas ||
      restored.renderCurrentOutput() != croppedOutput) {
    error = QStringLiteral("Restoring text growth changed canvas: %1")
                .arg(restoreError);
    return false;
  }
  restored.close();
  editor.close();
  QFile::remove(snapshotPath);
  return true;
}

/** Fullscreen B starts with blue, runs through every shadowed color, then
 *  demonstrates shadowed gray, flat gray, Off, and its blue wrap. */
bool runFullscreenBackdropCycleSmoke(QApplication &application,
                                     QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 180, 120};
  capture.monitor.pixelSize = {180, 120};
  capture.monitor.scale = 1.0;
  capture.source = QImage(180, 120, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#527196")));
  capture.previewSize = capture.source.size();

  struct BackdropStep {
    BackgroundStyle style;
    bool shadow;
  };
  const std::array<BackdropStep, 8> fullscreenCycle{{
      {BackgroundStyle::Aurora, true},
      {BackgroundStyle::Sunset, true},
      {BackgroundStyle::Lagoon, true},
      {BackgroundStyle::Violet, true},
      {BackgroundStyle::Slate, true},
      {BackgroundStyle::Slate, false},
      {BackgroundStyle::Off, true},
      {BackgroundStyle::Aurora, true},
  }};

  CaptureEditor editor(capture, CaptureEditor::CaptureMode::File);
  editor.resize(640, 480);
  editor.show();
  application.processEvents();
  QImage shadowedGray;
  QImage flatGray;
  for (const BackdropStep step : fullscreenCycle) {
    QTest::keyClick(&editor, Qt::Key_B);
    application.processEvents();
    const Operation &operation = editor.operationLog().constLast();
    const QImage output = editor.renderCurrentOutput();
    const QImage expected = renderCapture(capture, editor.currentSelection(),
                                          {}, step.style, step.shadow);
    if (operation.type != Operation::Type::Background ||
        operation.background != step.style ||
        operation.imageShadow != step.shadow || output != expected) {
      error =
          QStringLiteral("Fullscreen backdrop cycle reached the wrong state");
      return false;
    }
    if (step.style == BackgroundStyle::Slate && step.shadow)
      shadowedGray = output;
    else if (step.style == BackgroundStyle::Slate && !step.shadow)
      flatGray = output;
  }
  if (shadowedGray.isNull() || flatGray.isNull() || shadowedGray == flatGray) {
    error = QStringLiteral(
        "Shadowed and flat fullscreen gray rendered identically");
    return false;
  }
  editor.close();

  CaptureEditor overrideEditor(capture, CaptureEditor::CaptureMode::File);
  overrideEditor.resize(640, 480);
  overrideEditor.show();
  application.processEvents();
  QTest::keyClick(&overrideEditor, Qt::Key_B, Qt::ShiftModifier);
  QTest::keyClick(&overrideEditor, Qt::Key_B);
  const Operation &firstColor = overrideEditor.operationLog().constLast();
  if (firstColor.background != BackgroundStyle::Aurora ||
      !firstColor.imageShadow) {
    error = QStringLiteral(
        "Fullscreen B did not restore shadow on its first blue backdrop");
    return false;
  }
  QTest::keyClick(&overrideEditor, Qt::Key_B, Qt::ShiftModifier);
  application.processEvents();
  const Operation &manualOff = overrideEditor.operationLog().constLast();
  if (manualOff.background != BackgroundStyle::Aurora ||
      manualOff.imageShadow) {
    error = QStringLiteral("Shift+B did not disable the current color shadow");
    return false;
  }
  QTest::keyClick(&overrideEditor, Qt::Key_B);
  application.processEvents();
  const Operation &nextColor = overrideEditor.operationLog().constLast();
  if (nextColor.background != BackgroundStyle::Sunset ||
      !nextColor.imageShadow) {
    error = QStringLiteral(
        "B did not restore the next color's canonical shadow");
    return false;
  }
  overrideEditor.close();
  return true;
}


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

  const QRectF paletteButton =
      editor.toolbarButtonRectForTest(QStringLiteral("palette"));
  QTest::mouseMove(&editor, paletteButton.center().toPoint(), 10);
  application.processEvents();
  if (!editor.colorPaletteOpenForTest()) {
    error = QStringLiteral("Hovering the palette button did not open its submenu");
    return false;
  }

  const QRectF palette = editor.colorPaletteRectForTest();
  const QPointF through = (paletteButton.center() + palette.center()) / 2.0;
  QTest::mouseMove(&editor, through.toPoint(), 10);
  application.processEvents();
  if (!editor.colorPaletteOpenForTest()) {
    error = QStringLiteral("Diagonal movement inside the submenu triangle closed the palette");
    return false;
  }

  QTest::mouseMove(&editor, palette.center().toPoint(), 10);
  application.processEvents();
  if (!editor.colorPaletteOpenForTest()) {
    error = QStringLiteral("Moving from the submenu triangle into the palette closed it");
    return false;
  }

  QTest::mouseMove(
      &editor,
      editor.toolbarButtonCenterForTest(QStringLiteral("tool-arrow")), 10);
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

  // 600x400 selection shown 1:1 at widget offset (100, 117).
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
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(200, 212));
  QTest::mouseMove(&editor, QPoint(400, 312), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(400, 312));
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
                    QPoint(500, 212));
  QTest::mouseMove(&editor, QPoint(560, 312), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::ShiftModifier,
                      QPoint(560, 312));
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
  for (const QPoint &miss : {QPoint(203, 215), QPoint(300, 262)}) {
    QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, miss);
    QTest::keyClick(&editor, Qt::Key_Delete);
    application.processEvents();
    if (!snapshotMatches(expected({ellipse, circle}))) {
      error = QStringLiteral("Ellipse hit-test selected outside its stroke");
      return false;
    }
  }
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(250, 212));
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
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(250, 212));
  QTest::mouseMove(&editor, QPoint(270, 242), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(270, 242));
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
  QTest::mouseMove(&editor,
                   editor.toolbarButtonCenterForTest(
                       QStringLiteral("tool-rectangle")),
                   10);
  application.processEvents();
  if (!editor.shapeMenuOpenForTest()) {
    error = QStringLiteral("Hovering the shape button did not open its submenu");
    return false;
  }
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                    editor.toolbarButtonCenterForTest(
                        QStringLiteral("shape-ellipse")));
  QTest::mouseMove(&editor, QPoint(300, 312), 20);
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
  drag(QPoint(200, 212), QPoint(400, 312));
  const Annotation hollow = shapeSized(Annotation::Kind::Rectangle, {100, 95},
                                       {300, 195}, false, 0, 5);
  if (!snapshotMatches(expected({hollow}))) {
    error = QStringLiteral("Rectangle did not start hollow at wheel size 5");
    return false;
  }
  wheel(-1);
  QTest::keyClick(&editor, Qt::Key_R);
  drag(QPoint(200, 362), QPoint(400, 462));
  const Annotation filled =
      shape(Annotation::Kind::Rectangle, {100, 245}, {300, 345}, true);
  if (!snapshotMatches(expected({hollow, filled}))) {
    error = QStringLiteral("R again did not fill the next rectangle");
    return false;
  }

  // Alt+wheel rounds the corners of new rectangles (2 px per notch, 0–24);
  // the plain wheel keeps meaning stroke size, as for every other tool.
  wheel(6, Qt::AltModifier);
  drag(QPoint(450, 212), QPoint(650, 312));
  const Annotation rounded =
      shape(Annotation::Kind::Rectangle, {350, 95}, {550, 195}, true, 12);
  if (!snapshotMatches(expected({hollow, filled, rounded}))) {
    error = QStringLiteral("Alt+wheel did not round the next rectangle");
    return false;
  }
  wheel(-20, Qt::AltModifier);
  drag(QPoint(450, 362), QPoint(650, 462));
  const Annotation squareAgain =
      shape(Annotation::Kind::Rectangle, {350, 245}, {550, 345}, true, 0);
  if (!snapshotMatches(expected({hollow, filled, rounded, squareAgain}))) {
    error = QStringLiteral("Alt+wheel did not clamp the corner radius to 0");
    return false;
  }

  // Alt+wheel on a selected rectangle rounds that rectangle, undoably,
  // instead of retuning the armed tool's default.
  QTest::keyClick(&editor, Qt::Key_V);
  application.processEvents();
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                    editor.toScreenPointForTest(QPointF(450, 145)).toPoint());
  wheel(1, Qt::AltModifier);
  Annotation roundedMore = rounded;
  roundedMore.cornerRadius = 14;
  if (!snapshotMatches(expected({hollow, filled, roundedMore, squareAgain}))) {
    error = QStringLiteral("Alt+wheel did not round the selected rectangle");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (!snapshotMatches(expected({hollow, filled, rounded, squareAgain}))) {
    error = QStringLiteral("Rounding the selected rectangle was not undoable");
    return false;
  }

  // Ellipses share the fill flag; E again toggles it back to hollow.
  QTest::keyClick(&editor, Qt::Key_E);
  QTest::keyClick(&editor, Qt::Key_E);
  drag(QPoint(200, 492), QPoint(300, 512));
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
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(300, 262));
  QTest::keyClick(&editor, Qt::Key_Delete);
  application.processEvents();
  if (!snapshotMatches(
          expected({hollow, filled, rounded, squareAgain, ellipse}))) {
    error = QStringLiteral("Hollow rectangle interior selected a layer");
    return false;
  }
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(300, 412));
  QTest::keyClick(&editor, Qt::Key_Delete);
  application.processEvents();
  if (!snapshotMatches(expected({hollow, rounded, squareAgain, ellipse}))) {
    error = QStringLiteral("Filled rectangle interior did not select it");
    return false;
  }

  // R with a selected rectangle toggles that layer's fill (undoable).
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(300, 212));
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
  QTest::mouseMove(&editor, QPoint(160, 492));
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
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(680, 492));
  QTest::keyClick(&editor, Qt::Key_R);
  application.processEvents();
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                    editor.toolbarButtonCenterForTest(
                        QStringLiteral("tool-rectangle")));
  application.processEvents();
  drag(QPoint(500, 492), QPoint(600, 512));
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
  QTest::mousePress(&editor, Qt::LeftButton, Qt::AltModifier, QPoint(400, 312));
  QTest::mouseMove(&editor, QPoint(500, 362), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::AltModifier,
                      QPoint(500, 362));
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
                    Qt::AltModifier | Qt::ShiftModifier, QPoint(400, 312));
  QTest::mouseMove(&editor, QPoint(460, 352), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton,
                      Qt::AltModifier | Qt::ShiftModifier, QPoint(460, 352));
  application.processEvents();
  const Annotation centeredSquare =
      shape(Annotation::Kind::Rectangle, {240, 135}, {360, 255});
  if (!snapshotMatches(expected({centeredEllipse, centeredSquare}))) {
    error = QStringLiteral("Alt+Shift-drag did not create a centered square");
    return false;
  }

  // Alt pressed mid-drag applies; released before the mouse it does not.
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(200, 212));
  QTest::mouseMove(&editor, QPoint(250, 242), 20);
  QTest::keyPress(&editor, Qt::Key_Alt);
  application.processEvents();
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::AltModifier,
                      QPoint(250, 242));
  QTest::keyRelease(&editor, Qt::Key_Alt);
  application.processEvents();
  const Annotation midDrag =
      shape(Annotation::Kind::Rectangle, {50, 65}, {150, 125});
  if (!snapshotMatches(expected({centeredEllipse, centeredSquare, midDrag}))) {
    error = QStringLiteral("Alt pressed mid-drag did not center the shape");
    return false;
  }
  QTest::mousePress(&editor, Qt::LeftButton, Qt::AltModifier, QPoint(600, 212));
  QTest::mouseMove(&editor, QPoint(650, 242), 20);
  QTest::keyRelease(&editor, Qt::Key_Alt);
  application.processEvents();
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(650, 242));
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
  QTest::mousePress(&editor, Qt::LeftButton, Qt::AltModifier, QPoint(600, 412));
  QTest::mouseMove(&editor, QPoint(660, 452), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::AltModifier,
                      QPoint(660, 452));
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
  QTest::mousePress(&editor, Qt::LeftButton, Qt::AltModifier, QPoint(200, 412));
  QTest::mouseMove(&editor, QPoint(300, 462), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::AltModifier,
                      QPoint(300, 462));
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

/** Bundled fonts resolve without system dependencies, render as distinct
 *  faces, cycle for the next/selected label, and survive the operation log. */
bool runTextFontSmoke(QApplication &application, QString &error) {
  error = QStringLiteral("Text font smoke failed");
  const std::array<std::pair<TextFont, QString>, 3> fonts{{
      {TextFont::Neucha, QStringLiteral("Neucha")},
      {TextFont::JetBrainsMono, QStringLiteral("JetBrains Mono")},
      {TextFont::InterDisplay, QStringLiteral("Inter Display")}}};
  for (const auto &[textFont, family] : fonts) {
    const QFont resolved = annotationTextFont(5.0, textFont);
    if (annotationTextFontName(textFont) != family ||
        QFontInfo(resolved).family() != family) {
      error = QStringLiteral("Bundled %1 did not resolve (got %2)")
                  .arg(family, QFontInfo(resolved).family());
      return false;
    }
  }

  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#182030")));
  capture.previewSize = capture.source.size();

  Annotation rendered;
  rendered.kind = Annotation::Kind::Text;
  rendered.start = {80, 100};
  rendered.text = QStringLiteral("Font 0123");
  rendered.color = QColor(QStringLiteral("#ff375f"));
  rendered.size = 5.0;
  rendered.textBackground = TextBackground::Plain;
  std::array<QImage, 3> renderedFonts;
  for (std::size_t index = 0; index < fonts.size(); ++index) {
    rendered.textFont = fonts.at(index).first;
    renderedFonts.at(index) = renderCapture(
        capture, QRectF(0, 0, 400, 200), {rendered}, BackgroundStyle::None);
  }
  if (renderedFonts.at(0) == renderedFonts.at(1) ||
      renderedFonts.at(1) == renderedFonts.at(2) ||
      renderedFonts.at(0) == renderedFonts.at(2)) {
    error = QStringLiteral("Text font selection did not change rendered ink");
    return false;
  }

  CaptureEditor editor(capture);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
  QTest::mouseMove(&editor, QPoint(700, 500), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(700, 500));
  application.processEvents();

  const auto typeText = [&](const QPoint &at, const QString &content) {
    QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, at);
    application.processEvents();
    QTest::keyClicks(QApplication::focusWidget(), content);
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return,
                    Qt::ControlModifier);
    application.processEvents();
  };
  const auto lastChangedText = [&]() -> const Annotation * {
    if (editor.operationLog().isEmpty() ||
        editor.operationLog().constLast().annotations.isEmpty())
      return nullptr;
    const Annotation &annotation =
        editor.operationLog().constLast().annotations.constFirst();
    return annotation.kind == Annotation::Kind::Text ? &annotation : nullptr;
  };

  // Neucha remains the default.
  QTest::keyClick(&editor, Qt::Key_T);
  typeText(QPoint(250, 250), QStringLiteral("Default"));
  if (lastChangedText() == nullptr ||
      lastChangedText()->textFont != TextFont::Neucha) {
    error = QStringLiteral("New text did not default to Neucha");
    return false;
  }

  // With no text selected, Shift+T cycles the next label and arms Text.
  QTest::keyClick(&editor, Qt::Key_T, Qt::ShiftModifier);
  if (editor.armedToolForTest() != CaptureEditor::Tool::Text ||
      !editor.statusForTest().contains(QStringLiteral("JetBrains Mono"))) {
    error = QStringLiteral("Shift+T did not select JetBrains Mono");
    return false;
  }
  const QPoint monoPoint(430, 250);
  typeText(monoPoint, QStringLiteral("Mono"));
  if (lastChangedText() == nullptr ||
      lastChangedText()->textFont != TextFont::JetBrainsMono) {
    error = QStringLiteral("Next text did not keep the cycled font");
    return false;
  }

  // A selected label cycles independently and records one undoable patch.
  QTest::keyClick(&editor, Qt::Key_V);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                    monoPoint + QPoint(12, 8));
  application.processEvents();
  if (editor.selectedCountForTest() != 1) {
    error = QStringLiteral("JetBrains Mono text could not be selected");
    return false;
  }
  const int beforeSelectedCycle = editor.operationIndex();
  QTest::keyClick(&editor, Qt::Key_T, Qt::ShiftModifier);
  application.processEvents();
  if (editor.operationIndex() != beforeSelectedCycle + 1 ||
      lastChangedText() == nullptr ||
      lastChangedText()->textFont != TextFont::InterDisplay ||
      !editor.statusForTest().contains(QStringLiteral("Inter Display"))) {
    error = QStringLiteral("Shift+T did not cycle selected text to Inter");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (editor.operationIndex() != beforeSelectedCycle) {
    error = QStringLiteral("Selected text font change was not undoable");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
  application.processEvents();
  if (editor.operationIndex() != beforeSelectedCycle + 1) {
    error = QStringLiteral("Selected text font change was not redoable");
    return false;
  }

  // Changing a selected layer did not change the next-label default: it was
  // still Mono, so one Shift+T advances that independent state to Inter.
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(650, 450));
  QTest::keyClick(&editor, Qt::Key_T, Qt::ShiftModifier);
  if (!editor.statusForTest().contains(QStringLiteral("Inter Display"))) {
    error = QStringLiteral("Selected font leaked into the next-text default");
    return false;
  }
  const QPoint interPoint(330, 380);
  typeText(interPoint, QStringLiteral("Inter"));
  if (lastChangedText() == nullptr ||
      lastChangedText()->textFont != TextFont::InterDisplay) {
    error = QStringLiteral("Inter Display did not apply to the next text");
    return false;
  }

  // Reopening a layer uses its own face in the native inline editor and
  // commits it unchanged.
  QTest::keyClick(&editor, Qt::Key_V);
  QTest::mouseDClick(&editor, Qt::LeftButton, Qt::NoModifier,
                     interPoint + QPoint(12, 8));
  application.processEvents();
  QWidget *draft = QApplication::focusWidget();
  if (draft == nullptr || draft == &editor ||
      QFontInfo(draft->font()).family() != QStringLiteral("Inter Display")) {
    error = QStringLiteral("Inline editor did not use the layer font");
    return false;
  }
  QTest::keyClick(draft, Qt::Key_Return, Qt::ControlModifier);
  application.processEvents();
  if (lastChangedText() == nullptr ||
      lastChangedText()->textFont != TextFont::InterDisplay) {
    error = QStringLiteral("Re-editing changed the layer font");
    return false;
  }

  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(650, 450));
  QTest::keyClick(&editor, Qt::Key_T, Qt::ShiftModifier);
  if (!editor.statusForTest().contains(QStringLiteral("Neucha"))) {
    error = QStringLiteral("Shift+T did not cycle Inter back to Neucha");
    return false;
  }

  QTemporaryDir directory;
  if (!directory.isValid()) {
    error = QStringLiteral("Could not create text-font log directory");
    return false;
  }
  OperationLog saved;
  saved.ops = editor.operationLog();
  saved.index = editor.operationIndex();
  saved.previewSize = capture.previewSize;
  const QString path =
      QDir(directory.path()).filePath(QStringLiteral("text-fonts.json"));
  OperationLog loaded;
  if (!saveOperationLog(path, saved, error) ||
      !loadOperationLog(path, loaded, error) || loaded != saved) {
    error = QStringLiteral("Text fonts did not survive operation-log reload: %1")
                .arg(error);
    return false;
  }
  editor.close();
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
  const QRectF multilineCanvas =
      captureCanvasRect(QSizeF(300, 100), {multiline});
  const QPoint multilineOrigin = (-multilineCanvas.topLeft()).toPoint();
  if (multilineImage.pixelColor(multilineOrigin + secondLinePill) !=
      QColor(248, 245, 235)) {
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
  typeText(QPoint(300, 312), QStringLiteral("Pill"));
  const Annotation pill =
      text({200, 195}, QStringLiteral("Pill"), TextBackground::Pill);
  if (!snapshotMatches(expected({pill}))) {
    error = QStringLiteral("New text did not get a readability pill");
    return false;
  }

  // T twice more (tool still armed) cycles pill, outline, plain, so the next
  // text lands on plain.
  QTest::keyClick(&editor, Qt::Key_T);
  QTest::keyClick(&editor, Qt::Key_T);
  typeText(QPoint(300, 412), QStringLiteral("Plain"));
  const Annotation plain =
      text({200, 295}, QStringLiteral("Plain"), TextBackground::Plain);
  if (!snapshotMatches(expected({pill, plain}))) {
    error = QStringLiteral("T twice did not cycle new text to plain");
    return false;
  }

  // T with a text layer selected cycles that layer, undoably.
  QTest::keyClick(&editor, Qt::Key_V);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(310, 302));
  QTest::keyClick(&editor, Qt::Key_T);
  application.processEvents();
  Annotation pillNowOutline = pill;
  pillNowOutline.textBackground = TextBackground::Outline;
  if (!snapshotMatches(expected({pillNowOutline, plain}))) {
    error = QStringLiteral("T did not cycle the selected text's style");
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
  QTest::mouseDClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(310, 422));
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
  drag(QPoint(200, 212), QPoint(400, 312));
  QTest::keyClick(&editor, Qt::Key_A);
  drag(QPoint(450, 212), QPoint(650, 312));
  QTest::keyClick(&editor, Qt::Key_M);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(250, 412));
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(350, 412));
  application.processEvents();
  const QVector<Annotation> all = {
      layer(Annotation::Kind::Rectangle, {100, 95}, {300, 195}),
      layer(Annotation::Kind::Arrow, {350, 95}, {550, 195}),
      layer(Annotation::Kind::Marker, {141, 286}, {}, 1),
      layer(Annotation::Kind::Marker, {241, 286}, {}, 2)};
  if (!snapshotMatches(expected(all))) {
    error = QStringLiteral("Select-all smoke could not draw its four layers");
    return false;
  }

  // Delete with nothing selected leaves everything alone.
  QTest::keyClick(&editor, Qt::Key_V);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(680, 492));
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
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(680, 492));
  application.processEvents();
  if (grouped == editor.grab().toImage()) {
    error = QStringLiteral("Select-all drew no selection indicator");
    return false;
  }

  // Clicking empty canvas drops the group selection again, so a following
  // Delete is a no-op.
  QTest::keyClick(&editor, Qt::Key_A, Qt::ControlModifier);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(680, 492));
  QTest::keyClick(&editor, Qt::Key_Backspace);
  application.processEvents();
  if (!snapshotMatches(expected(all))) {
    error = QStringLiteral("Delete after leaving select-all removed layers");
    return false;
  }

  // Ctrl+A then Delete removes every layer as one undo step, even with a
  // single layer selected beforehand.
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(300, 212));
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
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(300, 312));
  application.processEvents();
  if (!snapshotMatches(
          expected({layer(Annotation::Kind::Marker, {191, 186}, {}, 1)}))) {
    error = QStringLiteral("Marker numbering did not restart after clearing");
    return false;
  }

  editor.close();
  QFile::remove(snapshotPath);
  return true;
}

/** A grown canvas keeps selection chrome on real layers only. Screenshot
 *  crop chrome disappears for both single- and multi-layer selections, then
 *  returns when the layers are put down. */
bool runGrownCanvasSelectAllChromeSmoke(QApplication &application,
                                        QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 600, 400};
  capture.monitor.pixelSize = {600, 400};
  capture.monitor.scale = 1.0;
  capture.source = QImage(600, 400, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#182030")));
  capture.previewSize = capture.source.size();

  Annotation inside;
  inside.id = 1;
  inside.kind = Annotation::Kind::Rectangle;
  inside.start = {60, 100};
  inside.end = {160, 200};
  inside.color = QColor(QStringLiteral("#ff375f"));
  inside.size = 4;
  Annotation outside = inside;
  outside.id = 2;
  outside.start = {650, 250};
  outside.end = {750, 350};

  Operation annotate;
  annotate.type = Operation::Type::Annotate;
  annotate.annotations = {inside, outside};
  OperationLog log;
  log.ops = {annotate};
  log.index = 1;
  log.nextId = 3;
  log.previewSize = capture.previewSize;

  CaptureEditor editor(capture, CaptureEditor::CaptureMode::File,
                       QuickOutputMode::None, log);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();
  if (editor.currentCanvasForTest().right() <= capture.previewSize.width()) {
    error = QStringLiteral("Select-all chrome fixture did not grow its canvas");
    return false;
  }

  const QImage idle = editor.grab().toImage();
  const QColor liveShadow = grabLogicalPixel(
      idle, editor, editor.annotationPointToWidgetForTest({610, 220}));
  const QColor liveMatte = grabLogicalPixel(
      idle, editor, editor.annotationPointToWidgetForTest({648, 220}));
  if (liveShadow.alpha() != 255 || liveShadow.red() >= 36 ||
      liveShadow.green() >= 36 || liveShadow.blue() >= 36 ||
      liveMatte != QColor(QStringLiteral("#242424"))) {
    error = QStringLiteral(
        "Live grown canvas did not shadow only the source frame");
    return false;
  }

  // This canvas already shows the shadowed-gray opening state, so B advances
  // directly to the first shadowed color rather than repeating gray.
  QTest::keyClick(&editor, Qt::Key_B);
  application.processEvents();
  if (editor.operationLog().constLast().background !=
          BackgroundStyle::Aurora ||
      !editor.operationLog().constLast().imageShadow) {
    error = QStringLiteral(
        "B did not advance live grown gray to shadowed Aurora");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  const QColor automaticShadow = grabLogicalPixel(
      editor.grab().toImage(), editor,
      editor.annotationPointToWidgetForTest({610, 220}));
  if (automaticShadow.alpha() != 255 || automaticShadow.red() >= 36 ||
      automaticShadow.green() >= 36 || automaticShadow.blue() >= 36) {
    error = QStringLiteral(
        "Undo did not restore automatic grown-canvas shadow");
    return false;
  }

  QTest::keyClick(&editor, Qt::Key_B, Qt::ShiftModifier);
  application.processEvents();
  const QImage shadowDisabled = editor.grab().toImage();
  const QColor formerShadow = grabLogicalPixel(
      shadowDisabled, editor, editor.annotationPointToWidgetForTest({610, 220}));
  if (formerShadow != QColor(QStringLiteral("#242424"))) {
    error = QStringLiteral("Shift+B did not remove the live source shadow");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  const QColor restoredShadow = grabLogicalPixel(
      editor.grab().toImage(), editor,
      editor.annotationPointToWidgetForTest({610, 220}));
  if (restoredShadow.alpha() != 255 || restoredShadow.red() >= 36 ||
      restoredShadow.green() >= 36 || restoredShadow.blue() >= 36) {
    error = QStringLiteral("Undo did not restore the live source shadow");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_A, Qt::ControlModifier);
  application.processEvents();
  const QImage selected = editor.grab().toImage();
  if (editor.selectedCountForTest() != 2) {
    error = QStringLiteral("Ctrl+A did not select both grown-canvas layers");
    return false;
  }

  const auto widgetRectForAnnotationRect = [&](const QRectF &logical) {
    return QRectF(editor.annotationPointToWidgetForTest(logical.topLeft()),
                  editor.annotationPointToWidgetForTest(logical.bottomRight()))
        .normalized();
  };
  const auto imageRectForWidgetRect = [&](const QRectF &widgetRect) {
    const qreal scaleX =
        selected.width() / static_cast<qreal>(std::max(1, editor.width()));
    const qreal scaleY =
        selected.height() / static_cast<qreal>(std::max(1, editor.height()));
    return QRect(qFloor(widgetRect.left() * scaleX),
                 qFloor(widgetRect.top() * scaleY),
                 std::max(1, qCeil(widgetRect.width() * scaleX)),
                 std::max(1, qCeil(widgetRect.height() * scaleY)))
        .intersected(selected.rect());
  };
  const auto differenceCount = [](const QImage &first, const QImage &second,
                                  const QRect &region) {
    int differences = 0;
    for (int y = region.top(); y <= region.bottom(); ++y) {
      for (int x = region.left(); x <= region.right(); ++x) {
        if (first.pixel(x, y) != second.pixel(x, y))
          ++differences;
      }
    }
    return differences;
  };
  const auto blueChromeCount = [](const QImage &image, const QRect &region) {
    int bluePixels = 0;
    for (int y = region.top(); y <= region.bottom(); ++y) {
      for (int x = region.left(); x <= region.right(); ++x) {
        const QColor pixel = image.pixelColor(x, y);
        if (pixel.blue() > 80 && pixel.blue() > pixel.red() + 50 &&
            pixel.blue() > pixel.green() + 30)
          ++bluePixels;
      }
    }
    return bluePixels;
  };
  const auto brightChromeCount = [](const QImage &image,
                                    const QRect &region) {
    int brightPixels = 0;
    for (int y = region.top(); y <= region.bottom(); ++y) {
      for (int x = region.left(); x <= region.right(); ++x) {
        const QColor pixel = image.pixelColor(x, y);
        if (pixel.red() > 220 && pixel.green() > 220 && pixel.blue() > 220)
          ++brightPixels;
      }
    }
    return brightPixels;
  };

  const QRectF canvas = editor.currentCanvasForTest();
  const QPointF outerEdgeTop =
      editor.annotationPointToWidgetForTest({canvas.right(), 40});
  const QPointF outerEdgeBottom =
      editor.annotationPointToWidgetForTest({canvas.right(), 180});
  const QRect outerCanvasEdge = imageRectForWidgetRect(
      QRectF(outerEdgeTop - QPointF(3, 0),
             outerEdgeBottom + QPointF(3, 0))
          .normalized());
  if (blueChromeCount(idle, outerCanvasEdge) == 0 ||
      blueChromeCount(selected, outerCanvasEdge) != 0) {
    error = QStringLiteral(
        "Ctrl+A did not remove the grown-canvas selection perimeter");
    return false;
  }

  // The old union's top edge crossed this empty gap. Ctrl+A must leave it
  // untouched while adding dashed chrome around each actual rectangle.
  const QRect groupOnly = imageRectForWidgetRect(
      widgetRectForAnnotationRect(QRectF(230, 94, 360, 6)));
  const QRect firstLayer = imageRectForWidgetRect(
      widgetRectForAnnotationRect(QRectF(55, 94, 110, 8)));
  const QRect secondLayer = imageRectForWidgetRect(
      widgetRectForAnnotationRect(QRectF(645, 244, 110, 8)));
  if (differenceCount(idle, selected, groupOnly) != 0 ||
      differenceCount(idle, selected, firstLayer) == 0 ||
      differenceCount(idle, selected, secondLayer) == 0) {
    error = QStringLiteral(
        "Multi-selection drew a union box instead of per-layer chrome");
    return false;
  }

  const QRectF sourceFrame = editor.sourceFrameWidgetRectForTest();
  const QRect sourceFrameEdge = imageRectForWidgetRect(
      QRectF(QPointF(sourceFrame.right() - 3, sourceFrame.top() + 40),
             QPointF(sourceFrame.right() + 3, sourceFrame.top() + 180)));
  if (blueChromeCount(idle, sourceFrameEdge) == 0 ||
      blueChromeCount(selected, sourceFrameEdge) != 0 ||
      differenceCount(idle, selected, sourceFrameEdge) == 0) {
    error = QStringLiteral("Ctrl+A left the source-frame border selected");
    return false;
  }
  const QRect cropHandle = imageRectForWidgetRect(
      QRectF(sourceFrame.right() + 1, sourceFrame.center().y() - 6, 12, 12));
  if (brightChromeCount(idle, cropHandle) == 0 ||
      brightChromeCount(selected, cropHandle) != 0 ||
      differenceCount(idle, selected, cropHandle) == 0) {
    error = QStringLiteral("Ctrl+A left the source crop handles selected");
    return false;
  }

  // Putting the group down restores the source-frame border and crop handles.
  QTest::mouseClick(
      &editor, Qt::LeftButton, Qt::NoModifier,
      editor.annotationPointToWidgetForTest({400, 50}).toPoint());
  application.processEvents();
  const QImage restored = editor.grab().toImage();
  if (editor.selectedCountForTest() != 0 ||
      blueChromeCount(restored, outerCanvasEdge) == 0 ||
      blueChromeCount(restored, sourceFrameEdge) == 0 ||
      brightChromeCount(restored, cropHandle) == 0) {
    error = QStringLiteral(
        "Putting layers down did not restore screenshot crop chrome");
    return false;
  }

  // A single selected layer must hide the same screenshot chrome while
  // retaining that layer's own bounds and handles.
  QTest::mouseClick(
      &editor, Qt::LeftButton, Qt::NoModifier,
      editor.annotationPointToWidgetForTest({60, 150}).toPoint());
  application.processEvents();
  const QImage single = editor.grab().toImage();
  if (editor.selectedCountForTest() != 1) {
    error = QStringLiteral("Single-layer chrome check did not select a layer");
    return false;
  }
  if (blueChromeCount(single, sourceFrameEdge) != 0) {
    error = QStringLiteral("Single selection left the source border visible");
    return false;
  }
  if (brightChromeCount(single, cropHandle) != 0) {
    error = QStringLiteral("Single selection left a crop handle visible");
    return false;
  }
  if (differenceCount(restored, single, firstLayer) == 0) {
    error = QStringLiteral("Single selection did not retain layer chrome");
    return false;
  }

  editor.close();
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

  // 600x400 selection shown 1:1 at widget offset (100, 117).
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
  drag(QPoint(120, 212), QPoint(220, 272));
  const Annotation left = rectangle({20, 95}, {120, 155});
  QTest::keyClick(&editor, Qt::Key_V);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(170, 212));
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
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(150, 462));
  application.processEvents();
  QTest::keyClick(&editor, Qt::Key_R);
  drag(QPoint(400, 262), QPoint(500, 312));
  const Annotation middle = rectangle({300, 145}, {400, 195});
  QTest::keyClick(&editor, Qt::Key_V);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(450, 262));
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
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(150, 462));
  application.processEvents();
  QTest::keyClick(&editor, Qt::Key_C);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(600, 212));
  QTest::keyClick(&editor, Qt::Key_V);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(600, 212));
  QTest::keyClick(&editor, Qt::Key_D, Qt::AltModifier);
  application.processEvents();
  const Annotation first = marker({491, 86}, 1);
  const Annotation second = marker({391, 186}, 2);
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
  // 600x400 selection shown 1:1 at widget offset (100, 117).
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
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(200, 212));
  QTest::mouseMove(&editor, QPoint(400, 312), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(400, 312));
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
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(300, 212));
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
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(500, 462));
  application.processEvents();
  QTest::keyClick(QApplication::focusWidget(), Qt::Key_Down);
  QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
  settle();
  if (!snapshotMatches(drawn)) {
    error = QStringLiteral("Arrow key inside the text editor nudged a layer");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_V);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(300, 212));
  application.processEvents();

  // Shift while dragging the end handle keeps the original 2:1 aspect ratio;
  // the axis scaled more (x: 200 -> 300) wins, so y follows to 150.
  QTest::mousePress(&editor, Qt::LeftButton, Qt::ShiftModifier,
                    QPoint(400, 312));
  QTest::mouseMove(&editor, QPoint(500, 332), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::ShiftModifier,
                      QPoint(500, 332));
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
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(400, 312));
  QTest::mouseMove(&editor, QPoint(500, 332), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(500, 332));
  application.processEvents();
  if (!snapshotMatches(rectangle({100, 95}, {400, 215}))) {
    error = QStringLiteral("Plain resize was constrained without Shift");
    return false;
  }

  // A held keyboard nudge refits immediately, before the coalesced patch is
  // persisted, so an edge crossing never clips and then pops into place.
  for (int step = 0; step < 21; ++step)
    QTest::keyClick(&editor, Qt::Key_Right, Qt::ShiftModifier);
  application.processEvents();
  const Annotation nudgedOutside = rectangle({310, 95}, {610, 215});
  if (editor.currentCanvasForTest().right() <= selection.width() ||
      !snapshotMatches(nudgedOutside)) {
    error = QStringLiteral("Edge-crossing nudge did not refit immediately");
    return false;
  }
  settle();
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (!snapshotMatches(rectangle({100, 95}, {400, 215})) ||
      editor.currentCanvasForTest() != QRectF(QPointF(), selection.size())) {
    error = QStringLiteral("Undo did not contract a nudged canvas");
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
  // 600x400 selection shown 1:1 at widget offset (100, 116).
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
  drag(QPoint(200, 211), QPoint(400, 311));
  QTest::keyClick(&editor, Qt::Key_V);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(200, 261));
  application.processEvents();

  // Its right edge midpoint is at widget (400,261): a side handle stretches
  // that axis and leaves the other alone, however far the pointer wanders.
  QTest::mouseMove(&editor, QPoint(400, 261), 10);
  application.processEvents();
  if (editor.cursor().shape() != Qt::SizeHorCursor) {
    error = QStringLiteral("A box offered no handle on its right edge");
    return false;
  }
  drag(QPoint(400, 261), QPoint(460, 331));
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
  drag(QPoint(460, 261), QPoint(150, 261));
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
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(200, 311));
  QTest::mouseMove(&editor, QPoint(500, 311), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(500, 311));
  application.processEvents();
  QTest::keyClick(&editor, Qt::Key_4);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(350, 211));
  QTest::mouseMove(&editor, QPoint(350, 411), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(350, 411));
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
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(220, 311));
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
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(620, 211));
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(620, 211));
  application.processEvents();
  const QPoint counterCenter(511, 86);
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
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(560, 211));
  QTest::mouseMove(&editor, QPoint(690, 211), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(690, 211));
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

/** The capture-kind tabs across the top: clicking Window and Region moves
 *  between the two modes Space toggles, and Fullscreen selects the monitor the
 *  way Ctrl+A does. */
bool runSelectTabsSmoke(QApplication &application, QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#182030")));
  capture.previewSize = capture.source.size();
  capture.windows = {{QRect(100, 100, 300, 200), QStringLiteral("w1"),
                      QStringLiteral("One"), QStringLiteral("firefox")}};

  CaptureEditor editor(capture);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();
  const auto clickOn = [&](CaptureEditor &target, const QString &label) {
    const QRectF tab = target.selectTabRectForTest(label);
    if (tab.isNull())
      return false;
    const QPoint at = tab.center().toPoint();
    QTest::mouseMove(&target, at);
    QTest::mouseClick(&target, Qt::LeftButton, Qt::NoModifier, at);
    application.processEvents();
    return true;
  };
  const auto click = [&](const QString &label) { return clickOn(editor, label); };
  if (editor.windowModeForTest()) {
    error = QStringLiteral("Select overlay did not start in region mode");
    return false;
  }
  if (!click(QStringLiteral("WINDOW")) || !editor.windowModeForTest()) {
    error = QStringLiteral("Window tab did not enter window mode");
    return false;
  }
  if (!click(QStringLiteral("REGION")) || editor.windowModeForTest()) {
    error = QStringLiteral("Region tab did not return to region mode");
    return false;
  }
  if (!click(QStringLiteral("FULLSCREEN")) || editor.selectingForTest() ||
      editor.renderCurrentOutput().size() != QSize(800, 600)) {
    error = QStringLiteral("Fullscreen tab did not select the whole monitor");
    return false;
  }
  // The strip stays in the edit phase as the way back; a tab there drops the
  // edit and returns to the select phase in that mode.
  if (!click(QStringLiteral("WINDOW")) || !editor.selectingForTest() ||
      !editor.windowModeForTest()) {
    error = QStringLiteral("Window tab from the editor did not return to "
                           "window selection");
    return false;
  }
  if (!click(QStringLiteral("FULLSCREEN")) || editor.selectingForTest())
    return false;
  if (!click(QStringLiteral("REGION")) || !editor.selectingForTest() ||
      editor.windowModeForTest() || editor.annotationCountForTest() != 0) {
    error = QStringLiteral("Region tab from the editor did not return to a "
                           "clean region selection");
    return false;
  }
  editor.close();

  // Scrolling Region is a mode of the same surface: drawing a region in it
  // brings the scroll panel up in place; its tabs leave it; dismissing it
  // returns to selecting in scroll mode.
  {
    CaptureEditor scrollEditor(capture, CaptureEditor::CaptureMode::Scroll);
    scrollEditor.resize(800, 600);
    scrollEditor.show();
    application.processEvents();
    if (!scrollEditor.scrollModeForTest() || !scrollEditor.selectingForTest()) {
      error = QStringLiteral("--scroll did not open selecting in scroll mode");
      return false;
    }
    QTest::mousePress(&scrollEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(100, 100));
    QTest::mouseMove(&scrollEditor, QPoint(500, 400), 20);
    QTest::mouseRelease(&scrollEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(500, 400));
    application.processEvents();
    if (!scrollEditor.scrollPanelActiveForTest() ||
        !scrollEditor.selectingForTest()) {
      error = QStringLiteral("A region in scroll mode did not bring the scroll "
                             "panel up");
      return false;
    }
    // Region and Scrolling Region frame the same rectangle, so switching
    // between them keeps it: the frame drawn for the scroll panel is the
    // region that gets captured.
    if (!clickOn(scrollEditor, QStringLiteral("REGION")) ||
        !scrollEditor.editingForTest() ||
        scrollEditor.renderCurrentOutput().size() != QSize(400, 300)) {
      error = QStringLiteral("Region tab did not carry the scroll frame over");
      return false;
    }
    // And back again: the region just captured frames the scroll panel.
    if (!clickOn(scrollEditor, QStringLiteral("SCROLLING REGION")) ||
        !scrollEditor.scrollPanelActiveForTest()) {
      error = QStringLiteral("Scrolling Region tab did not carry the region "
                             "over");
      return false;
    }
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
    application.processEvents();
    if (scrollEditor.scrollPanelActiveForTest() ||
        !scrollEditor.scrollModeForTest() || !scrollEditor.isVisible()) {
      error = QStringLiteral("Esc on the scroll panel did not return to "
                             "selecting a scrolling region");
      return false;
    }
    // Space walks Region -> Window -> Scroll -> Region. Starting in scroll,
    // the next step is Region (Fullscreen is skipped).
    QTest::keyClick(&scrollEditor, Qt::Key_Space);
    if (scrollEditor.windowModeForTest() || scrollEditor.scrollModeForTest()) {
      error = QStringLiteral("Space from scroll mode did not step to region");
      return false;
    }
    QTest::keyClick(&scrollEditor, Qt::Key_Space);
    if (!scrollEditor.windowModeForTest() || scrollEditor.scrollModeForTest()) {
      error = QStringLiteral("Space from region did not step to window");
      return false;
    }
    QTest::keyClick(&scrollEditor, Qt::Key_Space);
    if (!scrollEditor.scrollModeForTest()) {
      error = QStringLiteral("Space did not cycle back round to scroll mode");
      return false;
    }
    // A stitched result is handed to the same editor and annotates like any
    // capture: the whole image is the selection, a drawn layer renders on it,
    // and Esc then steps back rather than closing (the editor, not selecting).
    QImage tall(400, 1800, QImage::Format_ARGB32);
    tall.fill(QColor(QStringLiteral("#204060")));
    scrollEditor.adoptStitchedForTest(tall);
    application.processEvents();
    if (scrollEditor.selectingForTest() ||
        scrollEditor.renderCurrentOutput().size() != tall.size() ||
        scrollEditor.scrollPanelActiveForTest()) {
      error = QStringLiteral("Stitched image did not open in the editor whole");
      return false;
    }
    QTest::keyClick(&scrollEditor, Qt::Key_R);
    QTest::mousePress(&scrollEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(380, 200));
    QTest::mouseMove(&scrollEditor, QPoint(420, 300), 20);
    QTest::mouseRelease(&scrollEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(420, 300));
    application.processEvents();
    if (scrollEditor.annotationCountForTest() != 1) {
      error = QStringLiteral("Could not annotate the stitched image");
      return false;
    }
    QTest::keyClick(&scrollEditor, Qt::Key_Escape);
    application.processEvents();
    if (!scrollEditor.isVisible() || scrollEditor.selectingForTest()) {
      error = QStringLiteral("Esc in the editor closed or left it");
      return false;
    }
    // Back to selecting through the tab: a handed image is not the screen,
    // so the monitor is captured again and selection resumes in scroll mode.
    if (!clickOn(scrollEditor, QStringLiteral("SCROLLING REGION")) ||
        !scrollEditor.selectingForTest() || !scrollEditor.scrollModeForTest()) {
      error = QStringLiteral("Tab from a stitched edit did not return to "
                             "selecting a scrolling region");
      return false;
    }
    scrollEditor.close();
    application.processEvents();
  }

  // A file has no screen to go back to: no tabs in its editor.
  CaptureEditor fileEditor(capture, CaptureEditor::CaptureMode::File);
  fileEditor.resize(800, 600);
  fileEditor.show();
  application.processEvents();
  if (!fileEditor.selectTabRectForTest(QStringLiteral("REGION")).isNull()) {
    error = QStringLiteral("Tabs are offered when editing a file");
    return false;
  }
  fileEditor.close();
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
  // 600x400 selection shown 1:1 at widget offset (100, 117).
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
  // A filled shape has no stroke to thicken, so its wheel grows it about its
  // center instead of doing nothing visible.
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(650, 150));
  application.processEvents();
  QTest::keyClick(&editor, Qt::Key_R);
  QTest::keyClick(&editor, Qt::Key_R); // R again: fill on for the next shape
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(450, 350));
  QTest::mouseMove(&editor, QPoint(550, 420), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(550, 420));
  application.processEvents();
  QTest::keyClick(&editor, Qt::Key_V);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(500, 385));
  application.processEvents();
  {
    // Filled interior: (350,265)-(450,335) in annotation space. Just outside
    // the left edge is bare; two notches up must cover it, and two back down
    // must uncover it again.
    const QImage before = flushedSnapshot(editor, snapshotPath);
    if (!ink(before, 360, 300) || ink(before, 340, 300)) {
      error = QStringLiteral("Filled shape did not render where expected");
      return false;
    }
    wheel(2);
    const QImage grown = flushedSnapshot(editor, snapshotPath);
    if (!ink(grown, 340, 300)) {
      error = QStringLiteral("The wheel did not grow the filled shape");
      return false;
    }
    wheel(-2);
    const QImage back = flushedSnapshot(editor, snapshotPath);
    if (ink(back, 340, 300)) {
      error = QStringLiteral("The wheel did not shrink the filled shape back");
      return false;
    }
  }
  editor.close();
  QFile::remove(snapshotPath);
  return true;
}

/** Checks that R restores the last drawn region on the same monitor and
 *  ignores one stored for a different monitor. */
bool runAreaLastRegionSmoke(QApplication &application, QString &error) {
  const auto makeCapture = [](const QString &name) {
    CaptureData capture;
    capture.monitor.name = name;
    capture.monitor.geometry = {0, 0, 800, 600};
    capture.monitor.pixelSize = {800, 600};
    capture.monitor.scale = 1.0;
    capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
    capture.source.fill(QColor(QStringLiteral("#182030")));
    capture.previewSize = capture.source.size();
    return capture;
  };

  // Draw a region; committing it writes the session memory.
  {
    CaptureEditor editor(makeCapture(QStringLiteral("TEST")));
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(120, 140));
    QTest::mouseMove(&editor, QPoint(520, 380), 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(520, 380));
    application.processEvents();
    editor.close();
    application.processEvents();
  }

  // A fresh overlay on the same monitor at the same size restores it with R.
  {
    CaptureEditor editor(makeCapture(QStringLiteral("TEST")));
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    QTest::keyClick(&editor, Qt::Key_R);
    application.processEvents();
    if (!editor.statusForTest().contains(QStringLiteral("Last area restored"))) {
      error = QStringLiteral("R did not restore the last region");
      return false;
    }
    const QImage out = editor.renderCurrentOutput();
    if (out.size() != QSize(400, 240)) {
      error = QStringLiteral("Restored region was not the drawn 400x240");
      return false;
    }
    editor.close();
    application.processEvents();
  }

  // A different monitor ignores it: R stays in the select phase.
  {
    CaptureEditor editor(makeCapture(QStringLiteral("OTHER")));
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    QTest::keyClick(&editor, Qt::Key_R);
    application.processEvents();
    if (editor.statusForTest().contains(QStringLiteral("Last area restored"))) {
      error = QStringLiteral("R restored a region stored for another monitor");
      return false;
    }
    editor.close();
    application.processEvents();
  }
  return true;
}

int main(int argc, char **argv) {
  // Re-executed by the instance-lock checks as the process holding the lock.
  const QString heldLockPath =
      qEnvironmentVariable(kInstanceLockHolderVariable);
  if (!heldLockPath.isEmpty())
    return runInstanceLockHolder(heldLockPath);

  QApplication application(argc, argv);
  QApplication::setFont(chromeDefaultFont()); // as main() does
  if (!loadCaptureFonts())
    return 17;

  // Every finish() in this suite shelves a working document; keep those out
  // of the developer's own recents.
  QTemporaryDir smokeShelf;
  if (!smokeShelf.isValid())
    return 18;
  qputenv("OMASNAP_RECENT_DIR", smokeShelf.path().toUtf8());

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
  if (!runAreaLastRegionSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 119;
  }

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
  if (!runTextBandDetectionCheck(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 128;
  }
  if (!runChromeFontCheck(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 130;
  }
  if (!runTextAwareHighlighterEditorCheck(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 129;
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
  if (!runPointerDamageRegionCheck(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 135;
  }
  if (!runQuickOutputChecks(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 73;
  }
  if (!runScreenshotFilenameChecks(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 126;
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
  if (!runSelectTabsSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 127;
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
  if (!runZoomOutCheck(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 10;
  }
  if (!runTextOutlineCheck(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 34;
  }
  if (!runNativeCaretHiddenCheck(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 121;
  }
  if (!runDraftViewLockCheck(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 11;
  }
  if (!runTextClickAwayCommitCheck(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 87;
  }
  if (!runTextEnterSemanticsCheck(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 121;
  }
  if (!runAnnotationLayerChecks(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 86;
  }
  if (!runMarkerPointerOffsetSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 128;
  }
  if (!runContinuousAnnotationToolsSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 80;
  }
  if (!runCanvasBoundaryModeSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 202;
  }
  if (!runOffCanvasCreationSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 203;
  }
  if (!runSelectOutsideCanvasSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 106;
  }
  if (!runFullscreenBackdropCycleSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 142;
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
  if (!runTextFontSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 128;
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
  if (!runGrownCanvasSelectAllChromeSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 134;
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
  if (!runRecentsShelfSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 120;
  }
  if (!runShellQuoteCheck(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 83;
  }
  if (!runOpLogCapKeepsLeadingCrop(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 84;
  }
  if (!runCropKeepsAnnotationsAnchored(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 125;
  }
  const QString outputRoot =
      argc > 1 ? QString::fromLocal8Bit(argv[1])
               : QDir(QDir::tempPath())
                     .filePath(QStringLiteral("omasnap-native-smoke"));
  if (!runCutMappingSmoke(application, outputRoot, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 96;
  }
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
    const QPointF constraintOrigin =
        constraintEditor.editImageRectForTest().topLeft();
    const qreal constraintScale = constraintEditor.editScaleForTest();
    const QPoint dragPress =
        (constraintOrigin + QPointF(175, 100) * constraintScale).toPoint();
    const QPoint dragTo =
        (constraintOrigin + QPointF(295, 160) * constraintScale).toPoint();
    const auto dragRectangle = [&](bool releaseShiftBeforeMouse) {
      QTest::mousePress(&constraintEditor, Qt::LeftButton, Qt::NoModifier,
                        dragPress);
      QTest::mouseMove(&constraintEditor, dragTo, 20);
      QTest::keyPress(&constraintEditor, Qt::Key_Shift);
      application.processEvents();
      if (releaseShiftBeforeMouse)
        QTest::keyRelease(&constraintEditor, Qt::Key_Shift);
      QTest::mouseRelease(&constraintEditor, Qt::LeftButton, Qt::NoModifier,
                          dragTo);
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
    if (!quickEditor.exportingForTest() || quickEditor.editingForTest())
      return 74;
    quickEditor.waitForExport();
    const QStringList files =
        QDir(savedRoot).entryList({QStringLiteral("*.png")}, QDir::Files);
    if (quickEditor.isVisible() || files.size() != 1 ||
        QImage(QDir(savedRoot).filePath(files.constFirst())).isNull())
      return 75;
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
    const QRectF image = cropEditor.editImageRectForTest();
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
  QTest::keyClick(&editor, Qt::Key_Space); // Region -> Window
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
  QTest::keyClick(&editor, Qt::Key_Space); // Window -> Scroll
  QTest::keyClick(&editor, Qt::Key_Space); // Scroll -> Region
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
  QTest::mouseMove(
      &editor,
      editor.toolbarButtonCenterForTest(QStringLiteral("tool-text")), 10);
  application.processEvents();
  const QRectF textSizes = editor.textSizePanelRectForTest();
  QTest::mouseClick(
      &editor, Qt::LeftButton, Qt::NoModifier,
      QPoint(qRound(textSizes.left() + 17), qRound(textSizes.center().y())));
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
  // Palette toolbar button in the Style group.
  QTest::mouseMove(
      &editor,
      editor.toolbarButtonCenterForTest(QStringLiteral("palette")), 20);
  application.processEvents();
  if (!editor.grab().save(outputRoot + QStringLiteral("-palette.png"), "PNG"))
    return 14;
  // Custom color control in the open palette strip.
  QTest::mouseClick(
      &editor, Qt::LeftButton, Qt::NoModifier,
      editor.toolbarButtonCenterForTest(QStringLiteral("custom-color")));
  const QRectF customColor = editor.customColorPanelRectForTest();
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                    customColor.center().toPoint());
  // Freehand toolbar button.
  QTest::mouseMove(
      &editor,
      editor.toolbarButtonCenterForTest(QStringLiteral("tool-freehand")), 20);
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
    const QPointF redactOrigin =
        redactionEditor.editImageRectForTest().topLeft();
    const qreal redactScale = redactionEditor.editScaleForTest();
    const auto redactToScreen = [&](const QPointF &point) {
      return (redactOrigin + point * redactScale).toPoint();
    };
    const QImage beforeRedaction = flushedSnapshot(redactionEditor, snapshotPath);
    const QImage beforeRedactionUi = redactionEditor.grab().toImage();
    QTest::keyClick(&redactionEditor, Qt::Key_D);
    QTest::mousePress(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                      redactToScreen({175, 80}));
    QTest::mouseMove(&redactionEditor, redactToScreen({295, 80}), 20);
    QTest::mouseRelease(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                        redactToScreen({295, 80}));
    application.processEvents();
    if (flushedSnapshot(redactionEditor, snapshotPath) != beforeRedaction)
      return 72;
    QTest::mousePress(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                      redactToScreen({175, 80}));
    QTest::mouseMove(&redactionEditor, redactToScreen({295, 140}), 20);
    QTest::mouseRelease(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                        redactToScreen({295, 140}));
    application.processEvents();
    const QImage pixelatedRedactionUi = redactionEditor.grab().toImage();
    if (pixelatedRedactionUi == beforeRedactionUi)
      return 83;
    const QImage pixelatedRedaction = flushedSnapshot(redactionEditor, snapshotPath);
    if (pixelatedRedaction.isNull() || pixelatedRedaction == beforeRedaction)
      return 73;

    QTest::keyClick(&redactionEditor, Qt::Key_V);
    QTest::mouseClick(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(360, 242));
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
                      QPoint(360, 242));
    QTest::mouseMove(&redactionEditor, QPoint(380, 257), 20);
    QTest::mouseRelease(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(380, 257));
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
                      QPoint(300, 212));
    QTest::mouseMove(&redactionEditor, QPoint(280, 202), 20);
    QTest::mouseRelease(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(280, 202));
    application.processEvents();
    if (flushedSnapshot(redactionEditor, snapshotPath) == solidRedaction ||
        !redactionEditor.grab().save(
            outputRoot + QStringLiteral("-secure-redaction.png"), "PNG"))
      return 79;
    QTest::mousePress(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(280, 202));
    QTest::mouseMove(&redactionEditor, QPoint(420, 202), 20);
    QTest::mouseRelease(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(420, 202));
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
                      QPoint(300, 312));
    QTest::mouseMove(&overlapEditor, QPoint(500, 312), 20);
    QTest::mouseRelease(&overlapEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(500, 312));
    QTest::keyClick(&overlapEditor, Qt::Key_D);
    QTest::mousePress(&overlapEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(350, 282));
    QTest::mouseMove(&overlapEditor, QPoint(450, 342), 20);
    QTest::mouseRelease(&overlapEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(450, 342));
    QTest::keyClick(&overlapEditor, Qt::Key_V);
    QTest::mouseClick(&overlapEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(400, 312));
    application.processEvents();
    const QImage overlapUi = overlapEditor.grab().toImage();
    if (overlapUi.pixelColor(300, 312) != QColor(QStringLiteral("#0a84ff")) ||
        overlapUi.pixelColor(500, 312) != QColor(QStringLiteral("#0a84ff")) ||
        !overlapUi.save(outputRoot + QStringLiteral("-redaction-overlap.png"),
                        "PNG"))
      return 80;

    QTest::keyClick(&overlapEditor, Qt::Key_R);
    QTest::mousePress(&overlapEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(330, 262));
    QTest::mouseMove(&overlapEditor, QPoint(470, 362), 20);
    QTest::mouseRelease(&overlapEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(470, 362));
    QTest::keyClick(&overlapEditor, Qt::Key_V);
    QTest::mouseClick(&overlapEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(400, 297));
    application.processEvents();
    const QImage enclosedRedactionUi = overlapEditor.grab().toImage();
    if (enclosedRedactionUi.pixelColor(350, 282) !=
            QColor(QStringLiteral("#0a84ff")) ||
        enclosedRedactionUi.pixelColor(450, 342) !=
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
  QTest::mouseMove(&compactToolbarEditor, QPoint(20, 54), 20);
  application.processEvents();
  QTest::mouseMove(&compactToolbarEditor, QPoint(700, 54), 20);
  application.processEvents();
  const QImage compactToolbarUi = compactToolbarEditor.grab().toImage();
  if (compactToolbarUi.pixelColor(20, 54).alpha() < 240 ||
      compactToolbarUi.pixelColor(700, 54).alpha() < 240 ||
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
  // image selected, Select tool armed, annotation canvas showing the pixels.
  // (Not asserting the cursor shape here: with several editors from earlier
  // in this sequence never closed, a synthetic move can be delivered to an
  // older top-level window instead of this one, at the same offscreen
  // position, which armedToolForTest() does not depend on.)
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
  if (fileEditor.armedToolForTest() != CaptureEditor::Tool::Select ||
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
                    QPoint(260, 222));
  QTest::mouseMove(&cropEditor, QPoint(520, 277), 20);
  QTest::mouseRelease(&cropEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(520, 277));
  QTest::keyClick(&cropEditor, Qt::Key_V);
  QTest::mouseClick(&cropEditor, Qt::LeftButton, Qt::NoModifier,
                    QPoint(390, 238));
  // Layer selection owns the chrome until it is put down. Clicking empty
  // canvas restores the source crop handles before the crop begins.
  QTest::mouseClick(&cropEditor, Qt::LeftButton, Qt::NoModifier,
                    QPoint(600, 400));
  application.processEvents();
  const QImage beforeCrop = cropEditor.grab().toImage();
  const QRectF beforeCropFrame = cropEditor.sourceFrameWidgetRectForTest();
  if (!beforeCrop.save(outputRoot + QStringLiteral("-crop-handles.png"), "PNG"))
    return 31;
  QTest::mouseMove(&cropEditor, QPoint(682, 509), 20);
  QTest::mousePress(&cropEditor, Qt::LeftButton, Qt::NoModifier,
                    QPoint(682, 509));
  QTest::mouseMove(&cropEditor, QPoint(620, 452), 20);
  QTest::mouseRelease(&cropEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(620, 452));
  application.processEvents();
  const QImage afterCrop = cropEditor.grab().toImage();
  if (beforeCrop == afterCrop ||
      cropEditor.sourceFrameWidgetRectForTest() == beforeCropFrame ||
      !afterCrop.save(outputRoot + QStringLiteral("-cropped.png"), "PNG"))
    return 32;
  QTest::keyClick(&cropEditor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (cropEditor.sourceFrameWidgetRectForTest() != beforeCropFrame)
  if (cropEditor.grab().toImage().pixelColor(260, 222) !=
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
                    QPoint(640, 312));
  QTest::keyClick(&previewClipEditor, Qt::Key_B);
  QTest::mousePress(&previewClipEditor, Qt::LeftButton, Qt::NoModifier,
                    QPoint(682, 317));
  QTest::mouseMove(&previewClipEditor, QPoint(500, 317), 20);
  QTest::mouseRelease(&previewClipEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(500, 317));
  application.processEvents();
  const QImage clippedPreview = previewClipEditor.grab().toImage();
  for (int y = 282; y <= 342; ++y) {
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
  const QRectF grownCanvas =
      captureCanvasRect(QSizeF(100, 100), {croppedOut});
  const QImage grownExport =
      renderCapture(clippingCapture, QRectF(0, 0, 100, 100), {croppedOut},
                    BackgroundStyle::Aurora);
  const QPoint grownOrigin(qRound(-grownCanvas.left()),
                           qRound(-grownCanvas.top()));
  bool paintedOutsideSource = false;
  for (int y = 0; y < grownExport.height(); ++y) {
    for (int x = grownOrigin.x() + 100; x < grownExport.width(); ++x) {
      const QColor pixel = grownExport.pixelColor(x, y);
      if (pixel.red() > 240 && pixel.green() < 32 && pixel.blue() > 240)
        paintedOutsideSource = true;
    }
  }
  const QImage slateExport =
      renderCapture(clippingCapture, QRectF(0, 0, 100, 100), {croppedOut},
                    BackgroundStyle::None);
  const QImage unshadowedSlateExport =
      renderCapture(clippingCapture, QRectF(0, 0, 100, 100), {croppedOut},
                    BackgroundStyle::None, false);
  CaptureData highDpiGrowth = clippingCapture;
  highDpiGrowth.monitor.scale = 2.0;
  highDpiGrowth.monitor.pixelSize = {200, 200};
  highDpiGrowth.source = clippingCapture.source.scaled(
      200, 200, Qt::IgnoreAspectRatio, Qt::FastTransformation);
  const QImage highDpiGrowthExport =
      renderCapture(highDpiGrowth, QRectF(0, 0, 100, 100), {croppedOut},
                    BackgroundStyle::None);
  const QColor slate(QStringLiteral("#242424"));
  const auto isSlateShadow = [&](const QColor &pixel) {
    return pixel.alpha() == 255 && pixel.red() < slate.red() &&
           pixel.green() < slate.green() && pixel.blue() < slate.blue();
  };
  QImage shadowProbe(220, 220, QImage::Format_ARGB32_Premultiplied);
  shadowProbe.fill(slate);
  {
    QPainter shadowPainter(&shadowProbe);
    const QRectF sourceCard(60, 50, 100, 100);
    paintCaptureImageShadow(shadowPainter, sourceCard);
    shadowPainter.fillRect(sourceCard, Qt::white);
  }
  const std::array<int, 6> bottomShadowProfile = {
      shadowProbe.pixelColor(110, 150).red(),
      shadowProbe.pixelColor(110, 157).red(),
      shadowProbe.pixelColor(110, 170).red(),
      shadowProbe.pixelColor(110, 190).red(),
      shadowProbe.pixelColor(110, 202).red(),
      shadowProbe.pixelColor(110, 206).red()};
  const bool softAmbientAndKeyShadow =
      bottomShadowProfile.at(0) <= bottomShadowProfile.at(1) &&
      bottomShadowProfile.at(1) < bottomShadowProfile.at(2) &&
      bottomShadowProfile.at(2) < bottomShadowProfile.at(3) &&
      bottomShadowProfile.at(3) < bottomShadowProfile.at(4) &&
      bottomShadowProfile.at(4) < bottomShadowProfile.at(5) &&
      bottomShadowProfile.back() == slate.red() &&
      isSlateShadow(shadowProbe.pixelColor(110, 40)) &&
      shadowProbe.pixelColor(110, 20) == slate;
  const std::array<int, 5> slateShadowProfile = {
      slateExport.pixelColor(grownOrigin + QPoint(100, 90)).red(),
      slateExport.pixelColor(grownOrigin + QPoint(111, 90)).red(),
      slateExport.pixelColor(grownOrigin + QPoint(123, 90)).red(),
      slateExport.pixelColor(grownOrigin + QPoint(138, 90)).red(),
      slateExport.pixelColor(grownOrigin + QPoint(142, 90)).red()};
  const QPoint highDpiGrowthOrigin = grownOrigin * 2;
  const std::array<int, 5> highDpiShadowProfile = {
      highDpiGrowthExport.pixelColor(highDpiGrowthOrigin + QPoint(200, 180))
          .red(),
      highDpiGrowthExport.pixelColor(highDpiGrowthOrigin + QPoint(222, 180))
          .red(),
      highDpiGrowthExport.pixelColor(highDpiGrowthOrigin + QPoint(246, 180))
          .red(),
      highDpiGrowthExport.pixelColor(highDpiGrowthOrigin + QPoint(276, 180))
          .red(),
      highDpiGrowthExport.pixelColor(highDpiGrowthOrigin + QPoint(284, 180))
          .red()};
  bool scaleIndependentShadow = true;
  for (std::size_t i = 0; i < slateShadowProfile.size(); ++i) {
    scaleIndependentShadow =
        scaleIndependentShadow &&
        std::abs(slateShadowProfile.at(i) - highDpiShadowProfile.at(i)) <= 2;
  }
  const bool softlyFadingShadow =
      slateShadowProfile.at(0) < slateShadowProfile.at(1) &&
      slateShadowProfile.at(1) < slateShadowProfile.at(2) &&
      slateShadowProfile.at(2) < slateShadowProfile.at(3) &&
      slateShadowProfile.at(3) < slateShadowProfile.at(4) &&
      slateShadowProfile.back() == slate.red();
  const bool restrainedShadowStrength =
      bottomShadowProfile.front() >= 20 && slateShadowProfile.front() >= 20;
  if (grownCanvas != QRectF(-64, -64, 228, 228) ||
      grownExport.size() != QSize(228, 228) || !paintedOutsideSource ||
      grownExport.pixelColor(grownOrigin) != QColor(Qt::white) ||
      slateExport.size() != grownExport.size() ||
      slateExport.pixelColor(grownOrigin + QPoint(99, 90)) !=
          QColor(Qt::white) ||
      unshadowedSlateExport.size() != slateExport.size() ||
      unshadowedSlateExport.pixelColor(grownOrigin + QPoint(99, 90)) !=
          QColor(Qt::white) ||
      unshadowedSlateExport.pixelColor(grownOrigin + QPoint(100, 90)) !=
          slate ||
      unshadowedSlateExport.pixelColor(grownOrigin + QPoint(152, 90)) !=
          slate ||
      !softAmbientAndKeyShadow || !softlyFadingShadow ||
      !restrainedShadowStrength ||
      !scaleIndependentShadow ||
      slateExport.pixelColor(grownOrigin + QPoint(152, 90)) != slate ||
      highDpiGrowthExport.size() != QSize(456, 456) ||
      highDpiGrowthExport.pixelColor(highDpiGrowthOrigin + QPoint(199, 180)) !=
          QColor(Qt::white) ||
      !isSlateShadow(highDpiGrowthExport.pixelColor(highDpiGrowthOrigin +
                                                    QPoint(200, 180))) ||
      highDpiGrowthExport.pixelColor(highDpiGrowthOrigin +
                                     QPoint(304, 180)) != slate ||
      slateExport != renderCapture(clippingCapture, QRectF(0, 0, 100, 100),
                                   {croppedOut}, BackgroundStyle::None))
    return 65;

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
  QTest::mouseMove(&editor, QPoint(400, 312), 20);
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
                      QPoint(470, 312));
    application.processEvents();
    const QImage snapshotBeforeSave =
        flushedSnapshot(finishEditor, snapshotPath)
            .convertToFormat(QImage::Format_ARGB32);
    if (snapshotBeforeSave.isNull())
      return 59;
    QTest::keyClick(&finishEditor, Qt::Key_S, Qt::ControlModifier);
    finishEditor.waitForExport();
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
