#include "capture.hpp"
#include "capture-delay.hpp"
#include "cli-path.hpp"
#include "editor.hpp"
#include "instance-lock.hpp"
#include "overlay-chrome.hpp"
#include "pin.hpp"
#include "recent-snaps.hpp"
#include "startup-timing.hpp"

#include <LayerShellQt/Window>


#include <QImageReader>
#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QtGui/qguiapplication_platform.h>
#include <QLockFile>
#include <QScreen>
#include <QSocketNotifier>
#include <QTimer>
#include <QUrl>
#include <QWindow>

#include <csignal>
#include <optional>
#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-client.h>

namespace {
class PosixSignalNotifier final : public QObject {
public:
  explicit PosixSignalNotifier(QObject *parent = nullptr) : QObject(parent) {
    if (::socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                     fds_) != 0)
      return; // Default signal disposition stays in effect.
    signalFd_ = fds_[0];

    struct sigaction sa{};
    sa.sa_handler = [](int) {
      const int savedErrno = errno;
      const char byte = 1;
      const int fd = signalFd_;
      if (fd >= 0)
        static_cast<void>(::write(fd, &byte, sizeof(byte)));
      errno = savedErrno;
    };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigintInstalled_ = ::sigaction(SIGINT, &sa, &previousSigint_) == 0;
    sigtermInstalled_ = ::sigaction(SIGTERM, &sa, &previousSigterm_) == 0;
    if (!sigintInstalled_ && !sigtermInstalled_) {
      closeSockets();
      return;
    }

    notifier_ = new QSocketNotifier(fds_[1], QSocketNotifier::Read, this);
    connect(notifier_, &QSocketNotifier::activated, this, [this] {
      notified_ = true;
      notifier_->setEnabled(false);
      char bytes[32];
      while (::read(fds_[1], bytes, sizeof(bytes)) > 0) {
      }
      QCoreApplication::quit();
    });
  }

  ~PosixSignalNotifier() override {
    if (sigintInstalled_)
      ::sigaction(SIGINT, &previousSigint_, nullptr);
    if (sigtermInstalled_)
      ::sigaction(SIGTERM, &previousSigterm_, nullptr);
    closeSockets();
  }

  [[nodiscard]] bool wasNotified() const { return notified_; }

private:
  void closeSockets() {
    signalFd_ = -1;
    for (int &fd : fds_) {
      if (fd >= 0) {
        ::close(fd);
        fd = -1;
      }
    }
  }

  static inline int fds_[2]{-1, -1};
  static inline volatile sig_atomic_t signalFd_ = -1;
  struct sigaction previousSigint_{};
  struct sigaction previousSigterm_{};
  bool sigintInstalled_ = false;
  bool sigtermInstalled_ = false;
  bool notified_ = false;
  QSocketNotifier *notifier_ = nullptr;
};

QScreen *screenForMonitor(const MonitorInfo &monitor,
                          bool fallbackToPrimary = true) {
  for (QScreen *screen : QGuiApplication::screens()) {
    if (screen->name() == monitor.name)
      return screen;
  }
  return fallbackToPrimary ? QGuiApplication::primaryScreen() : nullptr;
}

enum class DelayRunResult { Completed, Cancelled, Failed };

DelayRunResult runCaptureDelay(QScreen *screen, int seconds,
                               CaptureDelayPosition position,
                               const PosixSignalNotifier &signalNotifier,
                               QString &error) {
  if (seconds <= 0)
    return DelayRunResult::Completed;
  if (signalNotifier.wasNotified())
    return DelayRunResult::Cancelled;
  if (!screen) {
    error = QStringLiteral("Could not find a screen for the delay countdown");
    return DelayRunResult::Failed;
  }

  CaptureDelayWidget countdown(seconds, position);
  countdown.setScreen(screen);
  static_cast<void>(countdown.winId());
  QWindow *handle = countdown.windowHandle();
  LayerShellQt::Window *layer =
      handle ? LayerShellQt::Window::get(handle) : nullptr;
  if (!handle || !layer) {
    error = QStringLiteral("Could not create delay countdown layer");
    return DelayRunResult::Failed;
  }

  layer->setScope(QStringLiteral("omasnap-delay"));
  layer->setScreen(screen);
  layer->setLayer(LayerShellQt::Window::LayerOverlay);
  LayerShellQt::Window::Anchors anchors;
  if (position == CaptureDelayPosition::TopLeft ||
      position == CaptureDelayPosition::TopRight)
    anchors.setFlag(LayerShellQt::Window::AnchorTop);
  else
    anchors.setFlag(LayerShellQt::Window::AnchorBottom);
  if (position == CaptureDelayPosition::TopLeft ||
      position == CaptureDelayPosition::BottomLeft)
    anchors.setFlag(LayerShellQt::Window::AnchorLeft);
  else
    anchors.setFlag(LayerShellQt::Window::AnchorRight);
  layer->setAnchors(anchors);
  layer->setMargins(QMargins(24, 24, 24, 24));
  layer->setExclusiveZone(0);
  layer->setDesiredSize(countdown.size());
  layer->setKeyboardInteractivity(
      LayerShellQt::Window::KeyboardInteractivityNone);
  layer->setActivateOnShow(false);

  QEventLoop delayLoop;
  QTimer watchdog;
  watchdog.setSingleShot(true);
  watchdog.setInterval(seconds * 1000 + 2000);
  bool completed = false;
  bool timedOut = false;
  bool screenRemoved = false;
  QObject::connect(&countdown, &CaptureDelayWidget::countdownFinished,
                   &delayLoop, [&] {
                     completed = true;
                     delayLoop.quit();
                   });
  QObject::connect(&watchdog, &QTimer::timeout, &delayLoop, [&] {
    timedOut = true;
    delayLoop.quit();
  });
  QObject::connect(qGuiApp, &QGuiApplication::screenRemoved, &delayLoop,
                   [&](QScreen *removed) {
                     if (removed == screen) {
                       screenRemoved = true;
                       delayLoop.quit();
                     }
                   });
  countdown.show();
  QTimer::singleShot(0, &countdown,
                     &CaptureDelayWidget::startCountdown);
  watchdog.start();
  if (signalNotifier.wasNotified())
    delayLoop.quit();
  else
    delayLoop.exec();
  watchdog.stop();
  // Completion, cancellation, timeout, and output removal all converge here.
  countdown.hide();
  countdown.destroySurface();
  QCoreApplication::processEvents();

  if (signalNotifier.wasNotified())
    return DelayRunResult::Cancelled;
  if (screenRemoved) {
    error = QStringLiteral("Capture monitor was disconnected during the delay");
    return DelayRunResult::Failed;
  }
  if (timedOut || !completed) {
    error = QStringLiteral("Delay countdown stopped unexpectedly");
    return DelayRunResult::Failed;
  }

  // Qt and ext-image-copy-capture use separate Wayland connections. A sync on
  // Qt's connection guarantees the compositor has processed destruction of
  // the countdown surface before the capture request is sent on the other one.
  auto *wayland =
      qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
  if (!wayland || !wayland->display() ||
      wl_display_roundtrip(wayland->display()) < 0) {
    error = QStringLiteral("Could not synchronize the countdown surface");
    return DelayRunResult::Failed;
  }
  return DelayRunResult::Completed;
}
} // namespace

int main(int argc, char **argv) {
  startupTimingMark("entered main");
  QCoreApplication::setApplicationName(QStringLiteral("omasnap"));
  QCoreApplication::setApplicationVersion(QString::fromLatin1(OMASNAP_VERSION));
  QCoreApplication::setOrganizationName(QStringLiteral("Omarchy"));
  qputenv("QT_WAYLAND_SHELL_INTEGRATION", "layer-shell");
  // Omarchy exports QT_QPA_PLATFORMTHEME=gtk3 session-wide. Honouring it
  // loads the qgtk3 plugin, which initialises GTK inside this process
  // (measured 81-112 ms of QApplication construction, plus ~20-24 MiB of
  // RSS) for a hand-painted overlay that opens no dialogs and reads no palette.
  // Qt's built-in generic theme is all it needs, so select it by name
  // (an empty value would let Qt pick a theme from XDG_CURRENT_DESKTOP
  // instead). The chrome font is pinned in chromeFont() rather than taken
  // from the theme. `-platformtheme gtk3` on the command line still
  // overrides this for debugging.
  qputenv("QT_QPA_PLATFORMTHEME", "generic");
  QGuiApplication::setDesktopFileName(QStringLiteral("omasnap"));
  QApplication application(argc, argv);
  startupTimingMark("QApplication constructed");
  // With the external desktop theme bypassed, Qt's default font would be
  // generic "Sans Serif 9"; pin what the theme used to install before any
  // widget is created, so painter/widget default-font text keeps its size and
  // face.
  QApplication::setFont(chromeDefaultFont());

  // A stitched scroll capture (or any tall pinned image) exceeds Qt's default
  // 256 MB image-decode allocation limit; lift it so --file/--pin can open it.
  QImageReader::setAllocationLimit(0);
  PosixSignalNotifier signalNotifier(&application);

  QCommandLineParser parser;
  parser.setApplicationDescription(QStringLiteral(
      "Native Wayland screenshot and annotation overlay for Hyprland and "
      "Omarchy.\n"
      "\n"
      "Only one capture overlay runs at a time. Starting omasnap again while "
      "an\noverlay is open dismisses it: the running instance is asked to "
      "quit and the\nnew process exits without capturing, so the same hotkey "
      "opens and closes the\noverlay. Quick output (--copy, --save) dismisses "
      "it the same way instead of\nscreenshotting the overlay. With --file (or "
      "an image path) or --clipboard, the running\ninstance is stopped and "
      "the editor opens on that image instead.\n"
      "\n"
      "Exit codes: 0 success, including dismissing a running overlay; 1 "
      "capture,\nimage, or single-instance lock failure; 2 usage error."));
  parser.addHelpOption();
  parser.addVersionOption();
  const QCommandLineOption fullscreenOption(
      QStringLiteral("capture-fullscreen"),
      QStringLiteral("Start with the entire focused monitor selected."));
  const QCommandLineOption windowOption(
      {QStringLiteral("capture-window"), QStringLiteral("capture-windows")},
      QStringLiteral("Start in window selection mode."));
  const QCommandLineOption regionOption(
      QStringLiteral("capture-region"),
      QStringLiteral("Start in freeform region selection mode (default)."));
  parser.addOption(fullscreenOption);
  parser.addOption(windowOption);
  parser.addOption(regionOption);
  const QCommandLineOption copyOption(
      QStringLiteral("copy"),
      QStringLiteral("Copy the capture directly without opening the editor."));
  const QCommandLineOption saveOption(
      QStringLiteral("save"),
      QStringLiteral("Save the capture directly without opening the editor."));
  parser.addOption(copyOption);
  parser.addOption(saveOption);
  const QCommandLineOption fileOption(
      QStringLiteral("file"),
      QStringLiteral("Open an existing image file in the annotation editor "
                     "instead of capturing the screen."),
      QStringLiteral("path"));
  parser.addOption(fileOption);
  const QCommandLineOption clipboardOption(
      QStringLiteral("clipboard"),
      QStringLiteral("Open the current clipboard image in the annotation "
                     "editor instead of capturing the screen."));
  parser.addOption(clipboardOption);
  const QCommandLineOption pinOption(
      QStringLiteral("pin"),
      QStringLiteral("Show an image as a pinned always-visible layer."),
      QStringLiteral("path"));
  parser.addOption(pinOption);
  const QCommandLineOption scrollOption(
      QStringLiteral("scroll"),
      QStringLiteral("Capture a scrolling region and stitch it into one tall "
                     "image, then open it in the editor."));
  parser.addOption(scrollOption);
  const QCommandLineOption delayOption(
      QStringLiteral("delay"),
      QStringLiteral("Wait 0-3600 seconds before capturing and show a "
                     "countdown."),
      QStringLiteral("seconds"));
  const QCommandLineOption delayPositionOption(
      QStringLiteral("delay-position"),
      QStringLiteral("Countdown corner: top-left, top-right, bottom-left, or "
                     "bottom-right (default: top-right)."),
      QStringLiteral("position"));
  parser.addOption(delayOption);
  parser.addOption(delayPositionOption);
  parser.addPositionalArgument(
      QStringLiteral("target"),
      QStringLiteral("Capture mode (smart, region, windows, fullscreen) or the "
                     "path of an image file to edit."),
      QStringLiteral("[target]"));
  parser.process(application);
  startupTimingMark("command line parsed");

  QString filePath = parser.value(fileOption);
  const bool clipboardInput = parser.isSet(clipboardOption);
  int delaySeconds = 0;
  CaptureDelayPosition delayPosition = CaptureDelayPosition::TopRight;
  QString optionError;
  if (parser.isSet(delayOption) &&
      !parseCaptureDelay(parser.value(delayOption), delaySeconds, optionError)) {
    qCritical().noquote() << optionError;
    return 2;
  }
  if (parser.isSet(delayPositionOption) && !parser.isSet(delayOption)) {
    qCritical() << "--delay-position requires --delay";
    return 2;
  }
  if (parser.isSet(delayPositionOption) &&
      !parseCaptureDelayPosition(parser.value(delayPositionOption),
                                 delayPosition, optionError)) {
    qCritical().noquote() << optionError;
    return 2;
  }

  QuickOutputMode quickOutputMode = QuickOutputMode::None;
  if (parser.isSet(copyOption) && parser.isSet(saveOption))
    quickOutputMode = QuickOutputMode::Both;
  else if (parser.isSet(copyOption))
    quickOutputMode = QuickOutputMode::Copy;
  else if (parser.isSet(saveOption))
    quickOutputMode = QuickOutputMode::Save;

  CaptureEditor::CaptureMode captureMode = CaptureEditor::CaptureMode::Region;
  int requestedModes = parser.isSet(fullscreenOption) +
                       parser.isSet(windowOption) + parser.isSet(regionOption) +
                       parser.isSet(scrollOption);
  if (parser.isSet(fullscreenOption))
    captureMode = CaptureEditor::CaptureMode::Fullscreen;
  else if (parser.isSet(windowOption))
    captureMode = CaptureEditor::CaptureMode::Window;
  else if (parser.isSet(scrollOption))
    captureMode = CaptureEditor::CaptureMode::Scroll;

  const QStringList positional = parser.positionalArguments();
  if (parser.isSet(pinOption)) {
    if (!filePath.isEmpty() || clipboardInput || requestedModes > 0 ||
        !positional.isEmpty() || quickOutputMode != QuickOutputMode::None ||
        parser.isSet(delayOption) || parser.isSet(delayPositionOption)) {
      qCritical()
          << "Pinned mode cannot be combined with capture or edit targets";
      return 2;
    }
    QString pinPath = QUrl(parser.value(pinOption)).toLocalFile();
    if (pinPath.isEmpty())
      pinPath = parser.value(pinOption);
    return runPinnedCapture(pinPath);
  }
  if (positional.size() > 1) {
    qCritical() << "Only one capture target may be specified";
    return 2;
  }
  if (!positional.isEmpty()) {
    const QString localTarget = resolveLocalImagePath(positional.first());
    if (filePath.isEmpty() && !localTarget.isEmpty()) {
      filePath = localTarget;
    } else {
      ++requestedModes;
      const QString mode = positional.first();
      if (mode == QStringLiteral("fullscreen"))
        captureMode = CaptureEditor::CaptureMode::Fullscreen;
      else if (mode == QStringLiteral("windows") ||
               mode == QStringLiteral("window"))
        captureMode = CaptureEditor::CaptureMode::Window;
      else if (mode == QStringLiteral("smart") ||
               mode == QStringLiteral("region"))
        captureMode = CaptureEditor::CaptureMode::Region;
      else if (mode == QStringLiteral("scroll"))
        captureMode = CaptureEditor::CaptureMode::Scroll;
      else {
        qCritical().noquote()
            << QStringLiteral("Unknown capture target: %1").arg(mode);
        return 2;
      }
    }
  }
  if (filePath.isEmpty() && requestedModes > 1) {
    qCritical() << "Capture mode options are mutually exclusive";
    return 2;
  }
  if (!filePath.isEmpty() && requestedModes > 0) {
    qCritical() << "An image file cannot be combined with a capture mode";
    return 2;
  }
  if (clipboardInput && (!filePath.isEmpty() || requestedModes > 0)) {
    qCritical() << "Clipboard input cannot be combined with another target";
    return 2;
  }
  const bool editingImage = clipboardInput || !filePath.isEmpty();
  if (editingImage && parser.isSet(delayOption)) {
    qCritical() << "Delay options cannot be combined with an image input";
    return 2;
  }
  if (editingImage && quickOutputMode != QuickOutputMode::None) {
    qCritical()
        << "Quick output options cannot be combined with an image input";
    return 2;
  }
  startupTimingMark("options resolved");
  if (!loadCaptureFonts())
    return 1;
  startupTimingMark("capture font loaded");
  application.setQuitOnLastWindowClosed(true);

  const QString runtime = secureRuntimeDirectory();
  startupTimingMark("runtime directory ready");
  if (runtime.isEmpty()) {
    qCritical() << "Could not create private runtime directory";
    return 1;
  }
  QLockFile instanceLock(
      QDir(runtime).filePath(QStringLiteral("omasnap.instance")));
  // Every capture, quick output included, dismisses a running overlay instead
  // of starting a second one: a late capture would otherwise photograph that overlay.
  // Editing an image always takes over so the requested editor can open.
  const InstanceLockResult lockResult = acquireInstanceLock(
      instanceLock, editingImage ? InstanceMode::EditFile
                                 : InstanceMode::Capture);
  startupTimingMark("instance lock acquired");
  if (lockResult.signalledPid != 0)
    qInfo().noquote() << QStringLiteral("Asked the running omasnap (pid %1) to "
                                        "quit")
                             .arg(lockResult.signalledPid);
  if (!lockResult.proceed) {
    if (!lockResult.error.isEmpty())
      qCritical().noquote() << lockResult.error;
    return lockResult.exitCode;
  }

  CaptureData capture;
  OperationLog restoredLog;
  QString error;
  if (editingImage) {
    QImage image;
    QString inputName;
    if (clipboardInput) {
      if (!loadClipboardImage(image, error)) {
        const QString message =
            QStringLiteral("Could not load clipboard image: %1").arg(error);
        qCritical().noquote() << message;
        sendCaptureNotification(message);
        return 1;
      }
      inputName = QStringLiteral("clipboard image");
    } else {
      QString localFile = QUrl(filePath).toLocalFile();
      if (localFile.isEmpty())
        localFile = filePath;
      image.load(localFile);
      if (image.isNull()) {
        qCritical().noquote()
            << QStringLiteral("Could not load image: %1").arg(filePath);
        return 1;
      }
      inputName = localFile;
      const QString sidecar = operationLogPath(localFile);
      if (QFile::exists(sidecar) &&
          !loadOperationLog(sidecar, restoredLog, error)) {
        qCritical().noquote()
            << QStringLiteral("Could not restore operation log: %1").arg(error);
        return 1;
      }
    }
    describeFileCapture(capture, image, restoredLog);
    captureMode = CaptureEditor::CaptureMode::File;
    qInfo().noquote() << QStringLiteral("Opened %1 for annotation (%2x%3)")
                             .arg(inputName)
                             .arg(image.width())
                             .arg(image.height());
  } else if (!probeFocusedMonitor(capture.monitor, error)) {
    qCritical().noquote() << error;
    sendCaptureNotification(QStringLiteral("Screenshot failed: %1").arg(error));
    return 1;
  }
  startupTimingMark(editingImage ? "input image prepared"
                                 : "focused monitor probed");

  if (!editingImage && delaySeconds > 0) {
    QScreen *countdownScreen = screenForMonitor(capture.monitor, false);
    // The launch-time focused output remains the capture target while the user
    // arranges windows; changing keyboard focus during the wait must not move
    // the promised capture to another monitor.
    const DelayRunResult delayResult =
        runCaptureDelay(countdownScreen, delaySeconds, delayPosition,
                        signalNotifier, error);
    if (delayResult == DelayRunResult::Cancelled)
      return 0;
    if (delayResult == DelayRunResult::Failed) {
      qCritical().noquote() << error;
      return 1;
    }
  }

  // Grab the output before the editor layer exists. The delay surface has
  // already unmapped and settled; ext-image-copy-capture would otherwise
  // photograph either overlay.
  const bool instantFullscreenOutput =
      !editingImage && captureMode == CaptureEditor::CaptureMode::Fullscreen &&
      quickOutputMode != QuickOutputMode::None;
  if (!editingImage &&
      !captureMonitorPixels(capture.monitor, capture,
                            !instantFullscreenOutput, error)) {
    qCritical().noquote() << error;
    sendCaptureNotification(QStringLiteral("Screenshot failed: %1").arg(error));
    return 1;
  }
  startupTimingMark(editingImage ? "pixel capture skipped"
                                 : "monitor pixels captured");

  if (instantFullscreenOutput) {
    QString outputError;
    const QSize expectedSize(
        qRound(capture.previewSize.width() * capture.monitor.scale),
        qRound(capture.previewSize.height() * capture.monitor.scale));
    const QImage output = capture.monitor.scale <= 1.0 ||
                                  capture.source.size() == expectedSize
                              ? capture.source
                              : renderCapture(capture,
                                              QRectF(QPointF(), capture.previewSize), {},
                                              BackgroundStyle::None);
    if (!quickOutput(output, quickOutputMode, outputError)) {
      qCritical().noquote() << outputError;
      return 1;
    }
    return 0;
  }

  QScreen *targetScreen = screenForMonitor(capture.monitor);
  if (!targetScreen) {
    qCritical() << "Could not find a screen for the capture overlay";
    return 1;
  }

  if (!editingImage) {
    qInfo().noquote() << QStringLiteral(
                             "Captured %1 workspace %2 with %3 selectable "
                             "windows")
                             .arg(capture.monitor.name)
                             .arg(capture.monitor.workspaceId)
                             .arg(capture.windows.size());
  }

  CaptureEditor editor(std::move(capture), captureMode, quickOutputMode,
                       restoredLog);
  startupTimingMark("CaptureEditor constructed");
  editor.setScreen(targetScreen);
  editor.setGeometry(targetScreen->geometry());
  editor.winId();
  QWindow *window = editor.windowHandle();
  LayerShellQt::Window *layerWindow = LayerShellQt::Window::get(window);
  if (!window || !layerWindow) {
    qCritical() << "Could not create capture overlay layer";
    return 1;
  }
  layerWindow->setScope(QStringLiteral("omasnap"));
  layerWindow->setScreen(targetScreen);
  layerWindow->setLayer(LayerShellQt::Window::LayerOverlay);
  LayerShellQt::Window::Anchors anchors;
  anchors.setFlag(LayerShellQt::Window::AnchorTop);
  anchors.setFlag(LayerShellQt::Window::AnchorBottom);
  anchors.setFlag(LayerShellQt::Window::AnchorLeft);
  anchors.setFlag(LayerShellQt::Window::AnchorRight);
  layerWindow->setAnchors(anchors);
  layerWindow->setExclusiveZone(-1);
  layerWindow->setKeyboardInteractivity(
      LayerShellQt::Window::KeyboardInteractivityExclusive);
  layerWindow->setActivateOnShow(true);
  editor.setLayerWindow(layerWindow);
  startupTimingMark("layer surface configured");
  editor.show();
  editor.setFocus(Qt::ActiveWindowFocusReason);
  startupTimingMark("show requested; entering event loop");

  return application.exec();
}
