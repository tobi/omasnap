#include "shelf.hpp"

#include "capture.hpp"
#include "shelf-layout.hpp"

#include <LayerShellQt/Window>

#include <QApplication>
#include <QDir>
#include <QEnterEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMargins>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <utility>

namespace {

constexpr int kCornerRadius = 12;
constexpr int kEdgeMargin = 18;
constexpr int kSwipeThreshold = 42;

struct ShelfRequest {
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
  object.insert(QStringLiteral("path"), request.path);
  object.insert(QStringLiteral("screen"), request.screenName);
  return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

ShelfRequest decodeRequest(const QByteArray &payload) {
  const QJsonDocument document = QJsonDocument::fromJson(payload.trimmed());
  if (!document.isObject())
    return {};
  const QJsonObject object = document.object();
  return {object.value(QStringLiteral("path")).toString(),
          object.value(QStringLiteral("screen")).toString()};
}

bool notifyExistingShelf(const QString &socketPath,
                         const ShelfRequest &request) {
  QLocalSocket socket;
  socket.connectToServer(socketPath, QIODevice::WriteOnly);
  if (!socket.waitForConnected(150))
    return false;
  const QByteArray payload = encodeRequest(request);
  if (socket.write(payload) != payload.size())
    return false;
  socket.flush();
  if (socket.bytesToWrite() > 0 && !socket.waitForBytesWritten(500))
    return false;
  socket.disconnectFromServer();
  return true;
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

class CaptureShelfWindow final : public QWidget {
public:
  CaptureShelfWindow() {
    setWindowTitle(QStringLiteral("omasnap-shelf"));
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
  }

  void setLayerWindow(LayerShellQt::Window *layer) {
    layer_ = layer;
    applyLayout();
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
    while (items_.size() > captureShelfMaximumItems())
      items_.removeLast();

    presentation_ = ShelfPresentation::Stacked;
    hoveredItem_ = 0;
    if (QScreen *target = screenByName(request.screenName)) {
      setScreen(target);
      if (layer_)
        layer_->setScreen(target);
    }
    applyLayout();
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
      connect(socket, &QLocalSocket::readyRead, socket, [socket] {
        socket->setProperty("omasnap-payload",
                            socket->property("omasnap-payload").toByteArray() +
                                socket->readAll());
      });
      connect(socket, &QLocalSocket::disconnected, socket, [this, socket] {
        QByteArray payload = socket->property("omasnap-payload").toByteArray();
        payload += socket->readAll();
        const ShelfRequest request = decodeRequest(payload);
        if (!request.path.isEmpty())
          window_.addCapture(request);
        socket->deleteLater();
      });
    }
  }

  QLocalServer &server_;
  CaptureShelfWindow &window_;
};

} // namespace

int runCaptureShelf(const QString &path, const QString &screenName) {
  const ShelfRequest request{QFileInfo(path).absoluteFilePath(), screenName};
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
