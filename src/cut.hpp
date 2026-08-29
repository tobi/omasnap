/** @fileoverview Band cut-out engine: removes a horizontal or vertical band
 *  from an image and collapses the gap (Snagit-style "cut out"). */
#pragma once

#include <QImage>
#include <QSize>
#include <QVector>
#include <Qt>

/** One applied cut. Coordinates are in the space of the image as it existed
 *  when the cut was applied; replay applies ops in order. `source*` are
 *  native pixels, `logical*` are preview-logical pixels. `start` is
 *  inclusive, `end` exclusive. Qt::Horizontal removes rows (image gets
 *  shorter); Qt::Vertical removes columns (image gets narrower). */
struct CutOp {
  Qt::Orientation orientation = Qt::Horizontal;
  int sourceStart = 0;
  int sourceEnd = 0;
  int logicalStart = 0;
  int logicalEnd = 0;
  bool insert = false;
  bool operator==(const CutOp &) const = default;
};

/** Returns `source` with the band removed. Bounds are clamped to the image;
 *  an empty band or a band covering the full extent returns `source`
 *  unchanged (an image must never collapse to nothing). */
[[nodiscard]] QImage removeBand(const QImage &source,
                                Qt::Orientation orientation, int start,
                                int end);

/** Inserts a transparent band `[start, end)` and shifts the rest out. An
 *  empty band is a no-op. */
[[nodiscard]] QImage insertBand(const QImage &source,
                                Qt::Orientation orientation, int start,
                                int end);

[[nodiscard]] QImage applyCutOp(const QImage &source, const CutOp &cut);

/** Replays `cuts` in order over the pristine image. */
[[nodiscard]] QImage composeCuts(const QImage &pristine,
                                 const QVector<CutOp> &cuts);

/** Logical preview size after replaying `cuts` over `pristineLogical`. */
[[nodiscard]] QSize composedLogicalSize(QSize pristineLogical,
                                        const QVector<CutOp> &cuts);

/** Shifts one coordinate for a removed band [start, end): values past the
 *  band shift back by the band size, values inside clamp to the seam. */
[[nodiscard]] qreal shiftForCut(qreal value, qreal start, qreal end);

/** Shifts one coordinate for an inserted band of `size` at `start`: values
 *  on or past the seam move out by `size`. */
[[nodiscard]] qreal shiftForInsert(qreal value, qreal start, qreal size);
