#pragma once

#include <QImage>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

#include <fpdfview.h>

struct PDFWidgetRenderPatch
{
    QRect deviceRect;
    QImage pixels;
};

bool capturePDFWidgetRenderPatches(
    FPDF_PAGE page,
    const QSize &pixelSize,
    const QVector<QRectF> &pageRects,
    const QImage &image,
    QVector<PDFWidgetRenderPatch> &patches);
bool restorePDFWidgetRenderPatches(
    QImage &image,
    const QVector<PDFWidgetRenderPatch> &patches);

// The page must remain open and the process-wide PDFium lock must stay held
// until this guard is restored or destroyed.
class PDFWidgetAnnotationSuppression
{
    Q_DISABLE_COPY_MOVE(PDFWidgetAnnotationSuppression)

public:
    PDFWidgetAnnotationSuppression() = default;
    ~PDFWidgetAnnotationSuppression();

    bool suppress(FPDF_PAGE page, const QStringList &widgetIds);
    bool restore();
    bool restorationSucceeded() const;
    QVector<QRectF> pageRects() const;

private:
    struct State {
        int annotationIndex = -1;
        int flags = 0;
        QRectF pageRect;
    };

    FPDF_PAGE m_page = nullptr;
    QVector<State> m_states;
    bool m_restored = false;
    bool m_restoreSucceeded = true;
};
