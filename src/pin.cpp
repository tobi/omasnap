#include "pin.hpp"
#include "capture.hpp"
#include "pin-file.hpp"
#include "pin-layout.hpp"
#include "icons.hpp"

#include <QApplication>
#include <QBuffer>
#include <QDir>
#include <QDrag>
#include <QFile>
#include <QEnterEvent>
#include <QFontMetrics>
#include <QHash>
#include <QImage>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainterPath>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QSocketNotifier>
#include <QCloseEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QToolTip>

#include <QUrl>
#include <QWheelEvent>
#include <QWidget>
#include <QWindow>

#include <fcntl.h>
#include <linux/input.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <memory>
#include <utility>

namespace {

constexpr qreal kCloseButtonSize = 18;
constexpr qreal kCloseButtonInset = 7;
constexpr qreal kControlGap = 6;
constexpr qreal kDragButtonWidth = kCloseButtonSize * 2 + kControlGap;
constexpr qreal kCornerMargin = 14;
constexpr int kPinGap = 10;
constexpr int kToastMs = 1200;

// Every pin's title starts with this, followed by the process id, so pins
// can recognize each other in the compositor's client list and a dispatcher
// can name exactly one of them. Without the unique half a title pattern
// matches every pin and the compositor acts on whichever it finds first.
const QString kPinTitlePrefix = QStringLiteral("omasnap-pin");

QString pinTitle() {
  return QStringLiteral("%1 %2").arg(kPinTitlePrefix).arg(
      QCoreApplication::applicationPid());
}

// The compositors a pin knows how to ask for placement. Wayland has no
// protocol for a window to position itself, so corner placement goes
// through compositor IPC; anywhere else the pin still opens, drags, edits,
// copies and drags out, it just lands where the compositor decides.
enum class Desktop { Hyprland, Sway, Unknown };

Desktop detectDesktop() {
  if (qEnvironmentVariableIsSet("HYPRLAND_INSTANCE_SIGNATURE"))
    return Desktop::Hyprland;
  if (qEnvironmentVariableIsSet("SWAYSOCK"))
    return Desktop::Sway;
  return Desktop::Unknown;
}

QString runForOutput(const QString &program, const QStringList &arguments) {
  QProcess process;
  process.start(program, arguments);
  if (!process.waitForFinished(1000))
    return {};
  return QString::fromUtf8(process.readAllStandardOutput());
}

// Dispatchers rather than window rules: rules have to exist before a window
// maps and live in the user's config, and a pin should need neither.
void hyprDispatch(const QString &expression) {
  static_cast<void>(runForOutput(QStringLiteral("hyprctl"),
                                 {QStringLiteral("dispatch"), expression}));
}

void swayCommand(const QString &command) {
  static_cast<void>(runForOutput(QStringLiteral("swaymsg"), {command}));
}

// The focused output's size in logical pixels; windows are placed in
// logical pixels while Hyprland reports device ones.
QSize compositorScreenSize(Desktop desktop) {
  if (desktop == Desktop::Hyprland) {
    const QJsonDocument document = QJsonDocument::fromJson(
        runForOutput(QStringLiteral("hyprctl"),
                     {QStringLiteral("-j"), QStringLiteral("monitors")})
            .toUtf8());
    for (const QJsonValue &value : document.array()) {
      const QJsonObject monitor = value.toObject();
      if (!monitor.value(QStringLiteral("focused")).toBool())
        continue;
      const double scale =
          std::max(0.0001, monitor.value(QStringLiteral("scale")).toDouble(1.0));
      return {qRound(monitor.value(QStringLiteral("width")).toDouble() / scale),
              qRound(monitor.value(QStringLiteral("height")).toDouble() /
                     scale)};
    }
    return {};
  }
  if (desktop == Desktop::Sway) {
    const QJsonDocument document = QJsonDocument::fromJson(
        runForOutput(QStringLiteral("swaymsg"),
                     {QStringLiteral("-t"), QStringLiteral("get_outputs"),
                      QStringLiteral("-r")})
            .toUtf8());
    for (const QJsonValue &value : document.array()) {
      const QJsonObject output = value.toObject();
      if (!output.value(QStringLiteral("focused")).toBool())
        continue;
      const QJsonObject rect = output.value(QStringLiteral("rect")).toObject();
      return {rect.value(QStringLiteral("width")).toInt(),
              rect.value(QStringLiteral("height")).toInt()};
    }
  }
  return {};
}

struct CompositorPin {
  QString title;
  QRect rect;
};

// Where every pin currently sits, by title; the title is how a move
// addresses one pin and not the others.
QVector<CompositorPin> compositorPinRects(Desktop desktop) {
  QVector<CompositorPin> pins;
  if (desktop == Desktop::Hyprland) {
    const QJsonDocument document = QJsonDocument::fromJson(
        runForOutput(QStringLiteral("hyprctl"),
                     {QStringLiteral("-j"), QStringLiteral("clients")})
            .toUtf8());
    for (const QJsonValue &value : document.array()) {
      const QJsonObject client = value.toObject();
      const QString title = client.value(QStringLiteral("title")).toString();
      if (!title.startsWith(kPinTitlePrefix))
        continue;
      const QJsonArray at = client.value(QStringLiteral("at")).toArray();
      const QJsonArray size = client.value(QStringLiteral("size")).toArray();
      if (at.size() == 2 && size.size() == 2) {
        pins.push_back({title, QRect(at.at(0).toInt(), at.at(1).toInt(),
                                     size.at(0).toInt(), size.at(1).toInt())});
      }
    }
    return pins;
  }
  if (desktop == Desktop::Sway) {
    // The tree is nested: a floating pin hangs off a workspace's floating
    // list rather than sitting beside the tiled windows.
    const QJsonDocument document = QJsonDocument::fromJson(
        runForOutput(QStringLiteral("swaymsg"),
                     {QStringLiteral("-t"), QStringLiteral("get_tree"),
                      QStringLiteral("-r")})
            .toUtf8());
    QVector<QJsonObject> pending{document.object()};
    while (!pending.isEmpty()) {
      const QJsonObject node = pending.takeLast();
      const QString name = node.value(QStringLiteral("name")).toString();
      if (name.startsWith(kPinTitlePrefix)) {
        const QJsonObject rect = node.value(QStringLiteral("rect")).toObject();
        pins.push_back({name, QRect(rect.value(QStringLiteral("x")).toInt(),
                                    rect.value(QStringLiteral("y")).toInt(),
                                    rect.value(QStringLiteral("width")).toInt(),
                                    rect.value(QStringLiteral("height"))
                                        .toInt())});
      }
      for (const char *key : {"nodes", "floating_nodes"}) {
        for (const QJsonValue &child :
             node.value(QLatin1String(key)).toArray())
          pending.push_back(child.toObject());
      }
    }
  }
  return pins;
}

bool compositorSeesPin(Desktop desktop, const QString &title) {
  for (const CompositorPin &pin : compositorPinRects(desktop)) {
    if (pin.title == title)
      return true;
  }
  return false;
}

void movePin(Desktop desktop, const QString &title, const QPoint &position) {
  if (desktop == Desktop::Hyprland)
    hyprDispatch(pinMoveDispatch(title, position.x(), position.y()));
  else if (desktop == Desktop::Sway)
    swayCommand(pinSwayMoveCommand(title, position.x(), position.y()));
}

// Where a new pin lands: ask the compositor where the existing pins are
// rather than keeping a count. Every pin is its own process, a file of
// positions goes stale the first time one crashes, and every pin on screen
// blocks the space it covers, whatever its shape, so a new pin packs snugly
// above what is there and never lands on a pin the user placed or an older
// build left behind.
QPoint nextPinPosition(Desktop desktop, const QSize &screen,
                       const QSize &frame, const QString &ownTitle) {
  QVector<QRect> blockers;
  for (const CompositorPin &pin : compositorPinRects(desktop)) {
    if (pin.title != ownTitle)
      blockers.push_back(pin.rect);
  }
  return pinPackedPosition(blockers, screen, frame, kPinGap,
                           qRound(kCornerMargin));
}

// A pin left the column: pack the survivors back down, keeping their order.
// Only pins still hugging the right edge take part; one dragged elsewhere
// is left alone and packed around. `excludedTitle` names one to leave out
// even if the compositor still lists it, which it may while that pin is
// closing.
void compactPinColumn(Desktop desktop, const QString &excludedTitle) {
  const QSize screen = compositorScreenSize(desktop);
  if (screen.isEmpty())
    return;
  QVector<CompositorPin> column;
  QVector<QRect> blockers;
  for (const CompositorPin &pin : compositorPinRects(desktop)) {
    if (pin.title == excludedTitle)
      continue;
    if (pinInColumn(pin.rect, screen, qRound(kCornerMargin)))
      column.push_back(pin);
    else
      blockers.push_back(pin.rect);
  }
  std::sort(column.begin(), column.end(),
            [](const CompositorPin &a, const CompositorPin &b) {
              return a.rect.y() > b.rect.y();
            });
  for (const CompositorPin &pin : column) {
    const QPoint target = pinPackedPosition(
        blockers, screen, pin.rect.size(), kPinGap, qRound(kCornerMargin));
    if ((target - pin.rect.topLeft()).manhattanLength() > 4)
      movePin(desktop, pin.title, target);
    blockers.push_back(QRect(target, pin.rect.size()));
  }
}

class PinWindow final : public QWidget {
public:
  explicit PinWindow(QImage image, QString path, const QSize &frame)
      : image_(std::move(image)), path_(std::move(path)), snapshotFile_(path_),
        desktop_(detectDesktop()) {
    setWindowTitle(pinTitle());
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    // Fixed, not merely sized: min equal to max is the hint a compositor
    // honors when floating, and Hyprland floats an unresizable window on
    // its own instead of first stretching it into a tile.
    setFixedSize(frame);
    setAttribute(Qt::WA_AlwaysShowToolTips, true);
    dragWatchTimer_.setInterval(80);
    connect(&dragWatchTimer_, &QTimer::timeout, this,
            [this] { observeDrag(); });
  }

  [[nodiscard]] bool hasPinLock() const { return snapshotFile_.isLocked(); }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // The compositor draws the border and the shadow around a floating
    // window, so the picture is the whole window: a mat of our own would be
    // a second frame inside the first. Cover-cropped and anchored to the
    // top rather than the middle, because a capture's top is its title bar,
    // its tabs, its heading; a tall page cropped to its middle is a slab of
    // body text that could be any of them.
    painter.fillRect(rect(), QColor(18, 18, 22));
    if (!image_.isNull()) {
      const qreal scale =
          std::max(static_cast<qreal>(width()) / image_.width(),
                   static_cast<qreal>(height()) / image_.height());
      const QRectF source((image_.width() - width() / scale) / 2.0, 0.0,
                          width() / scale, height() / scale);
      painter.drawImage(QRectF(rect()), image_, source);
    }
    if (!toast_.isEmpty())
      paintToast(painter);
    if (!hovered_)
      return;

    drawControlButton(painter, dragButtonRect(), QStringLiteral("drag-handle"));
    drawControlButton(painter, editButtonRect(), QStringLiteral("edit"));
    drawControlButton(painter, pathButtonRect(), QStringLiteral("path"));
    drawControlButton(painter, copyButtonRect(), QStringLiteral("copy"));
    drawControlButton(painter, closeButtonRect(), QStringLiteral("close"));
  }

  void drawControlButton(QPainter &painter, const QRectF &rect,
                         const QString &action) const {
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(12, 12, 16, 190));
    painter.drawRoundedRect(rect, 6, 6);
    drawToolbarIcon(painter, rect, action, {}, QColor(245, 245, 247));
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
    if (event->button() == Qt::MiddleButton) {
      close();
      return;
    }
    const QPointF position = event->position();
    if (event->button() == Qt::LeftButton) {
      if (closeButtonRect().contains(position)) {
        close();
        return;
      }
      if (dragButtonRect().contains(position)) {
        beginFileDrag();
        return;
      }
      if (copyButtonRect().contains(position)) {
        QString error;
        showToast(copyImageToClipboard(image_, error)
                      ? QStringLiteral("Copied to clipboard")
                      : error);
        return;
      }
      if (pathButtonRect().contains(position)) {
        QString error;
        showToast(copyTextToClipboard(path_, error)
                      ? QStringLiteral("Copied path")
                      : error);
        return;
      }
      if (editButtonRect().contains(position)) {
        reopenInEditor();
        return;
      }
      if (QWindow *handle = windowHandle())
        handle->startSystemMove();
      beginDragWatch();
      event->accept();
    }
  }

  // startSystemMove hands the drag to the compositor and this side never
  // hears when it ends, so the end is read off the effect: the window left
  // its starting position and then held one spot for a few polls. A press
  // that never moves the window was a click and times out instead. Either
  // way the column closes the gap behind a pin that was dragged away.
  void beginDragWatch() {
    if (desktop_ == Desktop::Unknown)
      return;
    dragStartRect_ = ownCompositorRect();
    if (dragStartRect_.isNull())
      return;
    dragScreen_ = compositorScreenSize(desktop_);
    dragPreviousRect_ = {};
    commandedTargets_.clear();
    dragMoved_ = false;
    dragPolls_ = 0;
    dragStablePolls_ = 0;
    spreadActive_ = false;
    snapSpot_ = {};
    openButtonWatch();
    dragWatchTimer_.start();
  }

  void observeDrag() {
    const QRect rect = ownCompositorRect();
    ++dragPolls_;
    const bool clickTimeout = !dragMoved_ && dragPolls_ >= 12;
    if (rect.isNull() || clickTimeout || dragPolls_ >= 750) {
      dragWatchTimer_.stop();
      closeButtonWatch();
      if (spreadActive_)
        compactPinColumn(desktop_, windowTitle());
      spreadActive_ = false;
      return;
    }
    dragMoved_ = dragMoved_ || rect != dragStartRect_;
    if (dragMoved_)
      previewInsertion(rect);
    const bool still = rect == dragPreviousRect_;
    dragStablePolls_ = dragMoved_ && still ? dragStablePolls_ + 1 : 0;
    dragPreviousRect_ = rect;
    // The release normally arrives from the input device watch or as a
    // pointer event; this long stillness fallback only catches a session
    // where neither could be established.
    if (dragMoved_ && dragStablePolls_ >= 25)
      finishDrag();
  }

  // The kernel pushes the button release the instant it happens, no matter
  // whether the pointer ever moves again; the compositor tells this window
  // nothing until it does. Best effort: without permission to read the
  // devices, the pointer-event and stillness paths still finish the drag.
  void openButtonWatch() {
    closeButtonWatch();
    QDir devices(QStringLiteral("/dev/input/by-id"));
    const QStringList entries =
        devices.entryList({QStringLiteral("*-event-mouse")},
                          QDir::System | QDir::Files | QDir::NoDotAndDotDot);
    for (const QString &entry : entries) {
      const int fd =
          ::open(QFile::encodeName(devices.filePath(entry)).constData(),
                 O_RDONLY | O_NONBLOCK | O_CLOEXEC);
      if (fd < 0)
        continue;
      auto *notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
      connect(notifier, &QSocketNotifier::activated, this,
              [this, fd] { readButtonEvents(fd); });
      buttonWatches_.push_back({fd, notifier});
    }
  }

  void readButtonEvents(int fd) {
    struct input_event events[16];
    for (;;) {
      const ssize_t bytes = ::read(fd, events, sizeof events);
      if (bytes <= 0)
        return;
      const int count = static_cast<int>(bytes / sizeof(input_event));
      for (int index = 0; index < count; ++index) {
        if (events[index].type != EV_KEY || events[index].code != BTN_LEFT ||
            events[index].value != 0 || !dragWatchTimer_.isActive())
          continue;
        const QRect rect = ownCompositorRect();
        dragMoved_ =
            dragMoved_ || (!rect.isNull() && rect != dragStartRect_);
        if (dragMoved_)
          finishDrag();
        else
          dragWatchTimer_.stop();
        closeButtonWatch();
        return;
      }
    }
  }

  void closeButtonWatch() {
    for (const auto &[fd, notifier] : buttonWatches_) {
      delete notifier;
      ::close(fd);
    }
    buttonWatches_.clear();
  }

  // The compositor's move grab starves this window of pointer events, so
  // the first enter or hover after the grab began is the release itself,
  // and the snap can happen right then instead of waiting for a poll.
  // Holding the pin still mid-drag stays a drag for as long as the button
  // is down.
  void pointerWokeDuringWatch() {
    if (!dragWatchTimer_.isActive())
      return;
    const QRect rect = ownCompositorRect();
    dragMoved_ = dragMoved_ || (!rect.isNull() && rect != dragStartRect_);
    if (!dragMoved_) {
      // A click, not a drag: nothing moved, nothing to restore.
      dragWatchTimer_.stop();
      return;
    }
    finishDrag();
  }

  void finishDrag() {
    dragWatchTimer_.stop();
    closeButtonWatch();
    // One last look at the true final position; the last poll can be a
    // frame behind it.
    const QRect rect = ownCompositorRect();
    if (!rect.isNull())
      previewInsertion(rect);
    if (!snapSpot_.isNull())
      movePin(desktop_, windowTitle(), snapSpot_.topLeft());
    else
      compactPinColumn(desktop_, QString());
    spreadActive_ = false;
  }

  // While the drag hovers the column, the others step aside around a hole
  // where this pin would land, live; leaving the column packs them back.
  void previewInsertion(const QRect &rect) {
    if (dragScreen_.isEmpty())
      return;
    QVector<QPair<QString, QRect>> column;
    QVector<QRect> blockers;
    for (const CompositorPin &pin : compositorPinRects(desktop_)) {
      if (pin.title == windowTitle())
        continue;
      if (pinInColumn(pin.rect, dragScreen_, qRound(kCornerMargin)))
        column.push_back({pin.title, pin.rect});
      else
        blockers.push_back(pin.rect);
    }
    const PinInsertionPlan plan = pinInsertionPlan(
        column, blockers, rect, dragScreen_, kPinGap, qRound(kCornerMargin));
    if (plan.index < 0) {
      if (spreadActive_)
        compactPinColumn(desktop_, windowTitle());
      spreadActive_ = false;
      snapSpot_ = {};
      commandedTargets_.clear();
      return;
    }
    // Dispatch each move once, against what was last commanded rather than
    // the live rect: a pin mid-animation is never at its target yet, and
    // re-sending the same move every poll restarts the animation, which
    // reads as flicker. One command per new target lets the slide play out.
    for (const auto &[title, target] : plan.spread) {
      if (commandedTargets_.value(title, QPoint(INT_MIN, INT_MIN)) !=
          target.topLeft()) {
        movePin(desktop_, title, target.topLeft());
        commandedTargets_.insert(title, target.topLeft());
      }
    }
    spreadActive_ = true;
    snapSpot_ = plan.spot;
  }

  [[nodiscard]] QRect ownCompositorRect() const {
    for (const CompositorPin &pin : compositorPinRects(desktop_)) {
      if (pin.title == windowTitle())
        return pin.rect;
    }
    return {};
  }

  void reopenInEditor() {
    if (!QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                 {path_}))
      showToast(QStringLiteral("Could not start omasnap"));
    else {
      snapshotFile_.preserveForEditor();
      close();
    }
  }

  void mouseMoveEvent(QMouseEvent *event) override {
    pointerWokeDuringWatch();
    const QPointF position = event->position();
    setCursor(controlRectAt(position) >= 0 ? Qt::PointingHandCursor
                                           : Qt::ArrowCursor);

    const int control = controlRectAt(position);
    if (control != hoveredControl_) {
      hoveredControl_ = control;
      update();
    }
  }

  // The window is an ordinary toplevel now, so the app's own tooltip can
  // appear at the cursor instead of a pill painted in one corner. Anchored
  // to the control's rect so it stays up while the cursor is inside it.
  bool event(QEvent *event) override {
    if (event->type() == QEvent::ToolTip) {
      auto *help = static_cast<QHelpEvent *>(event);
      const int control = controlRectAt(help->pos());
      if (control >= 0) {
        QToolTip::showText(help->globalPos(), pinControlTip(control), this,
                           controlRect(control).toAlignedRect());
      } else {
        QToolTip::hideText();
      }
      return true;
    }
    return QWidget::event(event);
  }


  // A layer surface can still initiate a Wayland uri-list drag just like a
  // file manager; the six-dot control is the drag handle.
  void beginFileDrag() {
    QMimeData *mime = new QMimeData;
    const QList<QUrl> urls{QUrl::fromLocalFile(path_)};
    mime->setUrls(urls);
    mime->setText(urls.constFirst().toLocalFile());
    QByteArray pngData;
    QBuffer buffer(&pngData);
    buffer.open(QIODevice::WriteOnly);
    if (image_.save(&buffer, "PNG"))
      mime->setData(QStringLiteral("image/png"), pngData);

    QDrag drag(this);
    drag.setMimeData(mime);
    drag.setPixmap(QPixmap::fromImage(image_.scaled(
        256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    drag.exec(Qt::CopyAction | Qt::MoveAction);
  }

  void wheelEvent(QWheelEvent *event) override {
    // Pinned captures deliberately keep a stable 250x200 frame so the
    // controls remain usable and the image area never reflows.
    event->accept();
  }

  void keyPressEvent(QKeyEvent *event) override {
    if (event->key() == Qt::Key_Escape) {
      close();
      return;
    }
    if (event->matches(QKeySequence::Copy)) {
      QString error;
      showToast(copyImageToClipboard(image_, error)
                    ? QStringLiteral("Copied to clipboard")
                    : error);
      return;
    }
    QWidget::keyPressEvent(event);
  }

  void closeEvent(QCloseEvent *event) override {
    // The compositor may still list this window while it closes, so it is
    // excluded by name rather than trusted to be gone.
    compactPinColumn(desktop_, windowTitle());
    QWidget::closeEvent(event);
  }

  void enterEvent(QEnterEvent *) override {
    pointerWokeDuringWatch();
    hovered_ = true;
    hoveredControl_ = -1;
    update();
  }

  void leaveEvent(QEvent *) override {
    hovered_ = false;
    hoveredControl_ = -1;
    setCursor(Qt::ArrowCursor);
    update();
  }

private:
  void showToast(QString message) {
    toast_ = std::move(message);
    update();
    QTimer::singleShot(kToastMs, this, [this] {
      toast_.clear();
      update();
    });
  }

  // The wide drag handle stands alone in the top-left; edit, path, copy, and
  // close remain grouped in the top-right.
  [[nodiscard]] QRectF closeButtonRect() const { return controlRect(0); }

  [[nodiscard]] QRectF copyButtonRect() const { return controlRect(1); }

  [[nodiscard]] QRectF pathButtonRect() const { return controlRect(2); }

  [[nodiscard]] QRectF editButtonRect() const { return controlRect(3); }

  [[nodiscard]] QRectF dragButtonRect() const { return controlRect(4); }

  [[nodiscard]] QRectF controlRect(int index) const {
    const qreal right = width() - kCloseButtonSize - kCloseButtonInset;
    if (index < 4) {
      return QRectF(right - index * (kCloseButtonSize + kControlGap),
                    kCloseButtonInset, kCloseButtonSize, kCloseButtonSize);
    }
    return QRectF(kCloseButtonInset, kCloseButtonInset, kDragButtonWidth,
                  kCloseButtonSize);
  }

  [[nodiscard]] int controlRectAt(const QPointF &position) const {
    for (int index = 0; index < 5; ++index) {
      if (controlRect(index).contains(position))
        return index;
    }
    return -1;
  }

  QImage image_;
  QString path_;
  PinSnapshotFile snapshotFile_;
  Desktop desktop_;
  QTimer dragWatchTimer_;
  QRect dragStartRect_;
  QRect dragPreviousRect_;
  QSize dragScreen_;
  QHash<QString, QPoint> commandedTargets_;
  QVector<QPair<int, QSocketNotifier *>> buttonWatches_;
  QRect snapSpot_;
  bool spreadActive_ = false;
  bool dragMoved_ = false;
  int dragPolls_ = 0;
  int dragStablePolls_ = 0;
  QString toast_;
  bool hovered_ = false;
  int hoveredControl_ = -1;
};

} // namespace

int runPinnedCapture(const QString &path) {
  QImage image(path);
  if (image.isNull()) {
    qWarning("omasnap: could not load pinned image %s", qUtf8Printable(path));
    return 1;
  }

  const Desktop desktop = detectDesktop();
  PinWindow window(std::move(image), path,
                   pinFrameSize(compositorScreenSize(desktop)));
  if (!window.hasPinLock()) {
    qWarning("omasnap: could not lock pinned image %s", qUtf8Printable(path));
    return 1;
  }
  window.show();

  // A normal window, floated and pinned through the compositor, instead of
  // a layer surface: the compositor draws its frame, moves it, and keeps it
  // on every workspace. Not immediately though: showing is this side's word
  // for mapped, and the compositor has not necessarily registered the
  // window under its title yet; dispatches sent then report success and do
  // nothing, which leaves a pin centered and unpinned. Retry until the
  // client list has it, then float first (a tiled window has no position of
  // its own to set), pin it, and drop it into the lowest free slot.
  if (desktop != Desktop::Unknown) {
    auto attempts = std::make_shared<int>(0);
    QTimer *settle = new QTimer(&window);
    settle->setInterval(50);
    QObject::connect(settle, &QTimer::timeout, &window,
                     [&window, desktop, settle, attempts] {
                       ++*attempts;
                       if (!compositorSeesPin(desktop, window.windowTitle())) {
                         if (*attempts >= 10)
                           settle->stop();
                         return;
                       }
                       settle->stop();
                       const QString title = window.windowTitle();
                       if (desktop == Desktop::Hyprland) {
                         hyprDispatch(pinFloatDispatch(title));
                         hyprDispatch(pinPinDispatch(title));
                       }
                       const QSize screen = compositorScreenSize(desktop);
                       if (screen.isEmpty())
                         return;
                       const QPoint origin =
                           nextPinPosition(desktop, screen, window.size(),
                                           window.windowTitle());
                       if (desktop == Desktop::Hyprland) {
                         movePin(desktop, title, origin);
                       } else {
                         swayCommand(pinSwayArrangeCommand(title, origin.x(),
                                                           origin.y()));
                       }
                     });
    settle->start();
  }
  return QApplication::exec();
}
