#pragma once

#include "PDFShapeUtils.h"

#include <QCryptographicHash>
#include <QHash>
#include <QScopeGuard>
#include <QSet>

#include <atomic>
#include <memory>
#include <utility>

#include <fpdf_text.h>

// The grid owns every child. Neither incomplete groups nor marked orphan text
// may be adopted as independently editable page content.
namespace PDFTableCellMetadata {
inline constexpr char MarkName[] = "TrePDFTableCell";
inline constexpr int MaximumFragmentBytes = 16 * 1024;
inline constexpr int MaximumPageObjects = 50000;
inline constexpr int MaximumPageTextCharacters = 50000;
inline constexpr qsizetype MaximumPageMetadataBytes = 16 * 1024 * 1024;
struct FontFingerprints {
    QHash<FPDF_FONT, QByteArray> values;
    qsizetype remainingBytes = 256 * 1024 * 1024;
};

inline bool hasMark(FPDF_PAGEOBJECT object)
{
    const int count = FPDFPageObj_CountMarks(object);
    for (int index = 0; index < count; ++index) {
        const auto mark = FPDFPageObj_GetMark(object, index);
        std::array<FPDF_WCHAR, 32> name{};
        unsigned long length = 0;
        if (mark && FPDFPageObjMark_GetName(mark, name.data(), sizeof(name), &length)
            && length <= sizeof(name)
            && QString::fromUtf16(reinterpret_cast<const char16_t *>(name.data()))
                == QLatin1String(MarkName))
            return true;
    }
    return false;
}

inline QByteArray fontFingerprint(FPDF_FONT font, FontFingerprints &cache)
{
    auto entry = cache.values.constFind(font);
    if (entry != cache.values.cend())
        return *entry;
    size_t length = 0;
    QByteArray fingerprint;
    // Use the complete embedded program identity, not its editable display name.
    if (font && cache.values.size() < 128 && FPDFFont_GetIsEmbedded(font) == 1
        && FPDFFont_GetFontData(font, nullptr, 0, &length)
        && length > 0 && length <= 64 * 1024 * 1024
        && length <= static_cast<size_t>(cache.remainingBytes)) {
        cache.remainingBytes -= qsizetype(length);
        QByteArray bytes(qsizetype(length), Qt::Uninitialized);
        size_t actual = 0;
        if (FPDFFont_GetFontData(font, reinterpret_cast<uint8_t *>(bytes.data()),
                                 length, &actual) && actual == length)
            fingerprint = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
    }
    cache.values.insert(font, fingerprint);
    return fingerprint;
}

inline QJsonArray signature(FPDF_PAGEOBJECT object, FontFingerprints &cache,
                            qreal expectedFontSize = 12.0)
{
    if (!object || FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_TEXT
        || FPDFTextObj_GetTextRenderMode(object) != FPDF_TEXTRENDERMODE_FILL)
        return {};
    const FPDF_CLIPPATH clip = FPDFPageObj_GetClipPath(object);
    if (clip && FPDFClipPath_CountPaths(clip) > 0)
        return {};
    unsigned int red = 0, green = 0, blue = 0, alpha = 0;
    float size = 0, left = 0, bottom = 0, right = 0, top = 0;
    FS_MATRIX matrix{};
    if (!FPDFPageObj_GetFillColor(object, &red, &green, &blue, &alpha)
        || red != 0 || green != 0 || blue != 0 || alpha != 255
        || !FPDFTextObj_GetFontSize(object, &size) || std::abs(size - expectedFontSize) > .001
        || !FPDFPageObj_GetMatrix(object, &matrix)
        || !FPDFPageObj_GetBounds(object, &left, &bottom, &right, &top))
        return {};
    const QByteArray font = fontFingerprint(FPDFTextObj_GetFont(object), cache);
    if (font.isEmpty())
        return {};
    QJsonArray result{matrix.a, matrix.b, matrix.c, matrix.d, matrix.e, matrix.f,
                      left, bottom, right, top, size};
    for (const auto &value : std::as_const(result)) {
        if (!std::isfinite(value.toDouble()))
            return {};
    }
    result.append(QString::fromLatin1(font));
    return result;
}

inline bool signaturesMatch(const QJsonArray &actual, const QJsonArray &expected)
{
    if (actual.size() != 12 || expected.size() != 12 || actual.last() != expected.last())
        return false;
    for (int index = 0; index < 11; ++index) {
        if (!expected[index].isDouble() || !std::isfinite(expected[index].toDouble())
            || std::abs(actual[index].toDouble() - expected[index].toDouble()) > .025)
            return false;
    }
    return true;
}

inline QString extractedText(const QString &text)
{
    // PDFium coalesces consecutive U+0020 characters in object text extraction.
    // Keep the exact logical string in the parent/fragment metadata; compare
    // the extracted view separately from the validated physical signature.
    QString result;
    result.reserve(text.size());
    for (QChar character : text) {
        if (character != QLatin1Char(' ') || result.isEmpty() || result.back() != character)
            result.append(character);
    }
    return result;
}

inline QJsonObject styleObject(const PDFTableCellFormat::Style &style, bool includeFontSize = true)
{
    QJsonObject object = PDFTableCellFormat::styleJson(style);
    if (!includeFontSize)
        object.remove(QStringLiteral("fontSize"));
    return object;
}

inline QJsonArray underlineSignature(FPDF_PAGEOBJECT object)
{
    if (!object || FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_PATH
        || FPDFPath_CountSegments(object) != 2 || FPDFPageObj_GetDashCount(object) != 0
        || FPDFPageObj_GetLineCap(object) != FPDF_LINECAP_BUTT
        || FPDFPageObj_GetLineJoin(object) != FPDF_LINEJOIN_MITER)
        return {};
    const FPDF_CLIPPATH clip = FPDFPageObj_GetClipPath(object);
    if (clip && FPDFClipPath_CountPaths(clip) > 0)
        return {};
    FS_MATRIX matrix{};
    float width = 0, phase = 0;
    int fill = -1;
    FPDF_BOOL stroke = false;
    unsigned int red = 0, green = 0, blue = 0, alpha = 0;
    if (!FPDFPageObj_GetMatrix(object, &matrix)
        || matrix.a != 1 || matrix.b != 0 || matrix.c != 0 || matrix.d != 1
        || matrix.e != 0 || matrix.f != 0
        || !FPDFPageObj_GetStrokeWidth(object, &width) || !std::isfinite(width) || width <= 0
        || !FPDFPageObj_GetDashPhase(object, &phase) || phase != 0
        || !FPDFPath_GetDrawMode(object, &fill, &stroke) || fill != FPDF_FILLMODE_NONE || !stroke
        || !FPDFPageObj_GetStrokeColor(object, &red, &green, &blue, &alpha)
        || red != 0 || green != 0 || blue != 0 || alpha != 255)
        return {};
    QJsonArray result{width};
    for (int index = 0; index < 2; ++index) {
        const auto segment = FPDFPath_GetPathSegment(object, index);
        float x = 0, y = 0;
        if (!segment || FPDFPathSegment_GetType(segment)
                != (index == 0 ? FPDF_SEGMENT_MOVETO : FPDF_SEGMENT_LINETO)
            || FPDFPathSegment_GetClose(segment) || !FPDFPathSegment_GetPoint(segment, &x, &y)
            || !std::isfinite(x) || !std::isfinite(y))
            return {};
        result.append(x);
        result.append(y);
    }
    return result;
}

inline bool underlineSignaturesMatch(const QJsonArray &actual, const QJsonArray &expected)
{
    if (actual.size() != 5 || expected.size() != 5)
        return false;
    for (int index = 0; index < 5; ++index) {
        if (!expected[index].isDouble() || !std::isfinite(expected[index].toDouble())
            || std::abs(actual[index].toDouble() - expected[index].toDouble()) > .025)
            return false;
    }
    return true;
}

inline bool writeFragment(FPDF_DOCUMENT document, FPDF_PAGEOBJECT object, const QString &id,
                           int cell, int index, int count, int start, int length,
                           const QString &text, const QJsonArray &physical,
                           const PDFTableCellFormat::Cell *format, bool underline)
{
    if (physical.isEmpty() || FPDFPageObj_CountMarks(object) != 0 || (underline && !format))
        return false;
    QJsonObject data{
        {QStringLiteral("version"), format ? 3 : 1}, {QStringLiteral("id"), id},
        {QStringLiteral("cell"), cell}, {QStringLiteral("index"), index},
        {QStringLiteral("count"), count}, {QStringLiteral("start"), start},
        {QStringLiteral("length"), length}, {QStringLiteral("text"), text},
        {QStringLiteral("signature"), physical}};
    if (format) {
        data.insert(QStringLiteral("role"), underline ? QStringLiteral("underline") : QStringLiteral("text"));
        data.insert(QStringLiteral("style"), styleObject(PDFTableCellFormat::styleAt(*format, start)));
    }
    const QByteArray bytes = QJsonDocument(data).toJson(QJsonDocument::Compact);
    if (bytes.size() > MaximumFragmentBytes)
        return false;
    const auto mark = FPDFPageObj_AddMark(object, MarkName);
    return mark && FPDFPageObjMark_SetBlobParam(document, object, mark, "Data",
        reinterpret_cast<const unsigned char *>(bytes.constData()),
        static_cast<unsigned long>(bytes.size()));
}

inline bool write(FPDF_DOCUMENT document, FPDF_PAGEOBJECT object, const QString &id,
                   int cell, int index, int count, int start, int length,
                   const QString &text, FontFingerprints &fonts,
                   const PDFTableCellFormat::Cell *format = nullptr)
{
    return writeFragment(document, object, id, cell, index, count, start, length,
                         text, signature(object, fonts, format
                             ? PDFTableCellFormat::styleAt(*format, start).fontSize : 12.0), format, false);
}

inline bool writeUnderline(FPDF_DOCUMENT document, FPDF_PAGEOBJECT object, const QString &id,
                            int cell, int index, int count, int start, int length,
                            const QString &text, const PDFTableCellFormat::Cell &format)
{
    return writeFragment(document, object, id, cell, index, count, start, length,
                         text, underlineSignature(object), &format, true);
}

struct Group {
    QVector<QVector<int>> objectPaths;
    QByteArray fingerprint;
};

struct IndexedChild {
    int objectIndex = -1;
    QByteArray bytes;
    QJsonObject data;
};

// One immutable index per page/validation phase. Metadata and the fallback
// Unicode snapshot are bounded; object geometry is checked on every group read.
struct PageIndex {
    FPDF_PAGE page = nullptr;
    QSizeF pageSize;
    bool initialized = false;
    bool valid = false;
    QHash<QString, QVector<int>> parents;
    QHash<QString, QVector<IndexedChild>> children;
    bool textIndexed = false;
    bool textIndexValid = false;
    QHash<FPDF_PAGEOBJECT, QString> objectText;
    FontFingerprints fonts;
};

inline bool cancelled(const std::shared_ptr<std::atomic_bool> &cancellation)
{
    return cancellation && cancellation->load(std::memory_order_relaxed);
}

inline bool matchesObjectText(PageIndex &index, FPDF_TEXTPAGE page,
                              FPDF_PAGEOBJECT object, const QString &expected,
                              const std::shared_ptr<std::atomic_bool> &cancellation)
{
    if (!index.textIndexed) {
        index.textIndexed = true;
        const int count = FPDFText_CountChars(page);
        if (count < 0 || count > MaximumPageTextCharacters)
            return false;
        for (int character = 0; character < count; ++character) {
            if (cancelled(cancellation))
                return false;
            const auto owner = FPDFText_GetTextObject(page, character);
            if (!owner || FPDFText_IsGenerated(page, character) != 0)
                continue;
            const char32_t unicode = FPDFText_GetUnicode(page, character);
            if (unicode > 0x10ffff || (unicode >= 0xd800 && unicode <= 0xdfff))
                return false;
            index.objectText[owner] += QString::fromUcs4(&unicode, 1);
        }
        index.textIndexValid = true;
    }
    const auto actual = index.objectText.constFind(object);
    return index.textIndexValid && actual != index.objectText.cend()
        && extractedText(*actual) == extractedText(expected);
}

inline bool readBlob(FPDF_PAGEOBJECTMARK mark, qsizetype maximumBytes,
                     qsizetype &remainingBytes, QByteArray &bytes)
{
    unsigned long length = 0;
    if (!mark || FPDFPageObjMark_CountParams(mark) != 1
        || !FPDFPageObjMark_GetParamBlobValue(mark, "Data", nullptr, 0, &length)
        || length == 0 || length > static_cast<unsigned long>(maximumBytes)
        || length > static_cast<unsigned long>(remainingBytes))
        return false;
    remainingBytes -= qsizetype(length);
    bytes.resize(qsizetype(length));
    unsigned long actualLength = 0;
    return FPDFPageObjMark_GetParamBlobValue(mark, "Data",
        reinterpret_cast<unsigned char *>(bytes.data()), length, &actualLength)
        && actualLength == length;
}

inline bool initializeIndex(PageIndex &index, FPDF_PAGE page, const QSizeF &pageSize,
                            const std::shared_ptr<std::atomic_bool> &cancellation)
{
    if (index.initialized)
        return index.valid && index.page == page && index.pageSize == pageSize;
    index.initialized = true;
    index.page = page;
    index.pageSize = pageSize;
    const int count = FPDFPage_CountObjects(page);
    if (count < 0 || count > MaximumPageObjects)
        return false;
    qsizetype remainingBytes = MaximumPageMetadataBytes;
    for (int objectIndex = 0; objectIndex < count; ++objectIndex) {
        if (cancelled(cancellation))
            return false;
        const auto object = FPDFPage_GetObject(page, objectIndex);
        if (hasMark(object)) {
            if (FPDFPageObj_CountMarks(object) != 1)
                return false;
            QByteArray bytes;
            if (!readBlob(FPDFPageObj_GetMark(object, 0), MaximumFragmentBytes,
                          remainingBytes, bytes))
                return false;
            const QJsonDocument document = QJsonDocument::fromJson(bytes);
            if (!document.isObject())
                return false;
            const QJsonObject data = document.object();
            if (!data.value(QStringLiteral("id")).isString())
                return false;
            index.children[data.value(QStringLiteral("id")).toString()].append(
                IndexedChild{objectIndex, std::move(bytes), data});
            continue;
        }
        if (FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_PATH
            || FPDFPageObj_CountMarks(object) != 1)
            continue;
        const auto mark = FPDFPageObj_GetMark(object, 0);
        std::array<FPDF_WCHAR, 32> name{};
        unsigned long length = 0;
        if (!mark || !FPDFPageObjMark_GetName(mark, name.data(), sizeof(name), &length)
            || length != sizeof(FPDF_WCHAR) * sizeof(PDFShapeUtils::kMarkName)
            || QString::fromUtf16(reinterpret_cast<const char16_t *>(name.data()))
                != QLatin1String(PDFShapeUtils::kMarkName))
            continue;
        QByteArray bytes;
        if (!readBlob(mark, PDFShapeUtils::kMaximumMetadataBytes, remainingBytes, bytes))
            return false;
        const auto id = QJsonDocument::fromJson(bytes).object().value(QStringLiteral("id"));
        if (!id.isString() || id.toString().isEmpty())
            continue; // Legacy shapes and tables have no cell group.
        PDFShapeUtils::Metadata parent;
        if (PDFShapeUtils::readMetadata(page, pageSize, object, parent, bytes))
            index.parents[parent.tableId].append(objectIndex);
    }
    index.valid = true;
    return true;
}

inline bool readGroup(FPDF_PAGE page, const QSizeF &pageSize,
                       const PDFShapeUtils::Metadata &metadata,
                       FPDF_TEXTPAGE textPage, Group &group,
                       PageIndex *pageIndex = nullptr,
                       const std::shared_ptr<std::atomic_bool> &cancellation = {})
{
    group = {};
    if (metadata.tableId.isEmpty())
        return true;
    PageIndex localIndex;
    PageIndex &indexed = pageIndex ? *pageIndex : localIndex;
    if (cancelled(cancellation) || !initializeIndex(indexed, page, pageSize, cancellation))
        return false;
    const auto parents = indexed.parents.constFind(metadata.tableId);
    if (parents == indexed.parents.cend() || parents->size() != 1)
        return false;
    const int parentIndex = parents->constFirst();
    const auto rects = PDFTableCellLayout::cellRects(metadata.frame, pageSize,
        metadata.strokeWidthRatio, metadata.tableRows, metadata.tableColumns,
        metadata.tableRowStops, metadata.tableColumnStops);
    if (rects.size() != metadata.tableRows * metadata.tableColumns)
        return false;
    const bool ownedTextPage = !textPage;
    if (ownedTextPage)
        textPage = FPDFText_LoadPage(page);
    const auto closeTextPage = qScopeGuard([&] {
        if (ownedTextPage && textPage)
            FPDFText_ClosePage(textPage);
    });
    if (!textPage)
        return false;
    struct Fragment {
        int start; int length; int count; int index; int objectIndex;
        bool needsUnderline; bool hasUnderline;
        QJsonArray physical;
    };
    QVector<QVector<Fragment>> fragments(rects.size());
    QCryptographicHash fingerprint(QCryptographicHash::Sha256);
    const PDFShapeUtils::CoordinateMapper mapper(page, pageSize);
    const auto children = indexed.children.constFind(metadata.tableId);
    const QVector<IndexedChild> noChildren;
    const auto &records = children == indexed.children.cend() ? noChildren : *children;
    const bool styled = !metadata.tableCellFormats.isEmpty();
    const bool sized = styled && metadata.tableCellFormatVersion >= 6;
    for (const IndexedChild &record : records) {
        if (cancelled(cancellation))
            return false;
        const int objectIndex = record.objectIndex;
        // Save replaces the whole group at the grid's position. Interleaved
        // foreign paint must stay read-only rather than silently change layers.
        if (objectIndex != parentIndex + 1 + group.objectPaths.size())
            return false;
        const auto object = FPDFPage_GetObject(page, objectIndex);
        const QByteArray &bytes = record.bytes;
        const QJsonObject &data = record.data;
        const auto integer = [&data](const char *key) {
            const auto value = data.value(QLatin1String(key));
            const int number = value.toInt(-1);
            return value.isDouble() && value.toDouble() == number ? number : -1;
        };
        const int cell = integer("cell"), index = integer("index"), count = integer("count");
        const int start = integer("start"), spanLength = integer("length");
        if (data.size() != (styled ? 11 : 9) || integer("version") != (sized ? 3 : styled ? 2 : 1)
            || cell < 0 || cell >= metadata.tableCellTexts.size()
            || count < 1 || count > PDFTableCellLayout::MaximumCellCharacters
            || index < 0 || index >= count || start < 0 || spanLength < 1
            || start > metadata.tableCellTexts[cell].size() - spanLength
            || !data.value(QStringLiteral("text")).isString())
            return false;
        const QString encoded = data.value(QStringLiteral("text")).toString();
        if (encoded != metadata.tableCellTexts[cell].sliced(start, spanLength)
            || (!styled && encoded.trimmed().isEmpty()))
            return false;
        bool needsUnderline = false;
        bool underline = false;
        qreal fontSize = 12.0;
        if (styled) {
            const auto &format = metadata.tableCellFormats[cell];
            const auto style = PDFTableCellFormat::styleAt(format, start);
            const auto role = data.value(QStringLiteral("role"));
            if (!role.isString() || (role.toString() != QLatin1String("text")
                                    && role.toString() != QLatin1String("underline"))
                || !data.value(QStringLiteral("style")).isObject()
                || data.value(QStringLiteral("style")).toObject() != styleObject(style, sized))
                return false;
            // A physical run cannot silently cover a different semantic face.
            for (int position = start + 1; position < start + spanLength; ++position) {
                if (PDFTableCellFormat::styleAt(format, position) != style)
                    return false;
            }
            needsUnderline = style.underline;
            underline = role.toString() == QLatin1String("underline");
            fontSize = style.fontSize;
        }
        const QRectF cellFrame(rects[cell].x() * pageSize.width(),
            rects[cell].y() * pageSize.height(), rects[cell].width() * pageSize.width(),
            rects[cell].height() * pageSize.height());
        constexpr qreal overhang = PDFTableCellLayout::InkOverhang;
        const QRectF safeFrame = cellFrame.adjusted(-overhang, -overhang, overhang, overhang);
        QJsonArray physical;
        if (underline) {
            physical = underlineSignature(object);
            if (!needsUnderline || fragments[cell].isEmpty()
                || !underlineSignaturesMatch(physical, data.value(QStringLiteral("signature")).toArray()))
                return false;
            auto &part = fragments[cell].last();
            if (!part.needsUnderline || part.hasUnderline || part.objectIndex + 1 != objectIndex
                || part.start != start || part.length != spanLength || part.index != index || part.count != count)
                return false;
            QPointF first, last, baseline;
            if (!mapper.devicePoint(QPointF(physical[1].toDouble(), physical[2].toDouble()), first)
                || !mapper.devicePoint(QPointF(physical[3].toDouble(), physical[4].toDouble()), last)
                || !mapper.devicePoint(QPointF(part.physical[4].toDouble(), part.physical[5].toDouble()), baseline)
                || !safeFrame.contains(first) || !safeFrame.contains(last)
                || last.x() <= first.x() || std::abs(first.y() - last.y()) > .05
                || std::abs(first.x() - baseline.x()) > .05
                || std::abs(first.y() - baseline.y() - qMax<qreal>(.8, fontSize * .10)) > .05
                || std::abs(physical[0].toDouble() - mapper.strokeWidth(qMax<qreal>(.5, fontSize * .05))) > .025)
                return false;
            part.hasUnderline = true;
        } else {
            physical = signature(object, indexed.fonts, fontSize);
            if (!signaturesMatch(physical, data.value(QStringLiteral("signature")).toArray()))
                return false;
            const unsigned long textBytes = FPDFTextObj_GetText(object, textPage, nullptr, 0);
            if (textBytes < 2 || textBytes % 2)
                return false;
            bool textMatches = false;
            if (textBytes <= 2 * (PDFTableCellLayout::MaximumCellCharacters + 1)) {
                QVector<FPDF_WCHAR> text(textBytes / 2);
                if (FPDFTextObj_GetText(object, textPage, text.data(), textBytes) != textBytes)
                    return false;
                const QString decoded = QString::fromUtf16(
                    reinterpret_cast<const char16_t *>(text.constData()), text.size() - 1);
                // PDFium can omit all U+0020 glyphs from a rotated text object's
                // extracted view. Its validated font, matrix, advance and zero-ink
                // bounds still identify this mandatory child; coverage below must
                // continue rejecting missing space/underline objects.
                const bool omittedSpaces = styled && decoded.isEmpty()
                    && std::all_of(encoded.cbegin(), encoded.cend(), [](QChar character) {
                        return character == QLatin1Char(' ');
                    })
                    && (std::abs(physical[8].toDouble() - physical[6].toDouble()) < .001
                        || std::abs(physical[9].toDouble() - physical[7].toDouble()) < .001);
                textMatches = omittedSpaces || extractedText(decoded) == extractedText(encoded);
            }
            // GetText can append PDFium-generated spacing between neighboring
            // objects. Compare only this object's real characters in that case;
            // do not trim genuine spaces or relax the physical signature checks.
            // Oversized extraction also uses this bounded fallback without allocating it.
            if (!textMatches && !matchesObjectText(indexed, textPage, object, encoded, cancellation))
                return false;
            // Ink may extend fractionally beyond the advance box, within padding.
            for (int x : {6, 8}) {
                for (int y : {7, 9}) {
                    QPointF display;
                    if (!mapper.devicePoint(QPointF(physical[x].toDouble(), physical[y].toDouble()), display)
                        || !safeFrame.contains(display))
                        return false;
                }
            }
            fragments[cell].append({start, spanLength, count, index, objectIndex,
                                    needsUnderline, false, physical});
        }
        if (group.objectPaths.size() >= 2 * PDFTableCellLayout::MaximumTableCharacters)
            return false;
        group.objectPaths.append(QVector<int>{objectIndex});
        fingerprint.addData(QByteArray::number(objectIndex));
        fingerprint.addData(bytes);
        fingerprint.addData(QJsonDocument(physical).toJson(QJsonDocument::Compact));
    }
    for (int cell = 0; cell < metadata.tableCellTexts.size(); ++cell) {
        if (cancelled(cancellation))
            return false;
        auto &parts = fragments[cell];
        std::sort(parts.begin(), parts.end(), [](const Fragment &left, const Fragment &right) {
            return left.index < right.index;
        });
        const QString &text = metadata.tableCellTexts[cell];
        const auto allowedGap = [&metadata](const QString &gap) {
            if (metadata.tableCellFormats.isEmpty())
                return gap.trimmed().isEmpty();
            // v5 owns spaces as well as visible glyphs: removing an underlined
            // spaces-only fragment must not leave an apparently valid table.
            return std::all_of(gap.cbegin(), gap.cend(), [](QChar character) {
                return character == QLatin1Char('\n');
            });
        };
        int covered = 0;
        for (int index = 0; index < parts.size(); ++index) {
            const Fragment &part = parts[index];
            if (part.needsUnderline != part.hasUnderline
                || part.count != parts.size() || part.index != index || part.start < covered
                || !allowedGap(text.sliced(covered, part.start - covered)))
                return false;
            covered = part.start + part.length;
        }
        if (!allowedGap(text.sliced(covered)))
            return false;
    }
    group.fingerprint = fingerprint.result();
    return true;
}
} // namespace PDFTableCellMetadata
