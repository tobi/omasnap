#include "capture.hpp"
#include "cli-path.hpp"
#include "editor.hpp"
#include "instance-lock.hpp"
#include "output-config.hpp"
#include "overlay-chrome.hpp"
#include "pin.hpp"
#include "recent-snaps.hpp"
#include "startup-timing.hpp"

#include <LayerShellQt/Window>


#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>
#include <QApplication>

#include <algorithm>
#include <memory>
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
#include <optional>
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
  startupTimingMark("entered main");
  QCoreApplication::setApplicationName(QStringLiteral("omasnap"));
  QCoreApplication::setApplicationVersion(QString::fromLatin1(OMASNAP_VERSION));
  QCoreApplication::setOrganizationName(QStringLiteral("Omarchy"));
  // The overlay is a layer surface, but a windowed editor is an ordinary
  // compositor window the compositor tiles and floats; the shell
  // integration must be chosen before Qt connects, so the decision reads
  // the arguments and the config by hand. Only a file edit can be
  // windowed: a fresh capture always selects on the fullscreen overlay and
  // hands off afterward.
  bool editorWindowArg = false;
  bool editorOverlayArg = false;
  bool fileEditArg = false;
  for (int index = 1; index < argc; ++index) {
    const char *arg = argv[index];
    if (qstrcmp(arg, "--editor") == 0 && index + 1 < argc) {
      editorWindowArg = editorWindowArg || qstrcmp(argv[index + 1], "window") == 0;
      editorOverlayArg =
          editorOverlayArg || qstrcmp(argv[index + 1], "overlay") == 0;
      ++index;
    } else if (qstrcmp(arg, "--pin") == 0) {
      ++index;
    } else if (qstrcmp(arg, "--file") == 0) {
      fileEditArg = true;
      ++index;
    } else if (qstrcmp(arg, "--clipboard") == 0) {
      fileEditArg = true;
    } else if (arg[0] != '-') {
      fileEditArg =
          fileEditArg || (qstrcmp(arg, "smart") != 0 &&
                          qstrcmp(arg, "region") != 0 &&
                          qstrcmp(arg, "windows") != 0 &&
                          qstrcmp(arg, "fullscreen") != 0);
    }
  }
  const bool windowedEditorProcess =
      !editorOverlayArg && fileEditArg &&
      (editorWindowArg || loadEditorWindowMode(defaultConfigPath()));
  if (windowedEditorProcess) {
    // Unset rather than merely not set: a windowed editor spawned from the
    // overlay inherits the overlay's environment, layer-shell included.
    qunsetenv("QT_WAYLAND_SHELL_INTEGRATION");
  } else {
    qputenv("QT_WAYLAND_SHELL_INTEGRATION", "layer-shell");
  }
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
  const QCommandLineOption editorOption(
      QStringLiteral("editor"),
      QStringLiteral("Editor presentation: overlay (fullscreen, default) or "
                     "window (a normal compositor window). Also configurable "
                     "as [editor] mode in omasnap.conf; W switches a live "
                     "editor between the two."),
      QStringLiteral("mode"));
  parser.addOption(editorOption);
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
  startupTimingMark("command line parsed");

  QString filePath = parser.value(fileOption);
  const bool clipboardInput = parser.isSet(clipboardOption);

  const QString editorModeArg = parser.value(editorOption).trimmed().toLower();
  if (!editorModeArg.isEmpty() &&
      editorModeArg != QStringLiteral("window") &&
      editorModeArg != QStringLiteral("overlay")) {
    qCritical() << "--editor takes window or overlay";
    return 2;
  }
  const bool editorWindowMode =
      editorModeArg == QStringLiteral("window") ||
      (editorModeArg.isEmpty() && loadEditorWindowMode(defaultConfigPath()));

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

  // Grab the output before the layer exists. ext-image-copy-capture waits for
  // a composited frame, so mapping the dim overlay first photographs the veil.
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

  const QSize editingPreview = capture.previewSize;
  CaptureEditor editor(std::move(capture), captureMode, quickOutputMode,
                       restoredLog);
  startupTimingMark("CaptureEditor constructed");
  editor.setScreen(targetScreen);
  if (windowedEditorProcess && editingImage) {
    // An ordinary compositor window: the compositor manages it, and its own
    // float toggle works either way. The overlay chrome carries over
    // unchanged; only the surface role differs.
    editor.setWindowedPresentation(true);
    editor.setWindowedBackdropOpaque(
        loadEditorWindowBackdropOpaque(defaultConfigPath()));
    editor.setWindowTitle(
        filePath.isEmpty() ? QStringLiteral("omasnap")
                           : QStringLiteral("omasnap %1")
                                 .arg(QFileInfo(filePath).fileName()));
    // Size to the visible selection, not the pristine canvas: a handed-off
    // capture keeps its whole monitor underneath, but the window should hug
    // what is actually being annotated.
    const QSizeF selectionSize = editor.currentSelection().size();
    const QSize hugged =
        selectionSize.isEmpty() ? editingPreview : selectionSize.toSize();
    // The guide band's height depends on how wide the card may be, so
    // measure it at the width this window will have.
    const int legendHeight =
        hotkeyLegendAnchoredSize(editorHotkeyEntries(),
                                 std::max(392, hugged.width() + 100))
            .height();
    const QSize naturalSize =
        editorWindowSize(hugged, targetScreen->availableGeometry().size(),
                         legendHeight);
    // A hard floor clamps interactive floating resizes where the toolbar
    // still reads; a tiled window's compositor overrides the hint, and
    // that path is accepted as the tiled look.
    editor.setMinimumSize(640, 420);
    editor.resize(naturalSize);
    // Both rules registered before the window maps, so the compositor
    // floats, centers, and keeps it opaque from the first frame instead of
    // tiling briefly and popping out.
    const bool floatingWindow = loadEditorWindowFloating(defaultConfigPath());
    QProcess::execute(QStringLiteral("hyprctl"),
                      {QStringLiteral("eval"),
                       editorFloatRuleScript(floatingWindow)});
    // Exempt from the desktop's window-opacity rules: an editor whose mat
    // and capture ghost translucent when unfocused reads as broken.
    QProcess::execute(
        QStringLiteral("hyprctl"),
        {QStringLiteral("eval"),
         QStringLiteral("hl.window_rule({ name = \"omasnap-editor-opaque\", "
                        "match = { title = \"^omasnap( .+)?$\" }, "
                        "opacity = 1 })")});
    editor.show();
    editor.setFocus(Qt::ActiveWindowFocusReason);
    if (floatingWindow) {
      // Floating at the capture's natural size unless the config says
      // tiled. Dispatched once the compositor lists the window: asked too
      // early, a dispatch reports success and does nothing.
      auto attempts = std::make_shared<int>(0);
      QTimer *settle = new QTimer(&editor);
      settle->setInterval(50);
      QObject::connect(settle, &QTimer::timeout, &editor, [settle, attempts,
                                                           naturalSize] {
        ++*attempts;
        const qint64 pid = QCoreApplication::applicationPid();
        QProcess probe;
        probe.start(QStringLiteral("hyprctl"),
                    {QStringLiteral("-j"), QStringLiteral("clients")});
        const bool hyprland = probe.waitForFinished(500);
        const QByteArray clients = probe.readAllStandardOutput();
        bool listed = false;
        bool alreadyFloating = false;
        if (hyprland) {
          const QJsonArray parsed = QJsonDocument::fromJson(clients).array();
          for (const QJsonValue &value : parsed) {
            const QJsonObject client = value.toObject();
            if (client.value(QStringLiteral("pid")).toInteger() == pid) {
              listed = true;
              alreadyFloating =
                  client.value(QStringLiteral("floating")).toBool();
              break;
            }
          }
        }
        if (listed) {
          settle->stop();
          // The map rule normally floats and centers the window already;
          // these dispatches are the fallback for a compositor that
          // ignored it. hl.dsp.window.float toggles, so a window the rule
          // floated must not be dispatched back into the tiling.
          if (alreadyFloating)
            return;
          const QString selector =
              QStringLiteral("window = \"pid:%1\"").arg(pid);
          QProcess::execute(
              QStringLiteral("hyprctl"),
              {QStringLiteral("dispatch"),
               QStringLiteral("hl.dsp.window.float({ %1 })").arg(selector)});
          QProcess::execute(
              QStringLiteral("hyprctl"),
              {QStringLiteral("dispatch"),
               QStringLiteral(
                   "hl.dsp.window.resize({ x = %1, y = %2, relative = "
                   "false, %3 })")
                   .arg(naturalSize.width())
                   .arg(naturalSize.height())
                   .arg(selector)});
          // Centered in the workspace area, clear of bars, or a tall
          // window's toolbar ends up under the top bar.
          QProcess::execute(
              QStringLiteral("hyprctl"),
              {QStringLiteral("dispatch"),
               QStringLiteral("hl.dsp.window.center({ %1 })").arg(selector)});
          return;
        }
        if (!hyprland && qEnvironmentVariableIsSet("SWAYSOCK")) {
          settle->stop();
          QProcess::execute(
              QStringLiteral("swaymsg"),
              {QStringLiteral("[pid=%1] floating enable, resize set %2 %3, "
                              "move position center")
                   .arg(pid)
                   .arg(naturalSize.width())
                   .arg(naturalSize.height())});
          return;
        }
        if (*attempts >= 10)
          settle->stop();
      });
      settle->start();
    }
    return application.exec();
  }
  if (editorWindowMode && !editingImage)
    editor.setWindowedHandoffOnEdit(captureMode !=
                                    CaptureEditor::CaptureMode::Scroll);
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
