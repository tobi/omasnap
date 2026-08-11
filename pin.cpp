#include <QApplication>
#include <QBuffer>
#include <QEnterEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QScreen>
#include <QTimer>
#include <QWheelEvent>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <utility>

namespace {

constexpr int kMinimumEdge = 80;
constexpr qreal kCloseButtonSize = 22;
constexpr qreal kCloseButtonInset = 8;
constexpr qreal kResizeGripSize = 18;
constexpr qreal kInitialScreenShare = 0.4;
constexpr qreal kWheelStep = 0.1;
constexpr int kToastMs = 1200;

// wl-copy backgrounds itself and keeps serving the selection after this process
// exits, which QClipboard cannot do on Wayland.
bool copyPngToClipboard(const QImage &image, QString &error) {
  QByteArray png;
  QBuffer buffer(&png);
  buffer.open(QIODevice::WriteOnly);
  if (!image.save(&buffer, "PNG")) {
    error = QStringLiteral("Could not encode PNG");
    return false;
  }

  QProcess copy;
  copy.start(QStringLiteral("wl-copy"),
             {QStringLiteral("--type"), QStringLiteral("image/png")});
  if (!copy.waitForStarted(2000)) {
    error = copy.errorString();
    return false;
  }
  copy.write(png);
  copy.closeWriteChannel();
  if (!copy.waitForFinished(5000) || copy.exitCode() != 0) {
    error = QString::fromUtf8(copy.readAllStandardError()).trimmed();
    if (error.isEmpty())
      error = QStringLiteral("wl-copy failed");
    return false;
  }
  return true;
}

class PinWindow final : public QWidget {
public:
  explicit PinWindow(QImage image) : image_(std::move(image)) {
    setWindowTitle(QStringLiteral("omasnap-pin"));
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setMouseTracking(true);
    resize(initialSize());
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(rect(), image_);
    if (!toast_.isEmpty())
      paintToast(painter);
    if (!hovered_)
      return;

    const QRectF close = closeButtonRect();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(12, 12, 16, 190));
    painter.drawRoundedRect(close, 6, 6);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(255, 255, 255, 225), 1.6));
    const QPointF center = close.center();
    const qreal arm = 4.5;
    painter.drawLine(center + QPointF(-arm, -arm), center + QPointF(arm, arm));
    painter.drawLine(center + QPointF(arm, -arm), center + QPointF(-arm, arm));
  }

  void paintToast(QPainter &painter) const {
    const QFontMetrics metrics(painter.font());
    const QRectF pill((width() - metrics.horizontalAdvance(toast_) - 28) / 2.0,
                      height() - 42, metrics.horizontalAdvance(toast_) + 28,
                      26);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(12, 12, 16, 205));
    painter.drawRoundedRect(pill, 13, 13);
    painter.setPen(QColor(240, 240, 245));
    painter.drawText(pill, Qt::AlignCenter, toast_);
  }

  void mousePressEvent(QMouseEvent *event) override {
    if (event->button() == Qt::MiddleButton ||
        closeButtonRect().contains(event->position())) {
      close();
      return;
    }
    if (event->button() != Qt::LeftButton)
      return;

    QWindow *handle = windowHandle();
    if (!handle)
      return;
    if (resizeGripRect().contains(event->position()))
      handle->startSystemResize(Qt::BottomEdge | Qt::RightEdge);
    else
      handle->startSystemMove();
  }

  void mouseMoveEvent(QMouseEvent *event) override {
    setCursor(resizeGripRect().contains(event->position()) ? Qt::SizeFDiagCursor
                                                           : Qt::ArrowCursor);
  }

  void wheelEvent(QWheelEvent *event) override {
    const int steps = event->angleDelta().y();
    if (steps == 0)
      return;
    const qreal factor = steps > 0 ? 1 + kWheelStep : 1 - kWheelStep;
    resize(scaledSize(qRound(width() * factor)));
    event->accept();
  }

  void keyPressEvent(QKeyEvent *event) override {
    if (event->key() == Qt::Key_Escape) {
      close();
      return;
    }
    if (event->matches(QKeySequence::Copy)) {
      QString error;
      showToast(copyPngToClipboard(image_, error)
                    ? QStringLiteral("Copied to clipboard")
                    : error);
      return;
    }
    QWidget::keyPressEvent(event);
  }

  void enterEvent(QEnterEvent *) override {
    hovered_ = true;
    update();
  }

  void leaveEvent(QEvent *) override {
    hovered_ = false;
    setCursor(Qt::ArrowCursor);
    update();
  }

private:
  [[nodiscard]] QSize availableSize() const {
    const QScreen *target =
        screen() ? screen() : QGuiApplication::primaryScreen();
    return target ? target->availableGeometry().size() : QSize(1920, 1080);
  }

  // The rendered capture is in device pixels, so a scaled output would
  // otherwise open at twice its apparent size.
  [[nodiscard]] QSize initialSize() const {
    const QScreen *target =
        screen() ? screen() : QGuiApplication::primaryScreen();
    const qreal ratio =
        target ? std::max<qreal>(1.0, target->devicePixelRatio()) : 1.0;
    const QSizeF logical = QSizeF(image_.size()) / ratio;
    const QSizeF limit = QSizeF(availableSize()) * kInitialScreenShare;
    const qreal fit = std::min({1.0, limit.width() / logical.width(),
                                limit.height() / logical.height()});
    return scaledSize(qRound(logical.width() * fit));
  }

  [[nodiscard]] QSize scaledSize(int targetWidth) const {
    const QSize available = availableSize();
    const int maximumWidth = std::max(
        kMinimumEdge,
        qRound(image_.height() >= image_.width()
                   ? available.height() * static_cast<qreal>(image_.width()) /
                         image_.height()
                   : available.width()));
    const int clamped = std::clamp(targetWidth, kMinimumEdge, maximumWidth);
    const int height = std::max(
        kMinimumEdge,
        qRound(clamped * static_cast<qreal>(image_.height()) / image_.width()));
    return QSize(clamped, height);
  }

  void showToast(QString message) {
    toast_ = std::move(message);
    update();
    QTimer::singleShot(kToastMs, this, [this] {
      toast_.clear();
      update();
    });
  }

  [[nodiscard]] QRectF closeButtonRect() const {
    return QRectF(width() - kCloseButtonSize - kCloseButtonInset,
                  kCloseButtonInset, kCloseButtonSize, kCloseButtonSize);
  }

  [[nodiscard]] QRectF resizeGripRect() const {
    return QRectF(width() - kResizeGripSize, height() - kResizeGripSize,
                  kResizeGripSize, kResizeGripSize);
  }

  QImage image_;
  QString toast_;
  bool hovered_ = false;
};

} // namespace

int main(int argc, char **argv) {
  // The capture process exports this for its own layer-shell overlay;
  // inheriting it here would turn the pin into a layer surface with no window
  // rules.
  qunsetenv("QT_WAYLAND_SHELL_INTEGRATION");
  QCoreApplication::setApplicationName(QStringLiteral("omasnap-pin"));
  QGuiApplication::setDesktopFileName(QStringLiteral("omasnap-pin"));
  QApplication application(argc, argv);

  const QStringList arguments = QCoreApplication::arguments();
  if (arguments.size() < 2) {
    qWarning("usage: omasnap-pin <image>");
    return 2;
  }
  QImage image(arguments.at(1));
  if (image.isNull()) {
    qWarning("omasnap-pin: could not load %s", qUtf8Printable(arguments.at(1)));
    return 1;
  }

  PinWindow window(std::move(image));
  window.show();
  return application.exec();
}
