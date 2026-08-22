/** @fileoverview Pure scroll-capture stitcher: classifies inter-frame motion
 *  and assembles overlapping frames into one tall (or wide) image. Operates
 *  on QImage with no compositor or windowing dependency, so it runs in the
 *  offscreen smoke harness. */
#pragma once

#include <QImage>
#include <QString>
#include <QVector>

#include <cstdint>
#include <optional>
#include <vector>

namespace stitch {

/// Downsample factor across the direction of travel. The motion axis stays at
/// source resolution so deltas are exact; the cross axis is /4.
inline constexpr int kDownsampleCross = 4;
// Classifier/matcher thresholds, validated against the capture corpus.
inline constexpr int kMinOverlapDen = 3;
inline constexpr int kMinOverlapPixels = 32;
inline constexpr int kMinMotionPixels = 3;
/// Largest stitched image a capture may grow to, as RGBA bytes. The retained
/// bands already hold roughly the finished image, so peak memory is about
/// twice this. It exists because Linux overcommits: an unbounded capture is
/// not refused at allocation time, it is OOM-killed part-way through
/// assembly, after the user has spent a minute scrolling.
/// Longest edge that image software can be relied on to open. Many renderers
/// address pixels with signed 16-bit coordinates, so 32767 is a common wall
/// (Firefox refuses images past it). Nothing here breaks at this size, since
/// our own editor opens and saves them, so it is advisory rather than a
/// limit.
inline constexpr int kWidelyOpenableEdge = 32767;
inline constexpr long long kMaxStitchedBytes = 512LL * 1024 * 1024;
inline constexpr long long kMaxStitchedPixels = kMaxStitchedBytes / 4;
/// Whether growing a capture of `crossLen` by `axisExtent` along the motion
/// axis would pass that budget.
[[nodiscard]] constexpr bool exceedsStitchBudget(long long crossLen,
                                                 long long axisExtent) {
  return crossLen > 0 && axisExtent > 0 &&
         crossLen * axisExtent > kMaxStitchedPixels;
}
inline constexpr double kStationaryError = 1.0;
inline constexpr double kMaxStationaryError = 8.0;
inline constexpr double kMaxAmbiguousError = 2.0;
inline constexpr double kMaxMatchError = 24.0;
inline constexpr double kMinConfidence = 1.10;
inline constexpr double kMinErrorMargin = 0.75;
inline constexpr int kMaxForwardCandidates = 64;
inline constexpr int kPathDeltaTolerance = 2;
inline constexpr int kMaxStationaryEdge = 128;       // accumulator
inline constexpr double kStationaryEdgeError = 1.0;
inline constexpr int kMaxStationaryScoringEdgeDen = 4;
inline constexpr double kNearStationaryError = 1.0;  // accumulator

enum class Axis { Vertical, Horizontal };

/// Classification of the second frame relative to the first. Forward is
/// down (vertical) or right (horizontal); deltas are source-frame pixels.
enum class MotionKind { Stationary, Forward, Reverse, Ambiguous, Unmatchable };

struct Motion {
  MotionKind kind = MotionKind::Unmatchable;
  /// Forward/Reverse: magnitude ≥ 0. Ambiguous: signed (>0 forward,
  /// <0 reverse). Stationary/Unmatchable: 0.
  int delta = 0;
};

/// Motion plus diagnostics. `error` is mean absolute grayscale error per
/// sampled pixel; `confidence` is the ratio of the best distinct competing
/// peak to the chosen peak.
struct MotionEstimate {
  Motion motion;
  double error = 0.0;
  double confidence = 0.0;
};

/// Downsampled grayscale frame used by the motion matcher. Pixels are
/// row-major, `width * height`. The motion axis is at full source resolution;
/// the cross axis is downsampled by kDownsampleCross. `sourceWidth` records
/// the pre-downsample width so horizontal source pixels can be recovered.
class GrayView {
public:
  std::vector<std::uint8_t> pixels;
  int width = 0;
  int height = 0;
  int sourceWidth = 0;

  [[nodiscard]] int axisLen(Axis axis) const {
    return axis == Axis::Vertical ? height : width;
  }
  [[nodiscard]] int crossLen(Axis axis) const {
    return axis == Axis::Vertical ? width : height;
  }
  /// Source pixels each downsampled unit along `axis` represents: 1 on the
  /// motion axis (kept at source resolution so recovered deltas, and so the
  /// seams, are exact), kDownsampleCross on the cross axis.
  [[nodiscard]] int sourceScale(Axis axis) const;
  /// Sub-view spanning [start, end) along `axis`, cross axis untouched.
  [[nodiscard]] GrayView cropAxis(Axis axis, int start, int end) const;
};

/// Grayscale downsample of `image` for motion along `axis`
/// (Rec.601-ish luma: (r*77 + g*150 + b*29) >> 8).
[[nodiscard]] GrayView downsampleToGray(const QImage &image, Axis axis);

/// Classify signed motion between two downsampled frames.
[[nodiscard]] MotionEstimate classifyMotion(const GrayView &prev,
                                            const GrayView &cur, Axis axis);
/// Like classifyMotion but caps the search at `maxSourceDelta` source pixels.
[[nodiscard]] MotionEstimate classifyMotionBounded(const GrayView &prev,
                                                   const GrayView &cur,
                                                   Axis axis, int maxSourceDelta);

// --- Correlation core (exposed for the smoke's bit-exact scoring check) -----
/// Mean absolute grayscale error for aligning `cur` onto `prev` at `shift`
/// along `axis`, sampling every `axisStep`/`crossStep` pixel over a fixed
/// window `axisLen - maxShift`. +inf on any geometry guard.
[[nodiscard]] double scoreShift(const GrayView &prev, const GrayView &cur,
                                Axis axis, long shift, int maxShift,
                                int axisStep, int crossStep);
/// Naive per-pixel reference for scoreShift (smoke oracle only).
[[nodiscard]] double scoreShiftReference(const GrayView &prev,
                                         const GrayView &cur, Axis axis,
                                         long shift, int maxShift, int axisStep,
                                         int crossStep);
/// Leading/trailing edge counts unchanged at shift 0 (viewport-fixed chrome),
/// each capped at axisLen/kMaxStationaryScoringEdgeDen.
struct StationaryEdges {
  int lead = 0;
  int trail = 0;
};
[[nodiscard]] StationaryEdges stationaryScoringEdges(const GrayView &prev,
                                                     const GrayView &cur,
                                                     Axis axis);

// --- Forward lookahead (auto-scroll alignment verification) -----------------
/// One forward-only alignment candidate retained for lookahead resolution.
/// Deltas are source pixels; error is mean absolute grayscale error.
struct ForwardMatchCandidate {
  int delta = 0;
  double error = 0.0;
  bool operator==(const ForwardMatchCandidate &) const = default;
};
/// Two adjacent forward deltas supported by a three-frame alignment path.
struct ForwardMatchPath {
  int firstDelta = 0;
  int secondDelta = 0;
  double totalError = 0.0;
  bool operator==(const ForwardMatchPath &) const = default;
};
/// The distinct forward correlation basins of one frame pair, best first.
struct ForwardCandidateSet {
  std::vector<ForwardMatchCandidate> candidates;
  bool truncated = false;
  /// Largest source-pixel delta whose fixed comparison extent was searched;
  /// empty means the frame geometry itself was not searchable.
  std::optional<int> searchMaxDelta;
};
/// What the known-forward matcher can establish about F0→F1 when a probe is
/// stationary relative to F1: a unique non-truncated basin is safe to commit
/// automatically; Ambiguous preserves a best-effort candidate for an explicit
/// user choice.
struct StationaryProbeFirstMatch {
  enum class Tag { Unique, Ambiguous };
  Tag tag = Tag::Ambiguous;
  std::optional<ForwardMatchCandidate> candidate; // Unique: the basin;
                                                  // Ambiguous: best effort
  bool operator==(const StationaryProbeFirstMatch &) const = default;
};
/// Result of resolving an ambiguous forward pair with one lookahead frame.
struct ForwardLookaheadResolution {
  enum class Tag {
    Resolved,        ///< all three comparisons agree on a distinct path
    LowErrorPeriodic,///< accepted using the auto worker's known direction
    StationaryProbe, ///< the probe did not move relative to the pending frame
    Unresolved,      ///< only an explicit "continue anyway" may use bestEffort
  };
  Tag tag = Tag::Unresolved;
  std::optional<ForwardMatchPath> path;       // Resolved / LowErrorPeriodic /
                                              // Unresolved best-effort
  std::optional<StationaryProbeFirstMatch> firstMatch; // StationaryProbe
};
/// A pending forward alignment: the ambiguous pair's frames plus its retained
/// candidates, immutable while a lookahead frame resolves it.
class ForwardLookahead {
public:
  ForwardLookahead(GrayView origin, GrayView pending, Axis axis,
                   ForwardCandidateSet first, MotionEstimate estimate)
      : origin_(std::move(origin)), pending_(std::move(pending)), axis_(axis),
        first_(std::move(first)), estimate_(estimate) {}
  [[nodiscard]] MotionEstimate estimate() const { return estimate_; }
  [[nodiscard]] const std::vector<ForwardMatchCandidate> &candidates() const {
    return first_.candidates;
  }
  /// The only retained candidate no larger than a trusted physical bound
  /// (e.g. a previously observed full auto step); never narrows a truncated
  /// search.
  [[nodiscard]] std::optional<ForwardMatchCandidate>
  uniqueCandidateAtMost(int maxSourceDelta) const;
  /// Strict three-frame resolution (no physical-motion prior).
  [[nodiscard]] ForwardLookaheadResolution resolve(const GrayView &lookahead) const;
  /// Known-forward automatic resolution: additionally accepts matcher-grade
  /// periodic paths using the worker's known direction (cadence only breaks
  /// ties).
  [[nodiscard]] ForwardLookaheadResolution resolveAuto(const GrayView &lookahead) const;

private:
  [[nodiscard]] ForwardLookaheadResolution
  resolveWithPhysicalPrior(const GrayView &lookahead, bool allowPhysicalPrior) const;

  GrayView origin_;
  GrayView pending_;
  Axis axis_;
  ForwardCandidateSet first_;
  MotionEstimate estimate_;
};
/// Result of the first automatic-scroll comparison.
struct ForwardMatch {
  enum class Tag { Classified, Ambiguous };
  Tag tag = Tag::Classified;
  MotionEstimate classified;                 // Tag::Classified
  std::optional<ForwardLookahead> ambiguous; // Tag::Ambiguous
};
/// All distinct forward basins of the pair (positive shifts only).
[[nodiscard]] ForwardCandidateSet
forwardCandidateSet(const GrayView &prev, const GrayView &cur, Axis axis);
/// Classify the pair; an ambiguous-but-matcher-grade result carries the
/// frames needed to resolve it against one lookahead frame.
[[nodiscard]] ForwardMatch classifyForwardWithLookahead(const GrayView &prev,
                                                        const GrayView &cur,
                                                        Axis axis);
/// Resolve three candidate sets into a path (exposed for the smoke's
/// literal-set checks).
[[nodiscard]] ForwardLookaheadResolution
resolveForwardCandidateSets(const ForwardCandidateSet &first,
                            const ForwardCandidateSet &second,
                            const ForwardCandidateSet &cumulative,
                            bool allowPhysicalPrior);

// --- Accumulator (pixel assembly) ------------------------------------------
/// Assembles overlapping forward frames into one tall/wide image. A trailing
/// stationary strip (window borders, fixed footers) plus a near-stationary
/// zone (footer drop shadows) is excluded from every incremental slice and
/// copied once from the final frame. Frames must be RGBA (Format_RGBA8888).
class StitchAccumulator {
public:
  /// Begins a capture from its first frame. `ok` is set false and `error`
  /// filled on an invalid frame.
  StitchAccumulator(const QImage &first, Axis axis, bool &ok, QString &error);
  /// Retains one validated forward frame; `delta` is source pixels along the
  /// axis. Returns false with `error` set on failure (state unchanged).
  [[nodiscard]] bool pushForward(const QImage &frame, int delta, QString &error);
  /// Retain one reverse (scrolled-up/left) frame while keeping the final image
  /// in document order. Cannot be mixed with pushForward.
  [[nodiscard]] bool pushReverse(const QImage &frame, int delta, QString &error);
  /// Assembles the final image; returns a null QImage with `error` set on
  /// failure. Consumes the accumulator's buffers.
  [[nodiscard]] QImage finish(QString &error);
  [[nodiscard]] int frameCount() const { return static_cast<int>(bands_.size()) + 1; }
  /// Whether growing by `delta` along the motion axis would pass the budget.
  [[nodiscard]] bool wouldExceedBudget(int delta) const;
  /// Whether the finished image would be longer than most other software will
  /// open. Advisory only; see kWidelyOpenableEdge.
  [[nodiscard]] bool exceedsWidelyOpenableEdge() const;
  // Test accessors.
  [[nodiscard]] long retainedRgbaBytes() const;
  [[nodiscard]] int stationaryTrailingStripForTest() const {
    return stationaryTrailingStrip();
  }
  [[nodiscard]] int nearStationaryTrailingZoneForTest() const {
    return nearStationaryTrailingZone(stationaryTrailingStrip());
  }

private:
  struct TailBand {
    int delta = 0;
    int sourceAxisStart = 0;
    std::vector<std::uint8_t> rgba; // tight RGBA
  };
  [[nodiscard]] int stationaryTrailingStrip() const;
  [[nodiscard]] int nearStationaryTrailingZone(int strip) const;
  [[nodiscard]] bool pushOriented(const QImage &frame, int delta, QString &error);

  enum class Direction { None, Forward, Reverse };
  Direction direction_ = Direction::None;

  Axis axis_;
  bool valid_ = false;
  int width_ = 0, height_ = 0, axisLen_ = 0, crossLen_ = 0, maxEdge_ = 0;
  std::vector<std::uint8_t> firstRgba_;
  std::vector<TailBand> bands_;
  int totalDelta_ = 0;
  std::vector<std::uint64_t> edgeSums_, edgeCounts_, alignedSums_, alignedCounts_;
};

// --- Live manual capture session ------------------------------------------------
/// The decision loop of a manual scroll capture, independent of any window or
/// compositor: feed it the cropped region from each grab and it classifies the
/// motion against the last committed frame and grows the stitch. In manual
/// mode small movements are held as one replaceable pending frame until they
/// grow past a threshold (or finish), ambiguous small motion
/// on repeated content is recovered with a bounded re-search, the direction
/// locks once a band is committed, a frame that cannot be aligned keeps the
/// last verified reference (a later overlapping frame resumes), and, only
/// while nothing has been committed, a page that is at rest but no longer
/// matches the first frame (an autoplaying video, hover UI) re-seeds it.
class ManualCapture {
public:
  enum class Event {
    Blank,          ///< solid-color first frame; not seeded (copy raced the overlay)
    Seeded,         ///< first frame accepted
    Kept,           ///< a band was committed
    Pending,        ///< small movement held as the pending frame
    Still,          ///< no movement
    PendingDropped, ///< a pending movement was reversed
    ReSeeded,       ///< first frame replaced (content changed in place, nothing committed)
    Ambiguous,      ///< repeated content; keep scrolling
    Unmatchable,    ///< no reliable overlap with the reference; keep it and warn
    WrongDirection, ///< motion against the locked direction after a band exists
    Error,          ///< accumulator refused the frame (see `error`)
    Full,           ///< the capture reached its size budget; finish it
  };
  struct Outcome {
    Event event = Event::Still;
    MotionEstimate estimate;
    int keptFrames = 0;
    int pendingDelta = 0;
    QString error;
  };
  explicit ManualCapture(Axis axis);
  /// Consume one cropped grab (RGBA8888 or convertible).
  [[nodiscard]] Outcome feed(const QImage &cropped);
  /// Commit any pending frame and assemble the result (null + `error` on
  /// failure; the session stays usable).
  [[nodiscard]] QImage finish(QString &error);
  [[nodiscard]] bool started() const { return accumulator_.has_value(); }
  [[nodiscard]] int keptFrames() const { return kept_; }
  /// Whether the capture is already longer than most other software will
  /// open. Advisory: capture continues and the result edits normally.
  [[nodiscard]] bool exceedsWidelyOpenableEdge() const {
    return accumulator_ && accumulator_->exceedsWidelyOpenableEdge();
  }
  [[nodiscard]] Axis axis() const { return axis_; }
  /// Small movements below this are held back (axisLen/8, clamped 16..128).
  [[nodiscard]] static int coalesceThreshold(int axisLen);
  /// Bounded re-search radius for ambiguous manual motion (axisLen/8,
  /// clamped 128..512).
  [[nodiscard]] static int manualSearchBound(int axisLen);

private:
  enum class Direction { None, Forward, Reverse };
  struct PendingFrame {
    QImage frame;
    int delta = 0;
    Direction direction = Direction::None;
  };
  Outcome recordMotion(const QImage &cropped, const GrayView &gray,
                       Direction direction, int delta,
                       const MotionEstimate &estimate);
  Outcome outcome(Event event, const MotionEstimate &estimate = {},
                  QString error = {}) const;

  Axis axis_;
  std::optional<StitchAccumulator> accumulator_;
  GrayView lastGray_;     // last committed frame
  GrayView previousGray_; // previous grab, kept or not
  bool havePrevious_ = false;
  std::optional<PendingFrame> pending_;
  Direction direction_ = Direction::None;
  int blankFirstFrames_ = 0;
  int kept_ = 0;
};

/// Stitch already-validated forward frames using the measured per-pair delta
/// (deltas.size() must equal frames.size()-1).
[[nodiscard]] QImage stitchWithDeltas(const QVector<QImage> &frames,
                                      const QVector<int> &deltas, Axis axis,
                                      QString &error);

} // namespace stitch
