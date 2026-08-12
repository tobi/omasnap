/** @fileoverview Focused tests for original-source eyedropper coordinate mapping. */
#include "eyedropper.hpp"

#include <QCoreApplication>
#include <QImage>

#include <iostream>

namespace {
bool expectColor(const char *name, const QColor &actual, const QColor &expected) {
  if (actual == expected)
    return true;
  std::cerr << name << ": expected " << expected.name().toStdString()
            << ", got " << actual.name().toStdString() << '\n';
  return false;
}
}

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QImage source(16, 8, QImage::Format_RGB32);
  for (int y = 0; y < source.height(); ++y)
    for (int x = 0; x < source.width(); ++x)
      source.setPixelColor(x, y, QColor(x * 10, y * 20, 100));

  bool ok = true;
  ok &= expectColor("cropped mapping",
                    sampleSourceColor(source, QSizeF(8, 4), QRectF(2, 1, 4, 2),
                                      QRectF(10, 20, 80, 40), QPointF(50, 40)),
                    QColor(80, 80, 100));
  ok &= expectColor("high dpi mapping",
                    sampleSourceColor(source, QSizeF(8, 4), QRectF(0, 0, 8, 4),
                                      QRectF(0, 0, 160, 80), QPointF(95, 45)),
                    QColor(90, 80, 100));
  ok &= expectColor("bottom edge clamping",
                    sampleSourceColor(source, QSizeF(8, 4), QRectF(2, 1, 4, 2),
                                      QRectF(10, 20, 80, 40), QPointF(-100, 100)),
                    QColor(40, 100, 100));
  ok &= expectColor("right edge clamping",
                    sampleSourceColor(source, QSizeF(8, 4), QRectF(2, 1, 4, 2),
                                      QRectF(10, 20, 80, 40), QPointF(100, 40)),
                    QColor(110, 80, 100));
  return ok ? 0 : 1;
}
