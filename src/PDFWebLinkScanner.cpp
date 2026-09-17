#include "PDFWebLinkScanner.h"

#include "PDFiumSynchronization.h"

#include <QFile>
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QSet>
#include <QUrl>

#include <fpdf_annot.h>
#include <fpdf_doc.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace PDFWebLinks {
namespace {

constexpr int kMaximumPages = 10000;
constexpr int kMaximumPageCharacters = 50000;
constexpr int kMaximumTotalCharacters = 5000000;
constexpr int kMaximumCandidatesPerPage = 2048;
constexpr int kMaximumCandidates = 20000;
constexpr int kMaximumTotalUrlCharacters = 2000000;
constexpr int kMaximumOverlapComparisons = 2000000;
constexpr int kMaximumExistingQuadsPerLink = 64;
constexpr int kMappingExtent = 10000;
constexpr int kMaximumSourceAnnotations = 200000;
constexpr int kAnnotationFlagLockedContents = 1 << 9;

bool cancelled(const std::shared_ptr<std::atomic_bool> &cancellation)
{
    return cancellation && cancellation->load(std::memory_order_relaxed);
}

// This owner outlives individual page transactions, so its destructor owns the
// lock. All other handles are destroyed inside their page's existing lock.
class ScanDocument final
{
public:
    ScanDocument(const QString &filePath,
                 const std::shared_ptr<std::atomic_bool> &cancellation)
    {
        if (cancelled(cancellation)) {
            return;
        }
        PDFiumProcessLock lock;
        if (!cancelled(cancellation)) {
            const QByteArray path = QFile::encodeName(filePath);
            m_document = FPDF_LoadDocument(path.constData(), nullptr);
        }
    }

    ~ScanDocument()
    {
        if (m_document) {
            PDFiumProcessLock lock;
            FPDF_CloseDocument(m_document);
        }
    }

    FPDF_DOCUMENT get() const { return m_document; }
    Q_DISABLE_COPY_MOVE(ScanDocument)

private:
    FPDF_DOCUMENT m_document = nullptr;
};

template<typename Handle, void (*Close)(Handle)>
class ScopedHandle final
{
public:
    explicit ScopedHandle(Handle handle) : m_handle(handle) {}
    ~ScopedHandle()
    {
        assertPDFiumProcessLockHeld();
        if (m_handle) {
            Close(m_handle);
        }
    }
    Handle get() const { return m_handle; }
    Q_DISABLE_COPY_MOVE(ScopedHandle)

private:
    Handle m_handle;
};

using Page = ScopedHandle<FPDF_PAGE, FPDF_ClosePage>;
using TextPage = ScopedHandle<FPDF_TEXTPAGE, FPDFText_ClosePage>;
using WebLinks = ScopedHandle<FPDF_PAGELINK, FPDFLink_CloseWebLinks>;
using Annotation = ScopedHandle<FPDF_ANNOTATION, FPDFPage_CloseAnnot>;

class PageMapper final
{
public:
    explicit PageMapper(FPDF_PAGE page)
    {
        // Inverting DeviceToPage retains subpixel accuracy, with PDFium itself
        // resolving visible boxes and intrinsic 90-degree page rotations.
        std::array<QPointF, 3> points;
        const std::array<QPoint, 3> device{
            QPoint(0, 0), QPoint(kMappingExtent, 0),
            QPoint(0, kMappingExtent)};
        for (size_t i = 0; i < points.size(); ++i) {
            double x = 0.0;
            double y = 0.0;
            if (!FPDF_DeviceToPage(page, 0, 0, kMappingExtent,
                                   kMappingExtent, 0, device[i].x(),
                                   device[i].y(), &x, &y)
                || !std::isfinite(x) || !std::isfinite(y)) {
                return;
            }
            points[i] = QPointF(x, y);
        }
        m_origin = points[0];
        m_horizontal = points[1] - m_origin;
        m_vertical = points[2] - m_origin;
        m_determinant = m_horizontal.x() * m_vertical.y()
            - m_horizontal.y() * m_vertical.x();
        m_valid = std::isfinite(m_determinant)
            && std::abs(m_determinant) > 1e-12;
    }

    bool valid() const { return m_valid; }

    bool rectangle(double left, double top, double right, double bottom,
                   QRectF &rect) const
    {
        if (!m_valid || !std::isfinite(left) || !std::isfinite(top)
            || !std::isfinite(right) || !std::isfinite(bottom)
            || left == right || top == bottom) {
            return false;
        }
        const std::array<QPointF, 4> points{
            QPointF(left, top), QPointF(left, bottom),
            QPointF(right, top), QPointF(right, bottom)};
        qreal minimumX = 0.0;
        qreal maximumX = 0.0;
        qreal minimumY = 0.0;
        qreal maximumY = 0.0;
        for (size_t i = 0; i < points.size(); ++i) {
            const QPointF offset = points[i] - m_origin;
            const qreal x = (offset.x() * m_vertical.y()
                             - offset.y() * m_vertical.x()) / m_determinant;
            const qreal y = (m_horizontal.x() * offset.y()
                             - m_horizontal.y() * offset.x()) / m_determinant;
            if (!std::isfinite(x) || !std::isfinite(y)) {
                return false;
            }
            minimumX = i == 0 ? x : qMin(minimumX, x);
            maximumX = i == 0 ? x : qMax(maximumX, x);
            minimumY = i == 0 ? y : qMin(minimumY, y);
            maximumY = i == 0 ? y : qMax(maximumY, y);
        }
        // Clip before subtracting to avoid overflow for extreme off-page boxes.
        minimumX = qBound(0.0, minimumX, 1.0);
        maximumX = qBound(0.0, maximumX, 1.0);
        minimumY = qBound(0.0, minimumY, 1.0);
        maximumY = qBound(0.0, maximumY, 1.0);
        rect = QRectF(minimumX, minimumY, maximumX - minimumX,
                      maximumY - minimumY);
        return true;
    }

private:
    QPointF m_origin;
    QPointF m_horizontal;
    QPointF m_vertical;
    qreal m_determinant = 0.0;
    bool m_valid = false;
};

bool existingLinkRectangles(FPDF_PAGE page, const PageMapper &mapper,
                            QVector<QRectF> &rects,
                            const std::shared_ptr<std::atomic_bool> &cancellation)
{
    const int count = FPDFPage_GetAnnotCount(page);
    if (count < 0 || count > MaximumAnnotationsPerPage) {
        return false;
    }
    for (int index = 0; index < count; ++index) {
        if (cancelled(cancellation)) {
            return false;
        }
        const Annotation annotation(FPDFPage_GetAnnot(page, index));
        if (!annotation.get()) {
            return false;
        }
        if (FPDFAnnot_GetSubtype(annotation.get()) != FPDF_ANNOT_LINK) {
            continue;
        }
        FS_RECTF sourceRect{};
        QRectF rect;
        if (!FPDFAnnot_GetRect(annotation.get(), &sourceRect)
            || !mapper.rectangle(sourceRect.left, sourceRect.top,
                                  sourceRect.right, sourceRect.bottom, rect)) {
            return false;
        }
        if (!rect.isEmpty()) {
            rects.append(rect);
        }
        const size_t quadCount = FPDFAnnot_CountAttachmentPoints(annotation.get());
        if (quadCount > kMaximumExistingQuadsPerLink
            || rects.size() + static_cast<qsizetype>(quadCount)
                > MaximumExistingRectanglesPerPage) {
            return false;
        }
        for (size_t quadIndex = 0; quadIndex < quadCount; ++quadIndex) {
            FS_QUADPOINTSF quad{};
            if (!FPDFAnnot_GetAttachmentPoints(annotation.get(), quadIndex, &quad)) {
                return false;
            }
            const std::array<float, 8> values{
                quad.x1, quad.y1, quad.x2, quad.y2,
                quad.x3, quad.y3, quad.x4, quad.y4};
            if (!std::all_of(values.begin(), values.end(),
                             [](float value) { return std::isfinite(value); })
                || !mapper.rectangle(
                    std::min({quad.x1, quad.x2, quad.x3, quad.x4}),
                    std::max({quad.y1, quad.y2, quad.y3, quad.y4}),
                    std::max({quad.x1, quad.x2, quad.x3, quad.x4}),
                    std::min({quad.y1, quad.y2, quad.y3, quad.y4}), rect)) {
                return false;
            }
            if (!rect.isEmpty()) {
                rects.append(rect);
            }
        }
    }
    return true;
}

} // namespace

QString validWebUrl(const QString &value)
{
    if (value.isEmpty() || value.size() > MaximumUrlLength) {
        return {};
    }
    for (const QChar character : value) {
        if (character.unicode() <= 0x20 || character.unicode() == 0x7f
            || character.isSpace() || character.category() == QChar::Other_Control) {
            return {};
        }
    }
    const QUrl url(value, QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || url.isEmpty() || url.isRelative() || url.host().isEmpty()
        || (scheme != QStringLiteral("http") && scheme != QStringLiteral("https"))
        || url.authority(QUrl::FullyEncoded).contains(QLatin1Char('@'))) {
        return {};
    }
    const QByteArray encoded = url.toEncoded(QUrl::FullyEncoded);
    return encoded.size() <= MaximumUrlLength ? QString::fromLatin1(encoded) : QString();
}

SourceScanResult sourceLinksForPage(
    FPDF_DOCUMENT document, FPDF_PAGE page, int sourcePage,
    const std::shared_ptr<std::atomic_bool> &cancellation)
{
    assertPDFiumProcessLockHeld();
    SourceScanResult result;
    const int count = FPDFPage_GetAnnotCount(page);
    if (!document || !page || count < 0) {
        result.failed = true;
        return result;
    }
    if (count > MaximumAnnotationsPerPage) {
        result.limited = true;
        return result;
    }

    // A link can be the target of a relation even without having that key in
    // its own dictionary. Never leave a popup or reply with a removed parent.
    QSet<int> relatedAnnotations;
    constexpr std::array<const char *, 3> relationshipKeys{"Popup", "IRT", "Parent"};
    for (int index = 0; index < count; ++index) {
        if (cancelled(cancellation)) {
            return {};
        }
        const Annotation annotation(FPDFPage_GetAnnot(page, index));
        if (!annotation.get()) {
            result.failed = true;
            return result;
        }
        for (const char *key : relationshipKeys) {
            if (!FPDFAnnot_HasKey(annotation.get(), key)) {
                continue;
            }
            relatedAnnotations.insert(index);
            const Annotation target(FPDFAnnot_GetLinkedAnnot(annotation.get(), key));
            const int targetIndex = target.get()
                ? FPDFPage_GetAnnotIndex(page, target.get()) : -1;
            if (targetIndex < 0 || targetIndex >= count) {
                // An unresolvable relation cannot prove which annotations are
                // independent. Leave this page intact instead of guessing.
                result.limited = true;
                return result;
            }
            relatedAnnotations.insert(targetIndex);
        }
    }

    for (int index = 0; index < count; ++index) {
        if (cancelled(cancellation)) {
            return {};
        }
        const Annotation annotation(FPDFPage_GetAnnot(page, index));
        if (!annotation.get()) {
            result.links.clear();
            result.failed = true;
            return result;
        }
        if (FPDFAnnot_GetSubtype(annotation.get()) != FPDF_ANNOT_LINK) {
            continue;
        }
        const FPDF_LINK link = FPDFAnnot_GetLink(annotation.get());
        const FPDF_ACTION action = link ? FPDFLink_GetAction(link) : nullptr;
        if (!action || FPDFAction_GetType(action) != PDFACTION_URI) {
            continue;
        }
        const unsigned long uriSize = FPDFAction_GetURIPath(document, action, nullptr, 0);
        if (uriSize < 2 || uriSize > MaximumUrlLength + 1) {
            result.limited = true;
            continue;
        }
        QByteArray uri(static_cast<qsizetype>(uriSize), '\0');
        if (FPDFAction_GetURIPath(document, action, uri.data(), uriSize) != uriSize
            || uri.back() != '\0') {
            result.limited = true;
            continue;
        }
        uri.chop(1);
        const QString decodedUri = QString::fromUtf8(uri);
        const QUrl url(decodedUri, QUrl::StrictMode);
        const QString scheme = url.scheme().toLower();
        if (scheme != QLatin1String("http") && scheme != QLatin1String("https")) {
            continue;
        }
        if (decodedUri.toUtf8() != uri || uri.contains('\0')
            || !url.isValid() || url.isRelative() || url.host().isEmpty()
            || std::any_of(uri.cbegin(), uri.cend(), [](char value) {
                   const auto byte = static_cast<unsigned char>(value);
                   return byte <= 0x20 || byte == 0x7f;
               })) {
            result.limited = true;
            continue;
        }
        const int flags = FPDFAnnot_GetFlags(annotation.get());
        const bool malformedFlags = FPDFAnnot_HasKey(annotation.get(), "F")
            && FPDFAnnot_GetValueType(annotation.get(), "F") != FPDF_OBJECT_NUMBER;
        if (flags < 0 || malformedFlags
            || (flags & (FPDF_ANNOT_FLAG_READONLY | FPDF_ANNOT_FLAG_LOCKED
                         | kAnnotationFlagLockedContents)) != 0
            || relatedAnnotations.contains(index)
            || FPDFAnnot_HasKey(annotation.get(), "Dest")
            || FPDFAnnot_HasKey(annotation.get(), "AA")
            || FPDFAnnot_HasKey(annotation.get(), "RT")
            || FPDFAnnot_HasKey(annotation.get(), "StructParent")
            || FPDFAnnot_HasKey(annotation.get(), "State")
            || FPDFAnnot_HasKey(annotation.get(), "StateModel")) {
            result.limited = true;
            continue;
        }

        FS_RECTF rect{};
        if (!FPDFAnnot_GetRect(annotation.get(), &rect)
            || !std::isfinite(rect.left) || !std::isfinite(rect.right)
            || !std::isfinite(rect.top) || !std::isfinite(rect.bottom)
            || rect.left >= rect.right || rect.bottom >= rect.top) {
            result.limited = true;
            continue;
        }
        const int quadCount = FPDFLink_CountQuadPoints(link);
        if (quadCount < 0 || quadCount > kMaximumExistingQuadsPerLink
            || (FPDFAnnot_HasKey(annotation.get(), "QuadPoints")
                && (FPDFAnnot_GetValueType(annotation.get(), "QuadPoints")
                        != FPDF_OBJECT_ARRAY || quadCount == 0))) {
            result.limited = true;
            continue;
        }
        QByteArray identity;
        QDataStream stream(&identity, QIODevice::WriteOnly);
        stream.setVersion(QDataStream::Qt_6_0);
        stream << uri << qint32(flags) << rect.left << rect.bottom
               << rect.right << rect.top << qint32(quadCount);
        bool valid = true;
        for (int quadIndex = 0; quadIndex < quadCount; ++quadIndex) {
            FS_QUADPOINTSF quad{};
            if (!FPDFLink_GetQuadPoints(link, quadIndex, &quad)) {
                valid = false;
                break;
            }
            const std::array<float, 8> coordinates{
                quad.x1, quad.y1, quad.x2, quad.y2,
                quad.x3, quad.y3, quad.x4, quad.y4};
            for (float value : coordinates) {
                if (!std::isfinite(value)) {
                    valid = false;
                    break;
                }
                stream << value;
            }
        }
        if (!valid || stream.status() != QDataStream::Ok) {
            result.limited = true;
            continue;
        }
        result.links.append(SourceLink{
            sourcePage, index, QCryptographicHash::hash(identity, QCryptographicHash::Sha256)});
    }
    return result;
}

SourceScanResult scanSourceLinks(
    const QString &filePath,
    const std::shared_ptr<std::atomic_bool> &cancellation)
{
    assertPDFiumProcessLockNotHeld();
    SourceScanResult result;
    if (cancelled(cancellation)) {
        return result;
    }
    const ScanDocument document(filePath, cancellation);
    if (!document.get()) {
        result.failed = !cancelled(cancellation);
        return result;
    }
    int pageCount = 0;
    {
        PDFiumProcessLock lock;
        if (cancelled(cancellation)) {
            return {};
        }
        pageCount = FPDF_GetPageCount(document.get());
    }
    if (pageCount <= 0) {
        result.failed = true;
        return result;
    }
    result.limited = pageCount > kMaximumPages;
    int totalAnnotations = 0;
    for (int pageIndex = 0; pageIndex < qMin(pageCount, kMaximumPages); ++pageIndex) {
        if (cancelled(cancellation)) {
            return {};
        }
        PDFiumProcessLock lock;
        if (cancelled(cancellation)) {
            return {};
        }
        const Page page(FPDF_LoadPage(document.get(), pageIndex));
        if (!page.get()) {
            result.links.clear();
            result.failed = true;
            return result;
        }
        const int count = FPDFPage_GetAnnotCount(page.get());
        if (count < 0) {
            result.links.clear();
            result.failed = true;
            return result;
        }
        if (count > kMaximumSourceAnnotations - totalAnnotations) {
            result.limited = true;
            break;
        }
        totalAnnotations += count;
        SourceScanResult pageResult = sourceLinksForPage(
            document.get(), page.get(), pageIndex, cancellation);
        if (cancelled(cancellation)) {
            return {};
        }
        if (pageResult.failed) {
            result.links.clear();
            result.failed = true;
            return result;
        }
        result.limited = result.limited || pageResult.limited;
        // Keep a page's equivalent identities together. Save can then validate
        // the exact multiplicity before deleting any matching annotations.
        if (pageResult.links.size() > MaximumLinks - result.links.size()) {
            result.limited = true;
            break;
        }
        result.links += pageResult.links;
    }
    return result;
}

ScanResult scan(const QString &filePath,
                const std::shared_ptr<std::atomic_bool> &cancellation)
{
    assertPDFiumProcessLockNotHeld();
    ScanResult result;
    if (cancelled(cancellation)) {
        return result;
    }
    const ScanDocument document(filePath, cancellation);
    if (!document.get()) {
        result.failed = !cancelled(cancellation);
        return result;
    }
    int pageCount = 0;
    {
        PDFiumProcessLock lock;
        if (cancelled(cancellation)) {
            return {};
        }
        pageCount = FPDF_GetPageCount(document.get());
    }
    if (pageCount <= 0) {
        result.failed = true;
        return result;
    }
    result.limited = pageCount > kMaximumPages;
    int totalCharacters = 0;
    int totalCandidates = 0;
    int totalRectangles = 0;
    int totalUrlCharacters = 0;
    int comparisonsLeft = kMaximumOverlapComparisons;
    bool stop = false;
    for (int pageIndex = 0; pageIndex < qMin(pageCount, kMaximumPages) && !stop;
         ++pageIndex) {
        if (cancelled(cancellation)) {
            return {};
        }
        PDFiumProcessLock lock;
        if (cancelled(cancellation)) {
            return {};
        }
        const Page page(FPDF_LoadPage(document.get(), pageIndex));
        if (!page.get()) {
            result.failed = true;
            break;
        }
        const PageMapper mapper(page.get());
        QVector<QRectF> occupied;
        if (!mapper.valid()
            || !existingLinkRectangles(page.get(), mapper, occupied, cancellation)) {
            result.limited = true;
            continue;
        }
        const TextPage textPage(FPDFText_LoadPage(page.get()));
        if (!textPage.get()) {
            result.failed = true;
            break;
        }
        const int characterCount = FPDFText_CountChars(textPage.get());
        if (characterCount < 0) {
            result.failed = true;
            break;
        }
        if (characterCount > kMaximumPageCharacters) {
            result.limited = true;
            continue;
        }
        if (characterCount > kMaximumTotalCharacters - totalCharacters) {
            result.limited = true;
            break;
        }
        totalCharacters += characterCount;
        if (characterCount == 0 || cancelled(cancellation)) {
            continue;
        }
        const WebLinks webLinks(FPDFLink_LoadWebLinks(textPage.get()));
        if (!webLinks.get()) {
            result.failed = true;
            break;
        }
        const int count = FPDFLink_CountWebLinks(webLinks.get());
        if (count < 0) {
            result.failed = true;
            break;
        }
        result.limited = result.limited || count > kMaximumCandidatesPerPage;
        int pageRectangles = 0;
        for (int index = 0; index < qMin(count, kMaximumCandidatesPerPage); ++index) {
            if (cancelled(cancellation)) {
                return {};
            }
            if (++totalCandidates > kMaximumCandidates) {
                result.limited = stop = true;
                break;
            }
            const int units = FPDFLink_GetURL(webLinks.get(), index, nullptr, 0);
            if (units <= 1 || units > MaximumUrlLength + 1) {
                result.limited = result.limited || units > MaximumUrlLength + 1;
                continue;
            }
            QVector<unsigned short> buffer(units);
            if (FPDFLink_GetURL(webLinks.get(), index, buffer.data(), units) != units
                || buffer.back() != 0) {
                result.limited = true;
                continue;
            }
            const QString url = validWebUrl(QString::fromUtf16(
                reinterpret_cast<const char16_t *>(buffer.constData()), units - 1));
            if (url.isEmpty()) {
                continue;
            }
            const int rectCount = FPDFLink_CountRects(webLinks.get(), index);
            if (rectCount <= 0 || rectCount > MaximumRectanglesPerLink) {
                result.limited = true;
                continue;
            }
            Link link{pageIndex, url, {}};
            bool eligible = true;
            for (int rectIndex = 0; rectIndex < rectCount && eligible; ++rectIndex) {
                double left = 0.0, top = 0.0, right = 0.0, bottom = 0.0;
                QRectF rect;
                if (!FPDFLink_GetRect(webLinks.get(), index, rectIndex,
                                      &left, &top, &right, &bottom)
                    || !mapper.rectangle(left, top, right, bottom, rect)
                    || rect.isEmpty()) {
                    eligible = false;
                    result.limited = true;
                    break;
                }
                for (const QRectF &existing : std::as_const(occupied)) {
                    if (--comparisonsLeft < 0) {
                        result.limited = stop = true;
                        eligible = false;
                        break;
                    }
                    if (rect.intersects(existing)) {
                        eligible = false;
                        break;
                    }
                }
                if (!link.rects.contains(rect)) {
                    link.rects.append(rect);
                }
            }
            if (stop) {
                break;
            }
            if (!eligible) {
                continue;
            }
            if (pageRectangles + link.rects.size() > MaximumRectanglesPerPage) {
                result.limited = true;
                break;
            }
            if (result.links.size() >= MaximumLinks
                || totalRectangles + link.rects.size() > MaximumRectangles
                || totalUrlCharacters + url.size() > kMaximumTotalUrlCharacters) {
                result.limited = stop = true;
                break;
            }
            pageRectangles += static_cast<int>(link.rects.size());
            totalRectangles += static_cast<int>(link.rects.size());
            totalUrlCharacters += static_cast<int>(url.size());
            occupied += link.rects;
            result.links.append(std::move(link));
        }
    }
    if (cancelled(cancellation)) {
        return {};
    }
    if (result.failed) {
        result.links.clear();
    }
    return result;
}

} // namespace PDFWebLinks
