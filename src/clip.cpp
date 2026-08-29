/** @fileoverview Clip-out engine: copy a path and punch a hole. */
#include "clip.hpp"

#include <QColor>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QLineF>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QVector>
#include <Qt>
#include <algorithm>
#include <cmath>
#include <optional>
#include <queue>

namespace {

void ensurePremultiplied(QImage &image) {
  if (image.format() != QImage::Format_ARGB32 &&
      image.format() != QImage::Format_ARGB32_Premultiplied)
    image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

QRectF nativeScale(QSize preview, QSize source) {
  if (!preview.isValid() || preview.width() <= 0 || preview.height() <= 0 ||
      !source.isValid() || source.width() <= 0 || source.height() <= 0)
    return {};
  return QRectF(0, 0, source.width() / static_cast<qreal>(preview.width()),
                source.height() / static_cast<qreal>(preview.height()));
}

int lumaAt(const QImage &image, int x, int y) {
  return qGray(image.pixel(x, y));
}

int channelDist(QRgb a, QRgb b) {
  return std::max({std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)),
                   std::abs(qBlue(a) - qBlue(b))});
}

QRgb samplePaper(const QImage &img, const QRect &roi, bool sampleOutside) {
  QVector<QRgb> samples;
  samples.reserve(128);
  const auto add = [&](int x, int y) {
    if (img.rect().contains(x, y))
      samples.push_back(img.pixel(x, y));
  };
  if (sampleOutside) {
    const QRect outer = roi.adjusted(-2, -2, 2, 2).intersected(img.rect());
    for (int y = outer.top(); y <= outer.bottom(); ++y) {
      for (int x = outer.left(); x <= outer.right(); ++x) {
        if (!roi.contains(x, y))
          add(x, y);
      }
    }
  }
  if (samples.size() < 8) {
    const int xStep = std::max(1, roi.width() / 8);
    const int yStep = std::max(1, roi.height() / 8);
    for (int x = roi.left(); x <= roi.right(); x += xStep) {
      add(x, roi.top());
      add(x, roi.bottom());
    }
    for (int y = roi.top(); y <= roi.bottom(); y += yStep) {
      add(roi.left(), y);
      add(roi.right(), y);
    }
  }
  if (samples.isEmpty())
    return qRgb(0, 0, 0);
  std::sort(samples.begin(), samples.end(),
            [](QRgb a, QRgb b) { return qGray(a) < qGray(b); });
  return samples.at(samples.size() / 2);
}

struct ObjectBlob {
  QRect box;
  QRect roi;
  QImage mask;
};

constexpr int kSeedDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
constexpr int kSeedDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};

void closeMask(QImage &mask) {
  const int w = mask.width();
  const int h = mask.height();
  if (w < 1 || h < 1)
    return;
  QImage dilated = mask;
  for (int y = 0; y < h; ++y) {
    uchar *out = dilated.scanLine(y);
    for (int x = 0; x < w; ++x) {
      if (mask.constScanLine(y)[x] != 0)
        continue;
      for (int i = 0; i < 8; ++i) {
        const int nx = x + kSeedDx[i];
        const int ny = y + kSeedDy[i];
        if (nx < 0 || ny < 0 || nx >= w || ny >= h)
          continue;
        if (mask.constScanLine(ny)[nx] != 0) {
          out[x] = 255;
          break;
        }
      }
    }
  }
  mask.fill(0);
  for (int y = 0; y < h; ++y) {
    uchar *out = mask.scanLine(y);
    for (int x = 0; x < w; ++x) {
      if (dilated.constScanLine(y)[x] == 0)
        continue;
      bool all = true;
      for (int i = 0; i < 8; ++i) {
        const int nx = x + kSeedDx[i];
        const int ny = y + kSeedDy[i];
        if (nx < 0 || ny < 0 || nx >= w || ny >= h)
          continue;
        if (dilated.constScanLine(ny)[nx] == 0) {
          all = false;
          break;
        }
      }
      if (all)
        out[x] = 255;
    }
  }
}

QRect maskBounds(const QImage &mask, const QRect &roi) {
  int minX = roi.right();
  int maxX = roi.left();
  int minY = roi.bottom();
  int maxY = roi.top();
  bool any = false;
  for (int y = 0; y < mask.height(); ++y) {
    const uchar *row = mask.constScanLine(y);
    for (int x = 0; x < mask.width(); ++x) {
      if (row[x] == 0)
        continue;
      any = true;
      minX = std::min(minX, roi.left() + x);
      maxX = std::max(maxX, roi.left() + x);
      minY = std::min(minY, roi.top() + y);
      maxY = std::max(maxY, roi.top() + y);
    }
  }
  if (!any)
    return {};
  return QRect(QPoint(minX, minY), QPoint(maxX, maxY));
}

std::optional<QPoint> findObjectSeed(const QImage &img, QPoint click,
                                     const QRect &roi, const QRgb paper,
                                     int enter, bool userRoi) {
  const auto dist = [&](int x, int y) {
    return channelDist(img.pixel(x, y), paper);
  };
  if (dist(click.x(), click.y()) > enter)
    return click;
  // Click-snap only nudges onto a neighboring checker/AA pixel. A drag
  // searches the whole box so the object can sit off the drag centre.
  const int maxR =
      userRoi ? std::max(roi.width(), roi.height()) : 4;
  for (int r = 1; r <= maxR; ++r) {
    for (int y = click.y() - r; y <= click.y() + r; ++y) {
      for (int x = click.x() - r; x <= click.x() + r; ++x) {
        if (std::max(std::abs(x - click.x()), std::abs(y - click.y())) != r)
          continue;
        if (!roi.contains(x, y))
          continue;
        if (dist(x, y) > enter)
          return QPoint(x, y);
      }
    }
  }
  return std::nullopt;
}

std::optional<ObjectBlob> fillObjectBlob(const QImage &img, QPoint click,
                                         QRect roi, bool userRoi) {
  constexpr int kEnter = 12;
  roi = roi.intersected(img.rect());
  if (roi.width() < 6 || roi.height() < 6 || !roi.contains(click))
    return std::nullopt;
  const QRgb paper = samplePaper(img, roi, userRoi);
  const auto dist = [&](int x, int y) {
    return channelDist(img.pixel(x, y), paper);
  };
  const std::optional<QPoint> seed =
      findObjectSeed(img, click, roi, paper, kEnter, userRoi);
  if (!seed)
    return std::nullopt;
  QImage mask(roi.size(), QImage::Format_Grayscale8);
  mask.fill(0);
  std::queue<QPoint> pending;
  const auto mark = [&](int x, int y) {
    const int lx = x - roi.left();
    const int ly = y - roi.top();
    if (mask.scanLine(ly)[lx] != 0)
      return false;
    mask.scanLine(ly)[lx] = 255;
    pending.push(QPoint(x, y));
    return true;
  };
  mark(seed->x(), seed->y());
  while (!pending.empty()) {
    const QPoint p = pending.front();
    pending.pop();
    for (int i = 0; i < 8; ++i) {
      const int x = p.x() + kSeedDx[i];
      const int y = p.y() + kSeedDy[i];
      if (!roi.contains(x, y))
        continue;
      if (dist(x, y) <= kEnter)
        continue;
      mark(x, y);
    }
  }
  const QImage flooded = mask;
  const QRect floodBox = maskBounds(flooded, roi);
  closeMask(mask);
  QRect box = maskBounds(mask, roi);
  // 1 px close fills checker holes. If it also swallows a nearby glyph,
  // keep the flood so Rect snap can still see the card's corner bites.
  const bool merged =
      !floodBox.isEmpty() &&
      (box.left() < floodBox.left() - 2 || box.top() < floodBox.top() - 2 ||
       box.right() > floodBox.right() + 2 ||
       box.bottom() > floodBox.bottom() + 2);
  if (merged) {
    mask = flooded;
    box = floodBox;
  }
  box = box.intersected(img.rect());
  if (box.width() < 6 || box.height() < 6)
    return std::nullopt;
  ObjectBlob blob;
  blob.box = box;
  blob.roi = roi;
  blob.mask = std::move(mask);
  return blob;
}

bool maskAt(const ObjectBlob &blob, int x, int y) {
  if (!blob.roi.contains(x, y))
    return false;
  return blob.mask.constScanLine(y - blob.roi.top())[x - blob.roi.left()] != 0;
}

std::optional<QRect> blobEnclosingCircle(const ObjectBlob &blob,
                                         QSize sourceSize) {
  const QPointF origin = blob.box.center();
  qreal maxR = 0.0;
  for (int y = blob.box.top(); y <= blob.box.bottom(); ++y) {
    for (int x = blob.box.left(); x <= blob.box.right(); ++x) {
      if (!maskAt(blob, x, y))
        continue;
      maxR = std::max(maxR, QLineF(origin, QPointF(x, y)).length());
    }
  }
  if (maxR < 6.0)
    return std::nullopt;
  const int r = std::max(6, static_cast<int>(std::ceil(maxR)));
  const int cx = static_cast<int>(std::lround(origin.x()));
  const int cy = static_cast<int>(std::lround(origin.y()));
  QRect circle(QPoint(cx - r, cy - r), QPoint(cx + r, cy + r));
  circle = circle.intersected(QRect(QPoint(), sourceSize));
  const qreal cover = static_cast<qreal>(circle.width()) * circle.height() /
                      std::max(1, sourceSize.width() * sourceSize.height());
  if (circle.width() < 6 || circle.height() < 6 || cover > 0.70)
    return std::nullopt;
  return circle;
}

} // namespace

QColor sampleClipSurroundings(const QImage &source, QRect nativeRect) {
  if (source.isNull())
    return {};
  nativeRect = nativeRect.intersected(source.rect());
  if (nativeRect.isEmpty())
    return {};
  const QImage img = source.convertToFormat(QImage::Format_ARGB32);
  QVector<QRgb> samples;
  samples.reserve(128);
  const QRect outer =
      nativeRect.adjusted(-3, -3, 3, 3).intersected(img.rect());
  for (int y = outer.top(); y <= outer.bottom(); ++y) {
    for (int x = outer.left(); x <= outer.right(); ++x) {
      if (nativeRect.contains(x, y))
        continue;
      samples.push_back(img.pixel(x, y));
    }
  }
  if (samples.size() < 8)
    return {};
  std::sort(samples.begin(), samples.end(),
            [](QRgb a, QRgb b) { return qGray(a) < qGray(b); });
  return QColor(samples.at(samples.size() / 2));
}

QPainterPath clipPath(const ClipOp &clip) {
  QPainterPath path;
  const QRect box = clip.sourceRect;
  if (box.isEmpty())
    return path;
  switch (clip.shape) {
  case ClipShape::Ellipse:
    path.addEllipse(QRectF(box));
    break;
  case ClipShape::Lasso: {
    if (clip.points.size() < 3)
      return {};
    path.moveTo(clip.points.constFirst());
    for (int i = 1; i < clip.points.size(); ++i)
      path.lineTo(clip.points.at(i));
    path.closeSubpath();
    break;
  }
  case ClipShape::Rect:
    if (clip.radius >= 1.0) {
      const qreal r =
          std::min({clip.radius, box.width() / 2.0, box.height() / 2.0});
      path.addRoundedRect(QRectF(box), r, r);
    } else {
      path.addRect(QRectF(box));
    }
    break;
  }
  return path;
}

QImage copyRect(const QImage &source, QRect sourceRect) {
  if (source.isNull())
    return {};
  sourceRect = sourceRect.intersected(source.rect());
  if (sourceRect.isEmpty())
    return {};
  return source.copy(sourceRect);
}

QImage copyMasked(const QImage &source, const ClipOp &clip) {
  QImage tile = copyRect(source, clip.sourceRect);
  if (tile.isNull())
    return {};
  if (clip.shape == ClipShape::Rect && clip.radius < 1.0)
    return tile;
  ensurePremultiplied(tile);
  QImage mask(tile.size(), QImage::Format_ARGB32_Premultiplied);
  mask.fill(Qt::transparent);
  QPainter maskPainter(&mask);
  maskPainter.setRenderHint(QPainter::Antialiasing, true);
  maskPainter.setPen(Qt::NoPen);
  maskPainter.setBrush(Qt::white);
  QPainterPath path = clipPath(clip);
  path.translate(-clip.sourceRect.topLeft());
  maskPainter.drawPath(path);
  maskPainter.end();
  QPainter painter(&tile);
  painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
  painter.drawImage(0, 0, mask);
  return tile;
}

void punchRect(QImage &image, QRect sourceRect) {
  if (image.isNull())
    return;
  sourceRect = sourceRect.intersected(image.rect());
  if (sourceRect.isEmpty())
    return;
  ensurePremultiplied(image);
  QPainter painter(&image);
  painter.setCompositionMode(QPainter::CompositionMode_Clear);
  painter.fillRect(sourceRect, Qt::transparent);
}

void fillHole(QImage &image, QRect sourceRect, const QColor &fill) {
  fillHole(image, ClipOp{ClipShape::Rect, sourceRect, {}, fill});
}

void fillHole(QImage &image, const ClipOp &clip) {
  if (image.isNull())
    return;
  const QRect box = clip.sourceRect.intersected(image.rect());
  if (box.isEmpty())
    return;
  const QPainterPath path = clipPath(clip);
  if (path.isEmpty())
    return;
  ensurePremultiplied(image);
  QPainter painter(&image);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(Qt::NoPen);
  if (!clipFillOpaque(clip.fill)) {
    painter.setCompositionMode(QPainter::CompositionMode_Clear);
    painter.fillPath(path, Qt::transparent);
    return;
  }
  painter.setCompositionMode(QPainter::CompositionMode_Source);
  painter.fillPath(path, clip.fill);
}

QRect nativeClipRect(QRectF logical, QSize preview, QSize source) {
  logical = logical.normalized();
  if (logical.isEmpty() || !preview.isValid() || preview.width() <= 0 ||
      preview.height() <= 0 || !source.isValid() || source.width() <= 0 ||
      source.height() <= 0)
    return {};
  const qreal scaleX =
      source.width() / static_cast<qreal>(preview.width());
  const qreal scaleY =
      source.height() / static_cast<qreal>(preview.height());
  const int left = static_cast<int>(std::floor(logical.left() * scaleX));
  const int top = static_cast<int>(std::floor(logical.top() * scaleY));
  const int right = static_cast<int>(std::ceil(logical.right() * scaleX));
  const int bottom = static_cast<int>(std::ceil(logical.bottom() * scaleY));
  const QRect native(QPoint(left, top), QPoint(right - 1, bottom - 1));
  return native.intersected(QRect(QPoint(), source));
}

QPointF nativeClipPoint(QPointF logical, QSize preview, QSize source) {
  const QRectF scale = nativeScale(preview, source);
  if (scale.isEmpty())
    return {};
  return QPointF(logical.x() * scale.width(), logical.y() * scale.height());
}

ClipOp nativeClipOp(ClipShape shape, QRectF logical,
                    const QVector<QPointF> &logicalPoints, QSize preview,
                    QSize source, const QColor &fill, qreal logicalRadius) {
  ClipOp clip;
  clip.shape = shape;
  clip.fill = fill;
  if (logicalRadius > 0.0 && preview.width() > 0 && preview.height() > 0) {
    const qreal scale =
        (source.width() / static_cast<qreal>(preview.width()) +
         source.height() / static_cast<qreal>(preview.height())) /
        2.0;
    clip.radius = logicalRadius * scale;
  }
  if (shape == ClipShape::Lasso) {
    clip.points.reserve(logicalPoints.size());
    QRectF bounds;
    for (const QPointF &point : logicalPoints) {
      const QPointF native = nativeClipPoint(point, preview, source);
      clip.points.push_back(native);
      if (bounds.isNull())
        bounds = QRectF(native, QSizeF(0.01, 0.01));
      else
        bounds |= QRectF(native, QSizeF(0.01, 0.01));
    }
    if (!bounds.isEmpty()) {
      const int left = static_cast<int>(std::floor(bounds.left()));
      const int top = static_cast<int>(std::floor(bounds.top()));
      const int right = static_cast<int>(std::ceil(bounds.right()));
      const int bottom = static_cast<int>(std::ceil(bounds.bottom()));
      clip.sourceRect =
          QRect(QPoint(left, top), QPoint(right - 1, bottom - 1))
              .intersected(QRect(QPoint(), source));
    }
    return clip;
  }
  clip.sourceRect = nativeClipRect(logical, preview, source);
  return clip;
}

std::optional<ClipSnapHit> snapObject(const QImage &source, QPoint click,
                                      QRect roi) {
  if (source.isNull() || !source.rect().contains(click))
    return std::nullopt;
  const QImage img = source.convertToFormat(QImage::Format_ARGB32);
  constexpr int kRays = 48;
  const bool userRoi = !roi.isEmpty();
  if (roi.isEmpty())
    roi = QRect(click.x() - 256, click.y() - 256, 513, 513);
  const std::optional<ObjectBlob> blob = fillObjectBlob(img, click, roi, userRoi);
  if (!blob)
    return std::nullopt;
  const QRect box = blob->box;
  const qreal cover = static_cast<qreal>(box.width()) * box.height() /
                      std::max(1, source.width() * source.height());
  if (cover > 0.70)
    return std::nullopt;
  const int maxR = std::min(box.width(), box.height()) / 2;
  const auto inset = [&](int x0, int y0, int dx, int dy) {
    int i = 0;
    while (i < maxR && !maskAt(*blob, x0 + i * dx, y0 + i * dy))
      ++i;
    return i;
  };
  const auto cornerRadius = [&](int x0, int y0, int inx, int iny) {
    int sx = x0;
    int sy = y0;
    for (int t = 0; t < 3 && maskAt(*blob, sx, sy); ++t) {
      sx += inx;
      sy += iny;
    }
    return std::min(inset(sx, sy, inx, 0), inset(sx, sy, 0, iny));
  };
  int radii[4] = {
      cornerRadius(box.left(), box.top(), 1, 1),
      cornerRadius(box.right(), box.top(), -1, 1),
      cornerRadius(box.left(), box.bottom(), 1, -1),
      cornerRadius(box.right(), box.bottom(), -1, -1)};
  std::sort(radii, radii + 4);
  int strong = 0;
  for (int r : radii) {
    if (r >= 3)
      ++strong;
  }
  qreal radius = 0.0;
  if (strong >= 2)
    radius = radii[2];
  else if (radii[1] >= 3)
    radius = radii[1];

  QPoint origin = click;
  if (!maskAt(*blob, origin.x(), origin.y()))
    origin = box.center();
  ClipSnapHit hit;
  hit.box = box;
  hit.radius = radius;
  hit.contour.reserve(kRays);
  const int maxS = std::max(box.width(), box.height()) + 2;
  for (int i = 0; i < kRays; ++i) {
    const qreal ang = static_cast<qreal>(i) * (2.0 * M_PI / kRays);
    const qreal dx = std::cos(ang);
    const qreal dy = std::sin(ang);
    QPointF last(origin);
    bool sawInside = maskAt(*blob, origin.x(), origin.y());
    for (int s = 1; s <= maxS; ++s) {
      const int x = origin.x() + static_cast<int>(std::lround(dx * s));
      const int y = origin.y() + static_cast<int>(std::lround(dy * s));
      if (!blob->roi.contains(x, y))
        break;
      if (maskAt(*blob, x, y)) {
        last = QPointF(x, y);
        sawInside = true;
      } else if (sawInside) {
        break;
      }
    }
    if (sawInside)
      hit.contour.push_back(last);
  }
  if (hit.contour.size() < 8)
    hit.contour.clear();
  return hit;
}

std::optional<QRect> snapEllipseRect(const QImage &source, QPoint click,
                                     QRect roi) {
  if (source.isNull() || !source.rect().contains(click))
    return std::nullopt;
  const QImage img = source.convertToFormat(QImage::Format_ARGB32);
  constexpr int kRays = 36;
  constexpr int kThresh = 28;
  constexpr int kMinInliers = 16;
  constexpr qreal kTol = 0.14;
  const bool userRoi = !roi.isEmpty();
  if (roi.isEmpty())
    roi = QRect(click.x() - 256, click.y() - 256, 513, 513);
  roi = roi.intersected(img.rect());
  if (userRoi) {
    const std::optional<ObjectBlob> blob =
        fillObjectBlob(img, click, roi, true);
    if (!blob)
      return std::nullopt;
    return blobEnclosingCircle(*blob, source.size());
  }
  const int maxS = std::min(
      256, std::max(roi.width(), roi.height()));
  QVector<QVector<int>> jumps(kRays);
  QVector<int> all;
  all.reserve(kRays * 4);
  for (int i = 0; i < kRays; ++i) {
    const qreal ang = static_cast<qreal>(i) * (2.0 * M_PI / kRays);
    const qreal dx = std::cos(ang);
    const qreal dy = std::sin(ang);
    int prev = lumaAt(img, click.x(), click.y());
    for (int s = 4; s <= maxS; ++s) {
      const int x = click.x() + static_cast<int>(std::lround(dx * s));
      const int y = click.y() + static_cast<int>(std::lround(dy * s));
      if (!roi.contains(x, y) || !img.rect().contains(x, y))
        break;
      const int L = lumaAt(img, x, y);
      if (std::abs(L - prev) > kThresh) {
        jumps[i].push_back(s);
        all.push_back(s);
      }
      prev = L;
    }
  }
  int bestR = 0;
  int bestN = 0;
  for (int candidate : all) {
    if (candidate < 6)
      continue;
    int n = 0;
    const int lo = static_cast<int>(std::floor(candidate * (1.0 - kTol)));
    const int hi = static_cast<int>(std::ceil(candidate * (1.0 + kTol)));
    for (int i = 0; i < kRays; ++i) {
      for (int s : jumps.at(i)) {
        if (s >= lo && s <= hi) {
          ++n;
          break;
        }
      }
    }
    if (n > bestN) {
      bestN = n;
      bestR = candidate;
    }
  }
  std::optional<QRect> voted;
  if (bestN >= kMinInliers && bestR >= 6) {
    QRect circle(0, 0, bestR * 2, bestR * 2);
    circle.moveCenter(click);
    circle = circle.intersected(source.rect());
    if (circle.width() >= 6 && circle.height() >= 6 && circle.contains(click)) {
      const qreal cover = static_cast<qreal>(circle.width()) * circle.height() /
                          std::max(1, source.width() * source.height());
      if (cover <= 0.70)
        voted = circle;
    }
  }
  // Fallback: strongest jump per ray, AABB, circle-ify if near 1:1.
  QVector<QPoint> hits;
  hits.reserve(kRays);
  for (int i = 0; i < kRays; ++i) {
    const qreal ang = static_cast<qreal>(i) * (2.0 * M_PI / kRays);
    const qreal dx = std::cos(ang);
    const qreal dy = std::sin(ang);
    int prev = lumaAt(img, click.x(), click.y());
    int bestS = -1;
    int bestD = 0;
    for (int s = 4; s <= maxS; ++s) {
      const int x = click.x() + static_cast<int>(std::lround(dx * s));
      const int y = click.y() + static_cast<int>(std::lround(dy * s));
      if (!roi.contains(x, y) || !img.rect().contains(x, y))
        break;
      const int L = lumaAt(img, x, y);
      const int d = std::abs(L - prev);
      if (d > bestD) {
        bestD = d;
        bestS = s;
      }
      prev = L;
    }
    if (bestD > kThresh && bestS > 0) {
      hits.push_back(
          QPoint(click.x() + static_cast<int>(std::lround(dx * bestS)),
                 click.y() + static_cast<int>(std::lround(dy * bestS))));
    }
  }
  if (!voted && hits.size() >= 12) {
    int minX = hits.constFirst().x();
    int maxX = minX;
    int minY = hits.constFirst().y();
    int maxY = minY;
    qreal meanR = 0.0;
    for (const QPoint &hit : hits) {
      minX = std::min(minX, hit.x());
      maxX = std::max(maxX, hit.x());
      minY = std::min(minY, hit.y());
      maxY = std::max(maxY, hit.y());
      meanR += QLineF(click, hit).length();
    }
    meanR /= hits.size();
    qreal var = 0.0;
    for (const QPoint &hit : hits) {
      const qreal d = QLineF(click, hit).length() - meanR;
      var += d * d;
    }
    const qreal rel = std::sqrt(var / hits.size()) / std::max(1.0, meanR);
    QRect box(QPoint(minX, minY), QPoint(maxX, maxY));
    box = box.intersected(source.rect());
    if (rel <= 0.28 && meanR >= 6.0) {
      const int side = static_cast<int>(std::lround(meanR * 2.0));
      QRect circle(0, 0, side, side);
      circle.moveCenter(click);
      box = circle.intersected(source.rect());
    } else {
      const qreal ratio =
          box.width() / std::max(1.0, static_cast<qreal>(box.height()));
      if (ratio > 1.0 / 1.12 && ratio < 1.12) {
        const int side = std::max(box.width(), box.height());
        QRect circle(0, 0, side, side);
        circle.moveCenter(box.center());
        box = circle.intersected(source.rect());
      }
    }
    if (box.width() >= 6 && box.height() >= 6 && box.contains(click)) {
      const qreal cover = static_cast<qreal>(box.width()) * box.height() /
                          std::max(1, source.width() * source.height());
      if (cover <= 0.70)
        voted = box;
    }
  }
  const std::optional<ObjectBlob> blob =
      fillObjectBlob(img, click, roi, false);
  if (blob) {
    if (const std::optional<QRect> covered =
            blobEnclosingCircle(*blob, source.size())) {
      if (!voted)
        return covered;
      if (covered->width() <= voted->width() * 3 / 2 &&
          covered->height() <= voted->height() * 3 / 2)
        return covered;
      // Checker noise votes a tiny radius; the blob is the real object.
      if (voted->width() < blob->box.width() * 0.55 ||
          voted->height() < blob->box.height() * 0.55)
        return covered;
    }
  }
  return voted;
}

std::optional<QRect> snapRectRect(const QImage &source, QPoint click) {
  const std::optional<ClipSnapHit> hit = snapObject(source, click);
  if (!hit)
    return std::nullopt;
  QRect box = hit->box;
  const qreal ratio =
      box.width() / std::max(1.0, static_cast<qreal>(box.height()));
  if (ratio > 1.0 / 1.12 && ratio < 1.12) {
    const int side = std::max(box.width(), box.height());
    QRect square(0, 0, side, side);
    square.moveCenter(box.center());
    box = square.intersected(source.rect());
  }
  return box;
}

bool clipTraceSnapFits(const QRectF &drawn, const QRectF &snapped) {
  const QRectF drag = drawn.normalized();
  const QRectF snap = snapped.normalized();
  if (drag.isEmpty() || snap.isEmpty())
    return false;
  if (!drag.contains(snap.center()))
    return false;
  const qreal ratio =
      snap.width() / std::max<qreal>(1.0, snap.height());
  if (ratio < 1.0 / 1.35 || ratio > 1.35)
    return false;
  if (snap.width() < drag.width() * 0.4 || snap.height() < drag.height() * 0.4)
    return false;
  if (drag.width() < snap.width() * 0.6 || drag.height() < snap.height() * 0.6)
    return false;
  return true;
}

bool clipRectTraceSnapFits(const QRectF &drawn, const QRectF &snapped) {
  const QRectF drag = drawn.normalized();
  const QRectF snap = snapped.normalized();
  if (drag.isEmpty() || snap.isEmpty())
    return false;
  if (!drag.contains(snap.center()))
    return false;
  if (snap.width() < drag.width() * 0.4 || snap.height() < drag.height() * 0.4)
    return false;
  if (drag.width() < snap.width() * 0.6 || drag.height() < snap.height() * 0.6)
    return false;
  const qreal dragRatio = drag.width() / std::max<qreal>(1.0, drag.height());
  const qreal snapRatio = snap.width() / std::max<qreal>(1.0, snap.height());
  const qreal aspect = dragRatio / std::max<qreal>(0.001, snapRatio);
  if (aspect < 0.5 || aspect > 2.0)
    return false;
  return true;
}

qreal clipSnapEnterThreshold(qreal viewScale) {
  return 14.0 / std::max<qreal>(0.001, viewScale);
}

qreal clipSnapLeaveThreshold(qreal viewScale) {
  return 20.0 / std::max<qreal>(0.001, viewScale);
}

bool clipDestSnapped(const QRectF &dest, const QRectF &origin,
                     qreal threshold) {
  const QPointF delta = dest.center() - origin.center();
  return std::hypot(delta.x(), delta.y()) <= threshold;
}

QImage resolveClipTile(const QImage &composed, QRect sourceRect,
                       const QImage &existing, bool prefixChanged) {
  return resolveClipTile(composed,
                         ClipOp{ClipShape::Rect, sourceRect, {}, {}}, existing,
                         prefixChanged);
}

QImage resolveClipTile(const QImage &composed, const ClipOp &clip,
                       const QImage &existing, bool prefixChanged) {
  if (!existing.isNull() && !prefixChanged)
    return existing;
  return copyMasked(composed, clip);
}
