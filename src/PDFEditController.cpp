#include "PDFEditController.h"
#include "ApplicationFonts.h"
#include "PDFiumSynchronization.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include <QByteArray>
#include <QCryptographicHash>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
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
constexpr qreal kLineBaselineToleranceEm = 0.25;
constexpr qreal kLineMinimumHorizontalGapEm = -0.35;
constexpr qreal kLineMaximumHorizontalGapEm = 1.25;
constexpr qreal kLineWordSeparatorGapEm = 0.20;
constexpr qreal kLineFontSizeToleranceRatio = 0.08;
constexpr qreal kLineFontSizeTolerancePoints = 0.5;
constexpr int kLineFontStretchTolerance = 5;
constexpr int kPdfFontFlagItalic = 1 << 6;
constexpr int kPdfFontFlagForceBold = 1 << 18;
constexpr qsizetype kMaximumEmbeddedFontBytes = 32 * 1024 * 1024;
constexpr qsizetype kMaximumRegisteredFontBytes = 64 * 1024 * 1024;
constexpr FS_MATRIX kIdentityMatrix{1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F};

bool sameFontFamily(const QString &left, const QString &right)
{
    return left.compare(right, Qt::CaseInsensitive) == 0;
}

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

bool normalizedPagePoint(FPDF_PAGE page,
                         const QSizeF &pageSize,
                         const QPointF &pagePoint,
                         QPointF &normalizedPoint)
{
    const QSize deviceSize = mappingDeviceSize(pageSize);
    QPointF devicePoint;
    if (deviceSize.isEmpty()
        || !pagePointToDevice(page, deviceSize, pagePoint, devicePoint)) {
        return false;
    }

    normalizedPoint = QPointF(devicePoint.x() / deviceSize.width(),
                              devicePoint.y() / deviceSize.height());
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

int fontStretchForTransform(FPDF_PAGE page,
                            const QSizeF &pageSize,
                            const FS_MATRIX &transform)
{
    const QSize deviceSize = mappingDeviceSize(pageSize);
    QPointF origin;
    QPointF baselinePoint;
    QPointF verticalPoint;
    if (deviceSize.isEmpty()
        || !transformedPointToDevice(
            page, deviceSize, transform, QPointF(0.0, 0.0), origin)
        || !transformedPointToDevice(
            page, deviceSize, transform, QPointF(1.0, 0.0), baselinePoint)
        || !transformedPointToDevice(
            page, deviceSize, transform, QPointF(0.0, 1.0), verticalPoint)) {
        return QFont::Unstretched;
    }

    const QPointF baseline = baselinePoint - origin;
    const QPointF vertical = verticalPoint - origin;
    const qreal baselineLength = std::hypot(baseline.x(), baseline.y());
    const qreal verticalLength = std::hypot(vertical.x(), vertical.y());
    if (baselineLength <= 0.0 || verticalLength <= 0.0) {
        return QFont::Unstretched;
    }

    return qBound(static_cast<int>(QFont::UltraCondensed),
                  qRound(baselineLength / verticalLength * QFont::Unstretched),
                  static_cast<int>(QFont::UltraExpanded));
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

QString pdfFontName(FPDF_FONT font, bool baseName)
{
    if (!font) {
        return {};
    }

    const size_t length = baseName
        ? FPDFFont_GetBaseFontName(font, nullptr, 0)
        : FPDFFont_GetFamilyName(font, nullptr, 0);
    if (length == 0) {
        return {};
    }

    QByteArray buffer(static_cast<qsizetype>(length), '\0');
    const size_t actualLength = baseName
        ? FPDFFont_GetBaseFontName(font, buffer.data(), length)
        : FPDFFont_GetFamilyName(font, buffer.data(), length);
    if (actualLength == 0) {
        return {};
    }

    return QString::fromUtf8(buffer.constData()).trimmed();
}

QString normalizedPdfFontIdentity(FPDF_FONT font)
{
    const QString baseName = pdfFontName(font, true);
    return baseName.size() > 7 && baseName.at(6) == QLatin1Char('+')
        ? baseName.sliced(7)
        : baseName;
}

QString fontFamilyMatchKey(QString name)
{
    if (name.size() > 7 && name.at(6) == QLatin1Char('+')) {
        name = name.sliced(7);
    }

    const qsizetype styleSeparator = name.indexOf(QLatin1Char('-'));
    if (styleSeparator > 0) {
        name.truncate(styleSeparator);
    }

    if (name.endsWith(QStringLiteral("PSMT"), Qt::CaseInsensitive)) {
        name.chop(4);
    } else if (name.endsWith(QStringLiteral("MT"), Qt::CaseInsensitive)
               || name.endsWith(QStringLiteral("PS"), Qt::CaseInsensitive)) {
        name.chop(2);
    }

    QString key;
    key.reserve(name.size());
    for (const QChar character : name) {
        if (character.isLetterOrNumber()) {
            key.append(character.toCaseFolded());
        }
    }
    return key;
}

bool pdfFontIsBold(FPDF_FONT font)
{
    if (!font) {
        return false;
    }

    const int weight = FPDFFont_GetWeight(font);
    if (weight >= static_cast<int>(QFont::Bold)) {
        return true;
    }

    const int flags = FPDFFont_GetFlags(font);
    if (flags >= 0 && (flags & kPdfFontFlagForceBold) != 0) {
        return true;
    }

    const QString baseName = pdfFontName(font, true);
    return baseName.contains(QStringLiteral("bold"), Qt::CaseInsensitive)
        || baseName.contains(QStringLiteral("black"), Qt::CaseInsensitive)
        || baseName.contains(QStringLiteral("heavy"), Qt::CaseInsensitive);
}

bool pdfFontIsItalic(FPDF_FONT font)
{
    if (!font) {
        return false;
    }

    int italicAngle = 0;
    if (FPDFFont_GetItalicAngle(font, &italicAngle) && italicAngle != 0) {
        return true;
    }

    const int flags = FPDFFont_GetFlags(font);
    if (flags >= 0 && (flags & kPdfFontFlagItalic) != 0) {
        return true;
    }

    const QString baseName = pdfFontName(font, true);
    return baseName.contains(QStringLiteral("italic"), Qt::CaseInsensitive)
        || baseName.contains(QStringLiteral("oblique"), Qt::CaseInsensitive)
        || baseName.contains(QStringLiteral("slanted"), Qt::CaseInsensitive);
}

QString matchingFontFamily(FPDF_FONT font, const QStringList &availableFamilies)
{
    QStringList pdfNames;
    const auto appendUniqueName = [&pdfNames](const QString &name) {
        if (!name.isEmpty() && !pdfNames.contains(name, Qt::CaseInsensitive)) {
            pdfNames.append(name);
        }
    };
    appendUniqueName(pdfFontName(font, false));
    appendUniqueName(normalizedPdfFontIdentity(font));

    for (const QString &pdfName : std::as_const(pdfNames)) {
        for (const QString &availableFamily : availableFamilies) {
            if (pdfName.compare(availableFamily, Qt::CaseInsensitive) == 0) {
                return availableFamily;
            }
        }
    }

    QSet<QString> pdfNameKeys;
    for (const QString &pdfName : std::as_const(pdfNames)) {
        const QString key = fontFamilyMatchKey(pdfName);
        if (!key.isEmpty()) {
            pdfNameKeys.insert(key);
        }
    }
    for (const QString &availableFamily : availableFamilies) {
        if (pdfNameKeys.contains(fontFamilyMatchKey(availableFamily))) {
            return availableFamily;
        }
    }
    return {};
}

bool fontSupportsText(const QString &family, const QString &text)
{
    if (family.isEmpty()) {
        return false;
    }

    QFont font(family);
    font.setStyleStrategy(QFont::NoFontMerging);
    const QFontMetricsF metrics(font);
    for (uint character : text.toUcs4()) {
        if (!QChar::isSpace(static_cast<char32_t>(character))
            && !metrics.inFontUcs4(character)) {
            return false;
        }
    }
    return true;
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

PDFEditController::~PDFEditController()
{
    clearRegisteredFonts();
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

QStringList PDFEditController::bundledFontFamilies() const
{
    return ApplicationFonts::bundledFontFamilies();
}

QStringList PDFEditController::availableFontFamilies() const
{
    return ApplicationFonts::availableFontFamilies();
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

QVariantList PDFEditController::selectedSourceRects() const
{
    return m_hasSelection ? candidateSourceRects(m_selectedCandidate) : QVariantList();
}

bool PDFEditController::selectedHasBaseline() const
{
    return m_hasSelection && m_selectedCandidate.hasBaseline;
}

qreal PDFEditController::selectedOriginX() const
{
    return selectedHasBaseline() ? m_selectedCandidate.origin.x() : selectedX();
}

qreal PDFEditController::selectedBaselineY() const
{
    return selectedHasBaseline() ? m_selectedCandidate.origin.y() : selectedY();
}

QString PDFEditController::selectedText() const
{
    return m_hasSelection ? m_selectedText : QString();
}

QString PDFEditController::selectedFontFamily() const
{
    return m_hasSelection ? selectedCandidateForDisplay().fontFamily : QString();
}

qreal PDFEditController::selectedFontSizeRatio() const
{
    return m_hasSelection ? m_selectedCandidate.fontSizeRatio : 0.0;
}

qreal PDFEditController::selectedDisplayFontSizeRatio() const
{
    if (!m_hasSelection) {
        return 0.0;
    }
    return m_selectedCandidate.displayFontSizeRatio > 0.0
        ? m_selectedCandidate.displayFontSizeRatio
        : m_selectedCandidate.fontSizeRatio;
}

int PDFEditController::selectedFontStretch() const
{
    return m_hasSelection ? m_selectedCandidate.fontStretch : QFont::Unstretched;
}

bool PDFEditController::selectedFontBold() const
{
    return m_hasSelection && selectedCandidateForDisplay().fontBold;
}

bool PDFEditController::selectedFontItalic() const
{
    return m_hasSelection && selectedCandidateForDisplay().fontItalic;
}

bool PDFEditController::selectedFontUnderline() const
{
    return m_hasSelection && selectedCandidateForDisplay().fontUnderline;
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
            && candidate.renderSuppressible) {
            if (!candidate.parts.isEmpty()) {
                for (const CandidatePart &part : candidate.parts) {
                    if (!part.objectPath.isEmpty()) {
                        objectPaths.append(part.objectPath);
                    }
                }
            } else if (!candidate.objectPath.isEmpty()) {
                objectPaths.append(candidate.objectPath);
            }
        }
    }

    std::sort(objectPaths.begin(), objectPaths.end(), [](const QVector<int> &left,
                                                         const QVector<int> &right) {
        return std::lexicographical_compare(
            left.cbegin(), left.cend(), right.cbegin(), right.cend());
    });
    objectPaths.erase(std::unique(objectPaths.begin(), objectPaths.end()), objectPaths.end());
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

            QVariantMap map = candidateMap(draft->candidate, draft->text);
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
        QVariantMap map = candidateMap(draft->candidate, draft->text);
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

QFont PDFEditController::fontForDisplay(const QString &fontFamily,
                                        int fontStretch,
                                        bool fontBold,
                                        bool fontItalic,
                                        bool fontUnderline,
                                        qreal pixelSize) const
{
    QFont font;
    if (!fontFamily.isEmpty()) {
        font.setFamily(fontFamily);
    }
    font.setStretch(qBound(static_cast<int>(QFont::UltraCondensed),
                           fontStretch,
                           static_cast<int>(QFont::UltraExpanded)));
    font.setBold(fontBold);
    font.setItalic(fontItalic);
    font.setUnderline(fontUnderline);
    font.setPixelSize(qMax(1, qRound(pixelSize)));
    return font;
}

QString PDFEditController::resolvedFontFamily(FPDF_PAGEOBJECT textObject,
                                              const QString &text)
{
    FPDF_FONT font = FPDFTextObj_GetFont(textObject);
    if (!font) {
        return {};
    }

    const QString expectedFamily = pdfFontName(font, false);
    const QString baseName = pdfFontName(font, true);
    const QString cacheKey = (baseName.isEmpty() ? expectedFamily : baseName)
        + QLatin1Char('#')
        + QString::number(
            static_cast<qulonglong>(reinterpret_cast<quintptr>(font)),
            16);
    const QString bundledFamily = matchingFontFamily(
        font,
        ApplicationFonts::bundledFontFamilies());
    const QString systemFamily = matchingFontFamily(
        font,
        ApplicationFonts::systemFontFamilies());
    QString embeddedFamily;
    const auto cachedEmbeddedFamily = m_pdfEmbeddedFontFamilyCache.constFind(cacheKey);
    if (cachedEmbeddedFamily != m_pdfEmbeddedFontFamilyCache.cend()) {
        embeddedFamily = cachedEmbeddedFamily.value();
    } else {
        embeddedFamily = registerEmbeddedFont(font, expectedFamily);
        m_pdfEmbeddedFontFamilyCache.insert(cacheKey, embeddedFamily);
    }

    if (fontSupportsText(embeddedFamily, text)) {
        return embeddedFamily;
    }
    if (fontSupportsText(bundledFamily, text)) {
        return bundledFamily;
    }

    return fontSupportsText(systemFamily, text) ? systemFamily : QString();
}

QString PDFEditController::registerEmbeddedFont(FPDF_FONT font,
                                                const QString &expectedFamily)
{
    if (!font || FPDFFont_GetIsEmbedded(font) != 1) {
        return {};
    }

    size_t fontByteCount = 0;
    if (!FPDFFont_GetFontData(font, nullptr, 0, &fontByteCount)
        || fontByteCount == 0
        || fontByteCount > static_cast<size_t>(kMaximumEmbeddedFontBytes)) {
        return {};
    }

    QByteArray fontData(static_cast<qsizetype>(fontByteCount), '\0');
    size_t actualByteCount = fontByteCount;
    if (!FPDFFont_GetFontData(
            font,
            reinterpret_cast<uint8_t *>(fontData.data()),
            fontByteCount,
            &actualByteCount)
        || actualByteCount == 0
        || actualByteCount > fontByteCount) {
        return {};
    }
    fontData.resize(static_cast<qsizetype>(actualByteCount));

    const QByteArray fingerprint = QCryptographicHash::hash(
        fontData,
        QCryptographicHash::Sha256);
    const auto registeredFamily = m_registeredFontFamilies.constFind(fingerprint);
    if (registeredFamily != m_registeredFontFamilies.cend()) {
        return registeredFamily.value();
    }
    if (fontData.size() > kMaximumRegisteredFontBytes - m_registeredFontBytes) {
        m_registeredFontFamilies.insert(fingerprint, QString());
        return {};
    }

    const int fontId = QFontDatabase::addApplicationFontFromData(fontData);
    if (fontId < 0) {
        m_registeredFontFamilies.insert(fingerprint, QString());
        return {};
    }

    const QStringList families = QFontDatabase::applicationFontFamilies(fontId);
    if (families.isEmpty()) {
        QFontDatabase::removeApplicationFont(fontId);
        m_registeredFontFamilies.insert(fingerprint, QString());
        return {};
    }

    QString family = families.constFirst();
    for (const QString &candidateFamily : families) {
        if (candidateFamily.compare(expectedFamily, Qt::CaseInsensitive) == 0) {
            family = candidateFamily;
            break;
        }
    }

    m_registeredFontIds.append(fontId);
    m_registeredFontBytes += fontData.size();
    m_registeredFontFamilies.insert(fingerprint, family);
    return family;
}

void PDFEditController::clearRegisteredFonts()
{
    for (auto fontId = m_registeredFontIds.crbegin();
         fontId != m_registeredFontIds.crend();
         ++fontId) {
        QFontDatabase::removeApplicationFont(*fontId);
    }
    m_registeredFontIds.clear();
    m_registeredFontFamilies.clear();
    m_pdfEmbeddedFontFamilyCache.clear();
    m_registeredFontBytes = 0;
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
    const Candidate &beforeCandidate = m_textEditCommand.hadBefore
        ? existingDraft->candidate
        : m_selectedCandidate;
    m_textEditCommand.beforeText = m_textEditCommand.hadBefore
        ? existingDraft->text
        : m_selectedCandidate.text;
    m_textEditCommand.beforeFontFamily = beforeCandidate.fontFamily;
    m_textEditCommand.beforeFontBold = beforeCandidate.fontBold;
    m_textEditCommand.beforeFontItalic = beforeCandidate.fontItalic;
    m_textEditCommand.beforeFontUnderline = beforeCandidate.fontUnderline;
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
    const Candidate draftCandidate = existingDraft == m_drafts.cend()
        ? m_selectedCandidate
        : existingDraft->candidate;
    const bool matchesOriginal = text == m_selectedCandidate.text
        && sameDraftFormatting(draftCandidate, m_selectedCandidate);
    bool changed = false;
    if (matchesOriginal) {
        if (existingDraft != m_drafts.cend()) {
            m_drafts.remove(key);
            changed = true;
        }
    } else if (existingDraft == m_drafts.cend() || existingDraft->text != text) {
        m_drafts.insert(key, Draft{draftCandidate, text});
        changed = true;
    }

    m_selectedText = text;
    if (changed) {
        markDraftStateChanged(m_selectedCandidate.page, false);
        emit historyStateChanged();
    }
    emit selectionChanged();
}

void PDFEditController::setSelectedFontFamily(const QString &fontFamily)
{
    if (!m_hasSelection
        || !m_selectedCandidate.editable
        || fontFamily.trimmed().isEmpty()
        || !QFontDatabase::hasFamily(fontFamily)
        || sameFontFamily(selectedFontFamily(), fontFamily)) {
        return;
    }

    Candidate draftCandidate = selectedCandidateForDisplay();
    const bool finishImmediately = !m_textEditActive;
    if (finishImmediately) {
        beginSelectedTextEdit();
    }
    if (!m_textEditActive || !m_hasSelection) {
        return;
    }

    draftCandidate.fontFamily = fontFamily;
    updateSelectedDraftCandidate(draftCandidate);

    if (finishImmediately) {
        finishSelectedTextEdit();
    }
}

void PDFEditController::setSelectedFontBold(bool enabled)
{
    setSelectedFontStyle(FontStyle::Bold, enabled);
}

void PDFEditController::setSelectedFontItalic(bool enabled)
{
    setSelectedFontStyle(FontStyle::Italic, enabled);
}

void PDFEditController::setSelectedFontUnderline(bool enabled)
{
    setSelectedFontStyle(FontStyle::Underline, enabled);
}

void PDFEditController::setSelectedFontStyle(FontStyle style, bool enabled)
{
    if (!m_hasSelection || !m_selectedCandidate.editable) {
        return;
    }

    Candidate draftCandidate = selectedCandidateForDisplay();
    bool currentValue = false;
    switch (style) {
    case FontStyle::Bold:
        currentValue = draftCandidate.fontBold;
        break;
    case FontStyle::Italic:
        currentValue = draftCandidate.fontItalic;
        break;
    case FontStyle::Underline:
        currentValue = draftCandidate.fontUnderline;
        break;
    }
    if (currentValue == enabled) {
        return;
    }

    const bool finishImmediately = !m_textEditActive;
    if (finishImmediately) {
        beginSelectedTextEdit();
    }
    if (!m_textEditActive || !m_hasSelection) {
        return;
    }

    switch (style) {
    case FontStyle::Bold:
        draftCandidate.fontBold = enabled;
        break;
    case FontStyle::Italic:
        draftCandidate.fontItalic = enabled;
        break;
    case FontStyle::Underline:
        draftCandidate.fontUnderline = enabled;
        break;
    }
    updateSelectedDraftCandidate(draftCandidate);

    if (finishImmediately) {
        finishSelectedTextEdit();
    }
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
    const Candidate &afterCandidate = command.hadAfter
        ? currentDraft->candidate
        : m_selectedCandidate;
    command.afterFontFamily = afterCandidate.fontFamily;
    command.afterFontBold = afterCandidate.fontBold;
    command.afterFontItalic = afterCandidate.fontItalic;
    command.afterFontUnderline = afterCandidate.fontUnderline;
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
        || command.beforeText != command.afterText
        || !sameFontFamily(command.beforeFontFamily, command.afterFontFamily)
        || command.beforeFontBold != command.afterFontBold
        || command.beforeFontItalic != command.afterFontItalic
        || command.beforeFontUnderline != command.afterFontUnderline;
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
    const Candidate &beforeCandidate = hadBefore
        ? existingDraft->candidate
        : m_selectedCandidate;
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
    command.beforeFontFamily = beforeCandidate.fontFamily;
    command.beforeFontBold = beforeCandidate.fontBold;
    command.beforeFontItalic = beforeCandidate.fontItalic;
    command.beforeFontUnderline = beforeCandidate.fontUnderline;
    command.hadAfter = text != m_selectedCandidate.text
        || !sameDraftFormatting(beforeCandidate, m_selectedCandidate);
    command.afterText = text;
    command.afterFontFamily = beforeCandidate.fontFamily;
    command.afterFontBold = beforeCandidate.fontBold;
    command.afterFontItalic = beforeCandidate.fontItalic;
    command.afterFontUnderline = beforeCandidate.fontUnderline;

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
    clearRegisteredFonts();
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

QVector<PDFEditController::Candidate> PDFEditController::scanPage(int pageIndex)
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

    QHash<FPDF_PAGEOBJECT, TextObjectLayout> textObjectLayouts;
    if (textPage) {
        const int characterCount = FPDFText_CountChars(textPage);
        for (int index = 0; index < characterCount; ++index) {
            FPDF_PAGEOBJECT textObject = FPDFText_GetTextObject(textPage, index);
            if (!textObject) {
                continue;
            }

            auto objectLayout = textObjectLayouts.find(textObject);
            if (objectLayout == textObjectLayouts.end()) {
                objectLayout = textObjectLayouts.insert(textObject, TextObjectLayout());
            }
            const bool generated = FPDFText_IsGenerated(textPage, index) == 1;
            if (!generated) {
                if (!objectLayout->hasOrigin) {
                    double originX = 0.0;
                    double originY = 0.0;
                    if (FPDFText_GetCharOrigin(textPage, index, &originX, &originY)) {
                        objectLayout->origin = QPointF(originX, originY);
                        objectLayout->hasOrigin = true;
                    }
                }

                const unsigned int unicode = FPDFText_GetUnicode(textPage, index);
                if (unicode > 0
                    && unicode <= 0x10FFFF
                    && !QChar::isSurrogate(static_cast<char32_t>(unicode))) {
                    const char32_t codePoint = static_cast<char32_t>(unicode);
                    objectLayout->text.append(QString::fromUcs4(&codePoint, 1));
                }
            }

            double left = 0.0;
            double right = 0.0;
            double bottom = 0.0;
            double top = 0.0;
            if (!FPDFText_GetCharBox(
                    textPage, index, &left, &right, &bottom, &top)) {
                continue;
            }

            const QRectF characterBounds = QRectF(
                QPointF(left, bottom), QPointF(right, top)).normalized();
            if (characterBounds.isEmpty()) {
                continue;
            }

            if (objectLayout->bounds.isEmpty()) {
                objectLayout->bounds = characterBounds;
            } else {
                objectLayout->bounds = objectLayout->bounds.united(characterBounds);
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
            textObjectLayouts,
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
    candidates = groupTextLineCandidates(std::move(candidates), pageSize);
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate &left, const Candidate &right) {
        return left.editable < right.editable;
    });
    return candidates;
}

void PDFEditController::appendPageObjectCandidates(FPDF_PAGE page,
                                                   FPDF_PAGEOBJECT pageObject,
                                                   const QVector<int> &objectPath,
                                                   FPDF_TEXTPAGE textPage,
                                                   const QHash<FPDF_PAGEOBJECT, TextObjectLayout> &textObjectLayouts,
                                                   const FS_MATRIX &containerTransform,
                                                   const QSizeF &pageSize,
                                                   QVector<Candidate> &candidates)
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
                textObjectLayouts,
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
    const auto textLayout = type == FPDF_PAGEOBJ_TEXT
        ? textObjectLayouts.constFind(pageObject)
        : textObjectLayouts.cend();
    if (textLayout != textObjectLayouts.cend()) {
        if (!textLayout->bounds.isEmpty()) {
            const QRectF glyphRect = normalizedRect(
                page,
                textLayout->bounds.left(),
                textLayout->bounds.top(),
                textLayout->bounds.right(),
                textLayout->bounds.bottom(),
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
    if (textLayout != textObjectLayouts.cend() && textLayout->hasOrigin) {
        candidate.hasBaseline = normalizedPagePoint(
            page,
            pageSize,
            textLayout->origin,
            candidate.origin);
    }

    if (type == FPDF_PAGEOBJ_IMAGE) {
        candidate.kind = QStringLiteral("image");
        //% "Image"
        candidate.label = qtTrId("Image");
        //% "This content is an image. OCR is required to edit its text."
        candidate.reason = qtTrId("ImageTextRequiresOcr");
        candidates.append(candidate);
        return;
    }

    const QString text = textLayout != textObjectLayouts.cend()
            && !textLayout->text.isEmpty()
        ? textLayout->text
        : textObjectText(pageObject, textPage);
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
    FPDF_FONT pdfFont = FPDFTextObj_GetFont(pageObject);
    candidate.fontFamily = resolvedFontFamily(pageObject, candidate.text);
    candidate.fontIdentity = normalizedPdfFontIdentity(pdfFont);
    candidate.fontBold = pdfFontIsBold(pdfFont);
    candidate.fontItalic = pdfFontIsItalic(pdfFont);
    candidate.fontSizeRatio = qMax<qreal>(
        0.0,
        normalizedFontSize(page, pageSize, textTransform, fontSize));
    candidate.displayFontSizeRatio = candidate.fontSizeRatio;
    candidate.fontStretch = fontStretchForTransform(page, pageSize, textTransform);
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

    CandidatePart part;
    part.objectId = candidate.objectId;
    part.objectPath = candidate.objectPath;
    part.rect = candidate.rect;
    part.origin = candidate.origin;
    part.text = candidate.text;
    part.hasBaseline = candidate.hasBaseline;
    candidate.parts.append(part);
    candidates.append(candidate);
}

QVector<PDFEditController::Candidate> PDFEditController::groupTextLineCandidates(
    QVector<Candidate> candidates,
    const QSizeF &pageSize) const
{
    if (candidates.size() < 2 || pageSize.isEmpty()) {
        return candidates;
    }

    struct IndexedCandidate {
        Candidate candidate;
        qsizetype order = -1;
    };
    struct TextLine {
        QVector<IndexedCandidate> candidates;
        qreal baseline = 0.0;
        qreal emSize = 0.0;
    };

    const auto displayFontSizeRatio = [](const Candidate &candidate) {
        return candidate.displayFontSizeRatio > 0.0
            ? candidate.displayFontSizeRatio
            : candidate.fontSizeRatio;
    };
    const auto emSize = [&](const Candidate &candidate) {
        return displayFontSizeRatio(candidate) * pageSize.height();
    };
    const auto canJoinLine = [&](const Candidate &candidate) {
        return candidate.kind == QLatin1String("text")
            && candidate.editable
            && candidate.hasBaseline
            && !candidate.added
            && candidate.parts.size() == 1
            && emSize(candidate) > 0.0;
    };

    QVector<TextLine> lines;
    QVector<IndexedCandidate> output;
    output.reserve(candidates.size());
    for (qsizetype index = 0; index < candidates.size(); ++index) {
        Candidate candidate = std::move(candidates[index]);
        if (!canJoinLine(candidate)) {
            output.append(IndexedCandidate{std::move(candidate), index});
            continue;
        }

        const qreal candidateBaseline = candidate.origin.y() * pageSize.height();
        const qreal candidateEmSize = emSize(candidate);
        TextLine *bestLine = nullptr;
        qreal bestDistance = -1.0;
        for (TextLine &line : lines) {
            const qreal distance = qAbs(candidateBaseline - line.baseline);
            const qreal tolerance = kLineBaselineToleranceEm
                * qMax(candidateEmSize, line.emSize);
            if (distance <= tolerance
                && (bestDistance < 0.0 || distance < bestDistance)) {
                bestLine = &line;
                bestDistance = distance;
            }
        }

        if (!bestLine) {
            TextLine line;
            line.baseline = candidateBaseline;
            line.emSize = candidateEmSize;
            line.candidates.append(IndexedCandidate{std::move(candidate), index});
            lines.append(std::move(line));
            continue;
        }

        const qsizetype existingCount = bestLine->candidates.size();
        bestLine->baseline = (bestLine->baseline * existingCount + candidateBaseline)
            / (existingCount + 1);
        bestLine->emSize = qMax(bestLine->emSize, candidateEmSize);
        bestLine->candidates.append(IndexedCandidate{std::move(candidate), index});
    }

    const auto sameStyle = [&](const Candidate &left, const Candidate &right) {
        const qreal leftSize = emSize(left);
        const qreal rightSize = emSize(right);
        const qreal sizeTolerance = qMax(
            kLineFontSizeTolerancePoints,
            qMax(leftSize, rightSize) * kLineFontSizeToleranceRatio);
        const QString &leftFont = left.fontIdentity.isEmpty()
            ? left.fontFamily
            : left.fontIdentity;
        const QString &rightFont = right.fontIdentity.isEmpty()
            ? right.fontFamily
            : right.fontIdentity;
        return !leftFont.isEmpty()
            && leftFont.compare(rightFont, Qt::CaseInsensitive) == 0
            && qAbs(leftSize - rightSize) <= sizeTolerance
            && qAbs(left.fontStretch - right.fontStretch) <= kLineFontStretchTolerance
            && left.fontBold == right.fontBold
            && left.fontItalic == right.fontItalic
            && left.fontUnderline == right.fontUnderline
            && left.color == right.color;
    };
    const auto horizontalGap = [&](const Candidate &left, const Candidate &right) {
        return (right.rect.left() - left.rect.right()) * pageSize.width();
    };
    const auto areNeighbors = [&](const Candidate &left, const Candidate &right) {
        const qreal gap = horizontalGap(left, right);
        const qreal sharedEmSize = qMax(emSize(left), emSize(right));
        return gap >= kLineMinimumHorizontalGapEm * sharedEmSize
            && gap <= kLineMaximumHorizontalGapEm * sharedEmSize;
    };
    const auto separatorBetween = [&](const Candidate &left, const Candidate &right) {
        if (left.text.isEmpty()
            || right.text.isEmpty()
            || left.text.back().isSpace()
            || right.text.front().isSpace()) {
            return QString();
        }
        const qreal sharedEmSize = qMax(emSize(left), emSize(right));
        return horizontalGap(left, right) > kLineWordSeparatorGapEm * sharedEmSize
            ? QString(QLatin1Char(' '))
            : QString();
    };

    for (TextLine &line : lines) {
        std::stable_sort(
            line.candidates.begin(),
            line.candidates.end(),
            [](const IndexedCandidate &left, const IndexedCandidate &right) {
                if (!qFuzzyCompare(
                        left.candidate.rect.left() + 1.0,
                        right.candidate.rect.left() + 1.0)) {
                    return left.candidate.rect.left() < right.candidate.rect.left();
                }
                return left.order < right.order;
            });

        QVector<IndexedCandidate> run;
        const auto flushRun = [&]() {
            if (run.isEmpty()) {
                return;
            }
            if (run.size() == 1) {
                output.append(std::move(run.front()));
                run.clear();
                return;
            }

            Candidate grouped = run.front().candidate;
            grouped.objectPath.clear();
            grouped.parts.clear();
            grouped.text.clear();
            grouped.rect = run.front().candidate.rect;
            grouped.renderSuppressible = true;
            grouped.limited = false;
            qsizetype firstOrder = run.front().order;
            QByteArray identity;

            for (qsizetype index = 0; index < run.size(); ++index) {
                const Candidate &source = run[index].candidate;
                firstOrder = qMin(firstOrder, run[index].order);
                grouped.rect = grouped.rect.united(source.rect);
                grouped.renderSuppressible = grouped.renderSuppressible
                    && source.renderSuppressible;
                grouped.limited = grouped.limited || source.limited;

                if (index > 0) {
                    const QString separator = separatorBetween(
                        run[index - 1].candidate,
                        source);
                    grouped.parts.last().separatorAfter = separator;
                    grouped.text.append(separator);
                }

                CandidatePart part = source.parts.constFirst();
                part.separatorAfter.clear();
                grouped.parts.append(std::move(part));
                grouped.text.append(source.text);
                identity.append(source.objectId.toUtf8());
                identity.append('\0');
            }

            grouped.objectId = QStringLiteral("line:")
                + QString::fromLatin1(QCryptographicHash::hash(
                    identity,
                    QCryptographicHash::Sha256).toHex());
            output.append(IndexedCandidate{std::move(grouped), firstOrder});
            run.clear();
        };

        for (IndexedCandidate &candidate : line.candidates) {
            if (!run.isEmpty()) {
                const Candidate &previous = run.constLast().candidate;
                if (!sameStyle(previous, candidate.candidate)
                    || !areNeighbors(previous, candidate.candidate)) {
                    flushRun();
                }
            }
            run.append(std::move(candidate));
        }
        flushRun();
    }

    std::stable_sort(
        output.begin(),
        output.end(),
        [](const IndexedCandidate &left, const IndexedCandidate &right) {
            return left.order < right.order;
        });
    QVector<Candidate> groupedCandidates;
    groupedCandidates.reserve(output.size());
    for (IndexedCandidate &candidate : output) {
        groupedCandidates.append(std::move(candidate.candidate));
    }
    return groupedCandidates;
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

QVariantList PDFEditController::candidateSourceRects(const Candidate &candidate) const
{
    QVariantList sourceRects;
    const auto appendRect = [&sourceRects](const QRectF &rect) {
        QVariantMap map;
        map.insert(QStringLiteral("x"), rect.x());
        map.insert(QStringLiteral("y"), rect.y());
        map.insert(QStringLiteral("width"), rect.width());
        map.insert(QStringLiteral("height"), rect.height());
        sourceRects.append(map);
    };

    if (candidate.parts.isEmpty()) {
        appendRect(candidate.rect);
    } else {
        sourceRects.reserve(candidate.parts.size());
        for (const CandidatePart &part : candidate.parts) {
            appendRect(part.rect);
        }
    }
    return sourceRects;
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
    map.insert(QStringLiteral("sourceRects"), candidateSourceRects(candidate));
    map.insert(QStringLiteral("grouped"), candidate.parts.size() > 1);
    map.insert(QStringLiteral("hasBaseline"), candidate.hasBaseline);
    map.insert(QStringLiteral("originX"), candidate.origin.x());
    map.insert(QStringLiteral("baselineY"), candidate.origin.y());
    map.insert(QStringLiteral("text"), text);
    map.insert(QStringLiteral("fontFamily"), candidate.fontFamily);
    map.insert(QStringLiteral("fontSizeRatio"), candidate.fontSizeRatio);
    map.insert(
        QStringLiteral("displayFontSizeRatio"),
        candidate.displayFontSizeRatio > 0.0
            ? candidate.displayFontSizeRatio
            : candidate.fontSizeRatio);
    map.insert(QStringLiteral("fontStretch"), candidate.fontStretch);
    map.insert(QStringLiteral("fontBold"), candidate.fontBold);
    map.insert(QStringLiteral("fontItalic"), candidate.fontItalic);
    map.insert(QStringLiteral("fontUnderline"), candidate.fontUnderline);
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

bool PDFEditController::sameDraftFormatting(const Candidate &left,
                                            const Candidate &right)
{
    return sameFontFamily(left.fontFamily, right.fontFamily)
        && left.fontBold == right.fontBold
        && left.fontItalic == right.fontItalic
        && left.fontUnderline == right.fontUnderline;
}

const PDFEditController::Candidate &PDFEditController::selectedCandidateForDisplay() const
{
    if (!m_hasSelection) {
        return m_selectedCandidate;
    }

    const auto draft = m_drafts.constFind(
        draftKey(m_selectedCandidate.page, m_selectedCandidate.objectId));
    return draft == m_drafts.cend() ? m_selectedCandidate : draft->candidate;
}

void PDFEditController::updateSelectedDraftCandidate(const Candidate &candidate)
{
    const QString key = draftKey(m_selectedCandidate.page, m_selectedCandidate.objectId);
    const auto existingDraft = m_drafts.constFind(key);
    const QString draftText = existingDraft == m_drafts.cend()
        ? m_selectedText
        : existingDraft->text;
    const bool matchesOriginal = draftText == m_selectedCandidate.text
        && sameDraftFormatting(candidate, m_selectedCandidate);
    bool changed = false;
    if (matchesOriginal) {
        changed = m_drafts.remove(key) > 0;
    } else if (existingDraft == m_drafts.cend()
               || !sameDraftFormatting(existingDraft->candidate, candidate)) {
        m_drafts.insert(key, Draft{candidate, draftText});
        changed = true;
    }
    if (!changed) {
        return;
    }

    markDraftStateChanged(m_selectedCandidate.page, false);
    emit historyStateChanged();
    emit selectionChanged();
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
    const QString fontFamily = useAfterState
        ? command.afterFontFamily
        : command.beforeFontFamily;
    const bool fontBold = useAfterState
        ? command.afterFontBold
        : command.beforeFontBold;
    const bool fontItalic = useAfterState
        ? command.afterFontItalic
        : command.beforeFontItalic;
    const bool fontUnderline = useAfterState
        ? command.afterFontUnderline
        : command.beforeFontUnderline;
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
    candidate.fontFamily = fontFamily;
    candidate.fontBold = fontBold;
    candidate.fontItalic = fontItalic;
    candidate.fontUnderline = fontUnderline;

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
        ? existingDraft == m_drafts.cend()
            || existingDraft->text != text
            || !sameDraftFormatting(existingDraft->candidate, candidate)
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
