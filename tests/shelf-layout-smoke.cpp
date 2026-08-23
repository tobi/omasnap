#include "shelf-layout-smoke.hpp"

#include "shelf-layout.hpp"

bool runShelfLayoutSmoke(QString &error) {
  const CaptureShelfLayout stacked =
      captureShelfLayout(3, ShelfPresentation::Stacked);
  const CaptureShelfLayout expanded =
      captureShelfLayout(3, ShelfPresentation::Expanded);
  const CaptureShelfLayout tucked =
      captureShelfLayout(3, ShelfPresentation::Tucked);
  if (stacked.thumbnails.size() != 3 || expanded.thumbnails.size() != 3 ||
      !tucked.thumbnails.isEmpty() || tucked.handle.isEmpty()) {
    error = QStringLiteral("Shelf layouts did not expose the expected items");
    return false;
  }
  if (stacked.size.height() >= expanded.size.height() ||
      tucked.size.height() >= stacked.size.height()) {
    error = QStringLiteral("Shelf presentation heights are not ordered");
    return false;
  }
  if (stacked.thumbnails.at(0).top() <= stacked.thumbnails.at(1).top() ||
      expanded.thumbnails.at(0).top() <= expanded.thumbnails.at(1).top()) {
    error = QStringLiteral("Newest Shelf item was not placed at the front");
    return false;
  }
  const QPointF overlap = stacked.thumbnails.at(0).topLeft() + QPointF(5, 5);
  if (captureShelfItemAt(stacked, overlap) != 0) {
    error = QStringLiteral("Shelf hit testing did not prefer the newest item");
    return false;
  }
  if (captureShelfLayout(20, ShelfPresentation::Expanded).thumbnails.size() !=
      captureShelfMaximumItems()) {
    error = QStringLiteral("Shelf did not enforce its five-item limit");
    return false;
  }
  return true;
}
