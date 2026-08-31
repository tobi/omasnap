#include "shelf-layout.hpp"

#include <algorithm>

namespace {
constexpr int kThumbnailWidth = 230;
constexpr int kThumbnailHeight = 130;
constexpr int kSurfacePadding = 8;
constexpr int kStackPeek = 13;
constexpr int kExpandedGap = 10;
constexpr int kExpandedHandleHeight = 26;
constexpr int kTuckedHeight = 24;
} // namespace

CaptureShelfLayout captureShelfLayout(int itemCount,
                                      ShelfPresentation presentation) {
  CaptureShelfLayout layout;
  itemCount = std::clamp(itemCount, 0, captureShelfMaximumItems());
  const int width = kThumbnailWidth + kSurfacePadding * 2;
  if (presentation == ShelfPresentation::Tucked) {
    layout.size = QSize(width, kTuckedHeight);
    layout.handle = QRectF(kSurfacePadding, 0, kThumbnailWidth, kTuckedHeight);
    return layout;
  }

  const bool expanded = presentation == ShelfPresentation::Expanded;
  const int step = expanded ? kThumbnailHeight + kExpandedGap : kStackPeek;
  const int handleHeight = expanded ? kExpandedHandleHeight : 0;
  const int stackHeight =
      itemCount == 0 ? 0 : kThumbnailHeight + (itemCount - 1) * step;
  layout.size = QSize(width, stackHeight + kSurfacePadding * 2 + handleHeight);
  if (expanded)
    layout.handle = QRectF(kSurfacePadding, kSurfacePadding, kThumbnailWidth,
                           kExpandedHandleHeight - 4);
  layout.thumbnails.reserve(itemCount);
  for (int index = 0; index < itemCount; ++index) {
    const int y =
        kSurfacePadding + handleHeight + (itemCount - 1 - index) * step;
    layout.thumbnails.push_back(
        QRectF(kSurfacePadding, y, kThumbnailWidth, kThumbnailHeight));
  }
  return layout;
}

int captureShelfItemAt(const CaptureShelfLayout &layout,
                       const QPointF &position) {
  // Newest entries are painted last, so test them first in overlap regions.
  for (int index = 0; index < layout.thumbnails.size(); ++index) {
    if (layout.thumbnails.at(index).contains(position))
      return index;
  }
  return -1;
}
