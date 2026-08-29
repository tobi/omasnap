/** @fileoverview Clip-out engine: copy a native-pixel path and punch a hole
 *  without collapsing the gap.
 *
 *  Distinct from the Cut tool, which removes a band and shifts the rest.
 *  Coordinates follow Cut: `sourceRect` is the native-pixel bbox of the
 *  composed image as it existed when the clip was applied; replay applies
 *  ops in order so later cuts see the hole already punched. */
#pragma once

#include <QColor>
#include <QImage>
#include <QPainterPath>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QVector>
#include <optional>

enum class ClipShape { Rect, Ellipse, Lasso };

/** One applied clip. `sourceRect` is the integer bbox (inclusive top-left,
 *  usual QRect). For Rect and Ellipse that bbox *is* the shape. Lasso
 *  vertices live in `points` (native space, floating); the bbox is derived.
 *  An empty bbox, or one that misses the image, is a no-op. `fill` is the
 *  hole infill: default (invalid or alpha 0) punches transparent; a solid
 *  color paints that swatch into the hole. */
struct ClipOp {
  ClipShape shape = ClipShape::Rect;
  QRect sourceRect;
  QVector<QPointF> points;
  QColor fill;
  qreal radius = 0; // native px; Rect only, 0 is sharp
  bool operator==(const ClipOp &) const = default;
};

/** Outer object under a click: AABB, optional corner radius, silhouette. */
struct ClipSnapHit {
  QRect box;
  qreal radius = 0;
  QVector<QPointF> contour;
};

[[nodiscard]] inline bool clipFillOpaque(const QColor &fill) {
  return fill.isValid() && fill.alpha() > 0;
}

/** Median colour of a thin ring just outside `nativeRect` — the page the
 *  clipped object sat on. Invalid when the rect has no outside samples. */
[[nodiscard]] QColor sampleClipSurroundings(const QImage &source,
                                            QRect nativeRect);

[[nodiscard]] inline QString clipShapeName(ClipShape shape) {
  switch (shape) {
  case ClipShape::Ellipse:
    return QStringLiteral("ellipse");
  case ClipShape::Lasso:
    return QStringLiteral("lasso");
  case ClipShape::Rect:
    break;
  }
  return QStringLiteral("rect");
}

[[nodiscard]] inline bool clipShapeFromName(const QString &name,
                                            ClipShape &shape) {
  if (name.isEmpty() || name == QStringLiteral("rect")) {
    shape = ClipShape::Rect;
    return true;
  }
  if (name == QStringLiteral("ellipse")) {
    shape = ClipShape::Ellipse;
    return true;
  }
  if (name == QStringLiteral("lasso")) {
    shape = ClipShape::Lasso;
    return true;
  }
  return false;
}

[[nodiscard]] QPainterPath clipPath(const ClipOp &clip);

/** Returns a copy of `sourceRect` intersected with `source`. Null when the
 *  intersection is empty. */
[[nodiscard]] QImage copyRect(const QImage &source, QRect sourceRect);

/** Copy of the clip bbox with pixels outside the path alpha 0. */
[[nodiscard]] QImage copyMasked(const QImage &source, const ClipOp &clip);

/** Punches `sourceRect` to transparent. Converts `image` to premultiplied
 *  ARGB when needed so the hole can reveal a backdrop. No-op on an empty
 *  intersection. */
void punchRect(QImage &image, QRect sourceRect);

/** Fills `sourceRect` with `fill`. Transparent / invalid `fill` punches a
 *  hole, same as `punchRect`. */
void fillHole(QImage &image, QRect sourceRect, const QColor &fill);

/** Fills the clip path. Transparent `fill` punches the path. */
void fillHole(QImage &image, const ClipOp &clip);

/** Maps a logical (preview) rectangle onto native source pixels. Floor the
 *  start and ceil the end so a drag covers every pixel it touches, matching
 *  the Cut tool's band mapping. */
[[nodiscard]] QRect nativeClipRect(QRectF logical, QSize preview, QSize source);

/** Maps one logical point onto native space (not quantized to a pixel until
 *  rasterization). */
[[nodiscard]] QPointF nativeClipPoint(QPointF logical, QSize preview,
                                      QSize source);

/** Builds a native ClipOp from a logical shape. Lasso points are mapped
 *  individually; the bbox is their native bounds. */
[[nodiscard]] ClipOp nativeClipOp(ClipShape shape, QRectF logical,
                                  const QVector<QPointF> &logicalPoints,
                                  QSize preview, QSize source,
                                  const QColor &fill, qreal logicalRadius = 0);

/** Connected object inside `roi` (the user's drag). Paper is sampled just
 *  outside that box when possible, then high-contrast pixels grow through
 *  weaker chrome so a gray card margin is not mistaken for the page.
 *  `roi` empty → 256 px around `click`. Rect uses `box`+`radius`; lasso uses
 *  `contour`. */
[[nodiscard]] std::optional<ClipSnapHit>
snapObject(const QImage &source, QPoint click, QRect roi = {});

/** Circle-seeking snap from `click`. With a drag `roi`, this is the
 *  smallest circle that covers the connected object in that box (a wheel
 *  with a pin stays enclosed). With `roi` empty, rays vote for a consistent
 *  radius so a round crop on a rectangular card stays a circle. */
[[nodiscard]] std::optional<QRect> snapEllipseRect(const QImage &source,
                                                   QPoint click,
                                                   QRect roi = {});

/** Axis-aligned rectangle around `click`. Near-square results become 1:1. */
[[nodiscard]] std::optional<QRect> snapRectRect(const QImage &source,
                                                QPoint click);

/** True when a dragged Ellipse/Lasso is tracing a detected circle.
 *  The snap centre must sit inside the drag, the snap must be near-circular,
 *  and the two boxes must be within a tracing factor of each other. */
[[nodiscard]] bool clipTraceSnapFits(const QRectF &drawn, const QRectF &snapped);

/** True when a dragged Rect is tracing a detected rectangle/square. Same
 *  centre and size checks, but any aspect is allowed if it matches the drag. */
[[nodiscard]] bool clipRectTraceSnapFits(const QRectF &drawn,
                                         const QRectF &snapped);

/** Annotation-space distance at which a lifted clip *enters* the snap zone
 *  (~14 widget px). `viewScale` is the editor's annotation-to-widget scale.
 *  Dest is never clamped to the hole while dragging; snap applies on release. */
[[nodiscard]] qreal clipSnapEnterThreshold(qreal viewScale);

/** Annotation-space distance at which a lifted clip *leaves* the snap zone
 *  (~20 widget px). Wider than enter so the snap ghost does not chatter. */
[[nodiscard]] qreal clipSnapLeaveThreshold(qreal viewScale);

[[nodiscard]] inline qreal clipSnapThreshold(qreal viewScale) {
  return clipSnapEnterThreshold(viewScale);
}

/** True when `dest` is close enough to `origin` that releasing should restore
 *  the hole instead of committing a new layer. */
[[nodiscard]] bool clipDestSnapped(const QRectF &dest, const QRectF &origin,
                                   qreal threshold);

/** Tile for a Clip op. Keep `existing` (from beginClipLift / JSON png / a
 *  prior commit) unless it is null or the cut/clip prefix before that op
 *  changed. */
[[nodiscard]] QImage resolveClipTile(const QImage &composed, QRect sourceRect,
                                     const QImage &existing, bool prefixChanged);
[[nodiscard]] QImage resolveClipTile(const QImage &composed, const ClipOp &clip,
                                     const QImage &existing, bool prefixChanged);
