#include "PDFEditController.h"
#include "PDFEditController_p.h"
#include "PDFHeaderMetadata.h"
#include "PDFSourceImageGeometry.h"
#include "PDFiumDocument.h"
#include "PDFiumSynchronization.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QRunnable>
#include <QScopeGuard>
#include <QThreadPool>
#include <QTransform>
#include <QtMath>
#include <QtTranslation>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {
constexpr char imageGeometryMime[] = "application/x-trepdf-image-geometry";
constexpr qint64 maximumInputPixels = 40000000;
constexpr qint64 maximumCopyPixels = 16000000;
constexpr int maximumCopyDimension = 8192;
constexpr unsigned long copyPermission = 1UL << 4;

// Clipboard ownership is application-wide, even when the active PDF changes
// while the single source-image extraction worker is still finishing.
std::weak_ptr<std::atomic_bool> activeImageCopy;
QList<QPointer<PDFEditController>> clipboardControllers;

bool imageCopyPending()
{
    const auto active = activeImageCopy.lock();
    return active && !active->load(std::memory_order_relaxed);
}

void notifyClipboardControllers()
{
    clipboardControllers.removeIf([](const auto &controller) { return !controller; });
    const auto controllers = clipboardControllers;
    for (const auto &controller : controllers) {
        if (controller)
            emit controller->clipboardImageChanged();
    }
}

void finishImageCopy(const std::shared_ptr<std::atomic_bool> &cancellation)
{
    if (activeImageCopy.lock() == cancellation) {
        activeImageCopy.reset();
        notifyClipboardControllers();
    }
}

QString imageCopyMessage(bool copied)
{
    if (copied) {
        //% "Image copied. You can paste it with Ctrl/Cmd+V."
        return qtTrId("ImageCopied");
    }
    //% "Couldn't copy this image. Please select it and try again."
    return qtTrId("ImageCopyFailed");
}

bool finiteSize(const QSizeF &size)
{
    return qIsFinite(size.width()) && qIsFinite(size.height())
        && size.width() > 0.0 && size.height() > 0.0;
}

bool finiteFrame(const QRectF &frame)
{
    return qIsFinite(frame.x()) && qIsFinite(frame.y()) && finiteSize(frame.size());
}

bool cancelled(const std::shared_ptr<std::atomic_bool> &cancellation)
{
    return cancellation && cancellation->load(std::memory_order_relaxed);
}

QThreadPool &imageCopyPool()
{
    static QThreadPool pool;
    static const bool initialized = [] { pool.setMaxThreadCount(1); return true; }();
    Q_UNUSED(initialized);
    return pool;
}

// The ordinary image MIME must also show unlocked aspect-ratio changes when
// pasted into another application. Exact PDF dimensions travel separately.
QImage displayedImage(QImage image, int quarterTurns, const QSizeF &physicalSize)
{
    if (image.isNull() || !finiteSize(physicalSize)
        || qint64(image.width()) * image.height() > maximumInputPixels)
        return {};
    if (quarterTurns % 4 != 0)
        image = image.transformed(QTransform().rotate(quarterTurns * 90.0));
    if (image.isNull())
        return {};
    const qreal aspect = physicalSize.width() / physicalSize.height();
    // Integer pixel dimensions already approximate a physical frame to within
    // one pixel. Do not add another resampling pass for that rounding alone.
    if (qint64(image.width()) * image.height() <= maximumCopyPixels
        && qMax(image.width(), image.height()) <= maximumCopyDimension
        && qAbs(image.width() - image.height() * aspect) <= qMax(1.0, aspect)) {
        image = image.convertToFormat(QImage::Format_ARGB32);
        image.setDevicePixelRatio(1.0);
        return image;
    }
    const qreal area = qMin<qreal>(maximumCopyPixels,
        qreal(image.width()) * image.height());
    qreal width = std::sqrt(area * aspect);
    qreal height = width / aspect;
    const qreal scale = qMin<qreal>(1.0,
        maximumCopyDimension / qMax(width, height));
    width *= scale;
    height *= scale;
    if (!qIsFinite(width) || !qIsFinite(height) || width < 1.0 || height < 1.0)
        return {};
    const QSize size(qMax(1, qFloor(width)), qMax(1, qFloor(height)));
    if (image.size() != size)
        image = image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    image = image.convertToFormat(QImage::Format_ARGB32);
    image.setDevicePixelRatio(1.0);
    return image;
}

bool writeImageClipboard(QImage image, const QRectF &frame, const QSizeF &pageSize,
                         bool copiedFromHeader, bool copiedFromFooter)
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard || image.isNull() || !finiteFrame(frame) || !finiteSize(pageSize))
        return false;
    auto *mime = new QMimeData;
    mime->setImageData(image);
    const QJsonObject geometry{{QStringLiteral("version"), 1},
        {QStringLiteral("width"), frame.width() * pageSize.width()},
        {QStringLiteral("height"), frame.height() * pageSize.height()},
        {QStringLiteral("x"), frame.x()}, {QStringLiteral("y"), frame.y()},
        {QStringLiteral("copiedFromHeader"), copiedFromHeader},
        {QStringLiteral("copiedFromFooter"), copiedFromFooter},
        {QStringLiteral("footerYPoints"), frame.y() * pageSize.height() - qMax(0.0, pageSize.height() - 100.0)},
        {QStringLiteral("xPoints"), frame.x() * pageSize.width()},
        {QStringLiteral("yPoints"), frame.y() * pageSize.height()},
        {QStringLiteral("pixelWidth"), image.width()},
        {QStringLiteral("pixelHeight"), image.height()}};
    mime->setData(imageGeometryMime, QJsonDocument(geometry).toJson(QJsonDocument::Compact));
    clipboard->setMimeData(mime);
    return true;
}

struct ImageCopyRequest {
    QString path;
    QByteArray password;
    int sourcePage = -1;
    int objectIndex = -1;
    QSizeF pageSize;
    QRectF sourceRect;
    QSizeF physicalSize;
    int quarterTurns = 0;
    bool copiedFromHeader = false;
};

QImage renderSourceImage(const ImageCopyRequest &request,
                        const std::shared_ptr<std::atomic_bool> &cancellation)
{
    if (cancelled(cancellation) || !finiteFrame(request.sourceRect)
        || !finiteSize(request.pageSize))
        return {};
    PDFiumProcessLock lock;
    if (cancelled(cancellation))
        return {};
    PDFiumDocument document = PDFiumDocument::open(request.path, request.password);
    if (!document || !(FPDF_GetDocPermissions(document) & copyPermission))
        return {};
    const FPDF_PAGE page = FPDF_LoadPage(document, request.sourcePage);
    if (!page)
        return {};
    const auto closePage = qScopeGuard([&] { FPDF_ClosePage(page); });
    const int objectCount = FPDFPage_CountObjects(page);
    if (objectCount <= 0 || objectCount > 50000
        || request.objectIndex < 0 || request.objectIndex >= objectCount)
        return {};
    const FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, request.objectIndex);
    if (!PDFSourceImageGeometry::isImageObject(object))
        return {};
    FPDF_BOOL active = false;
    if (!FPDFPageObj_GetIsActive(object, &active) || !active)
        return {};
    float left = 0, bottom = 0, right = 0, top = 0;
    if (!FPDFPageObj_GetBounds(object, &left, &bottom, &right, &top))
        return {};
    const QRectF currentRect = PDFEditControllerPrivate::normalizedRect(page,
        left, bottom, right, top, PDFEditControllerPrivate::kIdentityMatrix, request.pageSize);
    constexpr qreal tolerance = 0.00002;
    if (!finiteFrame(currentRect)
        || std::abs(currentRect.x() - request.sourceRect.x()) > tolerance
        || std::abs(currentRect.y() - request.sourceRect.y()) > tolerance
        || std::abs(currentRect.width() - request.sourceRect.width()) > tolerance
        || std::abs(currentRect.height() - request.sourceRect.height()) > tolerance)
        return {};

    // Inspect the single image leaf before decoding, including image-only Forms.
    FPDF_PAGEOBJECT leaf = object;
    for (int depth = 0; FPDFPageObj_GetType(leaf) == FPDF_PAGEOBJ_FORM && depth < 16; ++depth)
        leaf = FPDFFormObj_GetObject(leaf, 0);
    FPDF_IMAGEOBJ_METADATA metadata{};
    if (!leaf || FPDFPageObj_GetType(leaf) != FPDF_PAGEOBJ_IMAGE
        || !FPDFImageObj_GetImageMetadata(leaf, page, &metadata)
        || metadata.width == 0 || metadata.height == 0
        || metadata.width > maximumInputPixels || metadata.height > maximumInputPixels
        || qint64(metadata.width) * metadata.height > maximumInputPixels)
        return {};
    FS_MATRIX nativePlacement{};
    if (request.copiedFromHeader && request.quarterTurns % 4 == 0
        && FPDFPageObj_GetType(object) == FPDF_PAGEOBJ_IMAGE
        && FPDFPage_GetRotation(page) == 0
        && FPDFPageObj_GetMatrix(object, &nativePlacement)
        && nativePlacement.a > 0 && nativePlacement.d > 0
        && nativePlacement.b == 0 && nativePlacement.c == 0
        && metadata.width <= maximumCopyDimension && metadata.height <= maximumCopyDimension
        && PDFSourceImageGeometry::canResize(page, object)) {
        // Keep decoded pixels identical to header matching. Physical frame size
        // travels separately, so resizing the bitmap would only lose identity.
        qint64 remainingPixels = PDFHeaderMetadata::kMaximumDigestPixels;
        QImage native = PDFHeaderMetadata::nativeImage(document, page, object, remainingPixels);
        if (!native.isNull())
            return cancelled(cancellation) ? QImage() : native;
    }
    for (int index = 0; index < objectCount; ++index) {
        if (cancelled(cancellation))
            return {};
        if (index != request.objectIndex
            && !FPDFPageObj_SetIsActive(FPDFPage_GetObject(page, index), false))
            return {};
    }
    const qreal physicalWidth = currentRect.width() * request.pageSize.width();
    const qreal physicalHeight = currentRect.height() * request.pageSize.height();
    qreal scale = std::max({2.0, metadata.width / physicalWidth,
                           metadata.height / physicalHeight});
    scale = std::min({scale, maximumCopyDimension / physicalWidth,
        maximumCopyDimension / physicalHeight,
        std::sqrt(qreal(maximumCopyPixels) / (physicalWidth * physicalHeight)),
        1000000.0 / qMax(request.pageSize.width(), request.pageSize.height())});
    if (!qIsFinite(scale) || scale <= 0.0)
        return {};
    const int width = qMax(1, qFloor(physicalWidth * scale));
    const int height = qMax(1, qFloor(physicalHeight * scale));
    if (width > maximumCopyDimension || height > maximumCopyDimension
        || qint64(width) * height > maximumCopyPixels)
        return {};
    // PDFium applies the page display matrix (CropBox and intrinsic rotation)
    // before this matrix. Map those displayed points directly to the exact
    // fractional frame edges, avoiding an outward-rounded crop's padding.
    const FS_MATRIX matrix{float(width / physicalWidth), 0, 0,
        float(height / physicalHeight),
        float(-currentRect.x() * width / currentRect.width()),
        float(-currentRect.y() * height / currentRect.height())};
    if (!PDFSourceImageGeometry::finiteInvertibleMatrix(matrix))
        return {};
    QImage image(width, height, QImage::Format_ARGB32);
    if (image.isNull())
        return {};
    image.fill(Qt::transparent);
    FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(width, height, FPDFBitmap_BGRA,
        image.bits(), image.bytesPerLine());
    if (!bitmap)
        return {};
    // Only this private document is isolated; no page background or annotation
    // is included, and the live PDF/document worker is never mutated.
    const FS_RECTF clipping{0, 0, float(width), float(height)};
    FPDF_RenderPageBitmapWithMatrix(bitmap, page, &matrix, &clipping, 0);
    FPDFBitmap_Destroy(bitmap);
    if (cancelled(cancellation))
        return {};
    return displayedImage(std::move(image), request.quarterTurns, request.physicalSize);
}
} // namespace

void PDFEditController::initializeImageClipboard()
{
    clipboardControllers.removeIf([](const auto &controller) { return !controller; });
    clipboardControllers.append(this);
    connect(this, &PDFEditController::selectionChanged, this, &PDFEditController::selectedImageCopyableChanged);
    connect(this, &PDFEditController::savingChanged, this, &PDFEditController::selectedImageCopyableChanged);
    connect(this, &PDFEditController::textEditActiveChanged, this, &PDFEditController::selectedImageCopyableChanged);
    connect(this, &PDFEditController::toolChanged, this, &PDFEditController::selectedImageCopyableChanged);
    connect(this, &PDFEditController::headerSelectionOnlyChanged, this, &PDFEditController::selectedImageCopyableChanged);
    connect(this, &PDFEditController::selectedGeometryChanged, this, &PDFEditController::selectedImageCopyableChanged);
    connect(this, &PDFEditController::selectionChanged, this, &PDFEditController::cancelImageCopy);
    connect(this, &PDFEditController::selectedGeometryChanged, this, &PDFEditController::cancelImageCopy);
    connect(this, &PDFEditController::historyStateChanged, this, &PDFEditController::cancelImageCopy);
    connect(this, &PDFEditController::toolChanged, this, &PDFEditController::cancelImageCopy);
    connect(this, &PDFEditController::headerSelectionOnlyChanged, this, &PDFEditController::cancelImageCopy);
    connect(this, &PDFEditController::savingChanged, this, &PDFEditController::cancelImageCopy);
    connect(this, &PDFEditController::documentChanged, this, &PDFEditController::cancelImageCopy);
    connect(this, &PDFEditController::textEditActiveChanged, this, &PDFEditController::cancelImageCopy);
    if (QClipboard *clipboard = QGuiApplication::clipboard()) {
        connect(clipboard, &QClipboard::dataChanged, this, [this] {
            cancelImageCopy();
            emit clipboardImageChanged();
        });
    }
}

void PDFEditController::cancelImageCopy()
{
    if (m_imageCopyCancellation) {
        const auto cancellation = std::move(m_imageCopyCancellation);
        cancellation->store(true, std::memory_order_relaxed);
        finishImageCopy(cancellation);
    }
}

bool PDFEditController::clipboardHasImage() const
{
    // Pending source extraction must not let Paste insert older clipboard data.
    if (imageCopyPending())
        return false;
    const QClipboard *clipboard = QGuiApplication::clipboard();
    const QMimeData *mime = clipboard ? clipboard->mimeData() : nullptr;
    return mime && mime->hasImage();
}

bool PDFEditController::selectedImageCopyable() const
{
    if (!m_document || !m_document->isLoaded() || !m_document->allowsTextCopy()
        || m_saving || m_textEditActive || m_frameResizeActive || !m_hasSelection
        || m_tool != SelectTool || !candidateMatchesSelectionScope(m_selectedCandidate))
        return false;
    const Draft draft = selectedDraftForDisplay();
    if (draft.deleted || !finiteFrame(draft.frameRect))
        return false;
    return (draft.candidate.kind == QLatin1String("addedImage") && !draft.candidate.image.isNull())
        || (draft.candidate.kind == QLatin1String("image")
            && draft.candidate.objectPath.size() == 1);
}

bool PDFEditController::copySelectedImage()
{
    if (!selectedImageCopyable() || imageCopyPending())
        return false;
    const Draft draft = selectedDraftForDisplay();
    const bool copiedFromHeader = m_headerSelectionOnly;
    const bool copiedFromFooter = copiedFromHeader && m_footerMode;
    const QSizeF pageSize = m_document->pageSize(draft.candidate.page);
    const QSizeF physicalSize(draft.frameRect.width() * pageSize.width(),
                              draft.frameRect.height() * pageSize.height());
    if (draft.candidate.added) {
        const QImage &pixels = draft.candidate.image;
        const bool preservePixels = copiedFromHeader && draft.imageQuarterTurns % 4 == 0
            && qint64(pixels.width()) * pixels.height() <= maximumCopyPixels
            && qMax(pixels.width(), pixels.height()) <= maximumCopyDimension;
        const bool copied = writeImageClipboard(preservePixels ? pixels : displayedImage(pixels,
            draft.imageQuarterTurns, physicalSize), draft.frameRect, pageSize, copiedFromHeader, copiedFromFooter);
        emit statusMessage(imageCopyMessage(copied));
        return copied;
    }
    const CandidateScanRequest source = candidateScanRequest(draft.candidate.page);
    if (source.sourcePage < 0)
        return false;
    const ImageCopyRequest request{source.filePath, source.password, source.sourcePage,
        draft.candidate.objectPath.constFirst(), pageSize, draft.candidate.geometry.sourceRect,
        physicalSize, draft.imageQuarterTurns, copiedFromHeader};
    const auto cancellation = std::make_shared<std::atomic_bool>(false);
    m_imageCopyCancellation = cancellation;
    activeImageCopy = cancellation;
    notifyClipboardControllers();
    const QPointer<PDFEditController> controller(this);
    const QPointer<PDFDocument> sourceDocument(m_document);
    QRunnable *task = QRunnable::create([controller, sourceDocument, source, request, draft,
                                       copiedFromHeader, copiedFromFooter, cancellation] {
        QImage image = renderSourceImage(request, cancellation);
        QObject *app = QCoreApplication::instance();
        if (!app || cancelled(cancellation))
            return;
        QMetaObject::invokeMethod(app, [controller, sourceDocument, source, draft, copiedFromHeader, copiedFromFooter, cancellation,
                                      image = std::move(image)]() mutable {
            if (!controller || cancelled(cancellation)
                || controller->m_imageCopyCancellation != cancellation)
                return;
            controller->m_imageCopyCancellation.reset();
            if (!sourceDocument || controller->m_document != sourceDocument
                || !controller->selectedImageCopyable()
                || sourceDocument->renderRevision() != source.revision
                || sourceDocument->renderFilePath() != source.filePath
                || sourceDocument->sourcePageIndex(source.page) != source.sourcePage
                || sourceDocument->pageSize(source.page) != source.pageSize
                || !sameDraftContent(controller->selectedDraftForDisplay(), draft)) {
                finishImageCopy(cancellation);
                return;
            }
            const bool copied = writeImageClipboard(std::move(image), draft.frameRect,
                                                    source.pageSize, copiedFromHeader, copiedFromFooter);
            finishImageCopy(cancellation);
            emit controller->statusMessage(imageCopyMessage(copied));
        }, Qt::QueuedConnection);
    });
    // A single process-wide job prevents queued image decodes from building up.
    if (!imageCopyPool().tryStart(task)) {
        delete task;
        cancelImageCopy();
        emit statusMessage(imageCopyMessage(false));
        return false;
    }
    return true;
}

QVariantMap PDFEditController::pasteImageFromClipboard(int page)
{
    if (m_saving || m_textEditActive || m_frameResizeActive || imageCopyPending() || !m_document
        || !m_document->isLoaded() || page < 0 || page >= m_document->pageCount()
        || m_tool != SelectTool)
        return {};
    if (!documentAllowsEditing()) {
        //% "This PDF does not permit content editing."
        emit statusMessage(qtTrId("PdfEditingNotPermitted"));
        return {};
    }
    const QClipboard *clipboard = QGuiApplication::clipboard();
    const QMimeData *mime = clipboard ? clipboard->mimeData() : nullptr;
    if (!mime || !mime->hasImage())
        return {};
    QImage image = qvariant_cast<QImage>(mime->imageData());
    if (image.isNull()) {
        //% "The clipboard image couldn't be read. Copy the image again and try pasting."
        emit statusMessage(qtTrId("ClipboardImageInvalid"));
        return {};
    }
    if (qint64(image.width()) * image.height() > maximumInputPixels) {
        //% "The selected image is too large. Choose an image up to 40 megapixels and 64 MiB."
        emit statusMessage(qtTrId("SelectedImageTooLarge"));
        return {};
    }
    const QSizeF pageSize = m_document->pageSize(page);
    if (!finiteSize(pageSize))
        return {};
    const qreal availableHeight = m_headerSelectionOnly ? qMin(100.0, pageSize.height()) : pageSize.height();
    const qreal minimumTop = m_headerSelectionOnly && m_footerMode
        ? pageSize.height() - availableHeight : 0.0;
    qreal width = pageSize.width() * 0.25;
    qreal height = width * image.height() / image.width();
    if (height > pageSize.height() * 0.25) {
        width *= pageSize.height() * 0.25 / height;
        height = pageSize.height() * 0.25;
    }
    QPointF topLeft((pageSize.width() - width) / 2.0, minimumTop + (availableHeight - height) / 2.0);
    const QByteArray geometryBytes = mime->data(imageGeometryMime);
    if (!geometryBytes.isEmpty() && geometryBytes.size() <= 2048) {
        const QJsonObject geometry = QJsonDocument::fromJson(geometryBytes).object();
        const qreal unknown = std::numeric_limits<qreal>::quiet_NaN();
        const QSizeF size(geometry.value(QStringLiteral("width")).toDouble(unknown),
                          geometry.value(QStringLiteral("height")).toDouble(unknown));
        const QPointF position(geometry.value(QStringLiteral("x")).toDouble(unknown),
                               geometry.value(QStringLiteral("y")).toDouble(unknown));
        if (geometry.value(QStringLiteral("version")).toInt() == 1
            && geometry.value(QStringLiteral("pixelWidth")).toInt() == image.width()
            && geometry.value(QStringLiteral("pixelHeight")).toInt() == image.height()
            && finiteSize(size) && size.width() <= 1000000 && size.height() <= 1000000
            && qIsFinite(position.x()) && qIsFinite(position.y())
            && position.x() >= 0 && position.x() <= 1 && position.y() >= 0 && position.y() <= 1) {
            width = size.width();
            height = size.height();
            topLeft = QPointF(position.x() * pageSize.width() + 12.0,
                              position.y() * pageSize.height() + 12.0);
            const QPointF headerPosition(geometry.value(QStringLiteral("xPoints")).toDouble(unknown),
                                         geometry.value(QStringLiteral("yPoints")).toDouble(unknown));
            // Header copies must align across pages, even when page sizes differ.
            // Read the origin from the clipboard, not the currently active tab.
            if (geometry.value(QStringLiteral("copiedFromHeader")).toBool()
                && qIsFinite(headerPosition.x()) && qIsFinite(headerPosition.y())
                && headerPosition.x() >= 0 && headerPosition.x() <= 1000000
                && headerPosition.y() >= 0 && headerPosition.y() <= 1000000) {
                topLeft = headerPosition;
                const qreal footerY = geometry.value(QStringLiteral("footerYPoints")).toDouble(unknown);
                if (geometry.value(QStringLiteral("copiedFromFooter")).toBool()
                    && qIsFinite(footerY) && footerY >= 0.0 && footerY <= 100.0) {
                    topLeft.setY(qMax(0.0, pageSize.height() - 100.0) + footerY);
                }
            }
        }
    }
    const qreal fit = std::min({1.0, pageSize.width() / width, availableHeight / height});
    width *= fit;
    height *= fit;
    topLeft.setX(qBound(0.0, topLeft.x(), pageSize.width() - width));
    topLeft.setY(qBound(minimumTop, topLeft.y(), minimumTop + availableHeight - height));
    return addImageData(page, (topLeft.x() + width / 2.0) / pageSize.width(),
        (topLeft.y() + height / 2.0) / pageSize.height(), std::move(image), width, height);
}
