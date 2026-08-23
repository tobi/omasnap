#include "shelf.hpp"

#include "capture.hpp"
#include "icons.hpp"
#include "shelf-layout.hpp"

#include <LayerShellQt/Window>

#include <QApplication>
#include <QDir>
#include <QEnterEvent>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QMargins>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QScreen>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <utility>

namespace {

constexpr int kCornerRadius = 12;
constexpr int kEdgeMargin = 18;
constexpr int kSwipeThreshold = 42;
constexpr int kActionSize = 28;
constexpr int kActionInset = 7;

struct ShelfRequest {
  QString action = QStringLiteral("add");
  QString path;
  QString screenName;
};

QString shelfSocketPath() {
  const QString runtime = secureRuntimeDirectory();
  return runtime.isEmpty()
             ? QString()
             : QDir(runtime).filePath(QStringLiteral("shelf.socket"));
}

QByteArray encodeRequest(const ShelfRequest &request) {
  QJsonObject object;
  object.insert(QStringLiteral("action"), request.action);
  object.insert(QStringLiteral("path"), request.path);
  object.insert(QStringLiteral("screen"), request.screenName);
  return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

ShelfRequest decodeRequest(const QByteArray &payload) {
  const QJsonDocument document = QJsonDocument::fromJson(payload.trimmed());
  if (!document.isObject())
    return {};
  const QJsonObject object = document.object();
  return {
      object.value(QStringLiteral("action")).toString(QStringLiteral("add")),
      object.value(QStringLiteral("path")).toString(),
      object.value(QStringLiteral("screen")).toString()};
}

bool notifyExistingShelf(const QString &socketPath,
                         const ShelfRequest &request) {
  QLocalSocket socket;
  socket.connectToServer(socketPath, QIODevice::ReadWrite);
  if (!socket.waitForConnected(150))
    return false;
  const QByteArray payload = encodeRequest(request);
  if (socket.write(payload) != payload.size())
    return false;
  socket.flush();
  if (socket.bytesToWrite() > 0 && !socket.waitForBytesWritten(500))
    return false;
  static_cast<void>(socket.waitForReadyRead(1000));
  const QByteArray response = socket.readAll().trimmed();
  const bool acknowledged = response == QByteArrayLiteral("ok");
  socket.disconnectFromServer();
  return acknowledged;
}

QScreen *screenByName(const QString &name) {
  if (!name.isEmpty()) {
    for (QScreen *screen : QGuiApplication::screens()) {
      if (screen->name() == name)
        return screen;
    }
  }
  return QGuiApplication::primaryScreen();
}

void removeOwnedShelfSnapshot(const QString &path) {
  if (!isShelfSnapshotPath(path))
    return;
  QFile::remove(operationLogPath(path));
  QFile::remove(path);
}

class CaptureShelfWindow final : public QWidget {
public:
  CaptureShelfWindow() {
    setWindowTitle(QStringLiteral("omasnap-shelf"));
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
  }

  ~CaptureShelfWindow() override {
    for (const Item &item : std::as_const(items_))
      removeOwnedShelfSnapshot(item.path);
  }

  void setLayerWindow(LayerShellQt::Window *layer) {
    layer_ = layer;
    applyLayout();
  }

  void handleRequest(const ShelfRequest &request) {
    if (request.action == QStringLiteral("hide")) {
      captureHidden_ = true;
      hide();
      QGuiApplication::sync();
    } else if (request.action == QStringLiteral("show")) {
      captureHidden_ = false;
      if (!items_.isEmpty() && !editingProcess_)
        show();
      QGuiApplication::sync();
    } else {
      captureHidden_ = false;
      addCapture(request);
    }
  }

  void addCapture(const ShelfRequest &request) {
    const QFileInfo info(request.path);
    QImage image(info.absoluteFilePath());
    if (!info.isFile() || image.isNull())
      return;

    const QString path = info.absoluteFilePath();
    for (int index = items_.size() - 1; index >= 0; --index) {
      if (items_.at(index).path == path)
        items_.removeAt(index);
    }
    items_.prepend({path, std::move(image)});
    while (items_.size() > captureShelfMaximumItems()) {
      removeOwnedShelfSnapshot(items_.constLast().path);
      items_.removeLast();
    }

    presentation_ = ShelfPresentation::Stacked;
    hoveredItem_ = 0;
    if (QScreen *target = screenByName(request.screenName)) {
      setScreen(target);
      if (layer_)
        layer_->setScreen(target);
    }
    applyLayout();
    if (!captureHidden_ && !editingProcess_)
      show();
    update();
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    if (presentation_ == ShelfPresentation::Tucked) {
      paintHandle(painter);
      return;
    }

    // Oldest first, newest last: index zero remains front-most.
    for (int index = items_.size() - 1; index >= 0; --index)
      paintThumbnail(painter, index);
    if (presentation_ == ShelfPresentation::Expanded)
      paintExpandedHandle(painter);
    if (!toast_.isEmpty())
      paintToast(painter);
  }

  void mousePressEvent(QMouseEvent *event) override {
    if (event->button() != Qt::LeftButton)
      return;
    pressPosition_ = event->position();
    dragging_ = true;
    event->accept();
  }

  void mouseMoveEvent(QMouseEvent *event) override {
    hoveredItem_ = captureShelfItemAt(layout_, event->position());
    if (dragging_ && presentation_ == ShelfPresentation::Stacked) {
      swipeOffset_ =
          std::max<qreal>(0, event->position().y() - pressPosition_.y());
    }
    setCursor(Qt::PointingHandCursor);
    update();
  }

  void mouseReleaseEvent(QMouseEvent *event) override {
    if (event->button() != Qt::LeftButton || !dragging_)
      return;
    dragging_ = false;
    if (presentation_ == ShelfPresentation::Tucked) {
      presentation_ = ShelfPresentation::Stacked;
    } else if (presentation_ == ShelfPresentation::Stacked) {
      presentation_ = swipeOffset_ >= kSwipeThreshold
                          ? ShelfPresentation::Tucked
                          : ShelfPresentation::Expanded;
    } else if (layout_.handle.contains(event->position())) {
      presentation_ = ShelfPresentation::Stacked;
    } else {
      const int index = captureShelfItemAt(layout_, event->position());
      if (index >= 0) {
        if (deleteButtonRect(index).contains(event->position()))
          removeItem(index);
        else if (copyButtonRect(index).contains(event->position()))
          copyItem(index);
        else
          annotateItem(index);
      }
    }
    swipeOffset_ = 0;
    applyLayout();
    update();
    event->accept();
  }

  void keyPressEvent(QKeyEvent *event) override {
    if (event->key() == Qt::Key_Escape &&
        presentation_ == ShelfPresentation::Expanded) {
      presentation_ = ShelfPresentation::Stacked;
      applyLayout();
      update();
      return;
    }
    QWidget::keyPressEvent(event);
  }

  void enterEvent(QEnterEvent *) override { update(); }

  void leaveEvent(QEvent *) override {
    hoveredItem_ = -1;
    setCursor(Qt::ArrowCursor);
    update();
  }

private:
  struct Item {
    QString path;
    QImage image;
  };

  void applyLayout() {
    layout_ = captureShelfLayout(items_.size(), presentation_);
    if (layout_.size.isEmpty()) {
      hide();
      return;
    }
    resize(layout_.size);
    if (layer_) {
      layer_->setDesiredSize(layout_.size);
      layer_->setMargins(QMargins(
          0, 0, kEdgeMargin,
          presentation_ == ShelfPresentation::Tucked ? -5 : kEdgeMargin));
    }
  }

  void paintThumbnail(QPainter &painter, int index) const {
    QRectF frame = layout_.thumbnails.at(index);
    if (presentation_ == ShelfPresentation::Stacked)
      frame.translate(0, std::min<qreal>(swipeOffset_, frame.height()));

    for (int layer = 4; layer > 0; --layer) {
      const qreal spread = layer * 1.2;
      painter.setPen(Qt::NoPen);
      painter.setBrush(QColor(0, 0, 0, 12 + (4 - layer) * 5));
      painter.drawRoundedRect(frame.adjusted(-spread, -spread, spread, spread),
                              kCornerRadius + spread, kCornerRadius + spread);
    }
    const bool newest =
        index == 0 && presentation_ == ShelfPresentation::Stacked;
    painter.setPen(
        QPen(newest ? QColor(70, 145, 255) : QColor(245, 245, 247, 75),
             newest ? 2 : 1));
    painter.setBrush(QColor(18, 18, 22, 246));
    painter.drawRoundedRect(frame, kCornerRadius, kCornerRadius);

    const QRectF imageRect = frame.adjusted(4, 4, -4, -4);
    QPainterPath clip;
    clip.addRoundedRect(imageRect, kCornerRadius - 3, kCornerRadius - 3);
    painter.save();
    painter.setClipPath(clip);
    const QImage &image = items_.at(index).image;
    const QSize fitted = image.size().scaled(imageRect.size().toSize(),
                                             Qt::KeepAspectRatioByExpanding);
    const QRectF target(imageRect.center().x() - fitted.width() / 2.0,
                        imageRect.center().y() - fitted.height() / 2.0,
                        fitted.width(), fitted.height());
    painter.drawImage(target, image);
    if (presentation_ == ShelfPresentation::Expanded && hoveredItem_ == index)
      painter.fillRect(imageRect, QColor(0, 0, 0, 35));
    painter.restore();
    if (presentation_ == ShelfPresentation::Expanded && hoveredItem_ == index)
      paintActionButton(painter, annotateButtonRect(index),
                        QStringLiteral("edit"));
    if (presentation_ == ShelfPresentation::Expanded && hoveredItem_ == index)
      paintActionButton(painter, copyButtonRect(index), QStringLiteral("copy"));
    if (presentation_ == ShelfPresentation::Expanded && hoveredItem_ == index)
      paintActionButton(painter, deleteButtonRect(index),
                        QStringLiteral("close"));
  }

  QRectF annotateButtonRect(int index) const {
    const QRectF frame = layout_.thumbnails.at(index);
    return QRectF(frame.left() + kActionInset, frame.top() + kActionInset,
                  kActionSize, kActionSize);
  }

  QRectF copyButtonRect(int index) const {
    return annotateButtonRect(index).translated(kActionSize + 4, 0);
  }

  QRectF deleteButtonRect(int index) const {
    const QRectF frame = layout_.thumbnails.at(index);
    return QRectF(frame.right() - kActionInset - kActionSize,
                  frame.top() + kActionInset, kActionSize, kActionSize);
  }

  void paintActionButton(QPainter &painter, const QRectF &button,
                         const QString &icon) const {
    painter.setPen(QPen(QColor(245, 245, 247, 65), 1));
    painter.setBrush(QColor(12, 12, 16, 215));
    painter.drawEllipse(button);
    drawToolbarIcon(painter, button, icon, {}, QColor(245, 245, 247));
  }

  void annotateItem(int index) {
    if (editingProcess_ || index < 0 || index >= items_.size())
      return;
    const QString path = items_.at(index).path;
    auto *process = new QProcess(this);
    editingProcess_ = process;
    hide();
    const auto finish = [this, process, path] {
      if (editingProcess_ != process)
        return;
      for (Item &item : items_) {
        if (item.path == path) {
          const QImage updated(path);
          if (!updated.isNull())
            item.image = updated;
          break;
        }
      }
      editingProcess_ = nullptr;
      process->deleteLater();
      applyLayout();
      show();
      update();
    };
    connect(process, &QProcess::finished, this,
            [finish](int, QProcess::ExitStatus) { finish(); });
    connect(process, &QProcess::errorOccurred, this,
            [finish](QProcess::ProcessError) { finish(); });
    process->start(QCoreApplication::applicationFilePath(),
                   {QStringLiteral("--edit-shelf"), path});
  }

  void copyItem(int index) {
    if (index < 0 || index >= items_.size())
      return;
    QString error;
    showToast(copyPngFileToClipboard(items_.at(index).path, error)
                  ? QStringLiteral("Copied to clipboard")
                  : error);
  }

  void removeItem(int index) {
    if (index < 0 || index >= items_.size())
      return;
    const QString path = items_.at(index).path;
    items_.removeAt(index);
    removeOwnedShelfSnapshot(path);
    hoveredItem_ = items_.isEmpty()
                       ? -1
                       : std::min(index, static_cast<int>(items_.size()) - 1);
    if (items_.isEmpty())
      presentation_ = ShelfPresentation::Stacked;
    applyLayout();
    update();
  }

  void showToast(QString message) {
    toast_ = std::move(message);
    update();
    QTimer::singleShot(1200, this, [this] {
      toast_.clear();
      update();
    });
  }

  void paintToast(QPainter &painter) const {
    const QFontMetrics metrics(painter.font());
    const qreal width = metrics.horizontalAdvance(toast_) + 28;
    const QRectF pill((this->width() - width) / 2.0, this->height() - 36, width,
                      26);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(12, 12, 16, 220));
    painter.drawRoundedRect(pill, 13, 13);
    painter.setPen(QColor(245, 245, 247));
    painter.drawText(pill, Qt::AlignCenter, toast_);
  }

  void paintExpandedHandle(QPainter &painter) const {
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(18, 18, 22, 185));
    painter.drawRoundedRect(layout_.handle, 8, 8);
    painter.setPen(QColor(245, 245, 247, 190));
    painter.drawText(layout_.handle, Qt::AlignCenter,
                     QStringLiteral("Collapse · Esc"));
  }

  void paintHandle(QPainter &painter) const {
    painter.setPen(QPen(QColor(70, 145, 255, 145), 1));
    painter.setBrush(QColor(18, 18, 22, 145));
    painter.drawRoundedRect(layout_.handle, 8, 8);
    painter.setPen(QColor(245, 245, 247, 185));
    painter.drawText(layout_.handle, Qt::AlignCenter, QStringLiteral("⌃"));
  }

  QVector<Item> items_;
  CaptureShelfLayout layout_;
  ShelfPresentation presentation_ = ShelfPresentation::Stacked;
  LayerShellQt::Window *layer_ = nullptr;
  QPointF pressPosition_;
  qreal swipeOffset_ = 0;
  bool dragging_ = false;
  int hoveredItem_ = -1;
  QProcess *editingProcess_ = nullptr;
  QString toast_;
  bool captureHidden_ = false;
};

class ShelfServer final : public QObject {
public:
  ShelfServer(QLocalServer &server, CaptureShelfWindow &window)
      : server_(server), window_(window) {
    connect(&server_, &QLocalServer::newConnection, this,
            [this] { acceptConnections(); });
  }

private:
  void acceptConnections() {
    while (server_.hasPendingConnections()) {
      QLocalSocket *socket = server_.nextPendingConnection();
      connect(socket, &QLocalSocket::readyRead, socket, [this, socket] {
        QByteArray payload = socket->property("omasnap-payload").toByteArray();
        payload += socket->readAll();
        if (!payload.contains('\n')) {
          socket->setProperty("omasnap-payload", payload);
          return;
        }
        const ShelfRequest request = decodeRequest(payload);
        if (request.action != QStringLiteral("add") ||
            !request.path.isEmpty()) {
          window_.handleRequest(request);
          socket->write(QByteArrayLiteral("ok\n"));
          socket->flush();
        }
      });
      connect(socket, &QLocalSocket::disconnected, socket,
              &QObject::deleteLater);
    }
  }

  QLocalServer &server_;
  CaptureShelfWindow &window_;
};

} // namespace

bool queueCaptureOnShelf(const QImage &image, const QString &screenName,
                         QString &error) {
  const QString path = shelfSnapshotPath();
  if (path.isEmpty() || !saveTemporarySnapshot(image, path, error, -1))
    return false;
  if (!copyPngFileToClipboard(path, error)) {
    QFile::remove(path);
    return false;
  }
  QStringList arguments{QStringLiteral("--shelf"), path};
  if (!screenName.isEmpty())
    arguments << QStringLiteral("--shelf-screen") << screenName;
  if (!QProcess::startDetached(QCoreApplication::applicationFilePath(),
                               arguments)) {
    QFile::remove(path);
    error = QStringLiteral("Could not start the capture Shelf");
    return false;
  }
  return true;
}

bool setCaptureShelfHidden(bool hidden) {
  const QString socketPath = shelfSocketPath();
  return !socketPath.isEmpty() &&
         notifyExistingShelf(socketPath, {hidden ? QStringLiteral("hide")
                                                 : QStringLiteral("show"),
                                          {},
                                          {}});
}

int runCaptureShelf(const QString &path, const QString &screenName) {
  const ShelfRequest request{QStringLiteral("add"),
                             QFileInfo(path).absoluteFilePath(), screenName};
  if (!QFileInfo::exists(request.path)) {
    qCritical("omasnap: Shelf image does not exist: %s",
              qUtf8Printable(request.path));
    return 1;
  }

  const QString socketPath = shelfSocketPath();
  if (socketPath.isEmpty())
    return 1;
  if (notifyExistingShelf(socketPath, request))
    return 0;

  const QString runtime = secureRuntimeDirectory();
  QLockFile shelfLock(QDir(runtime).filePath(QStringLiteral("shelf.instance")));
  shelfLock.setStaleLockTime(0);
  if (!shelfLock.tryLock()) {
    if (notifyExistingShelf(socketPath, request))
      return 0;
    qCritical("omasnap: the capture Shelf is running but did not respond");
    return 1;
  }

  QLocalServer::removeServer(socketPath);
  QLocalServer server;
  if (!server.listen(socketPath)) {
    if (notifyExistingShelf(socketPath, request))
      return 0;
    qCritical("omasnap: could not listen for Shelf captures: %s",
              qUtf8Printable(server.errorString()));
    return 1;
  }

  QApplication::setQuitOnLastWindowClosed(false);
  CaptureShelfWindow window;
  static_cast<void>(window.winId());
  QWindow *handle = window.windowHandle();
  LayerShellQt::Window *layer =
      handle ? LayerShellQt::Window::get(handle) : nullptr;
  if (!handle || !layer) {
    qCritical("omasnap: could not create Shelf layer surface");
    return 1;
  }

  layer->setScope(QStringLiteral("omasnap-shelf"));
  if (QScreen *target = screenByName(screenName)) {
    window.setScreen(target);
    layer->setScreen(target);
  }
  LayerShellQt::Window::Anchors anchors;
  anchors.setFlag(LayerShellQt::Window::AnchorBottom);
  anchors.setFlag(LayerShellQt::Window::AnchorRight);
  layer->setAnchors(anchors);
  layer->setExclusiveZone(0);
  layer->setKeyboardInteractivity(
      LayerShellQt::Window::KeyboardInteractivityOnDemand);
  layer->setActivateOnShow(false);
  layer->setLayer(LayerShellQt::Window::LayerOverlay);
  window.setLayerWindow(layer);
  ShelfServer shelfServer(server, window);
  window.addCapture(request);
  return QApplication::exec();
}
