/** @fileoverview Captures monitors and windows on KDE Plasma through KWin.
 *
 * KWin does not expose the wlr or ext capture protocols to ordinary clients,
 * so this backend uses the interfaces Plasma sanctions for screenshot tools:
 * the org.kde.KWin.ScreenShot2 DBus service for pixels (authorized through
 * the installed desktop entry's X-KDE-DBUS-Restricted-Interfaces key) and a
 * transient KWin script for window discovery.
 */
#include "kwin.hpp"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScreen>
#include <QTimer>

#include <algorithm>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace {
constexpr int kDBusTimeoutMs = 3000;
constexpr int kCaptureTimeoutMs = 10000;

const QString kKWinService = QStringLiteral("org.kde.KWin");

/** Receives the window list a transient KWin script reports back over DBus. */
class KWinWindowSink final : public QObject {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.omasnap.WindowSink")
public:
  QByteArray payload;
  bool received = false;
  QEventLoop *loop = nullptr;

public Q_SLOTS:
  Q_SCRIPTABLE void windows(const QString &json) {
    payload = json.toUtf8();
    received = true;
    if (loop)
      loop->quit();
  }
};

QString activeOutputName(QString &error) {
  QDBusMessage message = QDBusMessage::createMethodCall(
      kKWinService, QStringLiteral("/KWin"), QStringLiteral("org.kde.KWin"),
      QStringLiteral("activeOutputName"));
  const QDBusReply<QString> reply =
      QDBusConnection::sessionBus().call(message, QDBus::Block, kDBusTimeoutMs);
  if (!reply.isValid() || reply.value().isEmpty()) {
    error = QStringLiteral("KWin did not report a focused monitor: %1")
                .arg(reply.error().message());
    return {};
  }
  return reply.value();
}

/** Runs one ScreenShot2 method and reads the raw image from a pipe. */
bool screenshot2Capture(const QString &method, const QString &target,
                        QVariantMap options, QImage &image, qreal &scale,
                        QString &error) {
  int fds[2];
  if (::pipe2(fds, O_CLOEXEC) != 0) {
    error = QStringLiteral("Could not create screenshot pipe");
    return false;
  }

  options.insert(QStringLiteral("native-resolution"), true);
  QDBusMessage message = QDBusMessage::createMethodCall(
      kKWinService, QStringLiteral("/org/kde/KWin/ScreenShot2"),
      QStringLiteral("org.kde.KWin.ScreenShot2"), method);
  message << target << options
          << QVariant::fromValue(QDBusUnixFileDescriptor(fds[1]));
  QDBusPendingCallWatcher watcher(
      QDBusConnection::sessionBus().asyncCall(message, kCaptureTimeoutMs));
  ::close(fds[1]);

  QEventLoop loop;
  QObject::connect(&watcher, &QDBusPendingCallWatcher::finished, &loop,
                   &QEventLoop::quit);
  QTimer::singleShot(kCaptureTimeoutMs, &loop, &QEventLoop::quit);
  loop.exec();
  if (!watcher.isFinished()) {
    ::close(fds[0]);
    error = QStringLiteral("KWin did not answer the screenshot request");
    return false;
  }
  const QDBusPendingReply<QVariantMap> reply = watcher;
  if (reply.isError()) {
    ::close(fds[0]);
    if (reply.error().name() ==
        QStringLiteral("org.kde.KWin.ScreenShot2.Error.NoAuthorized"))
      error = QStringLiteral(
          "KWin denied screenshot access; run install-kde so the omasnap "
          "desktop entry authorizes org.kde.KWin.ScreenShot2");
    else
      error = reply.error().message();
    return false;
  }

  const QVariantMap attributes = reply.value();
  const int width = attributes.value(QStringLiteral("width")).toInt();
  const int height = attributes.value(QStringLiteral("height")).toInt();
  const int stride = attributes.value(QStringLiteral("stride")).toInt();
  const auto format = static_cast<QImage::Format>(
      attributes.value(QStringLiteral("format")).toUInt());
  scale = attributes.value(QStringLiteral("scale")).toReal();
  if (scale <= 0.0)
    scale = 1.0;
  const qint64 total = static_cast<qint64>(stride) * height;
  if (width <= 0 || height <= 0 || stride <= 0 ||
      total > qint64(1) << 29 || format <= QImage::Format_Invalid ||
      format >= QImage::NImageFormats) {
    ::close(fds[0]);
    error = QStringLiteral("KWin returned an unusable screenshot format");
    return false;
  }

  QByteArray bytes;
  bytes.resize(total);
  qsizetype received = 0;
  while (received < bytes.size()) {
    struct pollfd poller{fds[0], POLLIN, 0};
    if (::poll(&poller, 1, kCaptureTimeoutMs) <= 0)
      break;
    const ssize_t count = ::read(fds[0], bytes.data() + received,
                                 static_cast<size_t>(bytes.size() - received));
    if (count <= 0)
      break;
    received += count;
  }
  ::close(fds[0]);
  if (received < bytes.size()) {
    error = QStringLiteral("KWin closed the screenshot stream early");
    return false;
  }

  image = QImage(reinterpret_cast<const uchar *>(bytes.constData()), width,
                 height, stride, format)
              .copy();
  return !image.isNull();
}

/** Asks a transient KWin script for the stacking-ordered window list. */
QByteArray listWindowsJson() {
  const QString runtime = secureRuntimeDirectory();
  if (runtime.isEmpty())
    return {};

  QDBusConnection bus = QDBusConnection::sessionBus();
  KWinWindowSink sink;
  const QString sinkPath = QStringLiteral("/windows");
  if (!bus.registerObject(sinkPath, &sink,
                          QDBusConnection::ExportScriptableSlots))
    return {};

  const QString scriptPath = QDir(runtime).filePath(
      QStringLiteral("omasnap-windows-%1.js")
          .arg(QCoreApplication::applicationPid()));
  const QString script =
      QStringLiteral(
          "const current = workspace.currentDesktop;\n"
          "const list = [];\n"
          "for (const w of workspace.stackingOrder) {\n"
          "  if (!w.normalWindow || w.minimized) continue;\n"
          "  if (!w.onAllDesktops && !w.desktops.includes(current)) continue;\n"
          "  list.push({title: w.caption, id: String(w.internalId),\n"
          "    x: Math.round(w.frameGeometry.x),\n"
          "    y: Math.round(w.frameGeometry.y),\n"
          "    width: Math.round(w.frameGeometry.width),\n"
          "    height: Math.round(w.frameGeometry.height)});\n"
          "}\n"
          "callDBus(\"%1\", \"%2\", \"org.omasnap.WindowSink\", \"windows\",\n"
          "  JSON.stringify(list));\n")
          .arg(bus.baseService(), sinkPath);
  QFile scriptFile(scriptPath);
  const bool written =
      scriptFile.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
      scriptFile.write(script.toUtf8()) >= 0;
  scriptFile.close();
  if (!written) {
    bus.unregisterObject(sinkPath);
    return {};
  }

  const QString pluginName = QStringLiteral("omasnap-windows-%1")
                                 .arg(QCoreApplication::applicationPid());
  const QString scriptingPath = QStringLiteral("/Scripting");
  const QString scriptingInterface = QStringLiteral("org.kde.kwin.Scripting");
  QDBusMessage load = QDBusMessage::createMethodCall(
      kKWinService, scriptingPath, scriptingInterface,
      QStringLiteral("loadScript"));
  load << scriptPath << pluginName;
  const QDBusReply<int> loaded = bus.call(load, QDBus::Block, kDBusTimeoutMs);
  if (loaded.isValid() && loaded.value() >= 0) {
    QEventLoop loop;
    sink.loop = &loop;
    bus.call(QDBusMessage::createMethodCall(kKWinService, scriptingPath,
                                            scriptingInterface,
                                            QStringLiteral("start")),
             QDBus::Block, kDBusTimeoutMs);
    if (!sink.received) {
      QTimer::singleShot(kDBusTimeoutMs, &loop, &QEventLoop::quit);
      loop.exec();
    }
    sink.loop = nullptr;
  }
  QDBusMessage unload = QDBusMessage::createMethodCall(
      kKWinService, scriptingPath, scriptingInterface,
      QStringLiteral("unloadScript"));
  unload << pluginName;
  bus.call(unload, QDBus::Block, kDBusTimeoutMs);
  QFile::remove(scriptPath);
  bus.unregisterObject(sinkPath);
  return sink.payload;
}
} // namespace

bool kwinSession() {
  if (!qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE"))
    return false;
  const QStringList desktops = qEnvironmentVariable("XDG_CURRENT_DESKTOP")
                                   .split(QLatin1Char(':'), Qt::SkipEmptyParts);
  return std::any_of(desktops.begin(), desktops.end(), [](const QString &entry) {
    return entry.compare(QStringLiteral("KDE"), Qt::CaseInsensitive) == 0;
  });
}

QVector<WindowTarget> kwinParseWindows(const QByteArray &json,
                                       const MonitorInfo &monitor) {
  QVector<WindowTarget> result;
  const QJsonDocument document = QJsonDocument::fromJson(json);
  if (!document.isArray())
    return result;

  for (const QJsonValue value : document.array()) {
    const QJsonObject object = value.toObject();
    QRect rect(object.value(QStringLiteral("x")).toInt() -
                   monitor.geometry.x(),
               object.value(QStringLiteral("y")).toInt() -
                   monitor.geometry.y(),
               object.value(QStringLiteral("width")).toInt(),
               object.value(QStringLiteral("height")).toInt());
    rect = rect.intersected(QRect(QPoint(), monitor.geometry.size()));
    if (rect.isEmpty())
      continue;

    QString title = object.value(QStringLiteral("title")).toString();
    if (title.isEmpty())
      title = QStringLiteral("window");
    result.push_back({rect, object.value(QStringLiteral("id")).toString(),
                      std::move(title)});
  }
  return result;
}

bool kwinCaptureFocusedMonitor(CaptureData &capture, QString &error) {
  const QString outputName = activeOutputName(error);
  if (outputName.isEmpty())
    return false;

  QScreen *screen = nullptr;
  for (QScreen *candidate : QGuiApplication::screens()) {
    if (candidate->name() == outputName) {
      screen = candidate;
      break;
    }
  }
  if (!screen) {
    error = QStringLiteral("KWin reported unknown monitor %1").arg(outputName);
    return false;
  }

  capture.monitor.name = outputName;
  capture.monitor.geometry = screen->geometry();
  capture.monitor.workspaceId = 0;
  if (!screenshot2Capture(QStringLiteral("CaptureScreen"), outputName, {},
                          capture.source, capture.monitor.scale, error))
    return false;
  capture.monitor.pixelSize = capture.source.size();

  capture.preview =
      capture.source.scaled(capture.monitor.geometry.size(),
                            Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  if (capture.preview.isNull()) {
    error = QStringLiteral("Could not prepare screenshot preview");
    return false;
  }

  capture.windows = kwinParseWindows(listWindowsJson(), capture.monitor);
  return true;
}

bool kwinCaptureWindowSurface(const WindowTarget &window, QImage &image,
                              QString &error) {
  qreal scale = 1.0;
  return screenshot2Capture(
      QStringLiteral("CaptureWindow"), window.stableId,
      {{QStringLiteral("include-decoration"), true},
       {QStringLiteral("include-shadow"), false}},
      image, scale, error);
}

#include "kwin.moc"
