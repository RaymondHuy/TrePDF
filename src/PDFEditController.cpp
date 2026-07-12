#include "PDFEditController.h"
#include "PDFiumSynchronization.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include <QByteArray>
#include <QFont>
#include <QFontInfo>
#include <QMutexLocker>
#include <QSet>
#include <QStringList>
#include <QVariantMap>
#include <QtTranslation>

#include <fpdf_annot.h>
#include <fpdf_edit.h>
#include <fpdf_text.h>

namespace {

constexpr unsigned long kModifyContentsPermission = 1UL << 3;
constexpr int kCoordinateMapExtent = 100000;
constexpr qreal kAddedTextWidthRatio = 0.25;
constexpr qreal kAddedTextMinimumWidthRatio = 0.02;
constexpr qreal kAddedTextHeightRatio = 0.04;
constexpr qreal kAddedTextFontSizeRatio = 0.022;
constexpr FS_MATRIX kIdentityMatrix{1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F};

QSize mappingDeviceSize(const QSizeF &pageSize)
{
    const qreal maximumDimension = qMax(pageSize.width(), pageSize.height());
    if (maximumDimension <= 0.0) {
        return {};
    }

    const qreal scale = kCoordinateMapExtent / maximumDimension;
    return QSize(qMax(1, qRound(pageSize.width() * scale)),
                 qMax(1, qRound(pageSize.height() * scale)));
}

QPointF mapPoint(const FS_MATRIX &matrix, const QPointF &point)
{
    return QPointF(matrix.a * point.x() + matrix.c * point.y() + matrix.e,
                   matrix.b * point.x() + matrix.d * point.y() + matrix.f);
}

FS_MATRIX combinedTransform(const FS_MATRIX &parent, const FS_MATRIX &local)
{
    return FS_MATRIX{
        static_cast<float>(parent.a * local.a + parent.c * local.b),
        static_cast<float>(parent.b * local.a + parent.d * local.b),
        static_cast<float>(parent.a * local.c + parent.c * local.d),
        static_cast<float>(parent.b * local.c + parent.d * local.d),
        static_cast<float>(parent.a * local.e + parent.c * local.f + parent.e),
        static_cast<float>(parent.b * local.e + parent.d * local.f + parent.f)};
}

bool pagePointToDevice(FPDF_PAGE page,
                       const QSize &deviceSize,
                       const QPointF &pagePoint,
                       QPointF &devicePoint)
{
    int deviceX = 0;
    int deviceY = 0;
    if (!FPDF_PageToDevice(page,
                           0,
                           0,
                           deviceSize.width(),
                           deviceSize.height(),
                           0,
                           pagePoint.x(),
                           pagePoint.y(),
                           &deviceX,
                           &deviceY)) {
        return false;
    }

    devicePoint = QPointF(deviceX, deviceY);
    return true;
}

bool transformedPointToDevice(FPDF_PAGE page,
                              const QSize &deviceSize,
                              const FS_MATRIX &transform,
                              const QPointF &point,
                              QPointF &devicePoint)
{
    return pagePointToDevice(page, deviceSize, mapPoint(transform, point), devicePoint);
}

bool supportsInlineTextLayout(FPDF_PAGE page,
                              const QSizeF &pageSize,
                              const FS_MATRIX &transform)
{
    const QSize deviceSize = mappingDeviceSize(pageSize);
    QPointF origin;
    QPointF baselinePoint;
    QPointF verticalPoint;
    if (deviceSize.isEmpty()
        || !transformedPointToDevice(page, deviceSize, transform, QPointF(0.0, 0.0), origin)
        || !transformedPointToDevice(page, deviceSize, transform, QPointF(1.0, 0.0), baselinePoint)
        || !transformedPointToDevice(page, deviceSize, transform, QPointF(0.0, 1.0), verticalPoint)) {
        return false;
    }

    const QPointF baseline = baselinePoint - origin;
    const QPointF vertical = verticalPoint - origin;
    const qreal baselineLength = std::hypot(baseline.x(), baseline.y());
    const qreal verticalLength = std::hypot(vertical.x(), vertical.y());
    if (baselineLength <= 0.0 || verticalLength <= 0.0) {
        return false;
    }

    return baseline.x() > 0.0
        && qAbs(baseline.y()) <= baselineLength * 0.05
        && vertical.y() < 0.0
        && qAbs(vertical.x()) <= verticalLength * 0.10;
}

qreal normalizedFontSize(FPDF_PAGE page,
                         const QSizeF &pageSize,
                         const FS_MATRIX &transform,
                         qreal fontSize)
{
    const QSize deviceSize = mappingDeviceSize(pageSize);
    QPointF origin;
    QPointF fontTop;
    if (fontSize <= 0.0
        || deviceSize.isEmpty()
        || !transformedPointToDevice(page, deviceSize, transform, QPointF(0.0, 0.0), origin)
        || !transformedPointToDevice(page, deviceSize, transform, QPointF(0.0, fontSize), fontTop)) {
        return 0.0;
    }

    return std::hypot(fontTop.x() - origin.x(), fontTop.y() - origin.y())
        / deviceSize.height();
}

QRectF normalizedRect(FPDF_PAGE page,
                      double left,
                      double bottom,
                      double right,
                      double top,
                      const FS_MATRIX &containerTransform,
                      const QSizeF &pageSize)
{
    const QSize deviceSize = mappingDeviceSize(pageSize);
    if (!page || deviceSize.isEmpty()) {
        return {};
    }

    const std::array<QPointF, 4> containerPoints{
        QPointF(left, bottom),
        QPointF(left, top),
        QPointF(right, bottom),
        QPointF(right, top)};
    std::array<QPointF, 4> devicePoints;
    for (size_t index = 0; index < containerPoints.size(); ++index) {
        if (!pagePointToDevice(page,
                               deviceSize,
                               mapPoint(containerTransform, containerPoints[index]),
                               devicePoints[index])) {
            return {};
        }
    }

    qreal minimumX = devicePoints.front().x();
    qreal maximumX = minimumX;
    qreal minimumY = devicePoints.front().y();
    qreal maximumY = minimumY;
    for (const QPointF &point : devicePoints) {
        minimumX = qMin(minimumX, point.x());
        maximumX = qMax(maximumX, point.x());
        minimumY = qMin(minimumY, point.y());
        maximumY = qMax(maximumY, point.y());
    }

    const QRectF normalized(minimumX / deviceSize.width(),
                            minimumY / deviceSize.height(),
                            (maximumX - minimumX) / deviceSize.width(),
                            (maximumY - minimumY) / deviceSize.height());
    return normalized.intersected(QRectF(0.0, 0.0, 1.0, 1.0));
}

QString textObjectText(FPDF_PAGEOBJECT object, FPDF_TEXTPAGE textPage)
{
    if (!object || !textPage) {
        return {};
    }

    const unsigned long byteLength = FPDFTextObj_GetText(object, textPage, nullptr, 0);
    if (byteLength < sizeof(FPDF_WCHAR)) {
        return {};
    }

    const qsizetype unitCount = static_cast<qsizetype>(
        (byteLength + sizeof(FPDF_WCHAR) - 1) / sizeof(FPDF_WCHAR));
    QVector<FPDF_WCHAR> buffer(unitCount);
    const unsigned long actualLength = FPDFTextObj_GetText(
        object,
        textPage,
        buffer.data(),
        byteLength);
    if (actualLength < sizeof(FPDF_WCHAR)) {
        return {};
    }

    qsizetype characterCount = static_cast<qsizetype>(actualLength / sizeof(FPDF_WCHAR));
    while (characterCount > 0 && buffer.at(characterCount - 1) == 0) {
        --characterCount;
    }
    return QString::fromUtf16(
        reinterpret_cast<const char16_t *>(buffer.constData()),
        characterCount);
}

QString annotationString(FPDF_ANNOTATION annotation, FPDF_BYTESTRING key)
{
    if (!annotation) {
        return {};
    }

    const unsigned long byteLength = FPDFAnnot_GetStringValue(annotation, key, nullptr, 0);
    if (byteLength < sizeof(FPDF_WCHAR)) {
        return {};
    }

    const qsizetype unitCount = static_cast<qsizetype>(
        (byteLength + sizeof(FPDF_WCHAR) - 1) / sizeof(FPDF_WCHAR));
    QVector<FPDF_WCHAR> buffer(unitCount);
    const unsigned long actualLength = FPDFAnnot_GetStringValue(
        annotation,
        key,
        buffer.data(),
        byteLength);
    if (actualLength < sizeof(FPDF_WCHAR)) {
        return {};
    }

    qsizetype characterCount = static_cast<qsizetype>(actualLength / sizeof(FPDF_WCHAR));
    while (characterCount > 0 && buffer.at(characterCount - 1) == 0) {
        --characterCount;
    }
    return QString::fromUtf16(
        reinterpret_cast<const char16_t *>(buffer.constData()),
        characterCount);
}

QString fontFamily(FPDF_PAGEOBJECT object)
{
    FPDF_FONT font = FPDFTextObj_GetFont(object);
    if (!font) {
        return {};
    }

    const size_t length = FPDFFont_GetFamilyName(font, nullptr, 0);
    if (length == 0) {
        return {};
    }

    QByteArray buffer(static_cast<qsizetype>(length), '\0');
    if (FPDFFont_GetFamilyName(font, buffer.data(), length) == 0) {
        return {};
    }

    const QString family = QString::fromUtf8(buffer.constData()).trimmed();
    if (family.isEmpty()) {
        return {};
    }

    const QFontInfo resolvedFont{QFont(family)};
    return resolvedFont.exactMatch() ? family : QString();
}

QColor textColor(FPDF_PAGEOBJECT object, FPDF_TEXT_RENDERMODE renderMode)
{
    unsigned int red = 0;
    unsigned int green = 0;
    unsigned int blue = 0;
    unsigned int alpha = 255;
    const bool strokeOnly = renderMode == FPDF_TEXTRENDERMODE_STROKE
        || renderMode == FPDF_TEXTRENDERMODE_STROKE_CLIP;
    const FPDF_BOOL foundColor = strokeOnly
        ? FPDFPageObj_GetStrokeColor(object, &red, &green, &blue, &alpha)
        : FPDFPageObj_GetFillColor(object, &red, &green, &blue, &alpha);
    if (!foundColor) {
        return Qt::black;
    }
    return QColor::fromRgb(red, green, blue, alpha);
}

bool isSuppressibleTextRenderMode(FPDF_TEXT_RENDERMODE renderMode)
{
    return renderMode == FPDF_TEXTRENDERMODE_FILL
        || renderMode == FPDF_TEXTRENDERMODE_STROKE
        || renderMode == FPDF_TEXTRENDERMODE_FILL_STROKE;
}

QString pageObjectId(const QVector<int> &objectPath)
{
    QStringList pathSegments;
    pathSegments.reserve(objectPath.size());
    for (int index : objectPath) {
        pathSegments.append(QString::number(index));
    }
    return QStringLiteral("object:") + pathSegments.join(QLatin1Char('/'));
}

} // namespace

PDFEditController::PDFEditController(QObject *parent)
    : QObject(parent)
{
}

PDFDocument *PDFEditController::document() const
{
    return m_document;
}

void PDFEditController::setDocument(PDFDocument *document)
{
    if (m_document == document) {
        return;
    }

    if (m_document) {
        disconnect(m_document, nullptr, this, nullptr);
    }

    m_document = document;
    if (m_document) {
        connect(m_document, &PDFDocument::sourceChanged, this, &PDFEditController::resetSession);
        connect(m_document, &PDFDocument::loadedChanged, this, &PDFEditController::resetSession);
        connect(m_document, &PDFDocument::pageCountChanged, this, &PDFEditController::resetSession);
        connect(m_document, &QObject::destroyed, this, [this]() {
            m_document = nullptr;
            resetSession();
            emit documentChanged();
        });
    }

    resetSession();
    emit documentChanged();
}

PDFEditController::Tool PDFEditController::tool() const
{
    return m_tool;
}

void PDFEditController::setTool(Tool tool)
{
    if (m_tool == tool) {
        return;
    }

    finishSelectedTextEdit();
    m_tool = tool;
    if (m_tool == BrowseTool) {
        clearSelection();
    }
    emit toolChanged();
}

bool PDFEditController::hasDrafts() const
{
    return !m_drafts.isEmpty();
}

bool PDFEditController::canUndo() const
{
    return !m_undoStack.isEmpty();
}

bool PDFEditController::canRedo() const
{
    return !m_redoStack.isEmpty();
}

bool PDFEditController::textEditActive() const
{
    return m_textEditActive;
}

int PDFEditController::selectedPage() const
{
    return m_hasSelection ? m_selectedCandidate.page : -1;
}

QString PDFEditController::selectedObjectId() const
{
    return m_hasSelection ? m_selectedCandidate.objectId : QString();
}

QString PDFEditController::selectedKind() const
{
    return m_hasSelection ? m_selectedCandidate.kind : QString();
}

qreal PDFEditController::selectedX() const
{
    return m_hasSelection ? m_selectedCandidate.rect.x() : 0.0;
}

qreal PDFEditController::selectedY() const
{
    return m_hasSelection ? m_selectedCandidate.rect.y() : 0.0;
}

qreal PDFEditController::selectedWidth() const
{
    return m_hasSelection ? m_selectedCandidate.rect.width() : 0.0;
}

qreal PDFEditController::selectedHeight() const
{
    return m_hasSelection ? m_selectedCandidate.rect.height() : 0.0;
}

QString PDFEditController::selectedText() const
{
    return m_hasSelection ? m_selectedText : QString();
}

QString PDFEditController::selectedFontFamily() const
{
    return m_hasSelection ? m_selectedCandidate.fontFamily : QString();
}

qreal PDFEditController::selectedFontSizeRatio() const
{
    return m_hasSelection ? m_selectedCandidate.fontSizeRatio : 0.0;
}

QColor PDFEditController::selectedColor() const
{
    return m_hasSelection ? m_selectedCandidate.color : QColor(Qt::black);
}

bool PDFEditController::selectedEditable() const
{
    return m_hasSelection && m_selectedCandidate.editable;
}

QString PDFEditController::selectedReason() const
{
    return m_hasSelection ? m_selectedCandidate.reason : QString();
}

quint64 PDFEditController::draftRevisionForPage(int page) const
{
    return m_pageDraftRevisions.value(page, 0);
}

QVector<QVector<int>> PDFEditController::draftedTextObjectPathsForPage(int page) const
{
    QVector<QVector<int>> objectPaths;
    for (auto draft = m_drafts.cbegin(); draft != m_drafts.cend(); ++draft) {
        const Candidate &candidate = draft->candidate;
        if (candidate.page == page
            && candidate.kind == QLatin1String("text")
            && candidate.renderSuppressible
            && !candidate.objectPath.isEmpty()) {
            objectPaths.append(candidate.objectPath);
        }
    }

    std::sort(objectPaths.begin(), objectPaths.end(), [](const QVector<int> &left,
                                                         const QVector<int> &right) {
        return std::lexicographical_compare(
            left.cbegin(), left.cend(), right.cbegin(), right.cend());
    });
    return objectPaths;
}

bool PDFEditController::allDraftedTextObjectsSuppressibleForPage(int page) const
{
    for (auto draft = m_drafts.cbegin(); draft != m_drafts.cend(); ++draft) {
        const Candidate &candidate = draft->candidate;
        if (candidate.page == page
            && candidate.kind == QLatin1String("text")
            && !candidate.renderSuppressible) {
            return false;
        }
    }
    return true;
}

QVariantList PDFEditController::candidatesForPage(int page)
{
    QVariantList result;
    const QVector<Candidate> &candidates = ensurePageCandidates(page);
    result.reserve(candidates.size() + m_addedCandidates.size());
    for (const Candidate &candidate : candidates) {
        const QString key = draftKey(candidate.page, candidate.objectId);
        const auto draft = m_drafts.constFind(key);
        const QString displayText = draft == m_drafts.cend() ? candidate.text : draft->text;
        result.append(candidateMap(candidate, displayText));
    }
    for (const Candidate &candidate : std::as_const(m_addedCandidates)) {
        if (candidate.page != page) {
            continue;
        }

        const auto draft = m_drafts.constFind(draftKey(page, candidate.objectId));
        const QString displayText = draft == m_drafts.cend() ? candidate.text : draft->text;
        result.append(candidateMap(candidate, displayText));
    }
    return result;
}

QVariantList PDFEditController::draftsForPage(int page) const
{
    QVariantList result;
    const auto pageCandidates = m_pageCandidates.constFind(page);
    if (pageCandidates != m_pageCandidates.cend()) {
        for (const Candidate &candidate : pageCandidates.value()) {
            const auto draft = m_drafts.constFind(draftKey(page, candidate.objectId));
            if (draft == m_drafts.cend()) {
                continue;
            }

            QVariantMap map = candidateMap(candidate, draft->text);
            map.insert(QStringLiteral("originalText"), candidate.text);
            result.append(map);
        }
    }

    for (const Candidate &candidate : m_addedCandidates) {
        if (candidate.page != page) {
            continue;
        }

        const auto draft = m_drafts.constFind(draftKey(page, candidate.objectId));
        if (draft == m_drafts.cend()) {
            continue;
        }
        QVariantMap map = candidateMap(candidate, draft->text);
        map.insert(QStringLiteral("originalText"), candidate.text);
        result.append(map);
    }
    return result;
}

QVariantMap PDFEditController::candidateAt(int page, qreal x, qreal y)
{
    const QVector<Candidate> &candidates = ensurePageCandidates(page);
    const QPointF point(x, y);
    const Candidate *bestCandidate = nullptr;
    qreal bestArea = 0.0;

    const auto considerCandidate = [&](const Candidate &candidate) {
        const QRectF hitRect = candidate.rect.adjusted(-0.0025, -0.0025, 0.0025, 0.0025);
        if (!hitRect.contains(point)) {
            return;
        }

        const qreal area = candidate.rect.width() * candidate.rect.height();
        const bool isBetter = !bestCandidate
            || (candidate.added && !bestCandidate->added)
            || (candidate.added == bestCandidate->added
                && candidate.editable && !bestCandidate->editable)
            || (candidate.added == bestCandidate->added
                && candidate.editable == bestCandidate->editable
                && area < bestArea);
        if (isBetter) {
            bestCandidate = &candidate;
            bestArea = area;
        }
    };

    for (auto candidate = candidates.crbegin(); candidate != candidates.crend(); ++candidate) {
        considerCandidate(*candidate);
    }
    for (auto candidate = m_addedCandidates.crbegin();
         candidate != m_addedCandidates.crend();
         ++candidate) {
        if (candidate->page == page) {
            considerCandidate(*candidate);
        }
    }

    if (!bestCandidate) {
        return {};
    }

    const auto draft = m_drafts.constFind(draftKey(page, bestCandidate->objectId));
    const QString displayText = draft == m_drafts.cend() ? bestCandidate->text : draft->text;
    return candidateMap(*bestCandidate, displayText);
}

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
    candidate.addedOrder = ++m_nextAddedTextId;
    candidate.objectId = QStringLiteral("added:")
        + QString::number(candidate.addedOrder);
    candidate.kind = QStringLiteral("addedText");
    //% "Add Text"
    candidate.label = qtTrId("AddText");
    candidate.text.clear();
    candidate.fontFamily = QFontInfo(QFont()).family();
    candidate.fontSizeRatio = kAddedTextFontSizeRatio;
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
    candidate.rect = QRectF(
        frameX,
        frameY,
        frameWidth,
        kAddedTextHeightRatio);

    m_addedCandidates.append(candidate);
    setSelection(&m_addedCandidates.constLast());
    emit pageDataChanged(page);
    beginSelectedTextEdit();
    return candidateMap(candidate, candidate.text);
}

bool PDFEditController::selectCandidate(int page, const QString &objectId)
{
    if (m_textEditActive
        && (m_selectedCandidate.page != page || m_selectedCandidate.objectId != objectId)) {
        finishSelectedTextEdit();
    }

    const Candidate *candidate = findCandidate(page, objectId);
    if (!candidate) {
        clearSelection();
        return false;
    }

    setSelection(candidate);
    if (!candidate->editable && !candidate->reason.isEmpty()) {
        emit statusMessage(candidate->reason);
    }
    return true;
}

void PDFEditController::clearSelection()
{
    if (!m_hasSelection) {
        return;
    }
    finishSelectedTextEdit();
    setSelection(nullptr);
}

void PDFEditController::beginSelectedTextEdit()
{
    if (m_textEditActive || !m_hasSelection || !m_selectedCandidate.editable) {
        return;
    }

    const QString key = draftKey(m_selectedCandidate.page, m_selectedCandidate.objectId);
    const auto existingDraft = m_drafts.constFind(key);
    m_textEditCommand = Command();
    m_textEditCommand.key = key;
    m_textEditCommand.page = m_selectedCandidate.page;
    m_textEditCommand.objectId = m_selectedCandidate.objectId;
    m_textEditCommand.candidate = m_selectedCandidate;
    m_textEditCommand.hadBefore = existingDraft != m_drafts.cend();
    m_textEditCommand.beforeText = m_textEditCommand.hadBefore
        ? existingDraft->text
        : m_selectedCandidate.text;
    m_textEditActive = true;
    emit textEditActiveChanged();
}

void PDFEditController::updateSelectedTextPreview(const QString &text)
{
    if (!m_textEditActive) {
        beginSelectedTextEdit();
    }
    if (!m_textEditActive || !m_hasSelection) {
        return;
    }

    const QString key = draftKey(m_selectedCandidate.page, m_selectedCandidate.objectId);
    const auto existingDraft = m_drafts.constFind(key);
    bool changed = false;
    if (text == m_selectedCandidate.text) {
        if (existingDraft != m_drafts.cend()) {
            m_drafts.remove(key);
            changed = true;
        }
    } else if (existingDraft == m_drafts.cend() || existingDraft->text != text) {
        m_drafts.insert(key, Draft{m_selectedCandidate, text});
        changed = true;
    }

    m_selectedText = text;
    if (changed) {
        markDraftStateChanged(m_selectedCandidate.page, false);
        emit historyStateChanged();
    }
    emit selectionChanged();
}

void PDFEditController::finishSelectedTextEdit()
{
    if (!m_textEditActive) {
        return;
    }

    const Command baseline = m_textEditCommand;
    m_textEditActive = false;
    m_textEditCommand = Command();
    emit textEditActiveChanged();

    Command command = baseline;
    const auto currentDraft = m_drafts.constFind(command.key);
    command.hadAfter = currentDraft != m_drafts.cend();
    command.afterText = command.hadAfter
        ? currentDraft->text
        : m_selectedCandidate.text;
    const bool removedAddedCandidate = command.candidate.added
        && !command.hadAfter
        && removeAddedCandidate(command.page, command.objectId);
    if (removedAddedCandidate) {
        if (m_hasSelection
            && m_selectedCandidate.page == command.page
            && m_selectedCandidate.objectId == command.objectId) {
            setSelection(nullptr);
        }
        emit pageDataChanged(command.page);
    }
    requestDraftRender(command.page, true);
    const bool changed = command.hadBefore != command.hadAfter
        || command.beforeText != command.afterText;
    if (!changed) {
        return;
    }

    m_undoStack.append(command);
    m_redoStack.clear();
    emit historyStateChanged();
    //% "Draft updated. The PDF file is unchanged."
    emit statusMessage(qtTrId("DraftUpdatedPdfUnchanged"));
}

void PDFEditController::cancelSelectedTextEdit()
{
    if (!m_textEditActive) {
        return;
    }

    const Command baseline = m_textEditCommand;
    m_textEditActive = false;
    m_textEditCommand = Command();
    emit textEditActiveChanged();
    applyDraftState(baseline, false, true);
    emit historyStateChanged();
}

void PDFEditController::commitSelectedText(const QString &text)
{
    if (!m_hasSelection || !m_selectedCandidate.editable) {
        if (m_hasSelection && !m_selectedCandidate.reason.isEmpty()) {
            emit statusMessage(m_selectedCandidate.reason);
        }
        return;
    }

    if (m_textEditActive) {
        updateSelectedTextPreview(text);
        finishSelectedTextEdit();
        return;
    }

    const QString key = draftKey(m_selectedCandidate.page, m_selectedCandidate.objectId);
    const auto existingDraft = m_drafts.constFind(key);
    const bool hadBefore = existingDraft != m_drafts.cend();
    const QString beforeText = hadBefore ? existingDraft->text : m_selectedCandidate.text;
    if (beforeText == text) {
        return;
    }

    Command command;
    command.key = key;
    command.page = m_selectedCandidate.page;
    command.objectId = m_selectedCandidate.objectId;
    command.candidate = m_selectedCandidate;
    command.hadBefore = hadBefore;
    command.beforeText = beforeText;
    command.hadAfter = text != m_selectedCandidate.text;
    command.afterText = text;

    m_undoStack.append(command);
    m_redoStack.clear();
    applyDraftState(command, true, true);
    emit historyStateChanged();
    //% "Draft updated. The PDF file is unchanged."
    emit statusMessage(qtTrId("DraftUpdatedPdfUnchanged"));
}

void PDFEditController::undo()
{
    if (m_textEditActive || m_undoStack.isEmpty()) {
        return;
    }

    const Command command = m_undoStack.takeLast();
    applyDraftState(command, false, true);
    m_redoStack.append(command);
    emit historyStateChanged();
}

void PDFEditController::redo()
{
    if (m_textEditActive || m_redoStack.isEmpty()) {
        return;
    }

    const Command command = m_redoStack.takeLast();
    applyDraftState(command, true, true);
    m_undoStack.append(command);
    emit historyStateChanged();
}

void PDFEditController::discardDrafts()
{
    if (!m_textEditActive
        && m_drafts.isEmpty()
        && m_undoStack.isEmpty()
        && m_redoStack.isEmpty()) {
        return;
    }

    QSet<int> affectedPages;
    for (auto revision = m_pageDraftRevisions.cbegin();
         revision != m_pageDraftRevisions.cend();
         ++revision) {
        affectedPages.insert(revision.key());
    }

    const bool wasEditing = m_textEditActive;
    m_textEditActive = false;
    m_textEditCommand = Command();
    m_addedCandidates.clear();
    m_drafts.clear();
    m_undoStack.clear();
    m_redoStack.clear();
    setSelection(nullptr);
    for (int page : std::as_const(affectedPages)) {
        markDraftStateChanged(page, true);
    }
    if (wasEditing) {
        emit textEditActiveChanged();
    }
    emit pageDataChanged(-1);
    emit historyStateChanged();
    //% "All draft changes were discarded."
    emit statusMessage(qtTrId("AllDraftChangesDiscarded"));
}

void PDFEditController::resetSession()
{
    m_pageCandidates.clear();
    m_addedCandidates.clear();
    m_drafts.clear();
    m_pageDraftRevisions.clear();
    m_undoStack.clear();
    m_redoStack.clear();
    m_nextAddedTextId = 0;
    m_cachedRevision = m_document ? m_document->renderRevision() : -1;
    m_hasSelection = false;
    m_selectedCandidate = Candidate();
    m_selectedText.clear();
    const bool wasEditing = m_textEditActive;
    m_textEditActive = false;
    m_textEditCommand = Command();

    if (wasEditing) {
        emit textEditActiveChanged();
    }

    if ((!m_document || !m_document->isLoaded()) && m_tool != BrowseTool) {
        m_tool = BrowseTool;
        emit toolChanged();
    }

    emit pageDataChanged(-1);
    emit historyStateChanged();
    emit selectionChanged();
}

void PDFEditController::ensureCacheRevision()
{
    const int revision = m_document ? m_document->renderRevision() : -1;
    if (m_cachedRevision == revision) {
        return;
    }

    m_cachedRevision = revision;
    m_pageCandidates.clear();
}

const QVector<PDFEditController::Candidate> &PDFEditController::ensurePageCandidates(int page)
{
    static const QVector<Candidate> empty;

    ensureCacheRevision();
    if (!m_document || !m_document->isLoaded() || page < 0 || page >= m_document->pageCount()) {
        return empty;
    }

    auto cached = m_pageCandidates.find(page);
    if (cached == m_pageCandidates.end()) {
        cached = m_pageCandidates.insert(page, scanPage(page));
    }
    return cached.value();
}

QVector<PDFEditController::Candidate> PDFEditController::scanPage(int pageIndex) const
{
    QVector<Candidate> candidates;
    if (!m_document || !m_document->m_document) {
        return candidates;
    }

    const QSizeF pageSize = m_document->pageSize(pageIndex);
    if (pageSize.isEmpty()) {
        return candidates;
    }

    FPDF_PAGE page = nullptr;
    FPDF_TEXTPAGE textPage = nullptr;
    {
        QMutexLocker pageLoadLock(&pdfiumPageLoadMutex());
        page = FPDF_LoadPage(m_document->m_document, pageIndex);
        if (page) {
            textPage = FPDFText_LoadPage(page);
        }
    }
    if (!page) {
        return candidates;
    }

    QHash<FPDF_PAGEOBJECT, QRectF> textObjectBounds;
    if (textPage) {
        const int characterCount = FPDFText_CountChars(textPage);
        for (int index = 0; index < characterCount; ++index) {
            FPDF_PAGEOBJECT textObject = FPDFText_GetTextObject(textPage, index);
            double left = 0.0;
            double right = 0.0;
            double bottom = 0.0;
            double top = 0.0;
            if (!textObject
                || !FPDFText_GetCharBox(
                    textPage, index, &left, &right, &bottom, &top)) {
                continue;
            }

            const QRectF characterBounds = QRectF(
                QPointF(left, bottom), QPointF(right, top)).normalized();
            if (characterBounds.isEmpty()) {
                continue;
            }

            auto objectBounds = textObjectBounds.find(textObject);
            if (objectBounds == textObjectBounds.end()) {
                textObjectBounds.insert(textObject, characterBounds);
            } else {
                objectBounds.value() = objectBounds->united(characterBounds);
            }
        }
    }

    const int objectCount = FPDFPage_CountObjects(page);
    for (int index = 0; index < objectCount; ++index) {
        appendPageObjectCandidates(
            page,
            FPDFPage_GetObject(page, index),
            QVector<int>{index},
            textPage,
            textObjectBounds,
            kIdentityMatrix,
            pageSize,
            candidates);
    }
    appendAnnotationCandidates(page, pageIndex, pageSize, candidates);

    if (textPage) {
        FPDFText_ClosePage(textPage);
    }
    FPDF_ClosePage(page);

    for (Candidate &candidate : candidates) {
        candidate.page = pageIndex;
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate &left, const Candidate &right) {
        return left.editable < right.editable;
    });
    return candidates;
}

void PDFEditController::appendPageObjectCandidates(FPDF_PAGE page,
                                                   FPDF_PAGEOBJECT pageObject,
                                                   const QVector<int> &objectPath,
                                                   FPDF_TEXTPAGE textPage,
                                                   const QHash<FPDF_PAGEOBJECT, QRectF> &textObjectBounds,
                                                   const FS_MATRIX &containerTransform,
                                                   const QSizeF &pageSize,
                                                   QVector<Candidate> &candidates) const
{
    if (!pageObject) {
        return;
    }

    const int type = FPDFPageObj_GetType(pageObject);
    if (type == FPDF_PAGEOBJ_FORM) {
        FS_MATRIX formTransform = kIdentityMatrix;
        if (!FPDFPageObj_GetMatrix(pageObject, &formTransform)) {
            formTransform = kIdentityMatrix;
        }
        const FS_MATRIX childTransform = combinedTransform(containerTransform, formTransform);
        const int count = FPDFFormObj_CountObjects(pageObject);
        for (int index = 0; index < count; ++index) {
            QVector<int> childPath = objectPath;
            childPath.append(index);
            appendPageObjectCandidates(
                page,
                FPDFFormObj_GetObject(pageObject, static_cast<unsigned long>(index)),
                childPath,
                textPage,
                textObjectBounds,
                childTransform,
                pageSize,
                candidates);
        }
        return;
    }

    if (type != FPDF_PAGEOBJ_TEXT && type != FPDF_PAGEOBJ_IMAGE) {
        return;
    }

    const FPDF_TEXT_RENDERMODE renderMode = type == FPDF_PAGEOBJ_TEXT
        ? FPDFTextObj_GetTextRenderMode(pageObject)
        : FPDF_TEXTRENDERMODE_UNKNOWN;
    if (type == FPDF_PAGEOBJ_TEXT
        && (renderMode == FPDF_TEXTRENDERMODE_UNKNOWN
            || renderMode == FPDF_TEXTRENDERMODE_INVISIBLE
            || renderMode == FPDF_TEXTRENDERMODE_CLIP)) {
        return;
    }

    float left = 0.0F;
    float bottom = 0.0F;
    float right = 0.0F;
    float top = 0.0F;
    if (!FPDFPageObj_GetBounds(pageObject, &left, &bottom, &right, &top)) {
        return;
    }

    QRectF rect = normalizedRect(
        page,
        left,
        bottom,
        right,
        top,
        containerTransform,
        pageSize);
    if (type == FPDF_PAGEOBJ_TEXT) {
        const auto glyphBounds = textObjectBounds.constFind(pageObject);
        if (glyphBounds != textObjectBounds.cend()) {
            const QRectF glyphRect = normalizedRect(
                page,
                glyphBounds->left(),
                glyphBounds->top(),
                glyphBounds->right(),
                glyphBounds->bottom(),
                kIdentityMatrix,
                pageSize);
            if (!glyphRect.isEmpty()) {
                rect = glyphRect;
            }
        }
    }
    if (rect.isEmpty()) {
        return;
    }

    Candidate candidate;
    candidate.objectId = pageObjectId(objectPath);
    candidate.objectPath = objectPath;
    candidate.rect = rect;

    if (type == FPDF_PAGEOBJ_IMAGE) {
        candidate.kind = QStringLiteral("image");
        //% "Image"
        candidate.label = qtTrId("Image");
        //% "This content is an image. OCR is required to edit its text."
        candidate.reason = qtTrId("ImageTextRequiresOcr");
        candidates.append(candidate);
        return;
    }

    const QString text = textObjectText(pageObject, textPage);
    if (text.trimmed().isEmpty()) {
        return;
    }

    float fontSize = 0.0F;
    FPDFTextObj_GetFontSize(pageObject, &fontSize);
    FS_MATRIX objectTransform = kIdentityMatrix;
    if (!FPDFPageObj_GetMatrix(pageObject, &objectTransform)) {
        objectTransform = kIdentityMatrix;
    }
    const FS_MATRIX textTransform = combinedTransform(containerTransform, objectTransform);
    const bool suppressionSupported = isSuppressibleTextRenderMode(renderMode);
    const bool inlineLayoutSupported = supportsInlineTextLayout(page, pageSize, textTransform)
        && !text.contains(QLatin1Char('\n'))
        && !text.contains(QLatin1Char('\r'));
    candidate.kind = QStringLiteral("text");
    //% "Text"
    candidate.label = qtTrId("Text");
    candidate.text = text;
    candidate.fontFamily = fontFamily(pageObject);
    candidate.fontSizeRatio = qMax<qreal>(
        0.0,
        normalizedFontSize(page, pageSize, textTransform, fontSize));
    candidate.color = textColor(pageObject, renderMode);
    const bool editingAllowed = documentAllowsEditing();
    candidate.editable = editingAllowed && inlineLayoutSupported;
    candidate.renderSuppressible = suppressionSupported;
    candidate.limited = true;
    if (!editingAllowed) {
        //% "This PDF does not permit content editing."
        candidate.reason = qtTrId("PdfEditingNotPermitted");
    } else if (!inlineLayoutSupported) {
        //% "Rotated, transformed, or multiline text is not supported by the inline draft editor yet."
        candidate.reason = qtTrId("InlineEditorUnsupportedTextLayout");
    }
    candidates.append(candidate);
}

void PDFEditController::appendAnnotationCandidates(FPDF_PAGE page,
                                                   int pageIndex,
                                                   const QSizeF &pageSize,
                                                   QVector<Candidate> &candidates) const
{
    const int annotationCount = FPDFPage_GetAnnotCount(page);
    for (int index = 0; index < annotationCount; ++index) {
        FPDF_ANNOTATION annotation = FPDFPage_GetAnnot(page, index);
        if (!annotation) {
            continue;
        }

        const int subtype = FPDFAnnot_GetSubtype(annotation);
        if (subtype != FPDF_ANNOT_FREETEXT && subtype != FPDF_ANNOT_WIDGET) {
            FPDFPage_CloseAnnot(annotation);
            continue;
        }

        FS_RECTF bounds{};
        if (!FPDFAnnot_GetRect(annotation, &bounds)) {
            FPDFPage_CloseAnnot(annotation);
            continue;
        }

        const QRectF rect = normalizedRect(
            page,
            bounds.left,
            bounds.bottom,
            bounds.right,
            bounds.top,
            kIdentityMatrix,
            pageSize);
        if (rect.isEmpty()) {
            FPDFPage_CloseAnnot(annotation);
            continue;
        }

        Candidate candidate;
        candidate.page = pageIndex;
        candidate.objectId = QStringLiteral("annotation:") + QString::number(index);
        candidate.rect = rect;
        candidate.fontFamily = QStringLiteral("Helvetica");
        candidate.fontSizeRatio = qMax<qreal>(0.01, rect.height() * 0.65);
        candidate.editable = false;
        candidate.limited = true;

        if (subtype == FPDF_ANNOT_FREETEXT) {
            candidate.kind = QStringLiteral("freeText");
            //% "Free text annotation"
            candidate.label = qtTrId("FreeTextAnnotation");
            candidate.text = annotationString(annotation, "Contents");
        } else {
            candidate.kind = QStringLiteral("widget");
            //% "Form field"
            candidate.label = qtTrId("FormField");
            candidate.text = annotationString(annotation, "V");
        }

        if (!documentAllowsEditing()) {
            //% "This PDF does not permit content editing."
            candidate.reason = qtTrId("PdfEditingNotPermitted");
        } else {
            //% "Editing form fields and free-text annotations is not available in this draft editor yet."
            candidate.reason = qtTrId("DraftEditorUnsupportedAnnotations");
        }
        candidates.append(candidate);
        FPDFPage_CloseAnnot(annotation);
    }
}

const PDFEditController::Candidate *PDFEditController::findCandidate(int page,
                                                                     const QString &objectId)
{
    for (const Candidate &candidate : std::as_const(m_addedCandidates)) {
        if (candidate.page == page && candidate.objectId == objectId) {
            return &candidate;
        }
    }

    const QVector<Candidate> &candidates = ensurePageCandidates(page);
    for (const Candidate &candidate : candidates) {
        if (candidate.objectId == objectId) {
            return &candidate;
        }
    }
    return nullptr;
}

QVariantMap PDFEditController::candidateMap(const Candidate &candidate, const QString &text) const
{
    QVariantMap map;
    map.insert(QStringLiteral("objectId"), candidate.objectId);
    map.insert(QStringLiteral("kind"), candidate.kind);
    map.insert(QStringLiteral("label"), candidate.label);
    map.insert(QStringLiteral("x"), candidate.rect.x());
    map.insert(QStringLiteral("y"), candidate.rect.y());
    map.insert(QStringLiteral("width"), candidate.rect.width());
    map.insert(QStringLiteral("height"), candidate.rect.height());
    map.insert(QStringLiteral("text"), text);
    map.insert(QStringLiteral("fontFamily"), candidate.fontFamily);
    map.insert(QStringLiteral("fontSizeRatio"), candidate.fontSizeRatio);
    map.insert(QStringLiteral("color"), candidate.color);
    map.insert(QStringLiteral("editable"), candidate.editable);
    map.insert(QStringLiteral("limited"), candidate.limited);
    map.insert(QStringLiteral("added"), candidate.added);
    map.insert(QStringLiteral("reason"), candidate.reason);
    return map;
}

bool PDFEditController::documentAllowsEditing() const
{
    if (!m_document || !m_document->m_document) {
        return false;
    }

    const unsigned long permissions = FPDF_GetDocPermissions(m_document->m_document);
    return permissions == 0xffffffffUL || (permissions & kModifyContentsPermission) != 0;
}

QString PDFEditController::draftKey(int page, const QString &objectId) const
{
    return QString::number(page) + QLatin1Char('|') + objectId;
}

void PDFEditController::markDraftStateChanged(int page, bool immediate)
{
    const quint64 revision = ++m_nextDraftRevision;
    m_pageDraftRevisions.insert(page, revision);
    emit draftRenderStateChanged(page, revision, immediate);
    emit pageDataChanged(page);
}

void PDFEditController::requestDraftRender(int page, bool immediate)
{
    emit draftRenderStateChanged(page, draftRevisionForPage(page), immediate);
}

void PDFEditController::applyDraftState(const Command &command,
                                        bool useAfterState,
                                        bool immediate)
{
    const bool shouldExist = useAfterState ? command.hadAfter : command.hadBefore;
    const QString text = useAfterState ? command.afterText : command.beforeText;
    Candidate candidate = command.candidate;
    if (candidate.objectId.isEmpty()) {
        const Candidate *currentCandidate = findCandidate(command.page, command.objectId);
        if (currentCandidate) {
            candidate = *currentCandidate;
        }
    }
    if (candidate.objectId.isEmpty()) {
        return;
    }

    bool candidateChanged = false;
    if (candidate.added && shouldExist && !findCandidate(command.page, command.objectId)) {
        const auto insertionPoint = std::lower_bound(
            m_addedCandidates.begin(),
            m_addedCandidates.end(),
            candidate.addedOrder,
            [](const Candidate &existing, quint64 addedOrder) {
                return existing.addedOrder < addedOrder;
            });
        m_addedCandidates.insert(insertionPoint, candidate);
        candidateChanged = true;
    }

    const auto existingDraft = m_drafts.constFind(command.key);
    const bool draftChanged = shouldExist
        ? existingDraft == m_drafts.cend() || existingDraft->text != text
        : existingDraft != m_drafts.cend();

    if (shouldExist && draftChanged) {
        m_drafts.insert(command.key, Draft{candidate, text});
    } else if (draftChanged) {
        m_drafts.remove(command.key);
    }

    if (candidate.added && !shouldExist) {
        candidateChanged = removeAddedCandidate(command.page, command.objectId)
            || candidateChanged;
    }

    if (candidate.added
        && !shouldExist
        && m_hasSelection
        && m_selectedCandidate.page == command.page
        && m_selectedCandidate.objectId == command.objectId) {
        setSelection(nullptr);
    } else {
        updateSelectedTextFromDraft();
    }

    const bool changed = candidateChanged || draftChanged;
    if (changed) {
        markDraftStateChanged(command.page, immediate);
    } else {
        requestDraftRender(command.page, immediate);
    }
}

bool PDFEditController::removeAddedCandidate(int page, const QString &objectId)
{
    for (qsizetype index = 0; index < m_addedCandidates.size(); ++index) {
        const Candidate &candidate = m_addedCandidates.at(index);
        if (candidate.page == page && candidate.objectId == objectId) {
            m_addedCandidates.removeAt(index);
            return true;
        }
    }
    return false;
}

void PDFEditController::updateSelectedTextFromDraft()
{
    if (!m_hasSelection) {
        return;
    }

    const auto draft = m_drafts.constFind(
        draftKey(m_selectedCandidate.page, m_selectedCandidate.objectId));
    m_selectedText = draft == m_drafts.cend() ? m_selectedCandidate.text : draft->text;
    emit selectionChanged();
}

void PDFEditController::setSelection(const Candidate *candidate)
{
    if (!candidate) {
        m_hasSelection = false;
        m_selectedCandidate = Candidate();
        m_selectedText.clear();
        emit selectionChanged();
        return;
    }

    m_selectedCandidate = *candidate;
    m_hasSelection = true;
    const auto draft = m_drafts.constFind(draftKey(candidate->page, candidate->objectId));
    m_selectedText = draft == m_drafts.cend() ? candidate->text : draft->text;
    emit selectionChanged();
}
