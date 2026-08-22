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
#include <optional>

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

/// One line describing the region last captured on a monitor, for the runtime
/// file that lets the next capture start from it. Session scratch, not
/// configuration: it lives beside the snapshots and the instance lock in
/// $XDG_RUNTIME_DIR, so it is gone at reboot, a region only means anything for
/// the screen it was drawn on.
[[nodiscard]] inline QString formatStoredScrollRegion(const QString &monitor,
                                                      const QSize &surface,
                                                      const QRect &region) {
  return QStringLiteral("%1 %2 %3 %4 %5 %6 %7")
      .arg(monitor.isEmpty() ? QStringLiteral("?") : monitor)
      .arg(surface.width())
      .arg(surface.height())
      .arg(region.x())
      .arg(region.y())
      .arg(region.width())
      .arg(region.height());
}

/// The region in `line`, or a null rect when it was written for a different
/// monitor or a different screen size, does not fit, or is not what we wrote.
/// Anything unreadable is simply not offered; there is nothing to migrate.
[[nodiscard]] inline QRect parseStoredScrollRegion(const QString &line,
                                                   const QString &monitor,
                                                   const QSize &surface) {
  const QStringList fields = line.trimmed().split(QLatin1Char(' '));
  if (fields.size() != 7)
    return {};
  if (fields.at(0) != (monitor.isEmpty() ? QStringLiteral("?") : monitor))
    return {};
  bool ok = true;
  int values[6] = {};
  for (int index = 0; index < 6; ++index) {
    bool field = false;
    values[index] = fields.at(index + 1).toInt(&field);
    ok = ok && field;
  }
  if (!ok || QSize(values[0], values[1]) != surface)
    return {};
  const QRect region(values[2], values[3], values[4], values[5]);
  if (region.width() < 32 || region.height() < 32 ||
      !QRect(QPoint(), surface).contains(region))
    return {};
  return region;
}

/// Overlay widget for one scroll capture on a single monitor: drag a region,
/// choose a mode (manual or automatic, vertical or horizontal), scroll, or let
/// the injection worker scroll, and stitch. Show it on an exclusive fullscreen
/// layer surface; when it closes, result() holds the stitched image (null if
/// cancelled).
class ScrollCaptureOverlay final : public QWidget {
public:
  explicit ScrollCaptureOverlay(MonitorInfo monitor, QWidget *parent = nullptr);
  ~ScrollCaptureOverlay() override;

  /// The layer surface backing this widget; used to toggle keyboard
  /// interactivity as the pointer crosses between chrome and the page.
  void setLayerWindow(LayerShellQt::Window *layer);
  /// The stitched capture, or a null image if the user cancelled.
  [[nodiscard]] QImage result() const { return result_; }
  /// Whether the overlay was left by asking for an ordinary area capture
  /// instead. The two are the same tool in two moods, so A swaps between them
  /// rather than making anyone close one and launch the other.
  [[nodiscard]] bool switchedToArea() const { return switchedToArea_; }

protected:
  void paintEvent(QPaintEvent *event) override;
  void showEvent(QShowEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;
  void enterEvent(QEnterEvent *event) override;
  void leaveEvent(QEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;

private:
  enum class Phase { Selecting, Selected, Capturing, Finishing, Finished };
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
  /// Rects the overlay must keep taking clicks in the current phase.
  [[nodiscard]] QVector<QRect> chromeRects() const;
  void setStatus(const QString &status, bool warning = false);
  [[nodiscard]] QRect regionLogical() const;
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
  void rememberRegion() const;
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
  Phase phase_ = Phase::Selecting;
  /// Last pointer position, for the crosshairs drawn while choosing a region.
  QPoint cursor_;
  QPoint dragStart_;
  QPoint dragEnd_;
  bool dragging_ = false;
  QRect region_; // logical widget pixels, normalized
  Mode mode_ = Mode::Manual;
  stitch::Axis axis_ = stitch::Axis::Vertical;
  QString status_;
  bool statusWarning_ = false;
  QImage result_;
  bool switchedToArea_ = false;
  /// Set when an auto capture stops before the end of the page, the pointer
  /// left the frame, or the injector gave up. What has been captured is still
  /// in the session, so it can be picked up again.
  bool autoStalled_ = false;
  /// The region this monitor was last captured with, if the runtime file had
  /// one that still fits. Not shown until asked for: most captures are of
  /// somewhere new, so the overlay opens empty and R brings this back.
  QRect storedRegion_;
  /// The badge's × , as the shared chrome laid it out this frame.
  QRectF modeBadgeClose_;
  /// The grip being dragged, and what the region and pointer were when it
  /// started.
  Grip activeGrip_ = Grip::None;
  QRect gripStartRegion_;
  QPoint gripStartPoint_;
  /// Whether the layer currently holds the exclusive keyboard grab. It does
  /// while a region is being drawn, and not once one exists.
  bool keyboardGrabbed_ = true;

  std::unique_ptr<Worker> worker_;
  QFuture<void> workerFuture_;
  std::atomic<bool> stopRequested_{false};
  /// Auto mode: the injection worker's shared stop flag (it also sets this
  /// itself on any exit) and the capture handshake.
  std::shared_ptr<std::atomic<bool>> injectorStop_;
  std::shared_ptr<stitch::CaptureHandshake> handshake_;
};

/// Runs a manual scroll capture on `monitor` (its own exclusive layer surface
/// and event loop). Returns the stitched image, or a null image if cancelled
/// or on error (with `error` set).
[[nodiscard]] QImage runScrollCapture(const MonitorInfo &monitor,
                                      QString &error, bool *switchedToArea);
