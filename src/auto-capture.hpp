/** @fileoverview Automatic scroll capture: the pure decision loop that pairs
 *  the injection worker's scroll ticks with captured frames, plus the
 *  lock-step handshake between them. No compositor dependency, so every
 *  decision runs in the offscreen smoke harness. */
#pragma once

#include "stitch.hpp"

#include <QImage>
#include <QString>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace stitch {

/// Consecutive stationary observations that confirm the end of content (a
/// single one may be a delayed paint or a swallowed wheel event).
inline constexpr int kAutoEndConfirmationProbes = 2;
/// Wheel notches per normal automatic scroll tick and per alignment probe.
inline constexpr int kNotchesPerTick = 3;
inline constexpr int kProbeNotches = 1;

/// Short history of fully verified normal auto-scroll steps. At a confirmed
/// endpoint the final partial movement cannot exceed a stable full step,
/// which rejects distant visual aliases in repeated content; unstable samples
/// deliberately yield no bound, preserving the explicit pause over a guess.
class AutoStepCalibration {
public:
  void recordVerifiedNormalStep(int delta);
  [[nodiscard]] std::optional<int> endpointUpperBound() const;

private:
  std::vector<int> recent_;
};

/// The lock-step handshake between the injection worker and the capture loop.
/// The worker publishes a monotonically increasing ready cycle (cycle 1 is
/// the unscrolled first frame) and blocks until that exact cycle is
/// acknowledged with the notch count for the NEXT scroll; the first
/// acknowledgment of a cycle wins and each is consumed exactly once. There
/// is deliberately no timeout: a slow grab blocks scrolling instead of racing
/// past unrecorded content.
class CaptureHandshake {
public:
  [[nodiscard]] std::uint64_t readyCycle() const {
    return readyCycle_.load(std::memory_order_acquire);
  }
  /// Worker side: announce the next settled frame; returns its cycle.
  std::uint64_t publishReady() {
    return readyCycle_.fetch_add(1, std::memory_order_acq_rel) + 1;
  }
  void acknowledge(std::uint64_t cycle) {
    acknowledgeWithNotches(cycle, kNotchesPerTick);
  }
  void acknowledgeWithNotches(std::uint64_t cycle, int notches);
  /// Worker side: wait until `cycle` is acknowledged; empty when stopped.
  [[nodiscard]] std::optional<int>
  waitForCapture(std::uint64_t cycle, const std::atomic<bool> &stop);

private:
  std::atomic<std::uint64_t> readyCycle_{0};
  mutable std::mutex mutex_;
  std::uint64_t ackCycle_ = 0;
  int ackNotches_ = kNotchesPerTick;
  bool ackConsumed_ = true;
};

/// What one lookahead resolution means for the automatic scroller.
struct AutoProbeDecision {
  enum class Kind { Commit, ProbeAgain, End, Pause };
  enum class PauseReason { StillAmbiguous, Stationary };
  Kind kind = Kind::ProbeAgain;
  ForwardMatchPath path;                          // Commit
  bool periodic = false;                          // Commit via known-forward
  std::optional<ForwardMatchCandidate> endCandidate; // End
  PauseReason pauseReason = PauseReason::StillAmbiguous; // Pause
  std::optional<ForwardMatchPath> bestEffort;     // Pause
};
[[nodiscard]] AutoProbeDecision
autoProbeDecision(const ForwardLookaheadResolution &resolution,
                  int priorStationaryProbes,
                  std::optional<ForwardMatchCandidate> calibratedEnd);

/// The automatic capture session: feed the cropped frame of each consumed
/// ready cycle; the outcome says what happened and how to acknowledge the
/// cycle. Frames are committed only once verified: an ambiguous pair is held
/// while a one-notch probe resolves it against a third frame.
class AutoCapture {
public:
  /// How the overlay must acknowledge the consumed cycle.
  enum class Ack {
    Normal, ///< acknowledge(cycle): next scroll is a full tick
    Probe,  ///< acknowledgeWithNotches(cycle, 1): a one-notch probe follows
    Hold,   ///< do not acknowledge: the worker stays parked on this cycle
  };
  enum class Event {
    Blank,            ///< solid first frame refused (copy raced the overlay)
    Seeded,           ///< first frame accepted
    Appended,         ///< a verified forward step was committed
    StillOnce,        ///< first stationary cycle (delayed paint?), retry
    ReachedEnd,       ///< second stationary cycle: content cannot advance
    ProbeStarted,     ///< ambiguous pair held; probing with one notch
    ProbeAgain,       ///< first stationary probe; probing once more
    Committed,        ///< probe resolved: both held frames committed
    ReachedEndAtSeam, ///< end confirmed at an ambiguous seam; F1 committed
    Paused,           ///< cannot verify; held frames kept for an explicit choice
    Halted,           ///< unrecoverable; capture keeps only committed frames
  };
  enum class HaltReason {
    None,
    LostAlignment,
    MovedBackward,
    Unmatchable,
    BlankFrames,
    AppendFailed,
    ReachedLimit, ///< the capture hit its size budget; finish it
  };
  struct Outcome {
    Event event = Event::StillOnce;
    Ack ack = Ack::Normal;
    MotionEstimate estimate;
    int firstDelta = 0;  ///< Committed / ReachedEndAtSeam
    int secondDelta = 0; ///< Committed
    HaltReason haltReason = HaltReason::None;
    AutoProbeDecision::PauseReason pauseReason =
        AutoProbeDecision::PauseReason::StillAmbiguous;
    bool hasBestEffort = false; ///< Paused: continueAnyway() is available
    QString error;
  };

  explicit AutoCapture(Axis axis);
  [[nodiscard]] Outcome feed(const QImage &cropped);
  /// Commit the paused best-effort path (an unverified seam the user chose to
  /// keep); counts toward unverifiedSeams().
  [[nodiscard]] Outcome continueAnyway();
  /// Drop the held frames and end the capture with only verified content.
  void abandonPause();
  /// Takes a finished session back to streaming, keeping every band it has.
  /// The end of a page and a capture that simply stopped scrolling look the
  /// same from here, both being a still screen, so resuming is the caller's
  /// call, and
  /// a page that really had ended just concludes again.
  void resumeFromEnd();
  [[nodiscard]] QImage finish(QString &error);
  [[nodiscard]] bool started() const { return accumulator_.has_value(); }
  [[nodiscard]] bool reachedEnd() const { return state_ == State::EndReached; }
  [[nodiscard]] bool halted() const { return state_ == State::Halted; }
  [[nodiscard]] int keptFrames() const { return kept_; }
  /// Whether the capture is already longer than most other software will
  /// open. Advisory: capture continues and the result edits normally.
  [[nodiscard]] bool exceedsWidelyOpenableEdge() const {
    return accumulator_ && accumulator_->exceedsWidelyOpenableEdge();
  }
  [[nodiscard]] int unverifiedSeams() const { return unverifiedSeams_; }

private:
  enum class State { Streaming, Probing, Paused, EndReached, Halted };
  Outcome outcome(Event event, Ack ack) const;

  Axis axis_;
  State state_ = State::Streaming;
  std::optional<StitchAccumulator> accumulator_;
  GrayView lastGray_; // last committed frame
  std::optional<ForwardLookahead> lookahead_;
  QImage heldF1_;
  GrayView heldF1Gray_;
  QImage heldF2_;
  GrayView heldF2Gray_;
  int stationaryProbes_ = 0;
  std::optional<ForwardMatchPath> pausedBestEffort_;
  AutoProbeDecision::PauseReason pausedReason_ =
      AutoProbeDecision::PauseReason::StillAmbiguous;
  AutoStepCalibration calibration_;
  int blankFirstFrames_ = 0;
  int consecutiveNoScroll_ = 0;
  int kept_ = 0;
  int unverifiedSeams_ = 0;
  HaltReason haltReason_ = HaltReason::None;
};

} // namespace stitch
