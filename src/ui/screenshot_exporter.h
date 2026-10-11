#pragma once

#include <QWidget>
#include <QImage>
#include <QString>
#include <QSize>
#include <functional>

class QPainter;

namespace simvis {

class MapWidget;

class ScreenshotExporter {
public:
    // Export current map view as a high-res PNG
    // scaleFactor: 1=native, 2=2x, 4=4x resolution
    static bool exportPng(MapWidget* mapWidget, QWidget* parent, int scaleFactor = 2);

    // Export current map view as a PDF with embedded high-res raster
    // scaleFactor: multiplier for the raster resolution embedded in the PDF
    static bool exportPdf(MapWidget* mapWidget, QWidget* parent, int scaleFactor = 4);

    // Ask for a file name and write a QPainter drawing to it: PNG or PDF, chosen
    // by the extension. `size` is the drawing size in screen pixels; the PNG is
    // written at 3x that, the PDF as vectors. `paint` draws into the rect it is
    // given. Returns false when cancelled or when the write fails.
    using PaintFn = std::function<void(QPainter&, const QRect&)>;
    static bool exportDrawing(QWidget* parent, const QString& title,
                              const QString& suggestedStem, const QSize& size,
                              const PaintFn& paint);

private:
    static QImage captureScaled(MapWidget* mapWidget, int scaleFactor);
};

} // namespace simvis
