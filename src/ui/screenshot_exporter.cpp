#include "screenshot_exporter.h"
#include "map_widget.h"
#include "core/logger.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QInputDialog>
#include <QPdfWriter>
#include <QPainter>
#include <QPageLayout>
#include <QPageSize>
#include <QApplication>

namespace simvis {

QImage ScreenshotExporter::captureScaled(MapWidget* mapWidget, int scaleFactor) {
    // Render the current view off-screen at scaleFactor x resolution. Falls back
    // to a plain framebuffer grab if the high-res render fails.
    QImage image = mapWidget->renderToImage(scaleFactor);
    if (image.isNull()) {
        LOG_WARN("captureScaled: high-res render failed, falling back to framebuffer grab");
        image = mapWidget->grabFramebuffer();
    }
    return image;
}

// Ask the user for a resolution multiplier. Returns 0 if cancelled.
static int promptScaleFactor(QWidget* parent, const QString& title) {
    QStringList options;
    options << "1x (screen resolution)"
            << "2x (recommended)"
            << "4x (high quality)"
            << "8x (very high — large file)";
    bool ok = false;
    QString choice = QInputDialog::getItem(parent, title,
        "Export resolution:", options, 1 /*default 2x*/, false, &ok);
    if (!ok) return 0;
    if (choice.startsWith("1x")) return 1;
    if (choice.startsWith("2x")) return 2;
    if (choice.startsWith("4x")) return 4;
    if (choice.startsWith("8x")) return 8;
    return 2;
}

bool ScreenshotExporter::exportPng(MapWidget* mapWidget, QWidget* parent, int scaleFactor) {
    Q_UNUSED(scaleFactor);  // user picks the factor interactively
    int factor = promptScaleFactor(parent, "Export as PNG");
    if (factor == 0) return false;  // cancelled

    QString filePath = QFileDialog::getSaveFileName(
        parent,
        "Export as PNG",
        QString(),
        "PNG Image (*.png);;All Files (*)"
    );

    if (filePath.isEmpty()) return false;

    if (!filePath.endsWith(".png", Qt::CaseInsensitive)) {
        filePath += ".png";
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);

    QImage image = captureScaled(mapWidget, factor);

    bool ok = !image.isNull() && image.save(filePath, "PNG");

    QApplication::restoreOverrideCursor();

    if (ok) {
        LOG_INFO(QString("PNG exported: %1 (%2x%3)")
            .arg(filePath).arg(image.width()).arg(image.height()));
        QMessageBox::information(parent, "Export PNG",
            QString("Screenshot saved to:\n%1\n\nResolution: %2 x %3")
                .arg(filePath).arg(image.width()).arg(image.height()));
    } else {
        LOG_ERROR(QString("PNG export failed: %1").arg(filePath));
        QMessageBox::critical(parent, "Export PNG",
            QString("Failed to save screenshot to:\n%1").arg(filePath));
    }

    return ok;
}

bool ScreenshotExporter::exportPdf(MapWidget* mapWidget, QWidget* parent, int scaleFactor) {
    Q_UNUSED(scaleFactor);  // user picks the factor interactively
    int factor = promptScaleFactor(parent, "Export as PDF");
    if (factor == 0) return false;  // cancelled

    QString filePath = QFileDialog::getSaveFileName(
        parent,
        "Export as PDF",
        QString(),
        "PDF Document (*.pdf);;All Files (*)"
    );

    if (filePath.isEmpty()) return false;

    if (!filePath.endsWith(".pdf", Qt::CaseInsensitive)) {
        filePath += ".pdf";
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);

    QImage image = captureScaled(mapWidget, factor);
    if (image.isNull()) {
        QApplication::restoreOverrideCursor();
        LOG_ERROR(QString("PDF export failed - capture returned null image: %1").arg(filePath));
        QMessageBox::critical(parent, "Export PDF",
            QString("Failed to capture the map for export."));
        return false;
    }

    // Set up PDF writer sized to match the captured image
    QPdfWriter pdfWriter(filePath);
    pdfWriter.setTitle("TAREEK-Vis Map Export");
    pdfWriter.setCreator("TAREEK-Vis");

    // Keep the physical page a sensible size while embedding the high-res image,
    // so the effective DPI scales with the chosen factor instead of the page
    // ballooning. We treat the high-res image as if printed at (96 * factor) DPI:
    // e.g. a 4x export prints at 384 DPI on a page the size of the screen view.
    const int baseDpi = 96;
    int pdfDpi = baseDpi * factor;
    pdfWriter.setResolution(pdfDpi);

    // Page size in millimeters from the image at the effective DPI. Because the
    // image is factor x larger AND the DPI is factor x higher, the physical page
    // stays constant across factors — only the print sharpness increases.
    double widthMm = (image.width() / static_cast<double>(pdfDpi)) * 25.4;
    double heightMm = (image.height() / static_cast<double>(pdfDpi)) * 25.4;

    QPageSize pageSize(QSizeF(widthMm, heightMm), QPageSize::Millimeter);
    QPageLayout layout(pageSize, QPageLayout::Portrait, QMarginsF(0, 0, 0, 0));
    pdfWriter.setPageLayout(layout);

    QPainter painter(&pdfWriter);
    if (!painter.isActive()) {
        QApplication::restoreOverrideCursor();
        LOG_ERROR(QString("PDF export failed - could not open painter: %1").arg(filePath));
        QMessageBox::critical(parent, "Export PDF",
            QString("Failed to create PDF file:\n%1").arg(filePath));
        return false;
    }

    // Draw the image to fill the entire page (no margins, no black border)
    QRect targetRect(0, 0, pdfWriter.width(), pdfWriter.height());
    painter.drawImage(targetRect, image);
    painter.end();

    QApplication::restoreOverrideCursor();

    LOG_INFO(QString("PDF exported: %1 (%2x%3 image)")
        .arg(filePath).arg(image.width()).arg(image.height()));
    QMessageBox::information(parent, "Export PDF",
        QString("PDF saved to:\n%1\n\nEmbedded resolution: %2 x %3")
            .arg(filePath).arg(image.width()).arg(image.height()));

    return true;
}

bool ScreenshotExporter::exportDrawing(QWidget* parent, const QString& title,
                                       const QString& suggestedStem, const QSize& size,
                                       const PaintFn& paint) {
    QString filePath = QFileDialog::getSaveFileName(
        parent, title,
        suggestedStem.isEmpty() ? QString() : suggestedStem + ".png",
        "PNG Image (*.png);;PDF Document (*.pdf)");
    if (filePath.isEmpty()) return false;

    const bool pdf = filePath.endsWith(".pdf", Qt::CaseInsensitive);
    if (!pdf && !filePath.endsWith(".png", Qt::CaseInsensitive)) {
        filePath += ".png";
    }

    bool ok = false;
    const QRect area(QPoint(0, 0), size);
    if (pdf) {
        // At 96 DPI one PDF unit is one screen pixel, so the drawing code and
        // its font sizes work unchanged, and the text stays as vectors.
        QPdfWriter pdfWriter(filePath);
        pdfWriter.setTitle(title);
        pdfWriter.setCreator("TAREEK-Vis");
        pdfWriter.setResolution(96);
        const QSizeF mm(size.width() / 96.0 * 25.4, size.height() / 96.0 * 25.4);
        pdfWriter.setPageLayout(QPageLayout(QPageSize(mm, QPageSize::Millimeter),
                                            QPageLayout::Portrait, QMarginsF(0, 0, 0, 0)));
        QPainter painter(&pdfWriter);
        if (painter.isActive()) {
            paint(painter, area);
            ok = painter.end();
        }
    } else {
        constexpr int kScale = 3;
        QImage image(size * kScale, QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(kScale);
        image.fill(Qt::white);
        {
            QPainter painter(&image);
            paint(painter, area);
        }
        ok = image.save(filePath, "PNG");
    }

    if (ok) {
        LOG_INFO(QString("Drawing exported: %1").arg(filePath));
        QMessageBox::information(parent, title, QString("Saved to:\n%1").arg(filePath));
    } else {
        LOG_ERROR(QString("Drawing export failed: %1").arg(filePath));
        QMessageBox::critical(parent, title, QString("Could not save the file:\n%1").arg(filePath));
    }
    return ok;
}

} // namespace simvis
