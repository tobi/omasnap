/** @fileoverview Pure scroll-capture stitcher (see stitch.hpp). */
#include "stitch.hpp"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace stitch {

int GrayView::sourceScale(Axis axis) const {
  if (axis == Axis::Vertical)
    return 1;
  // Callers pass the motion axis, which downsampleToGray leaves at source
  // resolution, so this is 1 by construction (width == sourceWidth for a
  // horizontal view). The division only bites on very narrow frames, where the
  // downsampler's clamp can leave the two out of step.
  return std::max(1, sourceWidth / std::max(1, width));
}

GrayView GrayView::cropAxis(Axis axis, int start, int end) const {
  start = std::clamp(start, 0, axisLen(axis));
  end = std::clamp(std::max(end, start), start, axisLen(axis));
  GrayView view;
  if (axis == Axis::Vertical) {
    view.width = width;
    view.height = end - start;
    view.sourceWidth = sourceWidth;
    view.pixels.assign(pixels.begin() + static_cast<std::ptrdiff_t>(start) * width,
                       pixels.begin() + static_cast<std::ptrdiff_t>(end) * width);
  } else {
    const int cropWidth = end - start;
    view.width = cropWidth;
    view.height = height;
    view.sourceWidth = cropWidth * sourceScale(axis);
    view.pixels.reserve(static_cast<std::size_t>(cropWidth) * height);
    for (int row = 0; row < height; ++row) {
      const auto base = static_cast<std::ptrdiff_t>(row) * width;
      view.pixels.insert(view.pixels.end(), pixels.begin() + base + start,
                         pixels.begin() + base + end);
    }
  }
  return view;
}

GrayView downsampleToGray(const QImage &image, Axis axis) {
  // Read the RGBA byte orders in place as well as the BGRA ones: scroll
  // capture hands frames over as RGBA8888, and converting here copied every
  // frame a second time. Only the channel order differs, which the two reads
  // below account for; the luma weights are unchanged either way.
  const bool rgbaOrder = image.format() == QImage::Format_RGBA8888 ||
                         image.format() == QImage::Format_RGBA8888_Premultiplied;
  const QImage rgb = rgbaOrder || image.format() == QImage::Format_RGB32 ||
                             image.format() == QImage::Format_ARGB32
                         ? image
                         : image.convertToFormat(QImage::Format_ARGB32);
  const int srcW = std::max(0, rgb.width());
  const int srcH = std::max(0, rgb.height());
  const int dstW = axis == Axis::Vertical
                       ? std::min(std::max(1, srcW / kDownsampleCross),
                                  std::max(1, srcW))
                       : srcW;
  const int dstH = axis == Axis::Horizontal
                       ? std::min(std::max(1, srcH / kDownsampleCross),
                                  std::max(1, srcH))
                       : srcH;
  GrayView view;
  view.width = dstW;
  view.height = dstH;
  view.sourceWidth = srcW;
  view.pixels.assign(static_cast<std::size_t>(dstW) * dstH, 0);
  for (int gy = 0; gy < dstH; ++gy) {
    const int srcY = axis == Axis::Vertical
                         ? gy
                         : std::min(gy * kDownsampleCross, std::max(0, srcH - 1));
    const auto *srcRow = reinterpret_cast<const QRgb *>(rgb.constScanLine(srcY));
    std::uint8_t *dstRow = view.pixels.data() + static_cast<std::size_t>(gy) * dstW;
    for (int gx = 0; gx < dstW; ++gx) {
      const int srcX = axis == Axis::Vertical
                           ? std::min(gx * kDownsampleCross, std::max(0, srcW - 1))
                           : gx;
      const std::uint32_t px = static_cast<std::uint32_t>(srcRow[srcX]);
      // BGRA in memory for the QRgb formats, RGBA for the 8888 ones.
      const std::uint32_t r = rgbaOrder ? (px & 0xffu) : ((px >> 16) & 0xffu);
      const std::uint32_t g = (px >> 8) & 0xffu;
      const std::uint32_t b = rgbaOrder ? ((px >> 16) & 0xffu) : (px & 0xffu);
      dstRow[gx] = static_cast<std::uint8_t>((r * 77 + g * 150 + b * 29) >> 8);
    }
  }
  return view;
}


namespace {
constexpr double kInf = std::numeric_limits<double>::infinity();

// Rust total_cmp order: finite ascending, NaN last. Used for sorting samples.
bool errorLess(double a, double b) {
  if (std::isnan(a))
    return false;
  if (std::isnan(b))
    return true;
  return a < b;
}

struct SearchSample {
  long shift;
  double error;
};

int crossMargin(int crossLen) {
  return std::min(crossLen / 12, std::max(0, crossLen - 1) / 2);
}

// unmatchable() sentinel.
MotionEstimate unmatchable() {
  return {{MotionKind::Unmatchable, 0}, kInf, 0.0};
}

double edgeLineError(const GrayView &prev, const GrayView &cur, Axis axis,
                     int along) {
  const int crossLen = prev.crossLen(axis);
  const int margin = crossMargin(crossLen);
  const int step = std::max(1, crossLen / 128);
  std::uint64_t total = 0;
  int count = 0;
  for (int cross = margin; cross < crossLen - margin; cross += step) {
    const int index = axis == Axis::Vertical ? along * prev.width + cross
                                             : cross * prev.width + along;
    total += static_cast<std::uint64_t>(
        std::abs(static_cast<int>(prev.pixels[index]) -
                 static_cast<int>(cur.pixels[index])));
    ++count;
  }
  return count == 0 ? kInf : static_cast<double>(total) / count;
}

MotionEstimate classifyMotionCropped(const GrayView &prev, const GrayView &cur,
                                     Axis axis,
                                     std::optional<int> maxSourceDelta) {
  const int axisLen = prev.axisLen(axis);
  const int crossLen = prev.crossLen(axis);
  if (axisLen < kMinOverlapPixels + kMinMotionPixels || crossLen < 2)
    return unmatchable();

  const int minOverlap = std::min(std::max(axisLen / kMinOverlapDen, kMinOverlapPixels),
                                  std::max(0, axisLen - 1));
  const int safeMaxShift = std::max(0, axisLen - minOverlap);
  const int sourceScale = prev.sourceScale(axis);
  int maxShift = safeMaxShift;
  if (maxSourceDelta)
    maxShift = std::min(*maxSourceDelta / sourceScale, safeMaxShift);
  if (maxShift < kMinMotionPixels)
    return unmatchable();

  const int matchExtent = axisLen - maxShift;
  const int coarseAxis = std::max(1, matchExtent / 32);
  const int coarseCross = std::max(1, crossLen / 48);
  const int fineAxis = std::max(1, matchExtent / 128);
  const int fineCross = std::max(1, crossLen / 128);

  const double zeroError =
      scoreShift(prev, cur, axis, 0, maxShift, fineAxis, fineCross);
  if (zeroError <= kStationaryError)
    return {{MotionKind::Stationary, 0}, zeroError, kInf};

  const long maxShiftSigned = maxShift;
  std::vector<SearchSample> coarse;
  coarse.reserve(static_cast<std::size_t>(maxShift) * 2 + 1);
  for (long shift = -maxShiftSigned; shift <= maxShiftSigned; ++shift)
    coarse.push_back({shift, scoreShift(prev, cur, axis, shift, maxShift,
                                        coarseAxis, coarseCross)});
  std::stable_sort(coarse.begin(), coarse.end(),
                   [](const SearchSample &a, const SearchSample &b) {
                     return errorLess(a.error, b.error);
                   });

  std::vector<SearchSample> fine;
  const int peaksToRefine = std::min<int>(12, static_cast<int>(coarse.size()));
  for (int i = 0; i < peaksToRefine; ++i) {
    const long start = std::max(coarse[i].shift - 1, -maxShiftSigned);
    const long end = std::min(coarse[i].shift + 1, maxShiftSigned);
    for (long candidate = start; candidate <= end; ++candidate) {
      if (std::any_of(fine.begin(), fine.end(),
                      [&](const SearchSample &s) { return s.shift == candidate; }))
        continue;
      fine.push_back({candidate, scoreShift(prev, cur, axis, candidate, maxShift,
                                            fineAxis, fineCross)});
    }
  }
  std::stable_sort(fine.begin(), fine.end(),
                   [](const SearchSample &a, const SearchSample &b) {
                     return errorLess(a.error, b.error);
                   });
  if (fine.empty())
    return unmatchable();
  const SearchSample best = fine.front();

  const long runnerNeighborhood = std::clamp(axisLen / 256, 6, 16);
  double runnerError = kInf;
  int taken = 0;
  for (const SearchSample &sample : coarse) {
    if (std::abs(sample.shift - best.shift) <= runnerNeighborhood)
      continue;
    runnerError = std::min(runnerError,
                           scoreShift(prev, cur, axis, sample.shift, maxShift,
                                      fineAxis, fineCross));
    if (++taken >= 8)
      break;
  }

  double confidence;
  if (best.error <= std::numeric_limits<double>::epsilon())
    confidence = runnerError <= std::numeric_limits<double>::epsilon() ? 1.0 : kInf;
  else
    confidence = runnerError / best.error;

  const int sourceDelta =
      static_cast<int>(std::abs(best.shift)) * sourceScale;
  if (sourceDelta <= kMinMotionPixels && best.error <= kMaxStationaryError)
    return {{MotionKind::Stationary, 0}, best.error, confidence};

  const double margin = runnerError - best.error;
  if (!std::isfinite(best.error) || best.error > kMaxMatchError)
    return {{MotionKind::Unmatchable, 0}, best.error, confidence};

  if (confidence < kMinConfidence || margin < kMinErrorMargin) {
    if (best.error <= kMaxAmbiguousError) {
      const int signed_ = best.shift > 0 ? sourceDelta : -sourceDelta;
      return {{MotionKind::Ambiguous, signed_}, best.error, confidence};
    }
    return {{MotionKind::Unmatchable, 0}, best.error, confidence};
  }
  return {{best.shift > 0 ? MotionKind::Forward : MotionKind::Reverse,
           sourceDelta},
          best.error, confidence};
}

MotionEstimate classifyWithBound(const GrayView &prev, const GrayView &cur,
                                 Axis axis, std::optional<int> maxSourceDelta) {
  if (prev.width != cur.width || prev.height != cur.height ||
      static_cast<int>(prev.pixels.size()) != prev.width * prev.height ||
      static_cast<int>(cur.pixels.size()) != cur.width * cur.height)
    return unmatchable();
  const int axisLen = prev.axisLen(axis);
  const int crossLen = prev.crossLen(axis);
  if (axisLen < kMinOverlapPixels + kMinMotionPixels || crossLen < 2)
    return unmatchable();
  const StationaryEdges edges = stationaryScoringEdges(prev, cur, axis);
  if (edges.lead + edges.trail > 0) {
    const GrayView pc = prev.cropAxis(axis, edges.lead, axisLen - edges.trail);
    const GrayView cc = cur.cropAxis(axis, edges.lead, axisLen - edges.trail);
    return classifyMotionCropped(pc, cc, axis, maxSourceDelta);
  }
  return classifyMotionCropped(prev, cur, axis, maxSourceDelta);
}
} // namespace

double scoreShift(const GrayView &prev, const GrayView &cur, Axis axis,
                  long shift, int maxShift, int axisStep, int crossStep) {
  const int axisLen = prev.axisLen(axis);
  const int crossLen = prev.crossLen(axis);
  const long magnitude = std::abs(shift);
  if (magnitude > maxShift || maxShift >= axisLen)
    return kInf;
  const int matchExtent = axisLen - maxShift;
  const long available = axisLen - magnitude;
  if (available < matchExtent || matchExtent == 0)
    return kInf;
  const long centered = (available - matchExtent) / 2;
  const long prevStart = shift >= 0 ? centered + magnitude : centered;
  const long curStart = shift >= 0 ? centered : centered + magnitude;
  const int margin = crossMargin(crossLen);
  const int crossStart = margin;
  const int crossEnd = crossLen - margin;
  const int aStep = std::max(1, axisStep);
  const int cStep = std::max(1, crossStep);
  std::uint64_t total = 0;
  std::uint64_t count = 0;
  if (axis == Axis::Vertical) {
    for (int along = 0; along < matchExtent; along += aStep) {
      const long prevRow = (prevStart + along) * prev.width;
      const long curRow = (curStart + along) * cur.width;
      for (int cross = crossStart; cross < crossEnd; cross += cStep) {
        total += static_cast<std::uint64_t>(
            std::abs(static_cast<int>(prev.pixels[prevRow + cross]) -
                     static_cast<int>(cur.pixels[curRow + cross])));
        ++count;
      }
    }
  } else {
    for (int along = 0; along < matchExtent; along += aStep) {
      const long prevCol = prevStart + along;
      const long curCol = curStart + along;
      for (int cross = crossStart; cross < crossEnd; cross += cStep) {
        total += static_cast<std::uint64_t>(std::abs(
            static_cast<int>(prev.pixels[static_cast<long>(cross) * prev.width + prevCol]) -
            static_cast<int>(cur.pixels[static_cast<long>(cross) * cur.width + curCol])));
        ++count;
      }
    }
  }
  return count == 0 ? kInf : static_cast<double>(total) / static_cast<double>(count);
}

double scoreShiftReference(const GrayView &prev, const GrayView &cur, Axis axis,
                           long shift, int maxShift, int axisStep,
                           int crossStep) {
  // Independent naive implementation (the smoke oracle). Same math, no fused
  // accumulation tricks.
  const int axisLen = prev.axisLen(axis);
  const int crossLen = prev.crossLen(axis);
  const long magnitude = std::abs(shift);
  if (magnitude > maxShift || maxShift >= axisLen)
    return kInf;
  const int matchExtent = axisLen - maxShift;
  const long available = axisLen - magnitude;
  if (available < matchExtent || matchExtent == 0)
    return kInf;
  const long centered = (available - matchExtent) / 2;
  const long prevStart = shift >= 0 ? centered + magnitude : centered;
  const long curStart = shift >= 0 ? centered : centered + magnitude;
  const int margin = crossMargin(crossLen);
  const int aStep = std::max(1, axisStep);
  const int cStep = std::max(1, crossStep);
  double total = 0.0;
  long count = 0;
  for (int along = 0; along < matchExtent; along += aStep) {
    for (int cross = margin; cross < crossLen - margin; cross += cStep) {
      int a, b;
      if (axis == Axis::Vertical) {
        a = prev.pixels[(prevStart + along) * prev.width + cross];
        b = cur.pixels[(curStart + along) * cur.width + cross];
      } else {
        a = prev.pixels[static_cast<long>(cross) * prev.width + (prevStart + along)];
        b = cur.pixels[static_cast<long>(cross) * cur.width + (curStart + along)];
      }
      total += std::abs(a - b);
      ++count;
    }
  }
  return count == 0 ? kInf : total / static_cast<double>(count);
}

StationaryEdges stationaryScoringEdges(const GrayView &prev, const GrayView &cur,
                                       Axis axis) {
  const int axisLen = prev.axisLen(axis);
  const int maxEdge = axisLen / kMaxStationaryScoringEdgeDen;
  StationaryEdges edges;
  while (edges.lead < maxEdge &&
         edgeLineError(prev, cur, axis, edges.lead) <= kStationaryEdgeError)
    ++edges.lead;
  while (edges.trail < maxEdge &&
         edgeLineError(prev, cur, axis, axisLen - 1 - edges.trail) <=
             kStationaryEdgeError)
    ++edges.trail;
  return edges;
}

MotionEstimate classifyMotion(const GrayView &prev, const GrayView &cur,
                              Axis axis) {
  return classifyWithBound(prev, cur, axis, std::nullopt);
}

MotionEstimate classifyMotionBounded(const GrayView &prev, const GrayView &cur,
                                     Axis axis, int maxSourceDelta) {
  return classifyWithBound(prev, cur, axis, std::optional<int>(maxSourceDelta));
}

// --- Accumulator ------------------------------------------------------------
namespace {
// A frame the accumulator can read as tight RGBA8888: returns bytes-per-line
// and a pointer, converting if needed (kept alive by `owned`).
struct RgbaFrame {
  QImage owned;
  const std::uint8_t *bits = nullptr;
  int stride = 0;
  int width = 0;
  int height = 0;
};
RgbaFrame asRgba(const QImage &image) {
  RgbaFrame frame;
  frame.owned = image.format() == QImage::Format_RGBA8888
                    ? image
                    : image.convertToFormat(QImage::Format_RGBA8888);
  frame.bits = frame.owned.constBits();
  frame.stride = static_cast<int>(frame.owned.bytesPerLine());
  frame.width = frame.owned.width();
  frame.height = frame.owned.height();
  return frame;
}

bool validateFrame(const RgbaFrame &frame, int expectedW, int expectedH,
                   QString &error) {
  if (frame.width <= 0 || frame.height <= 0) {
    error = QStringLiteral("frames have invalid dimensions");
    return false;
  }
  if ((expectedW || expectedH) &&
      (frame.width != expectedW || frame.height != expectedH)) {
    error = QStringLiteral("frames have inconsistent dimensions");
    return false;
  }
  return true;
}

// Reflect tight row-major RGBA pixels across the axis, in place.
void reverseRgbaAxisInPlace(std::vector<std::uint8_t> &pixels, int width,
                            int height, Axis axis) {
  const int rowBytes = width * 4;
  if (axis == Axis::Vertical) {
    std::vector<std::uint8_t> tmp(rowBytes);
    for (int top = 0; top < height / 2; ++top) {
      const int bottom = height - top - 1;
      std::uint8_t *topRow = pixels.data() + static_cast<std::size_t>(top) * rowBytes;
      std::uint8_t *bottomRow = pixels.data() + static_cast<std::size_t>(bottom) * rowBytes;
      std::memcpy(tmp.data(), topRow, rowBytes);
      std::memcpy(topRow, bottomRow, rowBytes);
      std::memcpy(bottomRow, tmp.data(), rowBytes);
    }
  } else {
    for (int row = 0; row < height; ++row) {
      std::uint8_t *r = pixels.data() + static_cast<std::size_t>(row) * rowBytes;
      for (int left = 0; left < width / 2; ++left) {
        const int right = width - left - 1;
        for (int c = 0; c < 4; ++c)
          std::swap(r[left * 4 + c], r[right * 4 + c]);
      }
    }
  }
}

// Tight (no row padding) RGBA copy.
std::vector<std::uint8_t> copyFrameTight(const RgbaFrame &frame) {
  const int rowBytes = frame.width * 4;
  std::vector<std::uint8_t> tight(static_cast<std::size_t>(rowBytes) * frame.height);
  for (int y = 0; y < frame.height; ++y)
    std::memcpy(tight.data() + static_cast<std::size_t>(y) * rowBytes,
                frame.bits + static_cast<std::size_t>(y) * frame.stride, rowBytes);
  return tight;
}
} // namespace

StitchAccumulator::StitchAccumulator(const QImage &first, Axis axis, bool &ok,
                                     QString &error)
    : axis_(axis) {
  const RgbaFrame frame = asRgba(first);
  if (!validateFrame(frame, 0, 0, error)) {
    ok = false;
    return;
  }
  width_ = frame.width;
  height_ = frame.height;
  axisLen_ = axis == Axis::Vertical ? height_ : width_;
  crossLen_ = axis == Axis::Vertical ? width_ : height_;
  maxEdge_ = std::min(axisLen_ / 8, kMaxStationaryEdge);
  firstRgba_ = copyFrameTight(frame);
  edgeSums_.assign(maxEdge_, 0);
  edgeCounts_.assign(maxEdge_, 0);
  alignedSums_.assign(maxEdge_, 0);
  alignedCounts_.assign(maxEdge_, 0);
  valid_ = true;
  ok = true;
}

bool StitchAccumulator::pushForward(const QImage &image, int delta,
                                    QString &error) {
  if (direction_ == Direction::Reverse) {
    error = QStringLiteral(
        "cannot mix forward and reverse frames in one scroll capture");
    return false;
  }
  if (!pushOriented(image, delta, error))
    return false;
  direction_ = Direction::Forward;
  return true;
}

bool StitchAccumulator::pushReverse(const QImage &image, int delta,
                                    QString &error) {
  if (direction_ == Direction::Forward) {
    error = QStringLiteral(
        "cannot mix forward and reverse frames in one scroll capture");
    return false;
  }
  // Reverse input is reflected along the stitch axis, turning it into the same
  // forward composition; the finished buffer is reflected back at finish().
  const QImage reflected = axis_ == Axis::Vertical
                               ? image.flipped(Qt::Vertical)
                               : image.flipped(Qt::Horizontal);
  const bool firstReverse = direction_ == Direction::None;
  if (firstReverse)
    reverseRgbaAxisInPlace(firstRgba_, width_, height_, axis_);
  if (!pushOriented(reflected, delta, error)) {
    if (firstReverse)
      reverseRgbaAxisInPlace(firstRgba_, width_, height_, axis_);
    return false;
  }
  direction_ = Direction::Reverse;
  return true;
}

bool StitchAccumulator::pushOriented(const QImage &image, int delta,
                                     QString &error) {
  if (!valid_) {
    error = QStringLiteral("accumulator is not initialized");
    return false;
  }
  const RgbaFrame frame = asRgba(image);
  if (!validateFrame(frame, width_, height_, error))
    return false;
  if (delta <= 0 || delta >= axisLen_) {
    error = QStringLiteral("invalid stitch delta %1 for frame extent %2")
                .arg(delta)
                .arg(axisLen_);
    return false;
  }
  const long newTotal = static_cast<long>(totalDelta_) + delta;
  if (axisLen_ + newTotal > 2147483647L) {
    error = QStringLiteral("stitched image extent exceeds the supported size");
    return false;
  }
  // Refuse before mutating, so the capture so far stays intact and can still
  // be finished: the caller reports this and the user stitches what they have.
  if (exceedsStitchBudget(crossLen_, axisLen_ + newTotal)) {
    error = QStringLiteral("scroll capture reached its maximum length "
                           "(%1 MB); finish to keep what was captured")
                .arg(kMaxStitchedBytes / (1024 * 1024));
    return false;
  }

  // Compute everything before mutating so a bad frame leaves no partial state.
  const int retainedExtent = delta + maxEdge_;
  const int sourceAxisStart = std::max(0, axisLen_ - retainedExtent);
  const int crossStep = std::max(1, crossLen_ / 512);

  // Retained tail band.
  TailBand band;
  band.delta = delta;
  band.sourceAxisStart = sourceAxisStart;
  if (axis_ == Axis::Vertical) {
    const int rowBytes = width_ * 4;
    band.rgba.reserve(static_cast<std::size_t>(rowBytes) * (height_ - sourceAxisStart));
    for (int y = sourceAxisStart; y < height_; ++y)
      band.rgba.insert(band.rgba.end(), frame.bits + static_cast<std::size_t>(y) * frame.stride,
                       frame.bits + static_cast<std::size_t>(y) * frame.stride + rowBytes);
  } else {
    const int bandWidth = width_ - sourceAxisStart;
    band.rgba.reserve(static_cast<std::size_t>(bandWidth) * 4 * height_);
    for (int y = 0; y < height_; ++y) {
      const std::uint8_t *rowStart =
          frame.bits + static_cast<std::size_t>(y) * frame.stride + sourceAxisStart * 4;
      band.rgba.insert(band.rgba.end(), rowStart, rowStart + bandWidth * 4);
    }
  }

  // Edge observation: trailing depth vs the SAME position in the first frame.
  std::vector<std::uint64_t> edgeSums(maxEdge_, 0), edgeCounts(maxEdge_, 0);
  for (int depth = 0; depth < maxEdge_; ++depth) {
    const int position = axisLen_ - depth - 1;
    for (int cross = 0; cross < crossLen_; cross += crossStep) {
      const int x = axis_ == Axis::Vertical ? cross : position;
      const int y = axis_ == Axis::Vertical ? position : cross;
      const std::size_t firstOff = (static_cast<std::size_t>(y) * width_ + x) * 4;
      const std::size_t otherOff = static_cast<std::size_t>(y) * frame.stride + x * 4;
      for (int c = 0; c < 3; ++c) {
        edgeSums[depth] += static_cast<std::uint64_t>(
            std::abs(static_cast<int>(firstRgba_[firstOff + c]) -
                     static_cast<int>(frame.bits[otherOff + c])));
        ++edgeCounts[depth];
      }
    }
  }

  // Aligned observation: previous trailing rows vs the same document rows in
  // this frame (shifted by delta).
  std::vector<std::uint64_t> alignedSums(maxEdge_, 0), alignedCounts(maxEdge_, 0);
  const std::vector<std::uint8_t> &previous =
      bands_.empty() ? firstRgba_ : bands_.back().rgba;
  const int previousStart = bands_.empty() ? 0 : bands_.back().sourceAxisStart;
  const int previousRowBytes =
      bands_.empty() ? width_ * 4
      : axis_ == Axis::Vertical
          ? width_ * 4
          : (axisLen_ - bands_.back().sourceAxisStart) * 4;
  for (int depth = 0; depth < maxEdge_; ++depth) {
    const int position = axisLen_ - depth - 1;
    const int aligned = position - delta;
    if (aligned < 0 || position < previousStart)
      continue;
    for (int cross = 0; cross < crossLen_; cross += crossStep) {
      std::size_t prevOff, otherOff;
      if (axis_ == Axis::Vertical) {
        prevOff = static_cast<std::size_t>(position - previousStart) * previousRowBytes + cross * 4;
        otherOff = static_cast<std::size_t>(aligned) * frame.stride + cross * 4;
      } else {
        prevOff = static_cast<std::size_t>(cross) * previousRowBytes + (position - previousStart) * 4;
        otherOff = static_cast<std::size_t>(cross) * frame.stride + aligned * 4;
      }
      for (int c = 0; c < 3; ++c) {
        alignedSums[depth] += static_cast<std::uint64_t>(
            std::abs(static_cast<int>(previous[prevOff + c]) -
                     static_cast<int>(frame.bits[otherOff + c])));
        ++alignedCounts[depth];
      }
    }
  }

  // Commit.
  for (int depth = 0; depth < maxEdge_; ++depth) {
    edgeSums_[depth] += edgeSums[depth];
    edgeCounts_[depth] += edgeCounts[depth];
    alignedSums_[depth] += alignedSums[depth];
    alignedCounts_[depth] += alignedCounts[depth];
  }
  bands_.push_back(std::move(band));
  totalDelta_ = static_cast<int>(newTotal);
  return true;
}

int StitchAccumulator::stationaryTrailingStrip() const {
  if (bands_.empty())
    return 0;
  int strip = 0;
  for (int depth = 0; depth < maxEdge_; ++depth) {
    const double error = edgeCounts_[depth] == 0
                             ? kInf
                             : static_cast<double>(edgeSums_[depth]) / edgeCounts_[depth];
    if (error > kStationaryEdgeError)
      break;
    ++strip;
  }
  return strip;
}

int StitchAccumulator::nearStationaryTrailingZone(int strip) const {
  if (bands_.empty())
    return 0;
  int zone = 0;
  for (int depth = strip; depth < maxEdge_; ++depth) {
    const double error = alignedCounts_[depth] == 0
                             ? 0.0
                             : static_cast<double>(alignedSums_[depth]) / alignedCounts_[depth];
    if (error <= kNearStationaryError)
      break;
    ++zone;
  }
  return zone;
}

bool StitchAccumulator::wouldExceedBudget(int delta) const {
  if (!valid_ || delta <= 0)
    return false;
  return exceedsStitchBudget(crossLen_,
                             axisLen_ + static_cast<long long>(totalDelta_) + delta);
}

bool StitchAccumulator::exceedsWidelyOpenableEdge() const {
  if (!valid_)
    return false;
  const long long motion = static_cast<long long>(axisLen_) + totalDelta_;
  return motion > kWidelyOpenableEdge || crossLen_ > kWidelyOpenableEdge;
}

long StitchAccumulator::retainedRgbaBytes() const {
  long total = static_cast<long>(firstRgba_.size());
  for (const TailBand &band : bands_)
    total += static_cast<long>(band.rgba.size());
  return total;
}

QImage StitchAccumulator::finish(QString &error) {
  if (!valid_) {
    error = QStringLiteral("accumulator is not initialized");
    return {};
  }
  const int strip = stationaryTrailingStrip();
  const int trailing = strip + nearStationaryTrailingZone(strip);
  const int contentExtent = std::max(0, axisLen_ - trailing);
  if (contentExtent == 0) {
    error = QStringLiteral("stationary trailing edge covers the entire capture");
    return {};
  }
  for (const TailBand &band : bands_) {
    if (band.delta >= contentExtent) {
      error = QStringLiteral("invalid stitch delta %1 for content extent %2")
                  .arg(band.delta)
                  .arg(contentExtent);
      return {};
    }
  }

  if (axis_ == Axis::Vertical) {
    const int contentBottom = height_ - trailing;
    const int totalHeight = height_ + totalDelta_;
    const int rowBytes = width_ * 4;
    // Rows go straight into the image. Assembling into a vector first and
    // copying would hold the finished image twice, which is the difference
    // between fitting and being OOM-killed on a very long capture. A reverse
    // capture is emitted back-to-front rather than reversed afterwards.
    QImage image(width_, totalHeight, QImage::Format_RGBA8888);
    if (image.isNull()) {
      error = QStringLiteral("could not allocate the stitched image (%1x%2)")
                  .arg(width_)
                  .arg(totalHeight);
      return {};
    }
    const bool reverse = direction_ == Direction::Reverse;
    int row = 0;
    const auto emitRows = [&](const std::vector<std::uint8_t> &source,
                              std::size_t byteStart, int rows) {
      for (int i = 0; i < rows; ++i, ++row) {
        const int destination = reverse ? totalHeight - 1 - row : row;
        std::memcpy(image.scanLine(destination),
                    source.data() + byteStart +
                        static_cast<std::size_t>(i) * rowBytes,
                    rowBytes);
      }
    };
    // First frame's content rows.
    emitRows(firstRgba_, 0, contentBottom);
    // Each band's delta rows.
    for (const TailBand &band : bands_) {
      const int sourceStart = contentBottom - band.delta;
      const int localStart = sourceStart - band.sourceAxisStart;
      emitRows(band.rgba, static_cast<std::size_t>(localStart) * rowBytes,
               band.delta);
    }
    // One trailing strip from the last band.
    if (trailing > 0) {
      const TailBand &last = bands_.back();
      const int localStart = contentBottom - last.sourceAxisStart;
      emitRows(last.rgba, static_cast<std::size_t>(localStart) * rowBytes,
               trailing);
    }
    return image;
  }

  // Horizontal.
  const int contentRight = width_ - trailing;
  const int totalWidth = width_ + totalDelta_;
  QImage image(totalWidth, height_, QImage::Format_RGBA8888);
  for (int y = 0; y < height_; ++y) {
    std::uint8_t *dst = image.scanLine(y);
    std::size_t cursor = 0;
    const std::size_t firstRow = static_cast<std::size_t>(y) * width_ * 4;
    std::memcpy(dst, firstRgba_.data() + firstRow, static_cast<std::size_t>(contentRight) * 4);
    cursor += static_cast<std::size_t>(contentRight) * 4;
    for (const TailBand &band : bands_) {
      const int bandWidth = width_ - band.sourceAxisStart;
      const int sourceStart = contentRight - band.delta;
      const int localStart = sourceStart - band.sourceAxisStart;
      const std::size_t byteStart = (static_cast<std::size_t>(y) * bandWidth + localStart) * 4;
      std::memcpy(dst + cursor, band.rgba.data() + byteStart, static_cast<std::size_t>(band.delta) * 4);
      cursor += static_cast<std::size_t>(band.delta) * 4;
    }
    if (trailing > 0) {
      const TailBand &last = bands_.back();
      const int bandWidth = width_ - last.sourceAxisStart;
      const int localStart = contentRight - last.sourceAxisStart;
      const std::size_t byteStart = (static_cast<std::size_t>(y) * bandWidth + localStart) * 4;
      std::memcpy(dst + cursor, last.rgba.data() + byteStart, static_cast<std::size_t>(trailing) * 4);
    }
  }
  if (direction_ == Direction::Reverse)
    image = image.flipped(Qt::Horizontal);
  return image;
}

QImage stitchWithDeltas(const QVector<QImage> &frames, const QVector<int> &deltas,
                        Axis axis, QString &error) {
  if (frames.isEmpty()) {
    error = QStringLiteral("no frames to stitch");
    return {};
  }
  if (deltas.size() != frames.size() - 1) {
    error = QStringLiteral("deltas must have one entry per frame gap");
    return {};
  }
  bool ok = false;
  StitchAccumulator accumulator(frames.first(), axis, ok, error);
  if (!ok)
    return {};
  for (int i = 1; i < frames.size(); ++i)
    if (!accumulator.pushForward(frames.at(i), deltas.at(i - 1), error))
      return {};
  return accumulator.finish(error);
}

// --- Forward lookahead (auto-scroll alignment verification) -----------------
namespace {
double plausibleErrorLimit(double best) {
  return std::max(best * kMinConfidence, best + kMinErrorMargin);
}

bool errorsAreDistinct(double best, double runner) {
  if (!std::isfinite(best) || !std::isfinite(runner))
    return std::isinf(runner) && std::isfinite(best);
  double confidence;
  if (best <= std::numeric_limits<double>::epsilon())
    confidence = runner <= std::numeric_limits<double>::epsilon() ? 1.0 : kInf;
  else
    confidence = runner / best;
  return confidence >= kMinConfidence && runner - best >= kMinErrorMargin;
}

bool forwardCandidateLess(const ForwardMatchCandidate &a,
                          const ForwardMatchCandidate &b) {
  if (a.error != b.error || std::isnan(a.error) || std::isnan(b.error))
    return errorLess(a.error, b.error);
  return a.delta < b.delta;
}

bool forwardPathLess(const ForwardMatchPath &a, const ForwardMatchPath &b) {
  if (a.totalError != b.totalError || std::isnan(a.totalError) ||
      std::isnan(b.totalError))
    return errorLess(a.totalError, b.totalError);
  if (a.firstDelta != b.firstDelta)
    return a.firstDelta < b.firstDelta;
  return a.secondDelta < b.secondDelta;
}

bool matcherGradeCandidate(const ForwardMatchCandidate &candidate) {
  return std::isfinite(candidate.error) && candidate.error <= kMaxMatchError;
}

/// The automatic worker uses 3 wheel notches followed by a 1-notch probe, or
/// its keyboard fallback of 5 arrows followed by 2. Cadence is only a soft
/// tie-breaker.
double autoCadencePriorScore(const ForwardMatchPath &path) {
  const double ratio =
      static_cast<double>(path.firstDelta) / static_cast<double>(path.secondDelta);
  return std::min(std::abs(ratio - 3.0), std::abs(ratio - 2.5));
}

bool autoPeriodicPathLess(const ForwardMatchPath &a, const ForwardMatchPath &b) {
  if (a.totalError != b.totalError || std::isnan(a.totalError) ||
      std::isnan(b.totalError))
    return errorLess(a.totalError, b.totalError);
  const double cadenceA = autoCadencePriorScore(a);
  const double cadenceB = autoCadencePriorScore(b);
  if (cadenceA != cadenceB)
    return errorLess(cadenceA, cadenceB);
  if (a.firstDelta != b.firstDelta)
    return a.firstDelta < b.firstDelta;
  return a.secondDelta < b.secondDelta;
}

std::vector<ForwardMatchPath>
consistentForwardPaths(const ForwardCandidateSet &first,
                       const ForwardCandidateSet &second,
                       const ForwardCandidateSet &cumulative) {
  std::vector<ForwardMatchPath> paths;
  for (const ForwardMatchCandidate &a : first.candidates) {
    for (const ForwardMatchCandidate &b : second.candidates) {
      const long sum = static_cast<long>(a.delta) + b.delta;
      const ForwardMatchCandidate *total = nullptr;
      for (const ForwardMatchCandidate &c : cumulative.candidates) {
        if (std::abs(static_cast<long>(c.delta) - sum) > kPathDeltaTolerance)
          continue;
        if (!total || forwardCandidateLess(c, *total))
          total = &c;
      }
      if (!total)
        continue;
      const double consistencyError =
          static_cast<double>(std::abs(static_cast<long>(total->delta) - sum));
      paths.push_back(
          {a.delta, b.delta, a.error + b.error + total->error + consistencyError});
    }
  }
  return paths;
}

ForwardCandidateSet matcherGradeSet(const ForwardCandidateSet &set) {
  ForwardCandidateSet graded;
  graded.truncated = set.truncated;
  graded.searchMaxDelta = set.searchMaxDelta;
  for (const ForwardMatchCandidate &candidate : set.candidates)
    if (matcherGradeCandidate(candidate))
      graded.candidates.push_back(candidate);
  return graded;
}

/// Cumulatively consistent path for known-forward automatic capture; each
/// candidate is error-filtered individually so a very good pair cannot mask
/// a poor seam in the summed score.
std::optional<ForwardMatchPath>
bestMatcherGradeAutoPath(const ForwardCandidateSet &first,
                         const ForwardCandidateSet &second,
                         const ForwardCandidateSet &cumulative) {
  std::vector<ForwardMatchPath> paths = consistentForwardPaths(
      matcherGradeSet(first), matcherGradeSet(second), matcherGradeSet(cumulative));
  if (paths.empty())
    return std::nullopt;
  std::stable_sort(paths.begin(), paths.end(), autoPeriodicPathLess);
  return paths.front();
}

/// If both adjacent seams are sound but their sum exceeds the largest F0→F2
/// displacement retaining minimum overlap, the absent cumulative candidate is
/// not contradictory evidence.
std::optional<ForwardMatchPath>
bestUnverifiableAutoPath(const ForwardCandidateSet &first,
                         const ForwardCandidateSet &second,
                         const ForwardCandidateSet &cumulative) {
  if (!cumulative.searchMaxDelta)
    return std::nullopt;
  std::vector<ForwardMatchPath> paths;
  for (const ForwardMatchCandidate &a : first.candidates) {
    if (!matcherGradeCandidate(a))
      continue;
    for (const ForwardMatchCandidate &b : second.candidates) {
      if (!matcherGradeCandidate(b))
        continue;
      const long sum = static_cast<long>(a.delta) + b.delta;
      if (sum <= *cumulative.searchMaxDelta)
        continue;
      paths.push_back({a.delta, b.delta, a.error + b.error});
    }
  }
  if (paths.empty())
    return std::nullopt;
  std::stable_sort(paths.begin(), paths.end(), autoPeriodicPathLess);
  return paths.front();
}

std::optional<ForwardMatchPath>
bestIndependentPath(const ForwardCandidateSet &first,
                    const ForwardCandidateSet &second) {
  if (first.candidates.empty() || second.candidates.empty())
    return std::nullopt;
  const ForwardMatchCandidate &a = first.candidates.front();
  const ForwardMatchCandidate &b = second.candidates.front();
  return ForwardMatchPath{a.delta, b.delta, a.error + b.error};
}
} // namespace

ForwardCandidateSet forwardCandidateSet(const GrayView &prev,
                                        const GrayView &cur, Axis axis) {
  ForwardCandidateSet result;
  if (prev.width != cur.width || prev.height != cur.height ||
      static_cast<int>(prev.pixels.size()) != prev.width * prev.height ||
      static_cast<int>(cur.pixels.size()) != cur.width * cur.height)
    return result;
  const int axisLen = prev.axisLen(axis);
  const int crossLen = prev.crossLen(axis);
  if (axisLen < kMinOverlapPixels + kMinMotionPixels || crossLen < 2)
    return result;

  const int minOverlap = std::min(
      std::max(axisLen / kMinOverlapDen, kMinOverlapPixels), std::max(0, axisLen - 1));
  const int maxShift = std::max(0, axisLen - minOverlap);
  const int sourceScale = prev.sourceScale(axis);
  result.searchMaxDelta = maxShift * sourceScale;
  const int minShift = kMinMotionPixels / sourceScale + 1;
  if (minShift > maxShift)
    return result;

  const int matchExtent = axisLen - maxShift;
  const int coarseAxis = std::max(1, matchExtent / 32);
  const int coarseCross = std::max(1, crossLen / 48);
  const int fineAxis = std::max(1, matchExtent / 256);
  const int fineCross = std::max(1, crossLen / 256);

  // Search only the physically possible direction; every forward shift that
  // preserves the global minimum overlap remains eligible.
  std::vector<SearchSample> coarse;
  coarse.reserve(static_cast<std::size_t>(maxShift - minShift) + 1);
  for (long shift = minShift; shift <= maxShift; ++shift)
    coarse.push_back({shift, scoreShift(prev, cur, axis, shift, maxShift,
                                        coarseAxis, coarseCross)});
  std::stable_sort(coarse.begin(), coarse.end(),
                   [](const SearchSample &a, const SearchSample &b) {
                     return errorLess(a.error, b.error);
                   });
  if (coarse.empty())
    return result;
  const SearchSample coarseBest = coarse.front();
  if (!std::isfinite(coarseBest.error) || coarseBest.error > kMaxMatchError)
    return result;

  const double coarseLimit =
      std::min(plausibleErrorLimit(coarseBest.error), kMaxMatchError);
  const long peakNeighborhood = std::clamp(axisLen / 256, 6, 16);
  std::vector<SearchSample> peaks;
  for (const SearchSample &sample : coarse) {
    if (sample.error > coarseLimit)
      break;
    if (std::any_of(peaks.begin(), peaks.end(), [&](const SearchSample &peak) {
          return std::abs(peak.shift - sample.shift) <= peakNeighborhood;
        }))
      continue;
    if (static_cast<int>(peaks.size()) == kMaxForwardCandidates) {
      result.truncated = true;
      break;
    }
    peaks.push_back(sample);
  }

  std::vector<ForwardMatchCandidate> refined;
  refined.reserve(peaks.size());
  for (const SearchSample &peak : peaks) {
    const long start = std::max<long>(peak.shift - 1, minShift);
    const long end = std::min<long>(peak.shift + 1, maxShift);
    SearchSample best{peak.shift, kInf};
    for (long shift = start; shift <= end; ++shift) {
      const double error =
          scoreShift(prev, cur, axis, shift, maxShift, fineAxis, fineCross);
      if (error < best.error)
        best = {shift, error};
    }
    const int delta = static_cast<int>(std::abs(best.shift)) * sourceScale;
    if (delta <= kMinMotionPixels || best.error > kMaxMatchError)
      continue;
    auto existing = std::find_if(
        refined.begin(), refined.end(), [&](const ForwardMatchCandidate &c) {
          return std::abs(static_cast<long>(c.delta) - delta) <= peakNeighborhood;
        });
    if (existing != refined.end()) {
      if (best.error < existing->error)
        *existing = {delta, best.error};
    } else {
      refined.push_back({delta, best.error});
    }
  }

  std::stable_sort(refined.begin(), refined.end(), forwardCandidateLess);
  if (!refined.empty()) {
    const ForwardMatchCandidate best = refined.front();
    refined.erase(std::remove_if(refined.begin(), refined.end(),
                                 [&](const ForwardMatchCandidate &c) {
                                   return !(c == best) &&
                                          errorsAreDistinct(best.error, c.error);
                                 }),
                  refined.end());
  }
  result.candidates = std::move(refined);
  return result;
}

ForwardMatch classifyForwardWithLookahead(const GrayView &prev,
                                          const GrayView &cur, Axis axis) {
  const MotionEstimate estimate = classifyMotion(prev, cur, axis);
  const bool matcherGradeAmbiguity =
      estimate.motion.kind == MotionKind::Unmatchable &&
      std::isfinite(estimate.error) && estimate.error <= kMaxMatchError;
  const bool needsLookahead = estimate.motion.kind == MotionKind::Ambiguous ||
                              estimate.motion.kind == MotionKind::Reverse ||
                              matcherGradeAmbiguity;
  ForwardMatch match;
  match.classified = estimate;
  if (!needsLookahead)
    return match;
  ForwardCandidateSet first = forwardCandidateSet(prev, cur, axis);
  if (first.candidates.empty() ||
      first.candidates.front().error > kMaxMatchError)
    return match;
  match.tag = ForwardMatch::Tag::Ambiguous;
  match.ambiguous.emplace(prev, cur, axis, std::move(first), estimate);
  return match;
}

ForwardLookaheadResolution
resolveForwardCandidateSets(const ForwardCandidateSet &first,
                            const ForwardCandidateSet &second,
                            const ForwardCandidateSet &cumulative,
                            bool allowPhysicalPrior) {
  std::vector<ForwardMatchPath> paths =
      consistentForwardPaths(first, second, cumulative);
  std::stable_sort(paths.begin(), paths.end(), forwardPathLess);
  const std::optional<ForwardMatchPath> bestConsistent =
      paths.empty() ? std::nullopt : std::optional(paths.front());
  std::optional<ForwardMatchPath> fallback = bestConsistent;
  if (!fallback)
    fallback = bestIndependentPath(first, second);

  ForwardLookaheadResolution resolution;
  if (!bestConsistent) {
    if (allowPhysicalPrior) {
      if (std::optional<ForwardMatchPath> path =
              bestUnverifiableAutoPath(first, second, cumulative)) {
        resolution.tag = ForwardLookaheadResolution::Tag::LowErrorPeriodic;
        resolution.path = path;
        return resolution;
      }
    }
    resolution.tag = ForwardLookaheadResolution::Tag::Unresolved;
    resolution.path = fallback;
    return resolution;
  }

  const bool unique =
      paths.size() < 2 ||
      errorsAreDistinct(bestConsistent->totalError, paths[1].totalError);
  if (unique && !first.truncated && !second.truncated && !cumulative.truncated) {
    resolution.tag = ForwardLookaheadResolution::Tag::Resolved;
    resolution.path = bestConsistent;
    return resolution;
  }
  if (allowPhysicalPrior) {
    if (std::optional<ForwardMatchPath> path =
            bestMatcherGradeAutoPath(first, second, cumulative)) {
      resolution.tag = ForwardLookaheadResolution::Tag::LowErrorPeriodic;
      resolution.path = path;
      return resolution;
    }
  }
  resolution.tag = ForwardLookaheadResolution::Tag::Unresolved;
  resolution.path = bestConsistent;
  return resolution;
}

std::optional<ForwardMatchCandidate>
ForwardLookahead::uniqueCandidateAtMost(int maxSourceDelta) const {
  if (first_.truncated)
    return std::nullopt;
  std::optional<ForwardMatchCandidate> match;
  for (const ForwardMatchCandidate &candidate : first_.candidates) {
    if (candidate.delta > maxSourceDelta)
      continue;
    if (match)
      return std::nullopt;
    match = candidate;
  }
  return match;
}

ForwardLookaheadResolution
ForwardLookahead::resolve(const GrayView &lookahead) const {
  return resolveWithPhysicalPrior(lookahead, false);
}

ForwardLookaheadResolution
ForwardLookahead::resolveAuto(const GrayView &lookahead) const {
  return resolveWithPhysicalPrior(lookahead, true);
}

ForwardLookaheadResolution
ForwardLookahead::resolveWithPhysicalPrior(const GrayView &lookahead,
                                           bool allowPhysicalPrior) const {
  // A probe can legitimately capture the pending viewport again (end of page,
  // wheel not yet applied, paint not advanced). The forward-only search
  // excludes zero and would find a periodic non-zero alias, fabricating a
  // second band, so classify zero motion first and keep only the first
  // match.
  const MotionEstimate probe = classifyMotion(pending_, lookahead, axis_);
  if (probe.motion.kind == MotionKind::Stationary) {
    ForwardLookaheadResolution resolution;
    resolution.tag = ForwardLookaheadResolution::Tag::StationaryProbe;
    StationaryProbeFirstMatch firstMatch;
    const std::optional<ForwardMatchCandidate> best =
        first_.candidates.empty() ? std::nullopt
                                  : std::optional(first_.candidates.front());
    if (!first_.truncated && first_.candidates.size() == 1) {
      firstMatch.tag = StationaryProbeFirstMatch::Tag::Unique;
      firstMatch.candidate = best;
    } else {
      firstMatch.tag = StationaryProbeFirstMatch::Tag::Ambiguous;
      firstMatch.candidate = best;
    }
    resolution.firstMatch = firstMatch;
    return resolution;
  }
  const ForwardCandidateSet second =
      forwardCandidateSet(pending_, lookahead, axis_);
  const ForwardCandidateSet cumulative =
      forwardCandidateSet(origin_, lookahead, axis_);
  return resolveForwardCandidateSets(first_, second, cumulative,
                                     allowPhysicalPrior);
}

// --- Live manual capture session ------------------------------------------------
namespace {
constexpr double kAmbiguousMaxError = 2.0;       // MANUAL_AMBIGUOUS_MAX_ERROR
constexpr double kAmbiguousMinConfidence = 1.02; // MANUAL_AMBIGUOUS_MIN_CONFIDENCE
constexpr int kMaxBlankFirstFrames = 8;          // MAX_CONSECUTIVE_BLANK_FRAMES

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

int ManualCapture::coalesceThreshold(int axisLen) {
  return std::min(std::clamp(axisLen / 8, 16, 128), std::max(1, axisLen - 1));
}

int ManualCapture::manualSearchBound(int axisLen) {
  return std::min(std::clamp(axisLen / 8, 128, 512), std::max(1, axisLen - 1));
}

ManualCapture::ManualCapture(Axis axis) : axis_(axis) {}

ManualCapture::Outcome ManualCapture::outcome(Event event,
                                              const MotionEstimate &estimate,
                                              QString error) const {
  Outcome result;
  result.event = event;
  result.estimate = estimate;
  result.keptFrames = kept_;
  result.pendingDelta = pending_ ? pending_->delta : 0;
  result.error = std::move(error);
  return result;
}

ManualCapture::Outcome ManualCapture::feed(const QImage &input) {
  const QImage cropped = input.format() == QImage::Format_RGBA8888
                             ? input
                             : input.convertToFormat(QImage::Format_RGBA8888);
  QString error;
  const GrayView gray = downsampleToGray(cropped, axis_);
  const int axisLen = gray.axisLen(axis_);

  if (!accumulator_) {
    if (imageIsUniform(cropped)) {
      ++blankFirstFrames_;
      return outcome(Event::Blank, {},
                     blankFirstFrames_ >= kMaxBlankFirstFrames
                         ? QStringLiteral("capture shows only a solid color")
                         : QString());
    }
    bool ok = false;
    accumulator_.emplace(cropped, axis_, ok, error);
    if (!ok) {
      accumulator_.reset();
      return outcome(Event::Error, {}, error);
    }
    lastGray_ = gray;
    previousGray_ = gray;
    havePrevious_ = true;
    kept_ = 1;
    return outcome(Event::Seeded);
  }

  MotionEstimate est = classifyMotion(lastGray_, gray, axis_);
  if (est.motion.kind == MotionKind::Ambiguous) {
    // Repeated content: re-search close to the small motion a hand scroll
    // makes and take a clean nearby peak (recover_bounded_manual_motion).
    const int bound = manualSearchBound(axisLen);
    const MotionEstimate bounded =
        classifyMotionBounded(lastGray_, gray, axis_, bound);
    int signedDelta = 0;
    switch (bounded.motion.kind) {
    case MotionKind::Forward: signedDelta = bounded.motion.delta; break;
    case MotionKind::Reverse: signedDelta = -bounded.motion.delta; break;
    case MotionKind::Ambiguous: signedDelta = bounded.motion.delta; break;
    default: break;
    }
    if (signedDelta != 0 && std::abs(signedDelta) <= bound &&
        std::isfinite(bounded.error) && bounded.error <= kAmbiguousMaxError &&
        bounded.confidence >= kAmbiguousMinConfidence)
      est = bounded;
  }

  // Motion since the previous grab tells "at rest but looks different" (a
  // video or hover UI changed in place) from real scrolling when the
  // reference cannot be matched.
  const bool atRest =
      havePrevious_ &&
      classifyMotion(previousGray_, gray, axis_).motion.kind ==
          MotionKind::Stationary;
  previousGray_ = gray;
  havePrevious_ = true;

  switch (est.motion.kind) {
  case MotionKind::Forward:
    return recordMotion(cropped, gray, Direction::Forward, est.motion.delta, est);
  case MotionKind::Reverse:
    return recordMotion(cropped, gray, Direction::Reverse, est.motion.delta, est);
  case MotionKind::Stationary:
    if (pending_) {
      // Stationary relative to the committed reference: the small pending
      // movement was reversed; drop it and re-arm.
      pending_.reset();
      if (accumulator_->frameCount() == 1)
        direction_ = Direction::None;
      return outcome(Event::PendingDropped, est);
    }
    return outcome(Event::Still, est);
  case MotionKind::Ambiguous: {
    // A small, clean ambiguous candidate may be held as the pending frame; a
    // large or noisy one asks the user to keep scrolling.
    const int delta = std::abs(est.motion.delta);
    const Direction direction =
        est.motion.delta > 0 ? Direction::Forward : Direction::Reverse;
    if (delta > 0 && delta < coalesceThreshold(axisLen) &&
        std::isfinite(est.error) && est.error <= kAmbiguousMaxError &&
        est.confidence >= kAmbiguousMinConfidence)
      return recordMotion(cropped, gray, direction, delta, est);
    return outcome(Event::Ambiguous, est);
  }
  case MotionKind::Unmatchable:
    if (accumulator_->frameCount() == 1 && !pending_ && atRest) {
      // Nothing committed yet and the page is at rest but no longer matches
      // the first frame: its content changed in place. Re-seed from the
      // current appearance, and nothing captured is lost.
      bool ok = false;
      QString seedError;
      accumulator_.emplace(cropped, axis_, ok, seedError);
      if (!ok)
        return outcome(Event::Error, est, seedError);
      lastGray_ = gray;
      direction_ = Direction::None;
      return outcome(Event::ReSeeded, est);
    }
    // Keep the last verified reference; a later frame that overlaps it
    // (scroll back a little) resumes the capture.
    return outcome(Event::Unmatchable, est);
  }
  return outcome(Event::Still, est);
}

ManualCapture::Outcome ManualCapture::recordMotion(const QImage &cropped,
                                                   const GrayView &gray,
                                                   Direction direction,
                                                   int delta,
                                                   const MotionEstimate &estimate) {
  const bool hasBand = accumulator_->frameCount() > 1;
  if (direction_ != Direction::None && direction_ != direction) {
    // One stitch is one direction; before any band is committed the user may
    // still choose it (crossing back over the first viewport).
    if (hasBand)
      return outcome(Event::WrongDirection, estimate);
    pending_.reset();
    direction_ = direction;
  } else if (direction_ == Direction::None) {
    direction_ = direction;
  }
  const int axisLen = gray.axisLen(axis_);
  if (delta < coalesceThreshold(axisLen)) {
    // Hold small movement back; it is replaced as it grows and committed by
    // the next large step or by finish().
    pending_ = PendingFrame{cropped, delta, direction};
    return outcome(Event::Pending, estimate);
  }
  if (accumulator_->wouldExceedBudget(delta)) {
    // A designed limit, not a failure: the capture so far is intact and the
    // caller stops here rather than refusing a frame every tick.
    return outcome(Event::Full, estimate);
  }
  QString error;
  const bool pushed = direction == Direction::Forward
                          ? accumulator_->pushForward(cropped, delta, error)
                          : accumulator_->pushReverse(cropped, delta, error);
  if (!pushed)
    return outcome(Event::Error, estimate, error);
  lastGray_ = gray;
  pending_.reset();
  ++kept_;
  return outcome(Event::Kept, estimate);
}

QImage ManualCapture::finish(QString &error) {
  if (!accumulator_) {
    error = QStringLiteral("no frames were captured");
    return {};
  }
  if (pending_) {
    const PendingFrame pending = *pending_;
    const bool pushed =
        pending.direction == Direction::Forward
            ? accumulator_->pushForward(pending.frame, pending.delta, error)
            : accumulator_->pushReverse(pending.frame, pending.delta, error);
    if (!pushed)
      return {};
    pending_.reset();
    ++kept_;
  }
  return accumulator_->finish(error);
}

} // namespace stitch
