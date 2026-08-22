#include "capture.hpp"
#include "cli-path.hpp"
#include "editor.hpp"
#include "instance-lock.hpp"
#include "pin.hpp"

#include <LayerShellQt/Window>

#include "scroll-capture.hpp"

#include <QImageReader>
#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QLockFile>
#include <QScreen>
#include <QSocketNotifier>
#include <QUrl>
#include <QWindow>

#include <csignal>
#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>

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
  QSocketNotifier *notifier_ = nullptr;
};
} // namespace

int main(int argc, char **argv) {
  QCoreApplication::setApplicationName(QStringLiteral("omasnap"));
  QCoreApplication::setApplicationVersion(QString::fromLatin1(OMASNAP_VERSION));
  QCoreApplication::setOrganizationName(QStringLiteral("Omarchy"));
  qputenv("QT_WAYLAND_SHELL_INTEGRATION", "layer-shell");
  QGuiApplication::setDesktopFileName(QStringLiteral("omasnap"));
  QApplication application(argc, argv);

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
  parser.addPositionalArgument(
      QStringLiteral("target"),
      QStringLiteral("Capture mode (smart, region, windows, fullscreen) or the "
                     "path of an image file to edit."),
      QStringLiteral("[target]"));
  parser.process(application);

  QString filePath = parser.value(fileOption);
  const bool clipboardInput = parser.isSet(clipboardOption);

  QuickOutputMode quickOutputMode = QuickOutputMode::None;
  if (parser.isSet(copyOption) && parser.isSet(saveOption))
    quickOutputMode = QuickOutputMode::Both;
  else if (parser.isSet(copyOption))
    quickOutputMode = QuickOutputMode::Copy;
  else if (parser.isSet(saveOption))
    quickOutputMode = QuickOutputMode::Save;

  CaptureEditor::CaptureMode captureMode = CaptureEditor::CaptureMode::Region;
  int requestedModes = parser.isSet(fullscreenOption) +
                       parser.isSet(windowOption) + parser.isSet(regionOption);
  if (parser.isSet(fullscreenOption))
    captureMode = CaptureEditor::CaptureMode::Fullscreen;
  else if (parser.isSet(windowOption))
    captureMode = CaptureEditor::CaptureMode::Window;

  const QStringList positional = parser.positionalArguments();
  if (parser.isSet(pinOption)) {
    if (!filePath.isEmpty() || clipboardInput || requestedModes > 0 ||
        !positional.isEmpty() || quickOutputMode != QuickOutputMode::None) {
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
  if (editingImage && quickOutputMode != QuickOutputMode::None) {
    qCritical()
        << "Quick output options cannot be combined with an image input";
    return 2;
  }
  if (!loadCaptureFonts())
    return 1;
  application.setQuitOnLastWindowClosed(true);

  const QString runtime = secureRuntimeDirectory();
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
  bool scrollRequested = parser.isSet(scrollOption);
  if (scrollRequested) {
    // Learn the focused monitor (name and scale) for the output-capture
    // session, run the overlay, then edit the stitched result. Leaving the
    // overlay with A asks for an ordinary area capture instead, so the two
    // kinds swap without anyone having to close one and launch the other.
    CaptureData probe;
    if (!captureFocusedMonitor(probe, false, error)) {
      qCritical().noquote() << error;
      return 1;
    }
    bool switchedToArea = false;
    const QImage stitched =
        runScrollCapture(probe.monitor, error, &switchedToArea);
    if (switchedToArea) {
      scrollRequested = false;
      captureMode = CaptureEditor::CaptureMode::Region;
    } else if (stitched.isNull()) {
      if (!error.isEmpty()) {
        qCritical().noquote() << error;
        return 1;
      }
      return 0; // cancelled
    } else {
      capture.source = stitched;
      capture.previewSize = stitched.size();
      capture.monitor.scale = 1.0;
      capture.monitor.pixelSize = stitched.size();
      capture.monitor.geometry = QRect(QPoint(0, 0), stitched.size());
      capture.monitor.name = probe.monitor.name;
      captureMode = CaptureEditor::CaptureMode::File;
    }
  }
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
    capture.source = image;
    capture.previewSize = image.size();
    capture.monitor.scale = 1.0;
    capture.monitor.pixelSize = image.size();
    capture.monitor.geometry = QRect(QPoint(0, 0), image.size());
    captureMode = CaptureEditor::CaptureMode::File;
    qInfo().noquote() << QStringLiteral("Opened %1 for annotation (%2x%3)")
                             .arg(inputName)
                             .arg(image.width())
                             .arg(image.height());
  } else if (!scrollRequested &&
             !probeFocusedMonitor(capture.monitor, error)) {
    qCritical().noquote() << error;
    sendCaptureNotification(QStringLiteral("Screenshot failed: %1").arg(error));
    return 1;
  }

  // Grab the output before the layer exists. ext-image-copy-capture waits for
  // a composited frame, so mapping the dim overlay first photographs the veil.
  const bool instantFullscreenOutput =
      !editingImage && captureMode == CaptureEditor::CaptureMode::Fullscreen &&
      quickOutputMode != QuickOutputMode::None;
  if (!editingImage && !scrollRequested &&
      !captureMonitorPixels(capture.monitor, capture,
                            !instantFullscreenOutput, error)) {
    qCritical().noquote() << error;
    sendCaptureNotification(QStringLiteral("Screenshot failed: %1").arg(error));
    return 1;
  }

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

  QScreen *targetScreen = QGuiApplication::primaryScreen();
  for (QScreen *screen : QGuiApplication::screens()) {
    if (screen->name() == capture.monitor.name) {
      targetScreen = screen;
      break;
    }
  }

  if (!editingImage) {
    qInfo().noquote() << QStringLiteral(
                             "Captured %1 workspace %2 with %3 selectable "
                             "windows")
                             .arg(capture.monitor.name)
                             .arg(capture.monitor.workspaceId)
                             .arg(capture.windows.size());
  }

  // Area capture and scroll capture hand back and forth with one key, so the
  // session is a loop rather than a sequence: each overlay runs to completion,
  // says whether it handed over, and the other one takes it from there. As
  // many times as anyone likes.
  for (;;) {
    CaptureEditor editor(std::move(capture), captureMode, quickOutputMode,
                         restoredLog);
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
    editor.show();
    editor.setFocus(Qt::ActiveWindowFocusReason);

    const int status = application.exec();
    if (!editor.switchedToScroll())
      return status;

    // S handed this over to scroll capture. Run it on the same monitor; A
    // there hands it straight back, which is why this is a loop. Whatever op
    // log a file edit restored belongs to that file, not to the next capture.
    restoredLog = OperationLog();
    CaptureData probe;
    if (!captureFocusedMonitor(probe, false, error)) {
      qCritical().noquote() << error;
      return 1;
    }
    bool backToArea = false;
    const QImage stitched = runScrollCapture(probe.monitor, error, &backToArea);
    if (backToArea) {
      // Handed straight back: the loop puts the area overlay up again, and A
      // and S can keep passing it between them for as long as anyone likes.
      capture = CaptureData();
      if (!captureMonitorPixels(probe.monitor, capture, true, error)) {
        qCritical().noquote() << error;
        return 1;
      }
      captureMode = CaptureEditor::CaptureMode::Region;
      continue;
    }
    if (stitched.isNull()) {
      if (!error.isEmpty()) {
        qCritical().noquote() << error;
        return 1;
      }
      return 0; // cancelled
    }
    capture = CaptureData();
    capture.source = stitched;
    capture.previewSize = stitched.size();
    capture.monitor.scale = 1.0;
    capture.monitor.pixelSize = stitched.size();
    capture.monitor.geometry = QRect(QPoint(0, 0), stitched.size());
    capture.monitor.name = probe.monitor.name;
    captureMode = CaptureEditor::CaptureMode::File;
  }
}
