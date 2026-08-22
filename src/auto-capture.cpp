/** @fileoverview Automatic scroll capture decision loop (see auto-capture.hpp). */
#include "auto-capture.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace stitch {

namespace {
/// Poll granularity while the worker waits for an acknowledgment.
constexpr int kAckPollMs = 5;
/// A solid-color first frame is the copy racing the overlay's paint; it must
/// never seed the stitcher.
constexpr int kMaxBlankFirstFrames = 8;
constexpr int kCalibrationWindow = 5;
constexpr int kCalibrationMinSamples = 3;
constexpr int kCalibrationMinJitter = 4;
constexpr int kCalibrationJitterPercent = 5;

bool imageIsUniform(const QImage &image) {
  if (image.isNull())
    return true;
  const QRgb first = image.pixel(0, 0);
  const int stepX = std::max(1, image.width() / 64);
  const int stepY = std::max(1, image.height() / 64);
  for (int y = 0; y < image.height(); y += stepY)
    for (int x = 0; x < image.width(); x += stepX)
      if (image.pixel(x, y) != first)
        return false;
  return true;
}
} // namespace

// --- AutoStepCalibration ------------------------------------------------------

void AutoStepCalibration::recordVerifiedNormalStep(int delta) {
  if (delta == 0)
    return;
  recent_.push_back(delta);
  if (static_cast<int>(recent_.size()) > kCalibrationWindow)
    recent_.erase(recent_.begin());
}

std::optional<int> AutoStepCalibration::endpointUpperBound() const {
  if (static_cast<int>(recent_.size()) < kCalibrationMinSamples)
    return std::nullopt;
  std::vector<int> sorted = recent_;
  std::sort(sorted.begin(), sorted.end());
  const int median = sorted[sorted.size() / 2];
  const int percentageJitter =
      (median * kCalibrationJitterPercent + 99) / 100; // ceil of 5%
  const int jitter = std::max(percentageJitter, kCalibrationMinJitter);
  if (sorted.back() - sorted.front() > jitter)
    return std::nullopt;
  return sorted.back() + jitter;
}

// --- CaptureHandshake ---------------------------------------------------------

void CaptureHandshake::acknowledgeWithNotches(std::uint64_t cycle, int notches) {
  const std::uint64_t published = readyCycle();
  const std::uint64_t clamped = std::min(cycle, published);
  const std::lock_guard lock(mutex_);
  // The first acknowledgment of a cycle wins; duplicates are ignored so a
  // repeated UI event cannot double-scroll.
  if (clamped <= ackCycle_)
    return;
  ackCycle_ = clamped;
  ackNotches_ = std::clamp(notches, 1, kNotchesPerTick);
  ackConsumed_ = false;
}

std::optional<int>
CaptureHandshake::waitForCapture(std::uint64_t cycle,
                                 const std::atomic<bool> &stop) {
  while (true) {
    if (stop.load(std::memory_order_acquire))
      return std::nullopt;
    {
      const std::lock_guard lock(mutex_);
      if (ackCycle_ >= cycle && !ackConsumed_) {
        ackConsumed_ = true;
        return ackNotches_;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kAckPollMs));
  }
}

// --- autoProbeDecision --------------------------------------------------------

AutoProbeDecision
autoProbeDecision(const ForwardLookaheadResolution &resolution,
                  int priorStationaryProbes,
                  std::optional<ForwardMatchCandidate> calibratedEnd) {
  AutoProbeDecision decision;
  switch (resolution.tag) {
  case ForwardLookaheadResolution::Tag::Resolved:
    decision.kind = AutoProbeDecision::Kind::Commit;
    decision.path = *resolution.path;
    decision.periodic = false;
    return decision;
  case ForwardLookaheadResolution::Tag::LowErrorPeriodic:
    decision.kind = AutoProbeDecision::Kind::Commit;
    decision.path = *resolution.path;
    decision.periodic = true;
    return decision;
  case ForwardLookaheadResolution::Tag::Unresolved:
    decision.kind = AutoProbeDecision::Kind::Pause;
    decision.pauseReason = AutoProbeDecision::PauseReason::StillAmbiguous;
    decision.bestEffort = resolution.path;
    return decision;
  case ForwardLookaheadResolution::Tag::StationaryProbe:
    break;
  }
  if (priorStationaryProbes + 1 < kAutoEndConfirmationProbes) {
    decision.kind = AutoProbeDecision::Kind::ProbeAgain;
    return decision;
  }
  const StationaryProbeFirstMatch firstMatch = *resolution.firstMatch;
  if (firstMatch.tag == StationaryProbeFirstMatch::Tag::Unique) {
    decision.kind = AutoProbeDecision::Kind::End;
    decision.endCandidate = firstMatch.candidate;
    return decision;
  }
  if (calibratedEnd) {
    decision.kind = AutoProbeDecision::Kind::End;
    decision.endCandidate = calibratedEnd;
    return decision;
  }
  if (firstMatch.candidate) {
    // Two stationary probes prove the endpoint. Repeated pixels may leave
    // more than one visually valid offset, but that is not a capture failure:
    // use the matcher-ranked forward seam and finish without interrupting the
    // user merely because the content repeats.
    decision.kind = AutoProbeDecision::Kind::End;
    decision.endCandidate = firstMatch.candidate;
    return decision;
  }
  decision.kind = AutoProbeDecision::Kind::Pause;
  decision.pauseReason = AutoProbeDecision::PauseReason::Stationary;
  decision.bestEffort = std::nullopt;
  return decision;
}

// --- AutoCapture --------------------------------------------------------------

AutoCapture::AutoCapture(Axis axis) : axis_(axis) {}

AutoCapture::Outcome AutoCapture::outcome(Event event, Ack ack) const {
  Outcome result;
  result.event = event;
  result.ack = ack;
  return result;
}

AutoCapture::Outcome AutoCapture::feed(const QImage &input) {
  if (state_ == State::Halted) {
    Outcome result = outcome(Event::Halted, Ack::Hold);
    result.haltReason = haltReason_;
    return result;
  }
  if (state_ == State::EndReached)
    return outcome(Event::ReachedEnd, Ack::Hold);
  if (state_ == State::Paused) {
    Outcome result = outcome(Event::Paused, Ack::Hold);
    result.pauseReason = pausedReason_;
    result.hasBestEffort = pausedBestEffort_.has_value();
    return result;
  }
  const QImage cropped = input.format() == QImage::Format_RGBA8888
                             ? input
                             : input.convertToFormat(QImage::Format_RGBA8888);
  QString error;
  const GrayView gray = downsampleToGray(cropped, axis_);

  if (!accumulator_) {
    if (imageIsUniform(cropped)) {
      if (++blankFirstFrames_ >= kMaxBlankFirstFrames) {
        state_ = State::Halted;
        haltReason_ = HaltReason::BlankFrames;
        Outcome result = outcome(Event::Halted, Ack::Hold);
        result.haltReason = haltReason_;
        return result;
      }
      return outcome(Event::Blank, Ack::Hold);
    }
    blankFirstFrames_ = 0;
    bool ok = false;
    accumulator_.emplace(cropped, axis_, ok, error);
    if (!ok) {
      accumulator_.reset();
      state_ = State::Halted;
      haltReason_ = HaltReason::AppendFailed;
      Outcome result = outcome(Event::Halted, Ack::Hold);
      result.haltReason = haltReason_;
      result.error = error;
      return result;
    }
    lastGray_ = gray;
    kept_ = 1;
    return outcome(Event::Seeded, Ack::Normal);
  }

  if (state_ == State::Probing) {
    // Resolve the held ambiguous pair against this probe frame.
    const std::optional<int> bound = calibration_.endpointUpperBound();
    const std::optional<ForwardMatchCandidate> calibratedEnd =
        bound ? lookahead_->uniqueCandidateAtMost(*bound) : std::nullopt;
    const ForwardLookaheadResolution resolution = lookahead_->resolveAuto(gray);
    const AutoProbeDecision decision =
        autoProbeDecision(resolution, stationaryProbes_, calibratedEnd);
    switch (decision.kind) {
    case AutoProbeDecision::Kind::Commit: {
      if (!accumulator_->pushForward(heldF1_, decision.path.firstDelta, error) ||
          !accumulator_->pushForward(cropped, decision.path.secondDelta, error)) {
        state_ = State::Halted;
        haltReason_ = HaltReason::AppendFailed;
        Outcome result = outcome(Event::Halted, Ack::Hold);
        result.haltReason = haltReason_;
        result.error = error;
        return result;
      }
      kept_ += 2;
      lastGray_ = gray;
      calibration_.recordVerifiedNormalStep(decision.path.firstDelta);
      consecutiveNoScroll_ = 0;
      state_ = State::Streaming;
      lookahead_.reset();
      heldF1_ = {};
      Outcome result = outcome(Event::Committed, Ack::Normal);
      result.firstDelta = decision.path.firstDelta;
      result.secondDelta = decision.path.secondDelta;
      return result;
    }
    case AutoProbeDecision::Kind::ProbeAgain:
      ++stationaryProbes_;
      return outcome(Event::ProbeAgain, Ack::Probe);
    case AutoProbeDecision::Kind::End: {
      // The probe frames are stationary duplicates of F1; commit only F1.
      if (!accumulator_->pushForward(heldF1_, decision.endCandidate->delta,
                                     error)) {
        state_ = State::Halted;
        haltReason_ = HaltReason::AppendFailed;
        Outcome result = outcome(Event::Halted, Ack::Hold);
        result.haltReason = haltReason_;
        result.error = error;
        return result;
      }
      ++kept_;
      lastGray_ = heldF1Gray_;
      consecutiveNoScroll_ = kAutoEndConfirmationProbes;
      state_ = State::EndReached;
      lookahead_.reset();
      Outcome result = outcome(Event::ReachedEndAtSeam, Ack::Hold);
      result.firstDelta = decision.endCandidate->delta;
      return result;
    }
    case AutoProbeDecision::Kind::Pause: {
      state_ = State::Paused;
      pausedReason_ = decision.pauseReason;
      pausedBestEffort_ = decision.bestEffort;
      heldF2_ = cropped;
      heldF2Gray_ = gray;
      Outcome result = outcome(Event::Paused, Ack::Hold);
      result.pauseReason = pausedReason_;
      result.hasBestEffort = pausedBestEffort_.has_value();
      return result;
    }
    }
    return outcome(Event::ProbeAgain, Ack::Probe);
  }

  // Streaming.
  const ForwardMatch match = classifyForwardWithLookahead(lastGray_, gray, axis_);
  if (match.tag == ForwardMatch::Tag::Ambiguous) {
    state_ = State::Probing;
    lookahead_ = match.ambiguous;
    heldF1_ = cropped;
    heldF1Gray_ = gray;
    stationaryProbes_ = 0;
    Outcome result = outcome(Event::ProbeStarted, Ack::Probe);
    result.estimate = match.classified;
    return result;
  }
  const MotionEstimate estimate = match.classified;
  Outcome result;
  switch (estimate.motion.kind) {
  case MotionKind::Forward:
    if (accumulator_->wouldExceedBudget(estimate.motion.delta)) {
      // The streaming path is the one that repeats, so stop it at the budget
      // as a designed limit rather than an append failure.
      state_ = State::Halted;
      haltReason_ = HaltReason::ReachedLimit;
      result = outcome(Event::Halted, Ack::Hold);
      result.haltReason = haltReason_;
      return result;
    }
    if (!accumulator_->pushForward(cropped, estimate.motion.delta, error)) {
      state_ = State::Halted;
      haltReason_ = HaltReason::AppendFailed;
      result = outcome(Event::Halted, Ack::Hold);
      result.haltReason = haltReason_;
      result.error = error;
      return result;
    }
    ++kept_;
    lastGray_ = gray;
    calibration_.recordVerifiedNormalStep(estimate.motion.delta);
    consecutiveNoScroll_ = 0;
    result = outcome(Event::Appended, Ack::Normal);
    break;
  case MotionKind::Stationary:
    if (++consecutiveNoScroll_ >= kAutoEndConfirmationProbes) {
      state_ = State::EndReached;
      result = outcome(Event::ReachedEnd, Ack::Normal);
    } else {
      result = outcome(Event::StillOnce, Ack::Normal);
    }
    break;
  case MotionKind::Ambiguous:
    state_ = State::Halted;
    haltReason_ = HaltReason::LostAlignment;
    result = outcome(Event::Halted, Ack::Hold);
    result.haltReason = haltReason_;
    break;
  case MotionKind::Reverse:
    state_ = State::Halted;
    haltReason_ = HaltReason::MovedBackward;
    result = outcome(Event::Halted, Ack::Hold);
    result.haltReason = haltReason_;
    break;
  case MotionKind::Unmatchable:
    state_ = State::Halted;
    haltReason_ = HaltReason::Unmatchable;
    result = outcome(Event::Halted, Ack::Hold);
    result.haltReason = haltReason_;
    break;
  }
  result.estimate = estimate;
  return result;
}

AutoCapture::Outcome AutoCapture::continueAnyway() {
  if (state_ != State::Paused || !pausedBestEffort_ || !accumulator_)
    return outcome(Event::Paused, Ack::Hold);
  QString error;
  const ForwardMatchPath path = *pausedBestEffort_;
  if (!accumulator_->pushForward(heldF1_, path.firstDelta, error) ||
      !accumulator_->pushForward(heldF2_, path.secondDelta, error)) {
    state_ = State::Halted;
    haltReason_ = HaltReason::AppendFailed;
    Outcome result = outcome(Event::Halted, Ack::Hold);
    result.haltReason = haltReason_;
    result.error = error;
    return result;
  }
  kept_ += 2;
  lastGray_ = heldF2Gray_;
  ++unverifiedSeams_;
  // An unverified seam never feeds the calibration.
  lookahead_.reset();
  heldF1_ = {};
  heldF2_ = {};
  pausedBestEffort_.reset();
  Outcome result;
  if (pausedReason_ == AutoProbeDecision::PauseReason::Stationary) {
    state_ = State::EndReached;
    result = outcome(Event::ReachedEnd, Ack::Hold);
  } else {
    state_ = State::Streaming;
    consecutiveNoScroll_ = 0;
    result = outcome(Event::Committed, Ack::Normal);
  }
  result.firstDelta = path.firstDelta;
  result.secondDelta = path.secondDelta;
  return result;
}

void AutoCapture::abandonPause() {
  if (state_ != State::Paused)
    return;
  lookahead_.reset();
  heldF1_ = {};
  heldF2_ = {};
  pausedBestEffort_.reset();
  state_ = State::EndReached;
}

void AutoCapture::resumeFromEnd() {
  if (state_ != State::EndReached)
    return;
  // The last committed frame stays the reference, so the next scroll appends
  // to what is already there rather than starting a second capture.
  lookahead_.reset();
  heldF1_ = {};
  heldF2_ = {};
  pausedBestEffort_.reset();
  consecutiveNoScroll_ = 0;
  state_ = State::Streaming;
}

QImage AutoCapture::finish(QString &error) {
  if (!accumulator_) {
    error = QStringLiteral("no frames were captured");
    return {};
  }
  return accumulator_->finish(error);
}

} // namespace stitch
