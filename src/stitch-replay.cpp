/** @fileoverview stitch-replay: offline scroll-capture stitcher harness, so
 *  a recorded capture can be re-stitched without a compositor. Loads dumped
 *  frames, classifies each adjacent pair (accepting only a decisive forward
 *  match), stitches the accepted deltas, and writes the result.
 *
 *  Env: OMASNAP_STITCH_REPLAY_DIR (frames dir), OMASNAP_STITCH_REPLAY_OUT
 *  (output PNG), OMASNAP_STITCH_REPLAY_AXIS (vertical|horizontal, default
 *  vertical). With --classify it prints per-frame classifications instead. */
#include "stitch.hpp"

#include <QDir>
#include <QImage>
#include <QStringList>

#include <cstdio>

using namespace stitch;

int main(int argc, char **argv) {
  const bool classifyOnly = argc > 1 && QString::fromLocal8Bit(argv[1]) ==
                                            QStringLiteral("--classify");
  const QString dir = qEnvironmentVariable("OMASNAP_STITCH_REPLAY_DIR");
  const QString out = qEnvironmentVariable("OMASNAP_STITCH_REPLAY_OUT");
  const Axis axis = qEnvironmentVariable("OMASNAP_STITCH_REPLAY_AXIS") ==
                            QStringLiteral("horizontal")
                        ? Axis::Horizontal
                        : Axis::Vertical;
  if (dir.isEmpty()) {
    std::fprintf(stderr, "OMASNAP_STITCH_REPLAY_DIR unset\n");
    return 2;
  }
  QStringList files = QDir(dir).entryList({QStringLiteral("frame-*.png")},
                                          QDir::Files, QDir::Name);
  if (files.isEmpty()) {
    std::fprintf(stderr, "no frames in %s\n", qPrintable(dir));
    return 2;
  }
  std::fprintf(stderr, "replay: %lld frames from %s\n",
               static_cast<long long>(files.size()), qPrintable(dir));

  QImage prevImage(QDir(dir).filePath(files.first()));
  prevImage = prevImage.convertToFormat(QImage::Format_RGBA8888);
  GrayView prevGray = downsampleToGray(prevImage, axis);

  QString accError;
  bool accOk = false;
  StitchAccumulator accumulator(prevImage, axis, accOk, accError);
  if (!accOk) {
    std::fprintf(stderr, "accumulator: %s\n", qPrintable(accError));
    return 1;
  }
  int forward = 0, skipped = 0;
  for (int i = 1; i < files.size(); ++i) {
    QImage curImage(QDir(dir).filePath(files.at(i)));
    curImage = curImage.convertToFormat(QImage::Format_RGBA8888);
    const GrayView curGray = downsampleToGray(curImage, axis);
    const MotionEstimate est = classifyMotion(prevGray, curGray, axis);
    const char *kind = "?";
    switch (est.motion.kind) {
    case MotionKind::Stationary: kind = "Stationary"; break;
    case MotionKind::Forward: kind = "Forward"; break;
    case MotionKind::Reverse: kind = "Reverse"; break;
    case MotionKind::Ambiguous: kind = "Ambiguous"; break;
    case MotionKind::Unmatchable: kind = "Unmatchable"; break;
    }
    if (classifyOnly)
      std::fprintf(stdout, "%s: %s(%d) err=%.3f conf=%.3f\n",
                   qPrintable(files.at(i)), kind, est.motion.delta, est.error,
                   est.confidence);
    if (est.motion.kind == MotionKind::Forward) {
      ++forward;
      if (!classifyOnly && !accumulator.pushForward(curImage, est.motion.delta,
                                                    accError)) {
        std::fprintf(stderr, "push_forward: %s\n", qPrintable(accError));
        return 1;
      }
      // Only an accepted frame advances the reference; a skipped frame keeps
      // the last kept frame so the next comparison spans the gap.
      prevGray = curGray;
    } else {
      ++skipped;
    }
  }
  std::fprintf(stderr, "replay: %d forward, %d skipped\n", forward, skipped);
  if (classifyOnly)
    return 0;
  const QImage stitched = accumulator.finish(accError);
  if (stitched.isNull()) {
    std::fprintf(stderr, "finish: %s\n", qPrintable(accError));
    return 1;
  }
  std::fprintf(stderr, "replay: stitched %dx%d\n", stitched.width(),
               stitched.height());
  if (!out.isEmpty()) {
    if (!stitched.save(out, "PNG")) {
      std::fprintf(stderr, "could not save %s\n", qPrintable(out));
      return 1;
    }
    std::fprintf(stderr, "replay: saved %s\n", qPrintable(out));
  }
  return 0;
}
