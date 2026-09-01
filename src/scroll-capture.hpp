/** @fileoverview Manual scroll capture: an overlay that dims the screen around
 *  a chosen region, lets the user scroll the page beneath it, grabs a frame
 *  per scroll step through the live output-capture session, and stitches them
 *  into one tall image with the pure stitcher. Ties together the keyboard-
 *  interactivity zone, ext-image-copy-capture output capture, and stitch.hpp.
 *  The capture loop (grab → crop → classify → accumulate) runs on a worker
 *  thread so the overlay stays responsive. In manual mode small movements
 *  are held as a replaceable pending frame until they grow
 *  past a threshold, ambiguous small motion is recovered with a bounded
 *  re-search, the direction locks on the first committed band, and a frame
 *  that cannot be aligned keeps the last verified reference and warns instead
 *  of silently continuing. */
#pragma once

#include "auto-capture.hpp"
#include "capture.hpp"
#include "overlay-chrome.hpp"
#include "stitch.hpp"

#include <QFuture>
#include <QImage>
#include <QRect>
#include <QPair>
#include <QRegion>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>
#include <optional>

class QWindow;

namespace LayerShellQt {
class Window;
}

/// The overlay's input region whenever a page is exposed: the whole surface
/// minus the chosen region, with the chrome added back so a button that
/// overlaps the region still takes clicks. Pointer input inside the hole goes
/// to the application underneath, which is what lets the page be scrolled into
/// place, and, during capture, scrolled at all.
[[nodiscard]] inline QRegion scrollOverlayInputRegion(
    const QRect &surface, const QRect &page, const QVector<QRect> &chrome) {
  QRegion region(surface);
  region -= page;
  for (const QRect &rect : chrome)
    region += rect.intersected(surface);
  return region;
}

/// One pill in a centered row of `count` pills, laid out just below `region`
/// (above it when there is no room, and never over it), clamped inside
/// `surface`. The chrome has to stay clear of the region: it would otherwise
/// be captured, and its clicks would fall through the input-region hole.
[[nodiscard]] inline QRect scrollOverlayPillRect(const QRect &surface,
                                                 const QRect &region, int count,
                                                 int index, int pillWidth,
                                                 int pillHeight, int gap) {
  const int totalW = count * pillWidth + (count - 1) * gap;
  int x = region.center().x() - totalW / 2;
  x = std::clamp(x, 12, std::max(12, surface.width() - totalW - 12));
  int y = region.bottom() + 18;
  if (y + pillHeight > surface.height() - 12)
    y = region.top() - pillHeight - 18;
  if (y < 12)
    y = 12;
  return QRect(x + index * (pillWidth + gap), y, pillWidth, pillHeight);
}

/// The scroll-capture state of the capture overlay, hosted inside the editor
/// as a child covering the surface. It takes over once a region has been
/// drawn (the editor's own region selection is the selection): choose manual
/// or automatic, scroll, or let the injection worker scroll, and stitch. It
/// manages the layer's input hole and keyboard grab while it is up, and
/// hands back with stitched() or dismissed().
class ScrollCapturePanel final : public QWidget {
  Q_OBJECT
public:
  /// `layer` is the surface the editor lives on; the panel toggles its
  /// keyboard interactivity and input mask while it is up and restores both
  /// on destruction. Null (headless tests) leaves both alone.
  ScrollCapturePanel(MonitorInfo monitor, LayerShellQt::Window *layer,
                     QWidget *parent);
  ~ScrollCapturePanel() override;
  /// Take over with `region` (logical surface pixels, clamped to the
  /// surface and the controls) already drawn. Call once, after show().
  void begin(const QRect &region);
  /// Stops any worker, hands the surface back whole (no hole, keyboard
  /// exclusive again) and hides. Safe to call from inside one of this
  /// panel's own signals; the destructor calls it too.
  void release();
signals:
  /// The capture is done: `image` is the stitched result.
  void stitched(const QImage &image);
  /// Cancelled, or a fresh region was asked for: back to selecting.
  void dismissed();

protected:
  void paintEvent(QPaintEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;
  void enterEvent(QEnterEvent *event) override;
  void leaveEvent(QEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;

private:
  enum class Phase { Selected, Capturing, Finishing, Finished };
  enum class Mode { Manual, Auto };
  /// What a press on the region's chrome does: the eight edges and corners
  /// resize it, the puck in the middle moves it. The middle is otherwise the
  /// page's, so moving needs a grip of its own.
  enum class Grip {
    None,
    TopLeft,
    Top,
    TopRight,
    Right,
    BottomRight,
    Bottom,
    BottomLeft,
    Left,
    Move
  };
  /// Everything the worker thread owns; the UI thread only reads it after
  /// the worker has stopped.
  struct Worker;

  void startCapture(Mode mode, stitch::Axis axis);
  void stopWorker();
  void finishCapture();
  void cancel();
  /// Region chosen (dragged or restored): remember it, expose the page, hand
  /// the keyboard over by zone, and offer the modes.
  void enterSelected();
  void applyInputRegion();
  /// Takes or releases the exclusive keyboard grab. Held only while a region
  /// is being drawn; released for as long as one exists, because Hyprland
  /// pins pointer focus to a layer that holds it and the page underneath has
  /// to stay scrollable.
  void setKeyboardGrab(bool grab);
  /// Holds the grab only while the pointer is on the overlay's own chrome, and
  /// only when an event has said where the pointer is.
  void updateKeyboardZone(const QPoint &point);
  /// Restarts the capture in the other mode, keeping the region.
  void switchMode(Mode mode);
  /// Where the injector parks the pointer for auto-scroll (physical pixels):
  /// the region's bottom-right corner, inset 10% so it stays inside it.
  [[nodiscard]] std::pair<int, int> autoScrollParkPoint() const;
  /// Rects the overlay must keep taking clicks in the current phase.
  [[nodiscard]] QVector<QRect> chromeRects() const;
  void setStatus(const QString &status, bool warning = false);
  [[nodiscard]] QRect regionPhysical() const;
  [[nodiscard]] QRect doneButtonRect() const;
  /// Leaves a capture in progress and returns to the mode row, keeping the
  /// region: the way out of having picked the wrong direction.
  void returnToModeChoice();
  /// The grips drawn on a chosen region, with what each one does. Empty in
  /// every phase but Selected: during a capture anything drawn inside the
  /// region would be captured with it.
  [[nodiscard]] QVector<QPair<QRect, Grip>> gripRects() const;
  [[nodiscard]] Grip gripAt(const QPoint &point) const;
  [[nodiscard]] static Qt::CursorShape gripCursor(Grip grip);
  /// Applies a grip drag: `point` is where the pointer is now, measured
  /// against where the region and the pointer started.
  void applyGrip(const QPoint &point);
  /// What the key guide in the corner says, for the phase in hand.
  [[nodiscard]] QVector<QPair<QString, QString>> legendEntries() const;
  /// Resumes an auto capture that stopped short, keeping what it has.
  void continueCapture();
  /// How many pills the capture row holds: Continue only appears when there
  /// is something to continue.
  [[nodiscard]] int capturePillCount() const;
  [[nodiscard]] QRect continueButtonRect() const;
  [[nodiscard]] QRect backButtonRect() const;
  [[nodiscard]] QRect cancelButtonRect() const;
  /// Cancel shown beside the mode pills, so backing out never depends on a
  /// key the page may be holding.
  [[nodiscard]] QRect selectedCancelButtonRect() const;
  /// Mode buttons shown in the Selected phase; index into kModeButtons.
  [[nodiscard]] QRect modeButtonRect(int index) const;
  [[nodiscard]] int modeButtonAt(const QPoint &point) const;
  // Worker-thread bodies.
  void captureLoop();     // manual: poll and classify
  void autoCaptureLoop(); // automatic: consume ready cycles and acknowledge
  void postStatus(const QString &status, bool warning = false);
  /// Worker-thread notice that an auto capture stopped short of the end.
  void postStalled();

  MonitorInfo monitor_;
  LayerShellQt::Window *layer_ = nullptr;
  Phase phase_ = Phase::Selected;
  QRect region_; // logical widget pixels, normalized
  Mode mode_ = Mode::Manual;
  stitch::Axis axis_ = stitch::Axis::Vertical;
  QString status_;
  bool statusWarning_ = false;
  /// Set when an auto capture stops before the end of the page, the pointer
  /// left the frame, or the injector gave up. What has been captured is still
  /// in the session, so it can be picked up again.
  bool autoStalled_ = false;
  void reserveChromeStrip();
  bool released_ = false;
  /// The top-level window's handle, for the input mask.
  [[nodiscard]] QWindow *surfaceWindow() const;
  /// The grip being dragged, and what the region and pointer were when it
  /// started.
  Grip activeGrip_ = Grip::None;
  QRect gripStartRegion_;
  QPoint gripStartPoint_;
  /// Whether the layer currently holds the exclusive keyboard grab: only
  /// while the pointer is on our chrome, never over the live page.
  bool keyboardGrabbed_ = true;

  std::unique_ptr<Worker> worker_;
  QFuture<void> workerFuture_;
  std::atomic<bool> stopRequested_{false};
  /// Auto mode: the injection worker's shared stop flag (it also sets this
  /// itself on any exit) and the capture handshake.
  std::shared_ptr<std::atomic<bool>> injectorStop_;
  std::shared_ptr<stitch::CaptureHandshake> handshake_;
};
