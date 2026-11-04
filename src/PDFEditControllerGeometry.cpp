#include "PDFEditController.h"
#include "PDFEditController_p.h"
#include "ApplicationFonts.h"
#include "PDFAnnotationUtils.h"
#include "PDFFormController.h"
#include "PDFFormDesignerController.h"
#include "PDFHeaderMetadata.h"
#include "PDFSaveWriter.h"
#include "PDFiumSynchronization.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMutexLocker>
#include <QPolygonF>
#include <QQuickTextDocument>
#include <QRunnable>
#include <QSet>
#include <QStringList>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QThreadPool>
#include <QTimeZone>
#include <QUuid>
#include <QVariantMap>
#include <QtTranslation>

#include <fpdf_annot.h>
#include <fpdf_edit.h>
#include <fpdf_text.h>

using namespace PDFEditControllerPrivate;

namespace {

constexpr qreal kAddedTextWidthRatio = 0.25;

constexpr qreal kAddedTextMinimumWidthRatio = 0.02;

constexpr qreal kAddedTextHeightRatio = 0.04;

constexpr qreal kAddedTextFontSizeRatio = 0.022;

constexpr qreal kAddedImageMaximumDimensionRatio = 0.25;

constexpr qreal kMinimumShapeFramePoints = 6.0;

constexpr qreal kAddedShapeStrokeWidthRatio = 0.0022;

constexpr qreal kShapeAxisSnapRatio = 0.12;

constexpr qint64 kMaximumAddedImageFileBytes = 64 * 1024 * 1024;

constexpr qint64 kMaximumAddedImagePixels = 40 * 1000 * 1000;

QString shapeLabel(PDFEditController::ShapeType shapeType)
{
    switch (shapeType) {
    case PDFEditController::RectangleShape:
        //% "Rectangle"
        return qtTrId("RectangleShape");
    case PDFEditController::EllipseShape:
        //% "Oval"
        return qtTrId("OvalShape");
    case PDFEditController::LineShape:
        //% "Line"
        return qtTrId("LineShape");
    case PDFEditController::ArrowShape:
        //% "Arrow"
        return qtTrId("ArrowShape");
    case PDFEditController::TriangleShape:
        //% "Triangle"
        return qtTrId("TriangleShape");
    case PDFEditController::DiamondShape:
        //% "Diamond"
        return qtTrId("DiamondShape");
    case PDFEditController::TableShape:
        //% "Table"
        return qtTrId("TableShape");
    }
    return {};
}

bool shapeDragIsLongEnough(const QSizeF &pageSize,
                           const QPointF &startPoint,
                           const QPointF &endPoint)
{
    return !pageSize.isEmpty()
        && std::hypot(
               (endPoint.x() - startPoint.x()) * pageSize.width(),
               (endPoint.y() - startPoint.y()) * pageSize.height())
            >= kMinimumShapeDragPoints;
}

} // namespace

QVariantMap PDFEditController::addText(int page, qreal x, qreal y)
{
    if (!m_document
        || !m_document->isLoaded()
        || page < 0
        || page >= m_document->pageCount()) {
        return {};
    }
    if (!documentAllowsEditing()) {
        //% "This PDF does not permit content editing."
        emit statusMessage(qtTrId("PdfEditingNotPermitted"));
        return {};
    }

    finishSelectedTextEdit();

    Candidate candidate;
    candidate.page = page;
    candidate.addedOrder = ++m_nextAddedObjectId;
    candidate.objectId = QStringLiteral("added:")
        + QString::number(candidate.addedOrder);
    candidate.kind = QStringLiteral("addedText");
    //% "Add Text"
    candidate.label = qtTrId("AddText");
    candidate.text.clear();
    candidate.fontFamily = headerTextFontFamily();
    candidate.fontSizeRatio = kAddedTextFontSizeRatio;
    candidate.displayFontSizeRatio = candidate.fontSizeRatio;
    candidate.color = Qt::black;
    candidate.editable = true;
    candidate.added = true;

    const qreal normalizedX = qBound<qreal>(0.0, x, 1.0);
    const qreal normalizedY = qBound<qreal>(0.0, y, 1.0);
    const qreal frameX = qMin(
        normalizedX,
        1.0 - kAddedTextMinimumWidthRatio);
    const qreal frameWidth = qMin(
        kAddedTextWidthRatio,
        1.0 - frameX);
    const qreal frameY = qBound<qreal>(
        0.0,
        normalizedY - kAddedTextHeightRatio / 2.0,
        1.0 - kAddedTextHeightRatio);
    const QRectF sourceRect(
        frameX,
        frameY,
        frameWidth,
        kAddedTextHeightRatio);
    setInitialGeometry(candidate, sourceRect);

    m_addedCandidates.append(candidate);
    setSelection(&m_addedCandidates.constLast());
    emit pageDataChanged(page);
    beginSelectedTextEdit();
    return candidateMap(initialDraftForCandidate(candidate));
}

void PDFEditController::requestAddImage(int page, qreal x, qreal y)
{
    if (!m_document
        || !m_document->isLoaded()
        || page < 0
        || page >= m_document->pageCount()) {
        return;
    }
    if (!documentAllowsEditing()) {
        //% "This PDF does not permit content editing."
        emit statusMessage(qtTrId("PdfEditingNotPermitted"));
        return;
    }

    emit imageFileRequested(
        page,
        qBound<qreal>(0.0, x, 1.0),
        qBound<qreal>(0.0, y, 1.0));
}

QVariantMap PDFEditController::addImage(int page,
                                        qreal x,
                                        qreal y,
                                        const QUrl &source)
{
    if (m_saving || !std::isfinite(x) || !std::isfinite(y)
        || !m_document
        || !m_document->isLoaded()
        || page < 0
        || page >= m_document->pageCount()) {
        return {};
    }
    if (!documentAllowsEditing()) {
        //% "This PDF does not permit content editing."
        emit statusMessage(qtTrId("PdfEditingNotPermitted"));
        return {};
    }
    if (!source.isLocalFile() || source.toLocalFile().isEmpty()) {
        //% "Only local image files are supported."
        emit statusMessage(qtTrId("LocalImageFilesOnly"));
        return {};
    }

    const QFileInfo imageFile(source.toLocalFile());
    if (!imageFile.isFile()
        || !imageFile.isReadable()
        || imageFile.size() <= 0) {
        //% "The selected image could not be opened."
        emit statusMessage(qtTrId("SelectedImageOpenFailed"));
        return {};
    }
    if (imageFile.size() > kMaximumAddedImageFileBytes) {
        //% "The selected image is too large. Choose an image up to 40 megapixels and 64 MiB."
        emit statusMessage(qtTrId("SelectedImageTooLarge"));
        return {};
    }

    QImageReader reader(imageFile.absoluteFilePath());
    reader.setAutoTransform(true);
    const QSize encodedSize = reader.size();
    const auto exceedsPixelLimit = [](const QSize &size) {
        return size.isValid()
            && static_cast<qint64>(size.width()) * size.height()
                > kMaximumAddedImagePixels;
    };
    if (exceedsPixelLimit(encodedSize)) {
        //% "The selected image is too large. Choose an image up to 40 megapixels and 64 MiB."
        emit statusMessage(qtTrId("SelectedImageTooLarge"));
        return {};
    }

    QImage image = reader.read();
    if (image.isNull()) {
        //% "The selected image could not be opened."
        emit statusMessage(qtTrId("SelectedImageOpenFailed"));
        return {};
    }
    if (exceedsPixelLimit(image.size())) {
        //% "The selected image is too large. Choose an image up to 40 megapixels and 64 MiB."
        emit statusMessage(qtTrId("SelectedImageTooLarge"));
        return {};
    }
    return addImageData(page, x, y, std::move(image));
}

QVariantMap PDFEditController::addImageData(int page, qreal x, qreal y,
                                         QImage image, qreal widthPoints, qreal heightPoints)
{
    if (m_saving || !m_document || !m_document->isLoaded()
        || page < 0 || page >= m_document->pageCount()
        || !std::isfinite(x) || !std::isfinite(y)
        || !std::isfinite(widthPoints) || widthPoints < 0.0
        || !std::isfinite(heightPoints) || heightPoints < 0.0
        || (heightPoints > 0.0 && widthPoints <= 0.0)
        || image.isNull()) {
        return {};
    }
    if (!documentAllowsEditing()) {
        //% "This PDF does not permit content editing."
        emit statusMessage(qtTrId("PdfEditingNotPermitted"));
        return {};
    }
    if (qint64(image.width()) * image.height() > kMaximumAddedImagePixels) {
        //% "The selected image is too large. Choose an image up to 40 megapixels and 64 MiB."
        emit statusMessage(qtTrId("SelectedImageTooLarge"));
        return {};
    }
    image = image.convertToFormat(QImage::Format_ARGB32);
    image.setDevicePixelRatio(1.0);

    const QSizeF pageSize = m_document->pageSize(page);
    if (!std::isfinite(pageSize.width()) || !std::isfinite(pageSize.height())
        || pageSize.width() <= 0.0 || pageSize.height() <= 0.0 || image.isNull()) {
        return {};
    }
    qreal frameWidth = widthPoints > 0.0
        ? qMin<qreal>(1.0, widthPoints / pageSize.width())
        : kAddedImageMaximumDimensionRatio;
    qreal frameHeight = heightPoints > 0.0
        ? heightPoints / pageSize.height() * (frameWidth * pageSize.width() / widthPoints)
        : frameWidth
        * pageSize.width()
        / pageSize.height()
        * static_cast<qreal>(image.height())
        / image.width();
    const qreal maximumHeight = widthPoints > 0.0 ? 1.0 : kAddedImageMaximumDimensionRatio;
    if (!std::isfinite(frameHeight) || frameWidth <= 0.0 || frameHeight <= 0.0) {
        return {};
    }
    if (frameHeight > maximumHeight) {
        const qreal scale = maximumHeight / frameHeight;
        frameWidth *= scale;
        frameHeight *= scale;
    }

    const qreal normalizedX = qBound<qreal>(0.0, x, 1.0);
    const qreal normalizedY = qBound<qreal>(0.0, y, 1.0);
    const qreal frameX = qBound<qreal>(
        0.0,
        normalizedX - frameWidth / 2.0,
        1.0 - frameWidth);
    const qreal frameY = qBound<qreal>(
        0.0,
        normalizedY - frameHeight / 2.0,
        1.0 - frameHeight);

    finishSelectedFrameResize();
    finishSelectedTextEdit();
    Candidate candidate;
    candidate.page = page;
    candidate.addedOrder = ++m_nextAddedObjectId;
    candidate.objectId = QStringLiteral("added-image:")
        + QString::number(candidate.addedOrder);
    candidate.kind = QStringLiteral("addedImage");
    //% "Image"
    candidate.label = qtTrId("Image");
    candidate.editable = true;
    candidate.added = true;
    candidate.image = std::move(image);
    candidate.headerImageDigest = PDFHeaderMetadata::imageDigest(candidate.image);
    setInitialGeometry(
        candidate,
        QRectF(frameX, frameY, frameWidth, frameHeight));

    m_addedCandidates.append(std::move(candidate));
    const Candidate &addedCandidate = m_addedCandidates.constLast();
    const Draft draft = initialDraftForCandidate(addedCandidate);
    const QString key = draftKey(page, addedCandidate.objectId);
    m_drafts.insert(key, draft);

    Command command;
    command.key = key;
    command.page = page;
    command.objectId = addedCandidate.objectId;
    command.hadBefore = false;
    command.before = draft;
    command.hadAfter = true;
    command.after = draft;
    m_undoStack.append(command);
    m_redoStack.clear();

    setTool(SelectTool);
    setSelection(&addedCandidate);
    markDraftStateChanged(page, true);
    emit historyStateChanged();
    return candidateMap(draft);
}

QVariantMap PDFEditController::addShape(int page,
                                        const QPointF &startPoint,
                                        const QPointF &endPoint)
{
    if (!m_document
        || !m_document->isLoaded()
        || m_saving
        || page < 0
        || page >= m_document->pageCount()) {
        return {};
    }
    if (!documentAllowsEditing()) {
        //% "This PDF does not permit content editing."
        emit statusMessage(qtTrId("PdfEditingNotPermitted"));
        return {};
    }
    if (!std::isfinite(startPoint.x())
        || !std::isfinite(startPoint.y())
        || !std::isfinite(endPoint.x())
        || !std::isfinite(endPoint.y())
        || !isSupportedShapeType(m_shapeType)) {
        return {};
    }

    const QSizeF pageSize = m_document->pageSize(page);
    if (pageSize.isEmpty()) {
        return {};
    }

    const QPointF boundedStart(
        qBound<qreal>(0.0, startPoint.x(), 1.0),
        qBound<qreal>(0.0, startPoint.y(), 1.0));
    const QPointF boundedEnd(
        qBound<qreal>(0.0, endPoint.x(), 1.0),
        qBound<qreal>(0.0, endPoint.y(), 1.0));
    const QPointF shapeEnd = constrainedShapeEnd(
        page,
        boundedStart,
        boundedEnd);
    if (!shapeDragIsLongEnough(pageSize, boundedStart, shapeEnd)) {
        return {};
    }

    const qreal minimumWidth = qMin<qreal>(
        1.0,
        kMinimumShapeFramePoints / pageSize.width());
    const qreal minimumHeight = qMin<qreal>(
        1.0,
        kMinimumShapeFramePoints / pageSize.height());
    const auto expandedInterval = [](qreal first,
                                     qreal second,
                                     qreal minimumSize) {
        const qreal minimum = qMin(first, second);
        const qreal maximum = qMax(first, second);
        if (maximum - minimum >= minimumSize) {
            return QPair<qreal, qreal>(minimum, maximum);
        }
        const qreal center = (first + second) / 2.0;
        const qreal start = qBound<qreal>(
            0.0,
            center - minimumSize / 2.0,
            1.0 - minimumSize);
        return QPair<qreal, qreal>(start, start + minimumSize);
    };
    qreal frameLeft = qMin(boundedStart.x(), shapeEnd.x());
    qreal frameTop = qMin(boundedStart.y(), shapeEnd.y());
    qreal frameRight = qMax(boundedStart.x(), shapeEnd.x());
    qreal frameBottom = qMax(boundedStart.y(), shapeEnd.y());
    if (m_shapeType == LineShape || m_shapeType == ArrowShape) {
        const qreal strokeWidth = shapeStrokeWidthPoints(
            kAddedShapeStrokeWidthRatio,
            pageSize.height());
        const auto includeDevicePoint = [&frameLeft,
                                         &frameTop,
                                         &frameRight,
                                         &frameBottom,
                                         &pageSize](
                                            const QPointF &point) {
            const qreal normalizedX = point.x() / pageSize.width();
            const qreal normalizedY = point.y() / pageSize.height();
            frameLeft = qMin(frameLeft, normalizedX);
            frameTop = qMin(frameTop, normalizedY);
            frameRight = qMax(frameRight, normalizedX);
            frameBottom = qMax(frameBottom, normalizedY);
        };
        if (m_shapeType == ArrowShape) {
            const QPointF startDevice(
                boundedStart.x() * pageSize.width(),
                boundedStart.y() * pageSize.height());
            const QPointF endDevice(
                shapeEnd.x() * pageSize.width(),
                shapeEnd.y() * pageSize.height());
            const QPointF direction = endDevice - startDevice;
            const qreal length = std::hypot(
                direction.x(),
                direction.y());
            const QPointF unit = direction / length;
            const QPointF normal(-unit.y(), unit.x());
            const qreal headLength = qMin(
                qMax<qreal>(6.0, strokeWidth * 4.0),
                length * 0.35);
            const qreal halfWidth = headLength * 0.55;
            const QPointF headBase = endDevice - unit * headLength;
            includeDevicePoint(headBase + normal * halfWidth);
            includeDevicePoint(headBase - normal * halfWidth);
        }

        const qreal halfStroke = strokeWidth / 2.0;
        frameLeft = qMax<qreal>(
            0.0,
            frameLeft - halfStroke / pageSize.width());
        frameTop = qMax<qreal>(
            0.0,
            frameTop - halfStroke / pageSize.height());
        frameRight = qMin<qreal>(
            1.0,
            frameRight + halfStroke / pageSize.width());
        frameBottom = qMin<qreal>(
            1.0,
            frameBottom + halfStroke / pageSize.height());
    }
    const auto horizontal = expandedInterval(
        frameLeft,
        frameRight,
        minimumWidth);
    const auto vertical = expandedInterval(
        frameTop,
        frameBottom,
        minimumHeight);
    const QRectF frameRect(
        horizontal.first,
        vertical.first,
        horizontal.second - horizontal.first,
        vertical.second - vertical.first);
    const auto pointRatio = [&frameRect](const QPointF &point) {
        return QPointF(
            qBound<qreal>(
                0.0,
                (point.x() - frameRect.x()) / frameRect.width(),
                1.0),
            qBound<qreal>(
                0.0,
                (point.y() - frameRect.y()) / frameRect.height(),
                1.0));
    };

    finishSelectedFrameResize();
    finishSelectedTextEdit();
    Candidate candidate;
    candidate.page = page;
    candidate.addedOrder = ++m_nextAddedObjectId;
    candidate.objectId = QStringLiteral("added-shape:")
        + QString::number(candidate.addedOrder);
    candidate.kind = QStringLiteral("addedShape");
    candidate.label = shapeLabel(m_shapeType);
    candidate.editable = true;
    candidate.added = true;
    candidate.shapeType = m_shapeType;
    if (m_shapeType == TableShape) {
        candidate.tableRows = m_tableRows;
        candidate.tableColumns = m_tableColumns;
    }
    candidate.shapeStartRatio = pointRatio(boundedStart);
    candidate.shapeEndRatio = pointRatio(shapeEnd);
    candidate.shapeStrokeWidthRatio = kAddedShapeStrokeWidthRatio;
    candidate.strokeColor = QColor(Qt::black);
    candidate.fillColor = QColor(Qt::transparent);
    setInitialGeometry(candidate, frameRect);

    m_addedCandidates.append(std::move(candidate));
    const Candidate &addedCandidate = m_addedCandidates.constLast();
    const Draft draft = initialDraftForCandidate(addedCandidate);
    const QString key = draftKey(page, addedCandidate.objectId);
    m_drafts.insert(key, draft);

    Command command;
    command.key = key;
    command.page = page;
    command.objectId = addedCandidate.objectId;
    command.hadBefore = false;
    command.before = draft;
    command.hadAfter = true;
    command.after = draft;
    m_undoStack.append(command);
    m_redoStack.clear();

    setTool(SelectTool);
    setSelection(&addedCandidate);
    markDraftStateChanged(page, true);
    emit historyStateChanged();
    return candidateMap(draft);
}

QPointF PDFEditController::constrainedShapeEnd(
    int page,
    const QPointF &startPoint,
    const QPointF &endPoint) const
{
    const QPointF boundedStart(
        qBound<qreal>(0.0, startPoint.x(), 1.0),
        qBound<qreal>(0.0, startPoint.y(), 1.0));
    QPointF boundedEnd(
        qBound<qreal>(0.0, endPoint.x(), 1.0),
        qBound<qreal>(0.0, endPoint.y(), 1.0));
    if ((m_shapeType != LineShape && m_shapeType != ArrowShape)
        || !m_document) {
        return boundedEnd;
    }

    const QSizeF pageSize = m_document->pageSize(page);
    if (pageSize.isEmpty()) {
        return boundedEnd;
    }

    const qreal deltaX = (boundedEnd.x() - boundedStart.x())
        * pageSize.width();
    const qreal deltaY = (boundedEnd.y() - boundedStart.y())
        * pageSize.height();
    const qreal absoluteX = qAbs(deltaX);
    const qreal absoluteY = qAbs(deltaY);
    if (absoluteY <= absoluteX * kShapeAxisSnapRatio) {
        boundedEnd.setY(boundedStart.y());
    } else if (absoluteX <= absoluteY * kShapeAxisSnapRatio) {
        boundedEnd.setX(boundedStart.x());
    }
    return boundedEnd;
}

bool PDFEditController::shapeDragIsValid(
    int page,
    const QPointF &startPoint,
    const QPointF &endPoint) const
{
    if (!m_document
        || !std::isfinite(startPoint.x())
        || !std::isfinite(startPoint.y())
        || !std::isfinite(endPoint.x())
        || !std::isfinite(endPoint.y())) {
        return false;
    }

    const QSizeF pageSize = m_document->pageSize(page);
    if (pageSize.isEmpty()) {
        return false;
    }

    const QPointF boundedStart(
        qBound<qreal>(0.0, startPoint.x(), 1.0),
        qBound<qreal>(0.0, startPoint.y(), 1.0));
    const QPointF boundedEnd(
        qBound<qreal>(0.0, endPoint.x(), 1.0),
        qBound<qreal>(0.0, endPoint.y(), 1.0));
    return shapeDragIsLongEnough(
        pageSize,
        boundedStart,
        constrainedShapeEnd(page, boundedStart, boundedEnd));
}

qreal PDFEditController::shapeHitToleranceRatio(
    int page,
    qreal strokeWidthRatio) const
{
    if (!m_document || !std::isfinite(strokeWidthRatio)) {
        return 0.0;
    }

    const QSizeF pageSize = m_document->pageSize(page);
    if (pageSize.isEmpty()) {
        return 0.0;
    }
    return shapeHitTolerancePoints(
               qMax<qreal>(0.0, strokeWidthRatio),
               pageSize.height())
        / pageSize.height();
}

int PDFEditController::addBlankPage()
{
    if (watermarkChangesPending()) {
        //% "Save or discard watermark changes before editing page content."
        emit statusMessage(qtTrId("SaveWatermarkBeforeEditing"));
        return -1;
    }
    if (!m_document || !m_document->isLoaded() || m_saving) {
        return -1;
    }
    if (!documentAllowsPageAssembly()) {
        //% "This PDF does not permit adding or deleting pages."
        emit statusMessage(qtTrId("PdfPageAssemblyNotPermitted"));
        return -1;
    }

    finishSelectedFrameResize();
    finishSelectedTextEdit();
    QSizeF pageSize = m_document->pageSize(m_document->currentPage());
    if (pageSize.isEmpty() && m_document->sourcePageCount() > 0) {
        pageSize = m_document->pageSize(0);
    }
    if (pageSize.isEmpty()) {
        pageSize = QSizeF(595.0, 842.0);
    }

    Command command;
    command.kind = Command::Kind::PageStructure;
    command.page = m_document->pageCount() > 0
        ? m_document->currentPage() + 1
        : 0;
    command.pageStateBefore = capturePageState();
    command.pageStateAfter = command.pageStateBefore;
    for (PDFPageInsertion &insertion
         : command.pageStateAfter.pageInsertions) {
        if (insertion.page >= command.page) {
            ++insertion.page;
        }
    }
    command.pageStateAfter.pageInsertions.append(
        PDFPageInsertion{command.page, pageSize});
    std::sort(
        command.pageStateAfter.pageInsertions.begin(),
        command.pageStateAfter.pageInsertions.end(),
        [](const PDFPageInsertion &left, const PDFPageInsertion &right) {
            return left.page < right.page;
        });

    for (Candidate &candidate : command.pageStateAfter.addedCandidates) {
        if (candidate.page >= command.page) {
            ++candidate.page;
        }
    }

    QHash<QString, Draft> shiftedDrafts;
    shiftedDrafts.reserve(command.pageStateAfter.drafts.size());
    for (Draft draft : std::as_const(command.pageStateAfter.drafts)) {
        if (draft.candidate.page >= command.page) {
            ++draft.candidate.page;
        }
        shiftedDrafts.insert(
            draftKey(draft.candidate.page, draft.candidate.objectId),
            std::move(draft));
    }
    command.pageStateAfter.drafts = std::move(shiftedDrafts);

    if (command.pageStateAfter.hasSelection
        && command.pageStateAfter.selectedCandidate.page >= command.page) {
        ++command.pageStateAfter.selectedCandidate.page;
    }
    command.pageStateAfter.currentPage = command.page;
    command.pageStateAfter.hasSelection = false;
    command.pageStateAfter.selectedCandidate = Candidate();
    applyDraftState(command, true, true);
    m_undoStack.append(command);
    m_redoStack.clear();
    emit historyStateChanged();
    return command.page;
}

int PDFEditController::deleteCurrentPage()
{
    if (watermarkChangesPending()) {
        //% "Save or discard watermark changes before editing page content."
        emit statusMessage(qtTrId("SaveWatermarkBeforeEditing"));
        return -1;
    }
    if (!m_document || !m_document->isLoaded() || m_saving) {
        return -1;
    }
    if (!documentAllowsPageAssembly()) {
        //% "This PDF does not permit adding or deleting pages."
        emit statusMessage(qtTrId("PdfPageAssemblyNotPermitted"));
        return -1;
    }
    if (m_document->pageCount() <= 1) {
        //% "A PDF must contain at least one page."
        emit statusMessage(qtTrId("CannotDeleteOnlyPage"));
        return -1;
    }

    finishSelectedFrameResize();
    finishSelectedTextEdit();

    const int deletedPage = m_document->currentPage();
    const int deletedSourcePage = m_document->sourcePageIndex(deletedPage);
    Command command;
    command.kind = Command::Kind::PageStructure;
    command.page = deletedPage;
    command.pageStateBefore = capturePageState();
    command.pageStateAfter = command.pageStateBefore;

    if (deletedSourcePage >= 0) {
        command.pageStateAfter.deletedSourcePages.insert(deletedSourcePage);
    } else {
        const auto insertion = std::find_if(
            command.pageStateAfter.pageInsertions.cbegin(),
            command.pageStateAfter.pageInsertions.cend(),
            [deletedPage](const PDFPageInsertion &candidate) {
                return candidate.page == deletedPage;
            });
        if (insertion == command.pageStateAfter.pageInsertions.cend()) {
            return -1;
        }
        command.pageStateAfter.pageInsertions.erase(insertion);
    }
    for (PDFPageInsertion &insertion
         : command.pageStateAfter.pageInsertions) {
        if (insertion.page > deletedPage) {
            --insertion.page;
        }
    }

    QVector<Candidate> shiftedCandidates;
    shiftedCandidates.reserve(command.pageStateAfter.addedCandidates.size());
    for (Candidate candidate
         : std::as_const(command.pageStateAfter.addedCandidates)) {
        if (candidate.page == deletedPage) {
            continue;
        }
        if (candidate.page > deletedPage) {
            --candidate.page;
        }
        shiftedCandidates.append(std::move(candidate));
    }
    command.pageStateAfter.addedCandidates = std::move(shiftedCandidates);

    QHash<QString, Draft> shiftedDrafts;
    shiftedDrafts.reserve(command.pageStateAfter.drafts.size());
    for (Draft draft : std::as_const(command.pageStateAfter.drafts)) {
        if (draft.candidate.page == deletedPage) {
            continue;
        }
        if (draft.candidate.page > deletedPage) {
            --draft.candidate.page;
        }
        shiftedDrafts.insert(
            draftKey(draft.candidate.page, draft.candidate.objectId),
            std::move(draft));
    }
    command.pageStateAfter.drafts = std::move(shiftedDrafts);

    if (command.pageStateAfter.hasSelection) {
        if (command.pageStateAfter.selectedCandidate.page == deletedPage) {
            command.pageStateAfter.hasSelection = false;
            command.pageStateAfter.selectedCandidate = Candidate();
        } else if (command.pageStateAfter.selectedCandidate.page > deletedPage) {
            --command.pageStateAfter.selectedCandidate.page;
        }
    }
    const int remainingPageCount = m_document->sourcePageCount()
        - command.pageStateAfter.deletedSourcePages.size()
        + command.pageStateAfter.pageInsertions.size();
    command.pageStateAfter.currentPage = qBound(
        0,
        deletedPage,
        remainingPageCount - 1);

    applyDraftState(command, true, true);
    m_undoStack.append(command);
    m_redoStack.clear();
    emit historyStateChanged();
    return m_document->currentPage();
}

QRectF PDFEditController::rotatedImageFrame(const Draft &draft, int quarterTurns) const
{
    if (!m_document || draft.candidate.page < 0
        || draft.candidate.page >= m_document->pageCount()) {
        return {};
    }

    const QSizeF pageSize = m_document->pageSize(draft.candidate.page);
    const QRectF frame = draft.frameRect;
    if (!std::isfinite(pageSize.width()) || !std::isfinite(pageSize.height())
        || pageSize.width() <= 0.0 || pageSize.height() <= 0.0
        || !std::isfinite(frame.x()) || !std::isfinite(frame.y())
        || !std::isfinite(frame.width()) || !std::isfinite(frame.height())
        || frame.width() <= 0.0 || frame.height() <= 0.0) {
        return {};
    }

    // Swap physical dimensions, not normalized ratios: PDF pages need not be square.
    const bool swapsAxes = quarterTurns % 2 != 0;
    const qreal width = swapsAxes
        ? frame.height() * pageSize.height() / pageSize.width() : frame.width();
    const qreal height = swapsAxes
        ? frame.width() * pageSize.width() / pageSize.height() : frame.height();
    const qreal minimumTop = selectionTopRatio(draft.candidate.page);
    const qreal maximumBottom = selectionBottomRatio(draft.candidate.page);
    if (!std::isfinite(width) || !std::isfinite(height)
        || width > 1.0 || height > maximumBottom - minimumTop) {
        return {};
    }

    // Keep the center when possible and shift away from page edges without scaling.
    const QPointF center = frame.center();
    return QRectF(qBound(0.0, center.x() - width / 2.0, 1.0 - width),
                  qBound(minimumTop, center.y() - height / 2.0, maximumBottom - height),
                  width, height);
}

bool PDFEditController::selectedImageRotatable() const
{
    return selectedImageResizable()
        && !rotatedImageFrame(selectedDraftForDisplay(), 1).isEmpty();
}

void PDFEditController::rotateSelectedImage(int quarterTurns)
{
    const int turns = (quarterTurns % 4 + 4) % 4;
    if (m_saving || m_textEditActive || m_frameResizeActive
        || turns == 0 || !selectedImageResizable()) {
        return;
    }

    const QString key = draftKey(m_selectedCandidate.page, m_selectedCandidate.objectId);
    const auto existing = m_drafts.constFind(key);
    const Draft before = existing == m_drafts.cend()
        ? initialDraftForCandidate(m_selectedCandidate) : *existing;
    const QRectF frame = rotatedImageFrame(before, turns);
    if (before.deleted || frame.isEmpty()) {
        return;
    }

    Command command;
    command.key = key;
    command.page = m_selectedCandidate.page;
    command.objectId = m_selectedCandidate.objectId;
    command.hadBefore = existing != m_drafts.cend();
    command.before = before;
    command.after = before;
    command.after.imageQuarterTurns = (before.imageQuarterTurns + turns) % 4;
    command.after.frameRect = frame;
    command.after.contentBounds = frame;
    command.hadAfter = m_selectedCandidate.added
        || !sameDraftContent(command.after, initialDraftForCandidate(m_selectedCandidate));
    m_undoStack.append(command);
    m_redoStack.clear();
    applyDraftState(command, true, true);
    emit historyStateChanged();
}

void PDFEditController::beginSelectedFrameResize()
{
    if (m_saving || m_frameResizeActive
        || !m_hasSelection
        || !m_selectedCandidate.editable) {
        return;
    }

    const bool ownsTextEdit = !m_textEditActive;
    if (ownsTextEdit) {
        beginSelectedTextEditSession(false);
    }
    if (!m_textEditActive || !m_hasSelection) {
        return;
    }

    const Draft draft = selectedDraftForDisplay();
    m_frameResizeActive = true;
    m_frameResizeOwnsTextEdit = ownsTextEdit;
    m_frameResizeStartGeometryExplicitlyChanged
        = m_textEditGeometryExplicitlyChanged;
    m_frameResizeStartFrameRect = draft.frameRect;
    m_frameResizeStartContentBounds = draft.contentBounds;
    if (isGeometryCommentCandidateKind(draft.candidate.kind)
        && !draft.candidate.added) {
        markDraftStateChanged(draft.candidate.page, true, false);
    }
}

void PDFEditController::updateSelectedFrameRect(const QRectF &frameRect)
{
    if (m_saving || !m_frameResizeActive || !m_textEditActive || !m_hasSelection
        || !qIsFinite(frameRect.x()) || !qIsFinite(frameRect.y())
        || !qIsFinite(frameRect.width()) || !qIsFinite(frameRect.height())
        || !qIsFinite(frameRect.right()) || !qIsFinite(frameRect.bottom())) {
        return;
    }

    const QRectF normalizedRect = frameRect.normalized();
    Draft draft = selectedDraftForDisplay();
    const qreal minimumTop = selectionTopRatio(draft.candidate.page);
    const qreal maximumBottom = selectionBottomRatio(draft.candidate.page);
    const QRectF currentFrameRect = draft.frameRect.normalized();
    constexpr qreal geometryEpsilon = 1e-9;
    const bool movesFrame
        = qAbs(normalizedRect.width() - currentFrameRect.width())
              <= geometryEpsilon
        && qAbs(normalizedRect.height() - currentFrameRect.height())
               <= geometryEpsilon;

    if (!movesFrame && draft.candidate.kind == QLatin1String("image")
        && !draft.candidate.imageResizable) {
        return;
    }

    QRectF boundedRect;
    QRectF resizedContentBounds;
    if (movesFrame) {
        const QRectF effectiveBounds = currentFrameRect.united(
            draft.contentBounds.normalized());
        const qreal requestedX
            = normalizedRect.x() - currentFrameRect.x();
        const qreal requestedY
            = normalizedRect.y() - currentFrameRect.y();
        const qreal translationX = qBound<qreal>(
            -effectiveBounds.left(),
            requestedX,
            1.0 - effectiveBounds.right());
        const qreal translationY = qBound<qreal>(
            minimumTop - effectiveBounds.top(),
            requestedY,
            maximumBottom - effectiveBounds.bottom());
        const QPointF translation(translationX, translationY);
        boundedRect = currentFrameRect.translated(translation);
        resizedContentBounds = draft.contentBounds.translated(translation);
    } else {
        const qreal left = qBound<qreal>(0.0, normalizedRect.left(), 1.0);
        const qreal top = qBound<qreal>(minimumTop, normalizedRect.top(), maximumBottom);
        const qreal right = qBound<qreal>(0.0, normalizedRect.right(), 1.0);
        const qreal bottom = qBound<qreal>(minimumTop, normalizedRect.bottom(), maximumBottom);
        if (right - left <= 0.0 || bottom - top <= 0.0) {
            return;
        }
        boundedRect = QRectF(left, top, right - left, bottom - top);
    }

    const bool sourceImageDraft = draft.candidate.kind == QLatin1String("image");
    const bool fixedBoundsDraft = sourceImageDraft
        || draft.candidate.kind == QLatin1String("addedImage")
        || draft.candidate.kind == QLatin1String("freeText")
        || draft.candidate.kind == QLatin1String("stamp")
        || draft.candidate.kind == QLatin1String("stickyNote")
        || isTextMarkupCandidateKind(draft.candidate.kind)
        || isDrawingCommentCandidateKind(draft.candidate.kind)
        || isShapeCandidateKind(draft.candidate.kind);
    if (!movesFrame) {
        const qreal previousContentHeight = qMax(
            draft.frameRect.height(),
            draft.contentBounds.bottom() - draft.frameRect.y());
        resizedContentBounds = fixedBoundsDraft
            ? boundedRect
            : QRectF(
                  boundedRect.x(),
                  boundedRect.y(),
                  boundedRect.width(),
                  qMin(
                      maximumBottom - boundedRect.y(),
                      qMax(boundedRect.height(), previousContentHeight)));
    }
    if (draft.frameRect == boundedRect
        && draft.contentBounds == resizedContentBounds) {
        return;
    }

    m_textEditGeometryExplicitlyChanged
        = boundedRect != m_textEditInitialDisplayFrameRect;
    draft.frameRect = boundedRect;
    draft.contentBounds = resizedContentBounds;
    if (isGeometryCommentCandidateKind(draft.candidate.kind)) {
        m_textEditDisplayDraft = draft;
        emit selectedGeometryChanged();
        return;
    }
    updateSelectedDraft(draft);
}

void PDFEditController::updateSelectedContentBounds(const QRectF &contentBounds)
{
    if (!m_textEditActive || !m_hasSelection) {
        return;
    }

    Draft draft = selectedDraftForDisplay();
    const QRectF &frameRect = draft.frameRect;
    const qreal contentBottom = qBound<qreal>(
        frameRect.bottom(),
        contentBounds.normalized().bottom(),
        1.0);
    const QRectF boundedContentBounds(
        frameRect.x(),
        frameRect.y(),
        frameRect.width(),
        contentBottom - frameRect.y());
    if (draft.contentBounds == boundedContentBounds) {
        return;
    }

    draft.contentBounds = boundedContentBounds;
    updateSelectedDraft(draft);
}

void PDFEditController::finishSelectedFrameResize()
{
    if (!m_frameResizeActive) {
        return;
    }

    const bool refreshesSourceComment = m_hasSelection
        && isGeometryCommentCandidateKind(m_selectedCandidate.kind)
        && !m_selectedCandidate.added;
    const int page = refreshesSourceComment
        ? m_selectedCandidate.page
        : -1;
    const quint64 suppressionRevision = refreshesSourceComment
        ? draftRevisionForPage(page)
        : 0;
    const bool ownsTextEdit = m_frameResizeOwnsTextEdit;
    resetFrameResizeState();
    if (ownsTextEdit) {
        finishSelectedTextEdit();
    }
    if (refreshesSourceComment
        && draftRevisionForPage(page) == suppressionRevision) {
        // Beginning a source comment gesture renders a texture with the source
        // annotation suppressed. A no-op finish still needs a new revision;
        // otherwise PDFPageItem can reuse that suppressed texture forever.
        markDraftStateChanged(page, true, false);
    }
}

void PDFEditController::cancelSelectedFrameResize()
{
    if (!m_frameResizeActive) {
        return;
    }

    const bool refreshesSourceComment = m_hasSelection
        && isGeometryCommentCandidateKind(m_selectedCandidate.kind)
        && !m_selectedCandidate.added;
    const int page = refreshesSourceComment
        ? m_selectedCandidate.page
        : -1;
    const quint64 suppressionRevision = refreshesSourceComment
        ? draftRevisionForPage(page)
        : 0;
    const bool ownsTextEdit = m_frameResizeOwnsTextEdit;
    const QRectF startFrameRect = m_frameResizeStartFrameRect;
    const QRectF startContentBounds = m_frameResizeStartContentBounds;
    const bool startGeometryExplicitlyChanged
        = m_frameResizeStartGeometryExplicitlyChanged;
    resetFrameResizeState();

    if (ownsTextEdit) {
        cancelSelectedTextEdit();
        if (refreshesSourceComment
            && draftRevisionForPage(page) == suppressionRevision) {
            markDraftStateChanged(page, true, false);
        }
        return;
    }

    Draft draft = selectedDraftForDisplay();
    draft.frameRect = startFrameRect;
    draft.contentBounds = startContentBounds;
    updateSelectedDraft(draft);
    m_textEditGeometryExplicitlyChanged = startGeometryExplicitlyChanged;
    if (refreshesSourceComment
        && draftRevisionForPage(page) == suppressionRevision) {
        markDraftStateChanged(page, true, false);
    }
}

void PDFEditController::resetFrameResizeState()
{
    m_frameResizeActive = false;
    m_frameResizeOwnsTextEdit = false;
    m_frameResizeStartGeometryExplicitlyChanged = false;
    m_frameResizeStartFrameRect = QRectF();
    m_frameResizeStartContentBounds = QRectF();
}
