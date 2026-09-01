/** @fileoverview Manual scroll capture overlay (see scroll-capture.hpp). */
#include "scroll-capture.hpp"

#include "scroll-inject.hpp"

#include <LayerShellQt/Window>

#include <QtConcurrent/QtConcurrentRun>

#include <QApplication>
#include <QCursor>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QScreen>
#include <QThread>
#include <QTimer>
#include <QWindow>

#include <algorithm>
#include <QElapsedTimer>

#include <cmath>

namespace {
/// Poll cadence of the capture loop.
constexpr int kCaptureIntervalMs = 100;
/// How long one grab may wait for the compositor to deliver damage. The loop
/// runs on a worker thread, so waiting is fine; a static screen simply yields
/// no frame this round.
constexpr int kGrabTimeoutMs = 400;
/// Automatic mode reads the screen until it has stopped changing rather than
/// after a fixed delay: browsers animate a wheel tick over anything from one
/// frame to several hundred milliseconds, and a frame taken mid-animation
/// lands wherever the easing curve happened to be. After a tick the page is
/// given kMotionWaitMs to start moving at all (no change by then is a real
/// stationary frame: the end of the page, or a wheel that went elsewhere).
/// Once it moves, a frame is settled when the next grab times out (no damage
/// for kSettleGrabMs) or repeats the previous one exactly; kMaxSettleMs caps
/// that for content that never holds still (video, spinners).
constexpr int kMotionWaitMs = 700;
constexpr int kSettleGrabMs = 120;
constexpr int kMaxSettleMs = 2000;
/// Fraction of the region an automatic step should travel. Overlap is what
/// the stitcher aligns on; half the region leaves it plenty at any scroll
/// speed, and the notch count is fitted to it after the first verified step.
constexpr double kTargetStepFraction = 0.45;
constexpr int kMaxNotchesPerTick = 12;
constexpr int kMinRegion = 32; // logical px
/// How far outside the region its grips and its draggable border reach. The
/// region itself has to stay untouched: it is what the capture crops.
constexpr int kGripBand = 16;
/// How long to let the compositor show a frame with the chrome hidden before
/// grabbing the first one. Two frames at 60 Hz, with room to spare.
constexpr int kChromeSettleMs = 60;
const QColor kDim(0, 0, 0, 150);
const QColor kAccent(10, 132, 255);
const QColor kWarn(255, 159, 10);

struct ModeButton {
  const char *label;
  bool automatic;
  stitch::Axis axis;
};
/// The Selected-phase mode choices (painted, hit-tested like the pills).
constexpr ModeButton kModeButtons[] = {
    {"Manual \u2193", false, stitch::Axis::Vertical},
    {"Auto \u2193", true, stitch::Axis::Vertical},
    {"Manual \u2192", false, stitch::Axis::Horizontal},
    {"Auto \u2192", true, stitch::Axis::Horizontal},
};
constexpr int kModeButtonCount = 4;
constexpr int kModeButtonWidth = 118;
constexpr int kModeButtonHeight = 40;
constexpr int kModeButtonGap = 10;

const char *eventName(stitch::ManualCapture::Event event) {
  using Event = stitch::ManualCapture::Event;
  switch (event) {
  case Event::Blank: return "Blank";
  case Event::Seeded: return "Seeded";
  case Event::Kept: return "Kept";
  case Event::Pending: return "Pending";
  case Event::Still: return "Still";
  case Event::PendingDropped: return "PendingDropped";
  case Event::ReSeeded: return "ReSeeded";
  case Event::Ambiguous: return "Ambiguous";
  case Event::Unmatchable: return "Unmatchable";
  case Event::WrongDirection: return "WrongDirection";
  case Event::Error: return "Error";
  case Event::Full: return "Full";
  }
  return "?";
}
} // namespace

struct ScrollCapturePanel::Worker {
  explicit Worker(stitch::Axis axis) : session(axis), autoSession(axis) {}
  OutputCapture output;
  stitch::ManualCapture session;    // manual mode
  stitch::AutoCapture autoSession;  // automatic mode
  QRect regionPhysical;
  int grabbed = 0;
  int consecutiveFailures = 0;
  std::uint64_t lastCycle = 0; // last consumed handshake cycle (auto)
  QImage firstCrop;
  QImage lastCrop;
  QString debugDir;
  /// Notches per normal automatic tick, fitted to the region once the first
  /// verified step says how far one notch moves this page.
  /// Starts at one notch, which overlaps on any page, and is fitted to the
  /// region once the first verified step says how far one notch moves here.
  int notchesPerTick = 1;
  bool notchesFitted = false;

  /// Crop of the region from a full output frame, in the stitcher's format.
  [[nodiscard]] QImage crop(const QImage &frame) const {
    return frame.copy(regionPhysical).convertToFormat(QImage::Format_RGBA8888);
  }

  /// Reads frames until the region has settled after a scroll tick (see the
  /// kMotionWaitMs block). `before` is the region as it was before the tick;
  /// null on the first cycle. Returns false only when the session is dead or
  /// nothing could be grabbed at all; `error` describes it.
  [[nodiscard]] bool acquireSettledFrame(const QImage &before, QImage &settled,
                                         QString &error) {
    QElapsedTimer clock;
    clock.start();
    bool moved = before.isNull();
    QImage last;
    int failures = 0;
    while (true) {
      QImage frame;
      const bool ok = output.grab(frame, error, moved ? kSettleGrabMs
                                                       : kSettleGrabMs / 2);
      if (!ok) {
        if (output.sessionStopped())
          return false;
        if (moved && !last.isNull()) {
          settled = last; // no damage since the last frame: it is the one
          return true;
        }
        if (!moved && clock.elapsed() >= kMotionWaitMs) {
          settled = before; // nothing moved: genuinely stationary
          return true;
        }
        if (++failures >= 40) // ~5 s of nothing but failures
          return false;
        continue;
      }
      failures = 0;
      const QImage current = crop(frame);
      if (!moved) {
        if (current != before) {
          moved = true;
        } else if (clock.elapsed() >= kMotionWaitMs) {
          settled = current;
          return true;
        } else {
          continue;
        }
      }
      if (!last.isNull() && current == last) {
        settled = current; // two identical frames in a row: settled
        return true;
      }
      last = current;
      if (clock.elapsed() >= kMaxSettleMs) {
        settled = current; // never holds still; take what is there
        return true;
      }
    }
  }
};

ScrollCapturePanel::ScrollCapturePanel(MonitorInfo monitor,
                                       LayerShellQt::Window *layer,
                                       QWidget *parent)
    : QWidget(parent), monitor_(std::move(monitor)), layer_(layer) {
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);
  setAttribute(Qt::WA_TranslucentBackground);
  if (parent)
    setGeometry(parent->rect());
}

ScrollCapturePanel::~ScrollCapturePanel() { release(); }

void ScrollCapturePanel::release() {
  if (released_)
    return;
  released_ = true;
  stopWorker();
  // Hand the surface back whole: no hole, keyboard exclusive again.
  if (QWindow *window = surfaceWindow())
    window->setMask(QRegion());
  if (layer_)
    layer_->setKeyboardInteractivity(
        LayerShellQt::Window::KeyboardInteractivityExclusive);
  hide();
}

QWindow *ScrollCapturePanel::surfaceWindow() const {
  const QWidget *top = window();
  return top ? top->windowHandle() : nullptr;
}

QRect ScrollCapturePanel::regionPhysical() const {
  // Round the edges, not the extents: rounding x and width independently can
  // shift the crop against the visible hole by a pixel at fractional scales,
  // stitching a stationary overlay-edge column into every band.
  const qreal scale = monitor_.scale > 0 ? monitor_.scale : 1.0;
  const int left = qRound(region_.x() * scale);
  const int top = qRound(region_.y() * scale);
  const int right = qRound((region_.x() + region_.width()) * scale);
  const int bottom = qRound((region_.y() + region_.height()) * scale);
  return QRect(left, top, std::max(1, right - left), std::max(1, bottom - top));
}

void ScrollCapturePanel::setStatus(const QString &status, bool warning) {
  status_ = status;
  statusWarning_ = warning;
  update();
}

void ScrollCapturePanel::postStalled() {
  // Called from the worker thread when an auto capture stops before the end.
  QMetaObject::invokeMethod(
      this,
      [this] {
        if (phase_ != Phase::Capturing || mode_ != Mode::Auto)
          return;
        autoStalled_ = true;
        applyInputRegion(); // the row grew by a pill
        update();
      },
      Qt::QueuedConnection);
}

void ScrollCapturePanel::postStatus(const QString &status, bool warning) {
  // Called from the worker thread; hop to the UI thread. A queued update can
  // arrive after Done stopped the worker, and it must not overwrite the
  // finishing/final status.
  QMetaObject::invokeMethod(
      this,
      [this, status, warning] {
        if (phase_ == Phase::Capturing)
          setStatus(status, warning);
      });
}

QVector<QRect> ScrollCapturePanel::chromeRects() const {
  QVector<QRect> rects;
  if (phase_ == Phase::Selected) {
    for (int index = 0; index < kModeButtonCount; ++index)
      rects.push_back(modeButtonRect(index));
    rects.push_back(selectedCancelButtonRect());
    for (const auto &[grip, which] : gripRects())
      rects.push_back(grip);
    return rects;
  }
  rects.push_back(doneButtonRect());
  rects.push_back(backButtonRect());
  rects.push_back(cancelButtonRect());
  if (autoStalled_)
    rects.push_back(continueButtonRect());
  return rects;
}

void ScrollCapturePanel::applyInputRegion() {
  QWindow *window = surfaceWindow();
  if (!window)
    return;
  // The region is exposed from the moment it is chosen, so the page can be
  // scrolled into place without losing it.
  if (phase_ != Phase::Capturing && phase_ != Phase::Selected) {
    window->setMask(QRegion()); // whole surface takes input
    return;
  }
  window->setMask(scrollOverlayInputRegion(rect(), region_, chromeRects()));
}

void ScrollCapturePanel::updateKeyboardZone(const QPoint &point) {
  // The grab pins pointer focus to this layer, which is what stops the wheel
  // reaching the page, so it is held only while the pointer is on our own
  // chrome, and only once a real event has said so. Assuming the pointer was on
  // the chrome is what broke scrolling before: a Wayland client cannot ask
  // where the pointer is, so the answer here always comes from an event.
  setKeyboardGrab(!region_.contains(point));
}

void ScrollCapturePanel::setKeyboardGrab(bool grab) {
  // Hyprland pins pointer focus to a layer that holds an exclusive keyboard
  // grab, even over an input-region hole, so while the grab is held the wheel
  // never reaches the page. Once a region exists the page has to be
  // scrollable, so the grab goes away for that whole phase instead of being
  // handed back and forth as the pointer crosses the region's edge: that
  // handoff depended on knowing where the pointer was, and a Wayland client
  // cannot ask. Restoring a region put the pointer somewhere we had never
  // seen it move, and the grab stayed on. The pills are the controls from
  // then on.
  if (!layer_ || keyboardGrabbed_ == grab)
    return;
  keyboardGrabbed_ = grab;
  layer_->setKeyboardInteractivity(
      grab ? LayerShellQt::Window::KeyboardInteractivityExclusive
           : LayerShellQt::Window::KeyboardInteractivityNone);
}

// ---- capture -----------------------------------------------------------------

void ScrollCapturePanel::startCapture(Mode mode, stitch::Axis axis) {
  if (region_.width() < kMinRegion || region_.height() < kMinRegion)
    return;
  mode_ = mode;
  axis_ = axis;
  auto worker = std::make_unique<Worker>(axis_);
  QString error;
  if (!worker->output.open(monitor_.name, error)) {
    setStatus(QStringLiteral("Output capture failed: %1").arg(error), true);
    return;
  }
  worker->regionPhysical = regionPhysical().intersected(
      QRect(QPoint(), worker->output.bufferSize()));
  worker->debugDir = qEnvironmentVariable("OMASNAP_SCROLL_DEBUG_DIR");
  worker_ = std::move(worker);
  phase_ = Phase::Capturing;
  applyInputRegion();
  // The move puck lives inside the region, so the frame on screen right now
  // still has it. Paint the phase change first and let it be shown: the
  // capture reads the screen back, so it must not start until the screen no
  // longer has any of our chrome on the part being captured.
  repaint();
  // Drop the exclusive keyboard grab for the whole capture. Hyprland pins
  // pointer focus to a layer that holds an exclusive grab (even over an
  // input-region hole), which stops the wheel reaching the page; with the grab
  // released the pointer is never pinned, so scrolling the exposed page always
  // works. Finish/cancel run off the on-screen buttons (mouse clicks, governed
  // by the input region, not the keyboard).
  setKeyboardGrab(false);
  stopRequested_ = false;
  if (mode_ == Mode::Manual) {
    setStatus(QStringLiteral("Scroll the page · Done stitches it"));
    // A couple of frames after the repaint above, so the compositor has
    // presented the puck-less frame before the first grab reads it back.
    QTimer::singleShot(kChromeSettleMs, this, [this] {
      if (phase_ != Phase::Capturing)
        return;
      workerFuture_ = QtConcurrent::run([this] { captureLoop(); });
    });
    return;
  }
  // Automatic: the injection worker scrolls one acknowledged tick at a time.
  injectorStop_ = std::make_shared<std::atomic<bool>>(false);
  handshake_ = std::make_shared<stitch::CaptureHandshake>();
  const auto [parkX, parkY] = autoScrollParkPoint();
  setStatus(QStringLiteral("Auto-scrolling… · keep the pointer still · "
                           "Done stitches it"));
  // Spawn after this frame's commit so the input-region hole and the released
  // keyboard land before the pointer warp.
  QTimer::singleShot(
      kChromeSettleMs, this,
      [this, parkX, parkY] {
        if (phase_ != Phase::Capturing)
          return;
        QString spawnError;
        if (!spawnScrollInjector(injectorStop_, handshake_, parkX, parkY,
                                 axis_, monitor_.name, spawnError)) {
          // Fall back to manual capture on the same region and axis.
          qInfo().noquote()
              << QStringLiteral("scroll: injector unavailable (%1)").arg(spawnError);
          mode_ = Mode::Manual;
          setStatus(QStringLiteral("Auto-scroll unavailable · scroll "
                                   "manually · Done stitches"),
                    true);
          workerFuture_ = QtConcurrent::run([this] { captureLoop(); });
          return;
        }
        workerFuture_ = QtConcurrent::run([this] { autoCaptureLoop(); });
      });
}

void ScrollCapturePanel::stopWorker() {
  stopRequested_ = true;
  if (injectorStop_)
    injectorStop_->store(true, std::memory_order_release);
  if (workerFuture_.isRunning())
    workerFuture_.waitForFinished();
}

void ScrollCapturePanel::captureLoop() {
  using Event = stitch::ManualCapture::Event;
  Worker &w = *worker_;
  QString error;
  bool captureFull = false;
  while (!stopRequested_) {
    QImage frame;
    if (!w.output.grab(frame, error, kGrabTimeoutMs)) {
      if (w.output.sessionStopped()) {
        // The compositor ended the session (output disconnected, mode
        // change); no retry can succeed. Leave what was captured for Done.
        postStatus(QStringLiteral("Screen capture stopped · press Done to "
                                  "stitch what was captured, or Cancel"),
                   true);
        break;
      }
      // A timeout means a quiet screen; an immediate failure (dead connection,
      // repeated frame failures) must not busy-spin, so pace the retries and
      // tell the user if it persists.
      QThread::msleep(kCaptureIntervalMs);
      if (++w.consecutiveFailures == 50)
        postStatus(QStringLiteral("Screen capture is not delivering frames: "
                                  "%1").arg(error),
                   true);
      continue;
    }
    w.consecutiveFailures = 0;
    if (stopRequested_)
      break;
    ++w.grabbed;
    const QImage cropped = frame.copy(w.regionPhysical)
                               .convertToFormat(QImage::Format_RGBA8888);
    if (!w.debugDir.isEmpty() && w.grabbed % 8 == 0)
      cropped.save(w.debugDir + QStringLiteral("/grab-%1-crop.png")
                                   .arg(w.grabbed, 3, 10, QChar('0')),
                   "PNG");
    const bool wasStarted = w.session.started();
    const stitch::ManualCapture::Outcome out = w.session.feed(cropped);
    if (!wasStarted && w.session.started())
      w.firstCrop = cropped;
    w.lastCrop = cropped;
    if (!w.debugDir.isEmpty())
      qInfo().noquote() << QStringLiteral("scroll grab %1: %2 motion=%3(%4) err=%5 conf=%6 kept=%7 pending=%8")
                               .arg(w.grabbed).arg(QString::fromLatin1(eventName(out.event)))
                               .arg(static_cast<int>(out.estimate.motion.kind))
                               .arg(out.estimate.motion.delta)
                               .arg(out.estimate.error, 0, 'f', 2)
                               .arg(out.estimate.confidence, 0, 'f', 2)
                               .arg(out.keptFrames).arg(out.pendingDelta);
    const QString frames = QStringLiteral("%1 frame%2")
                               .arg(out.keptFrames)
                               .arg(out.keptFrames == 1 ? QString() : QStringLiteral("s"));
    switch (out.event) {
    case Event::Blank:
      if (!out.error.isEmpty())
        postStatus(QStringLiteral("Capture shows only a solid color · "
                                  "Cancel and select a different region"),
                   true);
      break;
    case Event::Seeded:
      postStatus(QStringLiteral("Capturing · 1 frame · scroll the page · Done when finished"));
      break;
    case Event::Kept:
    case Event::Pending:
    case Event::PendingDropped:
      // Past the advisory edge the capture keeps going; say so plainly and
      // keep saying it, rather than firing an alert that the editor then
      // contradicts by opening the result perfectly well.
      postStatus(QStringLiteral("Capturing · %1%2 · Done when finished")
                     .arg(frames)
                     .arg(w.session.exceedsWidelyOpenableEdge()
                              ? QStringLiteral(" · very long: fine here, "
                                               "some apps may not open it")
                              : QString()));
      break;
    case Event::Still:
      break;
    case Event::ReSeeded:
      postStatus(QStringLiteral("Capturing · restarted from here (content changed in place) · scroll the page"));
      break;
    case Event::Ambiguous:
      postStatus(QStringLiteral("Repeated content · keep scrolling to a distinctive part"), true);
      break;
    case Event::Unmatchable:
      postStatus(QStringLiteral("Can't align · scroll back a little; moving content (video) can't be captured"), true);
      break;
    case Event::WrongDirection:
      postStatus(QStringLiteral("Scroll the other way to continue this capture (or Done to stitch what you have)"), true);
      break;
    case Event::Error:
      postStatus(QStringLiteral("Could not add frame: %1").arg(out.error), true);
      break;
    case Event::Full:
      // A designed limit. Stop like a finished capture rather than refusing a
      // frame every tick: what was captured is intact and Done stitches it.
      postStatus(QStringLiteral("Capture is as long as it can get · press Done "
                                "to stitch it, or Cancel"),
                 true);
      captureFull = true;
      break;
    }
    if (captureFull)
      break;
    QThread::msleep(kCaptureIntervalMs);
  }
}

void ScrollCapturePanel::autoCaptureLoop() {
  using Event = stitch::AutoCapture::Event;
  using Ack = stitch::AutoCapture::Ack;
  Worker &w = *worker_;
  QString error;
  while (!stopRequested_) {
    if (injectorStop_->load(std::memory_order_acquire) &&
        !w.autoSession.reachedEnd() && !w.autoSession.halted()) {
      postStatus(QStringLiteral("Auto-scroll stopped · Continue picks it back "
                                "up, Done stitches it"),
                 true);
      postStalled();
      break;
    }
    const std::uint64_t cycle = handshake_->readyCycle();
    if (cycle == 0 || cycle == w.lastCycle) {
      QThread::msleep(20);
      continue;
    }
    QImage cropped;
    if (!w.acquireSettledFrame(w.lastCrop, cropped, error)) {
      // Never acknowledge on a failed grab: the worker holds this cycle and
      // the same stable screen is retried.
      if (w.output.sessionStopped()) {
        postStatus(QStringLiteral("Screen capture stopped · Done stitches "
                                  "what was captured"),
                   true);
        break;
      }
      if (++w.consecutiveFailures >= 3)
        postStatus(QStringLiteral("Screen capture is failing: %1").arg(error),
                   true);
      QThread::msleep(20);
      continue;
    }
    w.consecutiveFailures = 0;
    ++w.grabbed;
    if (!w.debugDir.isEmpty())
      cropped.save(w.debugDir + QStringLiteral("/grab-%1-crop.png")
                                   .arg(w.grabbed, 3, 10, QChar('0')),
                   "PNG");
    const stitch::AutoCapture::Outcome out = w.autoSession.feed(cropped);
    if (!w.firstCrop.isNull() || out.event == Event::Seeded)
      w.lastCrop = cropped;
    if (out.event == Event::Seeded)
      w.firstCrop = cropped;
    qInfo().noquote() << QStringLiteral("auto grab %1 cycle %2: event=%3 ack=%4 "
                                        "delta=%5/%6/%7 halt=%8 kept=%9 "
                                        "notches=%10")
                             .arg(w.grabbed)
                             .arg(cycle)
                             .arg(static_cast<int>(out.event))
                             .arg(static_cast<int>(out.ack))
                             .arg(out.estimate.motion.delta)
                             .arg(out.firstDelta)
                             .arg(out.secondDelta)
                             .arg(static_cast<int>(out.haltReason))
                             .arg(w.autoSession.keptFrames())
                             .arg(w.notchesPerTick);
    if (out.event == Event::Blank) {
      // Not consumed: retry the same cycle once the overlay's paint settles.
      QThread::msleep(20);
      continue;
    }
    w.lastCycle = cycle;
    // Fit the step to the page after the first verified forward step: one
    // notch moves a different distance in every application (and with every
    // scroll_factor), and the stitcher needs the frames to overlap by about
    // half whatever that turns out to be.
    // A verified step arrives as Appended (its delta in the estimate) or,
    // after a probe, as Committed (first tick then the one-notch probe: the
    // probe is the cleaner per-notch figure).
    double perNotch = 0.0;
    if (out.event == Event::Appended && out.estimate.motion.delta > 0)
      perNotch = static_cast<double>(out.estimate.motion.delta) / w.notchesPerTick;
    else if (out.event == Event::Committed && out.secondDelta > 0)
      perNotch = out.secondDelta / static_cast<double>(stitch::kProbeNotches);
    else if (out.event == Event::Committed && out.firstDelta > 0)
      perNotch = static_cast<double>(out.firstDelta) / w.notchesPerTick;
    if (!w.notchesFitted && perNotch > 0.0) {
      const int extent = axis_ == stitch::Axis::Vertical
                             ? w.regionPhysical.height()
                             : w.regionPhysical.width();
      const int fitted = static_cast<int>(
          std::lround(extent * kTargetStepFraction / std::max(perNotch, 1.0)));
      w.notchesPerTick = std::clamp(fitted, 1, kMaxNotchesPerTick);
      w.notchesFitted = true;
      qInfo().noquote() << QStringLiteral("auto: %1 px per notch, region %2 px, "
                                          "%3 notches per tick from here")
                               .arg(perNotch, 0, 'f', 1)
                               .arg(extent)
                               .arg(w.notchesPerTick);
    }
    switch (out.ack) {
    case Ack::Normal:
      handshake_->acknowledgeWithNotches(cycle, w.notchesPerTick);
      break;
    case Ack::Probe:
      handshake_->acknowledgeWithNotches(cycle, 1);
      break;
    case Ack::Hold:
      break;
    }
    switch (out.event) {
    case Event::Seeded:
    case Event::Appended:
    case Event::Committed:
      postStatus(QStringLiteral("Auto-scrolling · %1 frame%2%3 · Done stitches")
                     .arg(w.autoSession.keptFrames())
                     .arg(w.autoSession.keptFrames() == 1 ? QString()
                                                          : QStringLiteral("s"))
                     .arg(w.autoSession.exceedsWidelyOpenableEdge()
                              ? QStringLiteral(" · very long: fine here, "
                                               "some apps may not open it")
                              : QString()));
      break;
    case Event::ProbeStarted:
    case Event::ProbeAgain:
      postStatus(QStringLiteral("Verifying scroll alignment…"));
      break;
    case Event::StillOnce:
      postStatus(QStringLiteral("Confirming end of content…"));
      break;
    case Event::ReachedEnd:
    case Event::ReachedEndAtSeam:
      // A page that ended and a page that stopped moving because the pointer
      // left the frame look the same from here, so this offers Continue
      // either way: on a real end it simply concludes again.
      injectorStop_->store(true, std::memory_order_release);
      postStatus(QStringLiteral("End reached · %1 frames · Continue carries "
                                "on, Done stitches it")
                     .arg(w.autoSession.keptFrames()));
      postStalled();
      return;
    case Event::Paused:
      // Keep only verified content and hand control back. What is verified
      // stays in the session, so Continue can pick it up from wherever the
      // page is now.
      injectorStop_->store(true, std::memory_order_release);
      w.autoSession.abandonPause();
      postStatus(QStringLiteral("Auto-scroll paused: capture lost alignment · "
                                "Continue tries again, Done stitches what was "
                                "verified"),
                 true);
      postStalled();
      return;
    case Event::Halted: {
      injectorStop_->store(true, std::memory_order_release);
      QString reason;
      switch (out.haltReason) {
      case stitch::AutoCapture::HaltReason::LostAlignment:
        reason = QStringLiteral("Stopped: capture lost alignment");
        break;
      case stitch::AutoCapture::HaltReason::MovedBackward:
        reason = QStringLiteral("Stopped: content moved backward");
        break;
      case stitch::AutoCapture::HaltReason::Unmatchable:
        reason = QStringLiteral("Stopped: retry with slower scrolling");
        break;
      case stitch::AutoCapture::HaltReason::BlankFrames:
        reason = QStringLiteral("Capture shows only a solid color: retry");
        break;
      case stitch::AutoCapture::HaltReason::ReachedLimit:
        reason = QStringLiteral("Capture is as long as it can get");
        break;
      default:
        reason = QStringLiteral("Capture failed: %1").arg(out.error);
        break;
      }
      postStatus(reason + QStringLiteral(" · Done stitches what was captured"),
                 true);
      return;
    }
    case Event::Blank:
      break;
    }
    QThread::msleep(20);
  }
}

void ScrollCapturePanel::finishCapture() {
  if (phase_ != Phase::Capturing)
    return;
  phase_ = Phase::Finishing;
  setStatus(QStringLiteral("Stitching…"));
  stopWorker();
  Worker &w = *worker_;
  QString error;
  const bool started =
      mode_ == Mode::Auto ? w.autoSession.started() : w.session.started();
  if (!started) {
    cancel();
    return;
  }
  QImage assembled = mode_ == Mode::Auto ? w.autoSession.finish(error)
                                        : w.session.finish(error);
  if (assembled.isNull()) {
    phase_ = Phase::Capturing;
    setStatus(QStringLiteral("Stitch failed: %1").arg(error), true);
    // The worker's state is intact; restart the (manual) loop so the user can
    // continue, since an auto capture that failed to stitch has already
    // stopped.
    if (mode_ == Mode::Manual) {
      stopRequested_ = false;
      workerFuture_ = QtConcurrent::run([this] { captureLoop(); });
    }
    return;
  }
  if (mode_ == Mode::Auto && w.autoSession.unverifiedSeams() > 0)
    qWarning().noquote()
        << QStringLiteral("scroll: capture may contain %1 repeated or missing "
                          "section(s)")
               .arg(w.autoSession.unverifiedSeams());
  const QImage result = assembled.convertToFormat(QImage::Format_ARGB32);
  if (!w.debugDir.isEmpty()) {
    result.save(w.debugDir + QStringLiteral("/scroll-stitched.png"), "PNG");
    if (!w.firstCrop.isNull())
      w.firstCrop.save(w.debugDir + QStringLiteral("/scroll-first-frame.png"), "PNG");
    if (!w.lastCrop.isNull())
      w.lastCrop.save(w.debugDir + QStringLiteral("/scroll-last-frame.png"), "PNG");
  }
  const int kept = mode_ == Mode::Auto ? w.autoSession.keptFrames()
                                       : w.session.keptFrames();
  qInfo().noquote() << QStringLiteral("scroll: stitched %1 frames into %2x%3")
                           .arg(kept).arg(result.width()).arg(result.height());
  phase_ = Phase::Finished;
  emit stitched(result);
}

void ScrollCapturePanel::switchMode(Mode mode) {
  // Wrong mode is the same mistake as the wrong direction: keep the region,
  // throw the frames away, start again the other way.
  if (phase_ == Phase::Selected) {
    startCapture(mode, axis_);
    return;
  }
  if (phase_ != Phase::Capturing || mode == mode_)
    return;
  const stitch::Axis axis = axis_;
  stopWorker();
  worker_.reset();
  injectorStop_.reset();
  handshake_.reset();
  stopRequested_ = false;
  phase_ = Phase::Selected;
  startCapture(mode, axis);
}

std::pair<int, int> ScrollCapturePanel::autoScrollParkPoint() const {
  // Bottom-right corner, inset 10% of the region so the point stays inside
  // it: a corner is more often page margin than a centre, which is where
  // hero content (a video, an embed) tends to sit and swallow the wheel
  // once the page has scrolled it under the parked point.
  const qreal scale = monitor_.scale > 0 ? monitor_.scale : 1.0;
  const qreal insetX = region_.width() * 0.10;
  const qreal insetY = region_.height() * 0.10;
  const int x = qRound((region_.x() + region_.width() - insetX) * scale);
  const int y = qRound((region_.y() + region_.height() - insetY) * scale);
  return {x, y};
}

void ScrollCapturePanel::continueCapture() {
  // Picks the same capture back up: the session keeps every band it already
  // has, so this carries on from the last one rather than starting a second
  // capture of the same page. The fresh injector parks the pointer back
  // inside the frame.
  if (phase_ != Phase::Capturing || !worker_ || mode_ != Mode::Auto)
    return;
  autoStalled_ = false;
  stopRequested_ = false;
  // Pressing Continue means the pointer was on our chrome, which is where we
  // hold the keyboard, and Hyprland pins pointer focus to a layer that holds
  // it, so the injected wheel would land on us instead of the page. Let it go
  // before parking the pointer back inside the frame.
  setKeyboardGrab(false);
  worker_->autoSession.resumeFromEnd(); // a stop looks just like an end
  worker_->lastCycle = 0; // a new handshake counts from one again
  injectorStop_ = std::make_shared<std::atomic<bool>>(false);
  handshake_ = std::make_shared<stitch::CaptureHandshake>();
  const auto [parkX, parkY] = autoScrollParkPoint();
  setStatus(QStringLiteral("Auto-scrolling… · keep the pointer still · "
                           "Done stitches it"));
  update();
  QString spawnError;
  if (!spawnScrollInjector(injectorStop_, handshake_, parkX, parkY, axis_,
                           monitor_.name, spawnError)) {
    autoStalled_ = true;
    setStatus(QStringLiteral("Could not start auto-scroll again: %1")
                  .arg(spawnError),
              true);
    return;
  }
  workerFuture_ = QtConcurrent::run([this] { autoCaptureLoop(); });
}

void ScrollCapturePanel::returnToModeChoice() {
  // Throw the frames away and go back to the mode row with the region intact.
  if (phase_ != Phase::Capturing)
    return;
  stopWorker();
  worker_.reset();
  injectorStop_.reset();
  handshake_.reset();
  stopRequested_ = false;
  phase_ = Phase::Selected;
  applyInputRegion();
  setKeyboardGrab(false);
  setStatus(QStringLiteral("The page inside is live · scroll it into position, "
                           "then choose a mode"));
  update();
}

void ScrollCapturePanel::cancel() {
  stopWorker();
  phase_ = Phase::Finished;
  emit dismissed();
}

// ---- chrome ------------------------------------------------------------------

int ScrollCapturePanel::capturePillCount() const {
  return autoStalled_ ? 4 : 3;
}

QRect ScrollCapturePanel::doneButtonRect() const {
  return scrollOverlayPillRect(rect(), region_, capturePillCount(), 0, 132, 40,
                               12);
}

QRect ScrollCapturePanel::continueButtonRect() const {
  if (!autoStalled_)
    return {};
  return scrollOverlayPillRect(rect(), region_, capturePillCount(), 1, 132, 40,
                               12);
}
QVector<QPair<QString, QString>> ScrollCapturePanel::legendEntries() const {
  // Once a region exists the keyboard belongs to the page, which is what makes
  // it scrollable, so nothing here may promise a key. The buttons on screen
  // are the controls, and they say so themselves.
  return {};
}

QVector<QPair<QRect, ScrollCapturePanel::Grip>>
ScrollCapturePanel::gripRects() const {
  // Every grip lives in the band *outside* the region, never over it: the
  // capture is that rectangle of the screen, so a bracket drawn inside it is a
  // bracket stitched into the result. Each corner is two arms, which is what
  // makes the bracket shape and keeps every rect clear of the region.
  if (phase_ != Phase::Selected || region_.isEmpty())
    return {};
  const int arm = 34;
  const int barLong = 62;
  const QPoint center = region_.center();
  const int outerTop = region_.top() - kGripBand;
  const int outerLeft = region_.left() - kGripBand;
  const int belowBottom = region_.bottom() + 1;
  const int pastRight = region_.right() + 1;
  const auto across = [&](int x, int y) { return QRect(x, y, arm, kGripBand); };
  const auto down = [&](int x, int y) { return QRect(x, y, kGripBand, arm); };
  return {
      {across(outerLeft, outerTop), Grip::TopLeft},
      {down(outerLeft, outerTop), Grip::TopLeft},
      {across(region_.right() - arm + kGripBand + 1, outerTop),
       Grip::TopRight},
      {down(pastRight, outerTop), Grip::TopRight},
      {across(region_.right() - arm + kGripBand + 1, belowBottom),
       Grip::BottomRight},
      {down(pastRight, region_.bottom() - arm + kGripBand + 1),
       Grip::BottomRight},
      {across(outerLeft, belowBottom), Grip::BottomLeft},
      {down(outerLeft, region_.bottom() - arm + kGripBand + 1),
       Grip::BottomLeft},
      {QRect(center.x() - barLong / 2, outerTop, barLong, kGripBand),
       Grip::Top},
      {QRect(center.x() - barLong / 2, belowBottom, barLong, kGripBand),
       Grip::Bottom},
      {QRect(outerLeft, center.y() - barLong / 2, kGripBand, barLong),
       Grip::Left},
      {QRect(pastRight, center.y() - barLong / 2, kGripBand, barLong),
       Grip::Right},
      // The puck is the one grip that has to sit inside the region, because the
      // middle is what it moves. It is drawn only while a mode is being chosen,
      // and a capture does not start until a frame without it has been painted
      // and shown, see startCapture.
      {QRect(center.x() - 22, center.y() - 22, 44, 44), Grip::Move},
  };
}

Qt::CursorShape ScrollCapturePanel::gripCursor(Grip grip) {
  switch (grip) {
  case Grip::TopLeft:
  case Grip::BottomRight:
    return Qt::SizeFDiagCursor;
  case Grip::TopRight:
  case Grip::BottomLeft:
    return Qt::SizeBDiagCursor;
  case Grip::Top:
  case Grip::Bottom:
    return Qt::SizeVerCursor;
  case Grip::Left:
  case Grip::Right:
    return Qt::SizeHorCursor;
  case Grip::Move:
    return Qt::SizeAllCursor;
  case Grip::None:
    break;
  }
  return Qt::ArrowCursor;
}

ScrollCapturePanel::Grip
ScrollCapturePanel::gripAt(const QPoint &point) const {
  if (phase_ != Phase::Selected || region_.isEmpty())
    return Grip::None;
  // Corners are listed first and sides overlap them at the ends, so the first
  // match wins: a corner is never stolen by the bar beside it.
  for (const auto &[rect, grip] : gripRects()) {
    if (rect.contains(point))
      return grip;
  }
  // The border moves the region as well: the puck is the obvious way, the
  // border is the one already under your hand after a resize.
  const QRect band =
      region_.adjusted(-kGripBand, -kGripBand, kGripBand, kGripBand);
  if (band.contains(point) && !region_.contains(point))
    return Grip::Move;
  return Grip::None;
}

void ScrollCapturePanel::applyGrip(const QPoint &point) {
  const QRect surface = rect();
  const QPoint delta = point - gripStartPoint_;
  QRect updated = gripStartRegion_;
  if (activeGrip_ == Grip::Move) {
    updated.moveTo(gripStartRegion_.topLeft() + delta);
    // Clamped whole, so dragging past an edge slides along it instead of
    // shrinking the region.
    updated.moveLeft(std::clamp(updated.left(), surface.left(),
                                surface.right() - updated.width() + 1));
    updated.moveTop(std::clamp(updated.top(), surface.top(),
                               surface.bottom() - updated.height() + 1));
    region_ = updated;
    return;
  }
  const bool movesLeft = activeGrip_ == Grip::TopLeft ||
                         activeGrip_ == Grip::Left ||
                         activeGrip_ == Grip::BottomLeft;
  const bool movesRight = activeGrip_ == Grip::TopRight ||
                          activeGrip_ == Grip::Right ||
                          activeGrip_ == Grip::BottomRight;
  const bool movesTop = activeGrip_ == Grip::TopLeft ||
                        activeGrip_ == Grip::Top ||
                        activeGrip_ == Grip::TopRight;
  const bool movesBottom = activeGrip_ == Grip::BottomLeft ||
                           activeGrip_ == Grip::Bottom ||
                           activeGrip_ == Grip::BottomRight;
  // Clamped against the edge that stays, so a region dragged through itself
  // stops at the minimum rather than turning inside out.
  if (movesLeft)
    updated.setLeft(std::clamp(point.x(), surface.left(),
                               gripStartRegion_.right() - kMinRegion));
  if (movesRight)
    updated.setRight(std::clamp(point.x(),
                                gripStartRegion_.left() + kMinRegion,
                                surface.right()));
  if (movesTop)
    updated.setTop(std::clamp(point.y(), surface.top(),
                              gripStartRegion_.bottom() - kMinRegion));
  if (movesBottom)
    updated.setBottom(std::clamp(point.y(),
                                 gripStartRegion_.top() + kMinRegion,
                                 surface.bottom()));
  region_ = updated;
}

QRect ScrollCapturePanel::backButtonRect() const {
  return scrollOverlayPillRect(rect(), region_, capturePillCount(),
                               autoStalled_ ? 2 : 1, 132, 40, 12);
}
QRect ScrollCapturePanel::cancelButtonRect() const {
  return scrollOverlayPillRect(rect(), region_, capturePillCount(),
                               autoStalled_ ? 3 : 2, 132, 40, 12);
}
QRect ScrollCapturePanel::selectedCancelButtonRect() const {
  return modeButtonRect(kModeButtonCount);
}
QRect ScrollCapturePanel::modeButtonRect(int index) const {
  // The mode pills plus an explicit Cancel: Esc alone cannot be the way out,
  // because the keyboard belongs to the page whenever the pointer is over it.
  return scrollOverlayPillRect(rect(), region_, kModeButtonCount + 1, index,
                               kModeButtonWidth, kModeButtonHeight,
                               kModeButtonGap);
}
int ScrollCapturePanel::modeButtonAt(const QPoint &point) const {
  for (int index = 0; index < kModeButtonCount; ++index)
    if (modeButtonRect(index).contains(point))
      return index;
  return -1;
}

void ScrollCapturePanel::paintEvent(QPaintEvent *) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  {
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect(), kDim);
    painter.fillRect(region_, Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    // Drawn first, low-opacity, no card: the live page, the region outline,
    // the pills paint over it wherever they overlap.
    drawHotkeyLegend(painter, rect(), legendEntries());
    painter.setPen(QPen(statusWarning_ ? kWarn : kAccent, 2));
    painter.setBrush(Qt::NoBrush);
    // Fully outside the region so no overlay pixel lands in the capture.
    painter.drawRect(region_.adjusted(-3, -3, 3, 3));

    // Grips live in the band outside the region: that rectangle is the
    // capture, so anything drawn inside it would be captured.
    if (phase_ == Phase::Selected) {
      const int thickness = 4;
      painter.setPen(Qt::NoPen);
      painter.setBrush(QColor(255, 255, 255, 235));
      for (const auto &[grip, which] : gripRects()) {
        if (which == Grip::Move) {
          const QPointF middle = grip.center();
          painter.setBrush(QColor(20, 20, 26, 215));
          painter.setPen(QPen(QColor(255, 255, 255, 235), 2));
          painter.drawEllipse(middle, grip.width() / 2.0 - 2,
                              grip.height() / 2.0 - 2);
          painter.drawLine(middle + QPointF(-9, 0), middle + QPointF(9, 0));
          painter.drawLine(middle + QPointF(0, -9), middle + QPointF(0, 9));
          painter.setPen(Qt::NoPen);
          painter.setBrush(QColor(255, 255, 255, 235));
          continue;
        }
        if (grip.bottom() < region_.top())
          painter.drawRect(QRect(grip.left(), grip.bottom() - thickness + 1,
                                 grip.width(), thickness));
        else if (grip.top() > region_.bottom())
          painter.drawRect(
              QRect(grip.left(), grip.top(), grip.width(), thickness));
        else if (grip.right() < region_.left())
          painter.drawRect(QRect(grip.right() - thickness + 1, grip.top(),
                                 thickness, grip.height()));
        else
          painter.drawRect(
              QRect(grip.left(), grip.top(), thickness, grip.height()));
      }
    }
    QFont buttonFont = painter.font();
    buttonFont.setPixelSize(15);
    buttonFont.setBold(true);
    painter.setFont(buttonFont);
    if (phase_ == Phase::Selected) {
      for (int index = 0; index < kModeButtonCount; ++index) {
        const QRect button = modeButtonRect(index);
        painter.setPen(Qt::NoPen);
        painter.setBrush(kModeButtons[index].automatic
                             ? kAccent
                             : QColor(40, 40, 48, 240));
        painter.drawRoundedRect(button, 8, 8);
        painter.setPen(Qt::white);
        painter.drawText(button, Qt::AlignCenter,
                         QString::fromUtf8(kModeButtons[index].label));
      }
      const QRect cancelSlot = selectedCancelButtonRect();
      painter.setPen(Qt::NoPen);
      painter.setBrush(QColor(40, 40, 48, 240));
      painter.drawRoundedRect(cancelSlot, 8, 8);
      painter.setPen(Qt::white);
      painter.drawText(cancelSlot, Qt::AlignCenter, QStringLiteral("Cancel"));
    } else {
      const QRect done = doneButtonRect();
      const QRect backRect = backButtonRect();
      const QRect cancelRect = cancelButtonRect();
      painter.setPen(Qt::NoPen);
      painter.setBrush(kAccent);
      painter.drawRoundedRect(done, 8, 8);
      painter.setBrush(QColor(40, 40, 48, 240));
      painter.drawRoundedRect(backRect, 8, 8);
      painter.drawRoundedRect(cancelRect, 8, 8);
      painter.setPen(Qt::white);
      painter.drawText(done, Qt::AlignCenter, QStringLiteral("Done · stitch"));
      painter.drawText(backRect, Qt::AlignCenter, QStringLiteral("Back"));
      painter.drawText(cancelRect, Qt::AlignCenter, QStringLiteral("Cancel"));
      if (autoStalled_) {
        const QRect resume = continueButtonRect();
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(40, 40, 48, 240));
        painter.drawRoundedRect(resume, 8, 8);
        painter.setPen(Qt::white);
        painter.drawText(resume, Qt::AlignCenter, QStringLiteral("Continue"));
      }
    }
  }
  drawStatusPill(painter, rect(), status_);
}

void ScrollCapturePanel::wheelEvent(QWheelEvent *event) {
  // The hole passes the wheel to the page; the overlay only sees wheel over its
  // own chrome. Log it (debug runs), during capture a wheel event arriving with
  // the pointer inside the region would mean the input-region hole is not in
  // effect.
  if (worker_ && !worker_->debugDir.isEmpty())
    qInfo().noquote() << QStringLiteral("scroll: overlay wheel at %1,%2 phase=%3")
                             .arg(event->position().x())
                             .arg(event->position().y())
                             .arg(static_cast<int>(phase_));
  QWidget::wheelEvent(event);
}

void ScrollCapturePanel::enterEvent(QEnterEvent *event) {
  updateKeyboardZone(event->position().toPoint());
  if (worker_ && !worker_->debugDir.isEmpty())
    qInfo().noquote() << QStringLiteral("scroll: pointer entered overlay at %1,%2")
                             .arg(event->position().x())
                             .arg(event->position().y());
  QWidget::enterEvent(event);
}

void ScrollCapturePanel::leaveEvent(QEvent *event) {
  // The pointer is off our input region entirely, over the page, or off the
  // screen. Either way it is not on our chrome, so the keyboard goes back to
  // whatever is under it and the page can be scrolled again.
  setKeyboardGrab(false);
  if (worker_ && !worker_->debugDir.isEmpty())
    qInfo().noquote() << QStringLiteral("scroll: pointer left overlay");
  QWidget::leaveEvent(event);
}

void ScrollCapturePanel::mousePressEvent(QMouseEvent *event) {
  if (event->button() == Qt::RightButton) {
    cancel();
    return;
  }
  if (event->button() != Qt::LeftButton)
    return;
  if (phase_ == Phase::Capturing) {
    const QPoint point = event->position().toPoint();
    if (doneButtonRect().contains(point))
      finishCapture();
    else if (autoStalled_ && continueButtonRect().contains(point))
      continueCapture();
    else if (backButtonRect().contains(point))
      returnToModeChoice();
    else if (cancelButtonRect().contains(point))
      cancel();
    return; // clicks elsewhere in the chrome do nothing
  }
  if (phase_ == Phase::Selected) {
    const QPoint point = event->position().toPoint();
    if (selectedCancelButtonRect().contains(point)) {
      cancel();
      return;
    }
    const int mode = modeButtonAt(point);
    if (mode >= 0) {
      startCapture(kModeButtons[mode].automatic ? Mode::Auto : Mode::Manual,
                   kModeButtons[mode].axis);
      update();
      return;
    }
    if (const Grip grip = gripAt(point); grip != Grip::None) {
      activeGrip_ = grip;
      gripStartRegion_ = region_;
      gripStartPoint_ = point;
      return;
    }
    // A press inside the region belongs to the page, the input region should
    // have sent it there. If one reaches the overlay anyway (a mask that has
    // not been committed yet, or the pixel or two beneath the outline), it must
    // not be read as "start over": losing the region to a click meant for the
    // page is the one thing this phase cannot do.
    if (region_.adjusted(-6, -6, 6, 6).contains(point)) {
      applyInputRegion();
      return;
    }
    // Anywhere else, that is, anywhere on the chrome, asks for a fresh
    // region: the editor's own selection takes it from here.
    stopWorker();
    phase_ = Phase::Finished;
    emit dismissed();
    return;
  }
}

void ScrollCapturePanel::mouseMoveEvent(QMouseEvent *event) {
  const QPoint point = event->position().toPoint();
  updateKeyboardZone(point);
  if (activeGrip_ != Grip::None) {
    // The button is down, so motion keeps arriving even over the hole.
    applyGrip(point);
    applyInputRegion(); // the hole follows the region as it is dragged
    update();
    return;
  }
  if (phase_ == Phase::Selected)
    setCursor(gripCursor(gripAt(point)));
}

void ScrollCapturePanel::mouseReleaseEvent(QMouseEvent *event) {
  if (event->button() == Qt::LeftButton && activeGrip_ != Grip::None) {
    activeGrip_ = Grip::None;
    applyInputRegion();
    update();
    return;
  }
}

void ScrollCapturePanel::reserveChromeStrip() {
  // Reserve a strip of real chrome: inside the hole the buttons' clicks fall
  // through and the chrome bakes into the capture, and a full-screen region
  // would empty the input mask entirely.
  constexpr int chromeStrip = 40 + 18 + 12;
  if (region_.top() < chromeStrip && region_.bottom() > height() - chromeStrip)
    region_.setBottom(height() - chromeStrip);
}

void ScrollCapturePanel::begin(const QRect &region) {
  const QRect clamped = region.intersected(rect());
  if (clamped.width() < kMinRegion || clamped.height() < kMinRegion) {
    emit dismissed();
    return;
  }
  region_ = clamped;
  reserveChromeStrip();
  enterSelected();
}

void ScrollCapturePanel::enterSelected() {
  phase_ = Phase::Selected;
  applyInputRegion();
  setCursor(Qt::ArrowCursor); // the grips take it from here
  setKeyboardGrab(false);
  setStatus(QStringLiteral("The page inside is live · scroll it into position, "
                           "then choose a mode"));
  update();
}

void ScrollCapturePanel::keyPressEvent(QKeyEvent *event) {
  if (event->key() == Qt::Key_Escape) {
    cancel();
    return;
  }
  // Enter stitches what has been captured so far, for when the pointer is
  // already on the chrome and the buttons are a reach.
  if (phase_ == Phase::Capturing &&
      (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
    finishCapture();
    return;
  }
  // A shortcut, never the advertised way: only the pointer being on our own
  // chrome puts the keyboard here at all.
  if ((phase_ == Phase::Selected || phase_ == Phase::Capturing) &&
      (event->key() == Qt::Key_S || event->key() == Qt::Key_A)) {
    switchMode(event->key() == Qt::Key_A ? Mode::Auto : Mode::Manual);
    return;
  }
  // While capturing the layer holds no keyboard, so no key arrives here.
  QWidget::keyPressEvent(event);
}
