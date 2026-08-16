#include "PDFAcroFormCanonicalizer.h"

#include "PDFFormAppearance.h"

#include <QByteArray>
#include <QFile>
#include <QFont>
#include <QHash>
#include <QImage>
#include <QList>
#include <QMap>
#include <QPainter>
#include <QPolygonF>
#include <QRawFont>
#include <QSet>
#include <QTextOption>
#include <QUuid>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace {

constexpr char kUnicodeFormFontPath[]
    = ":/trepdf/fonts/Arial/Arial-Regular.TTF";
constexpr char kUnicodeFormFontBaseName[] = "ArialMT";
constexpr char kUnicodeFormFontResourcePrefix[] = "TrePDFUF";
constexpr int kMaximumChoiceOptions = 4096;
constexpr qsizetype kMaximumChoiceOptionCharacters = 1024 * 1024;
constexpr int kMaximumTextFieldLength = 1000000;
constexpr int kTextFieldMultiline = 1 << 12;
constexpr int kTextFieldComb = 1 << 24;
constexpr int kButtonNoToggleToOff = 1 << 14;
constexpr int kButtonRadio = 1 << 15;
constexpr int kChoiceCombo = 1 << 17;
constexpr int kChoiceEdit = 1 << 18;
constexpr int kChoiceMultiSelect = 1 << 21;
constexpr int kChoiceCommitOnSelectionChange = 1 << 26;

bool isTrePDFTextFieldWidgetId(const QString &widgetId)
{
    constexpr qsizetype uuidLength = 36;
    const QString prefix = QStringLiteral("text-field-");
    if (!widgetId.startsWith(prefix)) {
        return false;
    }
    const QString suffix = widgetId.sliced(prefix.size());
    if (suffix.size() == uuidLength) {
        const QUuid uuid = QUuid::fromString(suffix);
        if (!uuid.isNull()
            && uuid.toString(QUuid::WithoutBraces).compare(
                   suffix,
                   Qt::CaseInsensitive)
                == 0) {
            return true;
        }
    }

    if (suffix.isEmpty()
        || suffix.size() > 20
        || suffix.front() == QLatin1Char('0')) {
        return false;
    }
    for (const QChar character : suffix) {
        if (!character.isDigit()) {
            return false;
        }
    }
    bool converted = false;
    const qulonglong sequence = suffix.toULongLong(&converted);
    return converted && sequence > 0;
}

bool isTrePDFDropdownWidgetId(const QString &widgetId)
{
    constexpr qsizetype uuidLength = 36;
    const QString prefix = QStringLiteral("dropdown-field-");
    if (!widgetId.startsWith(prefix)
        || widgetId.size() != prefix.size() + uuidLength) {
        return false;
    }
    const QString suffix = widgetId.sliced(prefix.size());
    const QUuid uuid = QUuid::fromString(suffix);
    return !uuid.isNull()
        && uuid.toString(QUuid::WithoutBraces).compare(
               suffix,
               Qt::CaseInsensitive)
            == 0;
}

bool isTrePDFListBoxWidgetId(const QString &widgetId)
{
    constexpr qsizetype uuidLength = 36;
    const QString prefix = QStringLiteral("listbox-field-");
    if (!widgetId.startsWith(prefix)
        || widgetId.size() != prefix.size() + uuidLength) {
        return false;
    }
    const QString suffix = widgetId.sliced(prefix.size());
    const QUuid uuid = QUuid::fromString(suffix);
    return !uuid.isNull()
        && uuid.toString(QUuid::WithoutBraces).compare(
               suffix,
               Qt::CaseInsensitive)
            == 0;
}

bool isTrePDFRadioButtonWidgetId(const QString &widgetId)
{
    constexpr qsizetype uuidLength = 36;
    const QString prefix = QStringLiteral("radio-field-");
    if (!widgetId.startsWith(prefix)
        || widgetId.size() != prefix.size() + uuidLength) {
        return false;
    }
    const QString suffix = widgetId.sliced(prefix.size());
    const QUuid uuid = QUuid::fromString(suffix);
    return !uuid.isNull()
        && uuid.toString(QUuid::WithoutBraces).compare(
               suffix,
               Qt::CaseInsensitive)
            == 0;
}

bool isFixedChoiceField(PDFAcroFormCanonicalizer::FieldType type)
{
    return type == PDFAcroFormCanonicalizer::FieldType::Dropdown
        || type == PDFAcroFormCanonicalizer::FieldType::ListBox;
}

bool validChoiceOptions(const QStringList &options)
{
    if (options.isEmpty() || options.size() > kMaximumChoiceOptions) {
        return false;
    }
    QSet<QString> uniqueOptions;
    qsizetype totalCharacters = 0;
    for (const QString &option : options) {
        totalCharacters += option.size();
        if (option.isEmpty()
            || totalCharacters > kMaximumChoiceOptionCharacters
            || uniqueOptions.contains(option)) {
            return false;
        }
        uniqueOptions.insert(option);
    }
    return true;
}

bool validCanonicalListBoxSelection(
    const QStringList &options,
    const QStringList &selectedValues)
{
    QSet<QString> selectedSet;
    QStringList canonicalSelection;
    for (const QString &value : selectedValues) {
        if (!options.contains(value) || selectedSet.contains(value)) {
            return false;
        }
        selectedSet.insert(value);
    }
    canonicalSelection.reserve(selectedSet.size());
    for (const QString &option : options) {
        if (selectedSet.contains(option)) {
            canonicalSelection.append(option);
        }
    }
    return canonicalSelection == selectedValues;
}

QVector<int> listBoxSelectedIndices(const QStringList &options,
                                    const QStringList &selectedValues)
{
    QVector<int> indices;
    indices.reserve(selectedValues.size());
    for (int optionIndex = 0;
         optionIndex < options.size();
         ++optionIndex) {
        if (selectedValues.contains(options.at(optionIndex))) {
            indices.append(optionIndex);
        }
    }
    return indices;
}

QByteArray radioButtonExportName(const QString &widgetId)
{
    return QByteArrayLiteral("/") + widgetId.toLatin1();
}

struct UnicodeFormFontTemplate {
    QByteArray compressedFontData;
    QByteArray compressedCidToGidMap;
    QByteArray widths;
    QByteArray toUnicode;
    qsizetype uncompressedFontSize = 0;
    int ascent = 0;
    int descent = 0;
    int capHeight = 0;
    int xHeight = 0;
    int leading = 0;
    int averageWidth = 0;
    int maximumWidth = 0;
    int boundingLeft = 0;
    int boundingBottom = 0;
    int boundingRight = 0;
    int boundingTop = 0;
    QString error;

    bool isValid() const
    {
        return error.isEmpty()
            && !compressedFontData.isEmpty()
            && !compressedCidToGidMap.isEmpty()
            && !widths.isEmpty()
            && !toUnicode.isEmpty()
            && uncompressedFontSize > 0;
    }
};

struct XrefEntry {
    qint64 offset = -1;
    int generation = 0;
};

struct ObjectReference {
    int number = -1;
    int generation = 0;

    bool operator==(const ObjectReference &other) const
    {
        return number == other.number && generation == other.generation;
    }
};

QByteArray referenceArray(const QList<ObjectReference> &references);

struct ParsedPdf {
    const char *data = nullptr;
    qsizetype size = 0;
    qint64 startXref = -1;
    int declaredSize = 0;
    ObjectReference root;
    QHash<int, XrefEntry> entries;
    QHash<int, qint64> objectEndOffsets;
};

bool isPdfWhitespace(char value)
{
    return value == '\0'
        || value == '\t'
        || value == '\n'
        || value == '\f'
        || value == '\r'
        || value == ' ';
}

bool isDelimiter(char value)
{
    return isPdfWhitespace(value)
        || value == '('
        || value == ')'
        || value == '<'
        || value == '>'
        || value == '['
        || value == ']'
        || value == '{'
        || value == '}'
        || value == '/'
        || value == '%';
}

qsizetype findBytes(const char *data,
                    qsizetype size,
                    QByteArrayView needle,
                    qsizetype from = 0)
{
    if (!data || needle.isEmpty() || from < 0 || from > size) {
        return -1;
    }
    const auto begin = data + from;
    const auto end = data + size;
    const auto match = std::search(
        begin,
        end,
        needle.begin(),
        needle.end());
    return match == end ? -1 : match - data;
}

qsizetype findLastBytes(const char *data,
                        qsizetype size,
                        QByteArrayView needle)
{
    if (!data || needle.isEmpty() || needle.size() > size) {
        return -1;
    }
    for (qsizetype offset = size - needle.size(); offset >= 0; --offset) {
        if (std::equal(needle.begin(), needle.end(), data + offset)) {
            return offset;
        }
    }
    return -1;
}

class Cursor
{
public:
    Cursor(const char *data, qsizetype size, qsizetype position = 0)
        : m_data(data)
        , m_size(size)
        , m_position(position)
    {
    }

    qsizetype position() const
    {
        return m_position;
    }

    bool atEnd() const
    {
        return m_position >= m_size;
    }

    void skipWhitespaceAndComments()
    {
        while (m_position < m_size) {
            if (isPdfWhitespace(m_data[m_position])) {
                ++m_position;
                continue;
            }
            if (m_data[m_position] != '%') {
                break;
            }
            while (m_position < m_size
                   && m_data[m_position] != '\n'
                   && m_data[m_position] != '\r') {
                ++m_position;
            }
        }
    }

    bool consume(QByteArrayView token)
    {
        skipWhitespaceAndComments();
        if (m_position + token.size() > m_size
            || !std::equal(
                token.begin(),
                token.end(),
                m_data + m_position)) {
            return false;
        }
        m_position += token.size();
        return true;
    }

    bool integer(qint64 &value)
    {
        skipWhitespaceAndComments();
        if (m_position >= m_size) {
            return false;
        }
        bool negative = false;
        if (m_data[m_position] == '+' || m_data[m_position] == '-') {
            negative = m_data[m_position] == '-';
            ++m_position;
        }
        const qsizetype digitStart = m_position;
        quint64 result = 0;
        while (m_position < m_size
               && m_data[m_position] >= '0'
               && m_data[m_position] <= '9') {
            const unsigned int digit
                = static_cast<unsigned int>(m_data[m_position] - '0');
            if (result
                > (std::numeric_limits<quint64>::max() - digit) / 10) {
                return false;
            }
            result = result * 10 + digit;
            ++m_position;
        }
        if (m_position == digitStart
            || result > static_cast<quint64>(
                std::numeric_limits<qint64>::max())) {
            return false;
        }
        value = negative
            ? -static_cast<qint64>(result)
            : static_cast<qint64>(result);
        return true;
    }

    char nextNonWhitespace()
    {
        skipWhitespaceAndComments();
        return m_position < m_size ? m_data[m_position++] : '\0';
    }

private:
    const char *m_data = nullptr;
    qsizetype m_size = 0;
    qsizetype m_position = 0;
};

bool parseReference(const char *data,
                    qsizetype size,
                    qsizetype position,
                    ObjectReference &reference,
                    qsizetype *endPosition = nullptr)
{
    Cursor cursor(data, size, position);
    qint64 number = -1;
    qint64 generation = -1;
    if (!cursor.integer(number)
        || !cursor.integer(generation)
        || !cursor.consume("R")
        || number < 0
        || number > std::numeric_limits<int>::max()
        || generation < 0
        || generation > std::numeric_limits<int>::max()) {
        return false;
    }
    reference.number = static_cast<int>(number);
    reference.generation = static_cast<int>(generation);
    if (endPosition) {
        *endPosition = cursor.position();
    }
    return true;
}

qsizetype keyPosition(const char *data,
                      qsizetype size,
                      QByteArrayView key)
{
    qsizetype position = 0;
    while ((position = findBytes(data, size, key, position)) >= 0) {
        const qsizetype after = position + key.size();
        const bool leftBoundary = key.startsWith('/')
            || position == 0
            || isDelimiter(data[position - 1]);
        const bool rightBoundary = after >= size
            || isDelimiter(data[after]);
        if (leftBoundary && rightBoundary) {
            return position;
        }
        position = after;
    }
    return -1;
}

bool matchingDictionaryEnd(const char *data,
                           qsizetype size,
                           qsizetype start,
                           qsizetype &end);
bool pdfValueEnd(const char *data,
                 qsizetype size,
                 qsizetype start,
                 qsizetype &end);

bool parseXref(const char *data,
               qsizetype size,
               ParsedPdf &pdf,
               QString &error)
{
    constexpr QByteArrayView startXrefToken("startxref");
    const qsizetype startXrefPosition = findLastBytes(
        data,
        size,
        startXrefToken);
    if (startXrefPosition < 0) {
        error = QStringLiteral("The intermediate PDF has no startxref marker.");
        return false;
    }
    Cursor startCursor(
        data,
        size,
        startXrefPosition + startXrefToken.size());
    qint64 startXref = -1;
    if (!startCursor.integer(startXref)
        || startXref < 0
        || startXref >= size) {
        error = QStringLiteral("The intermediate PDF has an invalid startxref offset.");
        return false;
    }

    QHash<int, XrefEntry> entries;
    QSet<int> resolvedObjectNumbers;
    QSet<qint64> visitedXrefs;
    ObjectReference root;
    qint64 declaredSize = -1;
    qint64 currentXref = startXref;
    while (currentXref >= 0) {
        if (currentXref >= size
            || visitedXrefs.contains(currentXref)) {
            error = QStringLiteral(
                "The intermediate PDF has an invalid xref chain.");
            return false;
        }
        visitedXrefs.insert(currentXref);
        Cursor cursor(data, size, static_cast<qsizetype>(currentXref));
        if (!cursor.consume("xref")) {
            error = QStringLiteral(
                "The intermediate PDF did not use a canonical xref table.");
            return false;
        }

        while (true) {
            cursor.skipWhitespaceAndComments();
            const qsizetype sectionPosition = cursor.position();
            if (cursor.consume("trailer")) {
                break;
            }

            qint64 firstObject = -1;
            qint64 objectCount = -1;
            Cursor sectionCursor(data, size, sectionPosition);
            if (!sectionCursor.integer(firstObject)
                || !sectionCursor.integer(objectCount)
                || firstObject < 0
                || objectCount < 0
                || firstObject + objectCount
                    > std::numeric_limits<int>::max()) {
                error = QStringLiteral(
                    "The intermediate PDF has an invalid xref subsection.");
                return false;
            }
            cursor = sectionCursor;
            for (qint64 index = 0; index < objectCount; ++index) {
                qint64 objectOffset = -1;
                qint64 generation = -1;
                if (!cursor.integer(objectOffset)
                    || !cursor.integer(generation)) {
                    error = QStringLiteral(
                        "The intermediate PDF has a truncated xref entry.");
                    return false;
                }
                const char state = cursor.nextNonWhitespace();
                if (state != 'n' && state != 'f') {
                    error = QStringLiteral(
                        "The intermediate PDF has an invalid xref state.");
                    return false;
                }
                const int objectNumber = static_cast<int>(
                    firstObject + index);
                if (resolvedObjectNumbers.contains(objectNumber)) {
                    continue;
                }
                resolvedObjectNumbers.insert(objectNumber);
                if (state == 'n') {
                    if (objectOffset < 0
                        || objectOffset >= size
                        || generation < 0
                        || generation
                            > std::numeric_limits<int>::max()) {
                        error = QStringLiteral(
                            "The intermediate PDF has an invalid object offset.");
                        return false;
                    }
                    entries.insert(
                        objectNumber,
                        XrefEntry{
                            objectOffset,
                            static_cast<int>(generation)});
                }
            }
        }

        cursor.skipWhitespaceAndComments();
        const qsizetype trailerStart = cursor.position();
        qsizetype trailerEnd = -1;
        if (!matchingDictionaryEnd(
                data,
                size,
                trailerStart,
                trailerEnd)) {
            error = QStringLiteral(
                "The intermediate PDF has no trailer dictionary.");
            return false;
        }
        const char *trailerData = data + trailerStart;
        const qsizetype trailerSize = trailerEnd - trailerStart;
        if (keyPosition(trailerData, trailerSize, "/Encrypt") >= 0) {
            error = QStringLiteral(
                "Creating fields in encrypted PDFs is not supported.");
            return false;
        }
        if (keyPosition(trailerData, trailerSize, "/XRefStm") >= 0) {
            error = QStringLiteral(
                "Hybrid-reference PDFs are not supported by the canonicalizer.");
            return false;
        }

        if (root.number < 0) {
            const qsizetype rootKey = keyPosition(
                trailerData,
                trailerSize,
                "/Root");
            if (rootKey >= 0
                && !parseReference(
                    trailerData,
                    trailerSize,
                    rootKey + QByteArrayView("/Root").size(),
                    root)) {
                error = QStringLiteral(
                    "The intermediate PDF has an invalid Catalog reference.");
                return false;
            }
        }
        if (declaredSize < 0) {
            const qsizetype sizeKey = keyPosition(
                trailerData,
                trailerSize,
                "/Size");
            if (sizeKey >= 0) {
                Cursor sizeCursor(
                    trailerData,
                    trailerSize,
                    sizeKey + QByteArrayView("/Size").size());
                if (!sizeCursor.integer(declaredSize)
                    || declaredSize <= 0
                    || declaredSize
                        > std::numeric_limits<int>::max()) {
                    error = QStringLiteral(
                        "The intermediate PDF trailer has an invalid Size entry.");
                    return false;
                }
            }
        }

        const qsizetype previousKey = keyPosition(
            trailerData,
            trailerSize,
            "/Prev");
        if (previousKey < 0) {
            currentXref = -1;
        } else {
            Cursor previousCursor(
                trailerData,
                trailerSize,
                previousKey + QByteArrayView("/Prev").size());
            if (!previousCursor.integer(currentXref)
                || currentXref < 0) {
                error = QStringLiteral(
                    "The intermediate PDF has an invalid Prev xref offset.");
                return false;
            }
        }
    }

    if (root.number < 0) {
        error = QStringLiteral(
            "The intermediate PDF has no indirect Catalog reference.");
        return false;
    }
    if (declaredSize <= 0) {
        error = QStringLiteral(
            "The intermediate PDF trailer has no Size entry.");
        return false;
    }
    const auto rootEntry = entries.constFind(root.number);
    if (rootEntry == entries.cend()
        || rootEntry->generation != root.generation) {
        error = QStringLiteral("The intermediate PDF Catalog is missing from the xref table.");
        return false;
    }

    QVector<QPair<qint64, int>> objectsByOffset;
    objectsByOffset.reserve(entries.size());
    for (auto entry = entries.cbegin(); entry != entries.cend(); ++entry) {
        objectsByOffset.append({entry->offset, entry.key()});
    }
    std::sort(
        objectsByOffset.begin(),
        objectsByOffset.end(),
        [](const auto &left, const auto &right) {
            return left.first < right.first;
        });
    QHash<int, qint64> objectEndOffsets;
    objectEndOffsets.reserve(objectsByOffset.size());
    for (qsizetype index = 0; index < objectsByOffset.size(); ++index) {
        const qint64 objectOffset = objectsByOffset.at(index).first;
        const qint64 endOffset = index + 1 < objectsByOffset.size()
            ? objectsByOffset.at(index + 1).first
            : startXref;
        if (endOffset <= objectOffset || endOffset > size) {
            error = QStringLiteral(
                "The intermediate PDF contains overlapping object offsets.");
            return false;
        }
        objectEndOffsets.insert(
            objectsByOffset.at(index).second,
            endOffset);
    }

    pdf.data = data;
    pdf.size = size;
    pdf.startXref = startXref;
    pdf.declaredSize = static_cast<int>(declaredSize);
    pdf.root = root;
    pdf.entries = std::move(entries);
    pdf.objectEndOffsets = std::move(objectEndOffsets);
    return true;
}

QByteArrayView objectView(const ParsedPdf &pdf, int objectNumber)
{
    const auto entry = pdf.entries.constFind(objectNumber);
    if (entry == pdf.entries.cend()) {
        return {};
    }
    const qint64 endOffset = pdf.objectEndOffsets.value(objectNumber, -1);
    if (endOffset <= entry->offset || endOffset > pdf.size) {
        return {};
    }
    return QByteArrayView(
        pdf.data + entry->offset,
        endOffset - entry->offset);
}

QByteArray completeObject(const ParsedPdf &pdf,
                          int objectNumber,
                          QString &error)
{
    const QByteArrayView view = objectView(pdf, objectNumber);
    if (view.isEmpty()) {
        error = QStringLiteral("A required PDF object is missing.");
        return {};
    }
    const qsizetype endObject = findBytes(
        view.data(),
        view.size(),
        "endobj");
    if (endObject < 0) {
        error = QStringLiteral("A required PDF object is truncated.");
        return {};
    }
    return QByteArray(view.data(), endObject + QByteArrayView("endobj").size());
}

int hexDigitValue(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

bool parsePdfString(const char *data,
                    qsizetype size,
                    qsizetype position,
                    QByteArray &bytes)
{
    Cursor cursor(data, size, position);
    cursor.skipWhitespaceAndComments();
    position = cursor.position();
    if (position >= size) {
        return false;
    }

    bytes.clear();
    if (data[position] == '(') {
        int depth = 1;
        ++position;
        while (position < size && depth > 0) {
            const char character = data[position++];
            if (character == '\\') {
                if (position >= size) {
                    return false;
                }
                char escaped = data[position++];
                if (escaped >= '0' && escaped <= '7') {
                    int value = escaped - '0';
                    int digitCount = 1;
                    while (digitCount < 3
                           && position < size
                           && data[position] >= '0'
                           && data[position] <= '7') {
                        value = value * 8 + data[position++] - '0';
                        ++digitCount;
                    }
                    bytes.append(static_cast<char>(value & 0xff));
                    continue;
                }
                switch (escaped) {
                case 'n':
                    bytes.append('\n');
                    break;
                case 'r':
                    bytes.append('\r');
                    break;
                case 't':
                    bytes.append('\t');
                    break;
                case 'b':
                    bytes.append('\b');
                    break;
                case 'f':
                    bytes.append('\f');
                    break;
                case '\r':
                    if (position < size && data[position] == '\n') {
                        ++position;
                    }
                    break;
                case '\n':
                    break;
                default:
                    bytes.append(escaped);
                    break;
                }
                continue;
            }
            if (character == '(') {
                ++depth;
                bytes.append(character);
                continue;
            }
            if (character == ')') {
                --depth;
                if (depth > 0) {
                    bytes.append(character);
                }
                continue;
            }
            bytes.append(character);
        }
        return depth == 0;
    }

    if (data[position] != '<'
        || position + 1 >= size
        || data[position + 1] == '<') {
        return false;
    }
    ++position;
    int highNibble = -1;
    while (position < size) {
        const char character = data[position++];
        if (character == '>') {
            if (highNibble >= 0) {
                bytes.append(static_cast<char>(highNibble << 4));
            }
            return true;
        }
        if (isPdfWhitespace(character)) {
            continue;
        }
        const int nibble = hexDigitValue(character);
        if (nibble < 0) {
            return false;
        }
        if (highNibble < 0) {
            highNibble = nibble;
        } else {
            bytes.append(static_cast<char>((highNibble << 4) | nibble));
            highNibble = -1;
        }
    }
    return false;
}

QString decodedPdfString(const QByteArray &bytes)
{
    if (bytes.size() >= 2
        && static_cast<uchar>(bytes.at(0)) == 0xfe
        && static_cast<uchar>(bytes.at(1)) == 0xff) {
        QString result;
        result.reserve((bytes.size() - 2) / 2);
        for (qsizetype index = 2; index + 1 < bytes.size(); index += 2) {
            const ushort value
                = static_cast<ushort>(
                      static_cast<uchar>(bytes.at(index)))
                    << 8
                | static_cast<uchar>(bytes.at(index + 1));
            result.append(QChar(value));
        }
        return result;
    }
    if (bytes.size() >= 2
        && static_cast<uchar>(bytes.at(0)) == 0xff
        && static_cast<uchar>(bytes.at(1)) == 0xfe) {
        QString result;
        result.reserve((bytes.size() - 2) / 2);
        for (qsizetype index = 2; index + 1 < bytes.size(); index += 2) {
            const ushort value
                = static_cast<uchar>(bytes.at(index))
                | static_cast<ushort>(
                      static_cast<uchar>(bytes.at(index + 1)))
                    << 8;
            result.append(QChar(value));
        }
        return result;
    }
    if (bytes.startsWith("\xef\xbb\xbf")) {
        return QString::fromUtf8(bytes.constData() + 3, bytes.size() - 3);
    }
    return QString::fromLatin1(bytes);
}

struct PlaceholderLocation {
    ObjectReference container;
    ObjectReference widget;
    qsizetype dictionaryStart = -1;
    qsizetype dictionaryEnd = -1;

    bool isDirect() const
    {
        return widget.number < 0;
    }
};

qsizetype keyPositionFrom(const char *data,
                          qsizetype size,
                          QByteArrayView key,
                          qsizetype from)
{
    qsizetype position = from;
    while ((position = findBytes(data, size, key, position)) >= 0) {
        const qsizetype after = position + key.size();
        const bool leftBoundary = key.startsWith('/')
            || position == 0
            || isDelimiter(data[position - 1]);
        const bool rightBoundary = after >= size
            || isDelimiter(data[after]);
        if (leftBoundary && rightBoundary) {
            return position;
        }
        position = after;
    }
    return -1;
}

bool matchingDictionaryEnd(const char *data,
                           qsizetype size,
                           qsizetype start,
                           qsizetype &end);

PlaceholderLocation annotationForStringId(
    const ParsedPdf &pdf,
    QByteArrayView key,
    const QString &identifier,
    QByteArrayView expectedSubtype)
{
    PlaceholderLocation directCandidate;
    for (auto entry = pdf.entries.cbegin(); entry != pdf.entries.cend(); ++entry) {
        const QByteArrayView view = objectView(pdf, entry.key());
        if (view.isEmpty()) {
            continue;
        }
        // An incremental xref contains only the newest revision of an object.
        // The next live xref entry can therefore be physically separated by
        // obsolete objects and trailers. Bound the lookup to this indirect
        // object's value so stale dictionaries cannot be mistaken for a
        // direct annotation.
        Cursor objectCursor(view.data(), view.size());
        qint64 objectNumber = -1;
        qint64 generation = -1;
        if (!objectCursor.integer(objectNumber)
            || !objectCursor.integer(generation)
            || !objectCursor.consume("obj")) {
            continue;
        }
        objectCursor.skipWhitespaceAndComments();
        const qsizetype objectValueStart = objectCursor.position();
        qsizetype objectValueEnd = -1;
        if (!pdfValueEnd(
                view.data(),
                view.size(),
                objectValueStart,
                objectValueEnd)) {
            continue;
        }
        qsizetype searchPosition = 0;
        while (true) {
            const qsizetype nameKey = keyPositionFrom(
                view.data(),
                objectValueEnd,
                key,
                searchPosition);
            if (nameKey < 0) {
                break;
            }
            searchPosition = nameKey + key.size();
            QByteArray encodedName;
            if (!parsePdfString(
                    view.data(),
                    objectValueEnd,
                    searchPosition,
                    encodedName)
                || decodedPdfString(encodedName) != identifier) {
                continue;
            }

            const qsizetype topLevelDictionary = findBytes(
                view.data(),
                objectValueEnd,
                "<<");
            qsizetype dictionaryStart = -1;
            qsizetype dictionaryEnd = -1;
            qsizetype candidateStart = topLevelDictionary;
            while (candidateStart >= 0 && candidateStart < nameKey) {
                qsizetype candidateEnd = -1;
                if (!matchingDictionaryEnd(
                        view.data(),
                        objectValueEnd,
                        candidateStart,
                        candidateEnd)) {
                    return {};
                }
                if (candidateEnd > nameKey
                    && candidateStart > dictionaryStart) {
                    dictionaryStart = candidateStart;
                    dictionaryEnd = candidateEnd;
                }
                candidateStart = findBytes(
                    view.data(),
                    objectValueEnd,
                    "<<",
                    candidateStart + 2);
            }
            if (dictionaryStart < 0
                || dictionaryEnd <= dictionaryStart
                || topLevelDictionary < 0) {
                return {};
            }
            const char *dictionaryData = view.data() + dictionaryStart;
            const qsizetype dictionaryLength
                = dictionaryEnd - dictionaryStart;
            const qsizetype subtypeKey = keyPosition(
                dictionaryData,
                dictionaryLength,
                "/Subtype");
            Cursor subtypeCursor(
                dictionaryData,
                dictionaryLength,
                subtypeKey >= 0
                    ? subtypeKey + QByteArrayView("/Subtype").size()
                    : dictionaryLength);
            if (subtypeKey < 0
                || (!expectedSubtype.isEmpty()
                    && !subtypeCursor.consume(expectedSubtype))) {
                continue;
            }
            const ObjectReference objectReference{
                entry.key(),
                entry->generation};
            if (dictionaryStart == objectValueStart) {
                return PlaceholderLocation{
                    {},
                    objectReference,
                    -1,
                    -1};
            }
            if (directCandidate.container.number < 0) {
                directCandidate = PlaceholderLocation{
                    objectReference,
                    {},
                    dictionaryStart,
                    dictionaryEnd};
            }
        }
    }
    return directCandidate;
}

PlaceholderLocation annotationForWidgetId(
    const ParsedPdf &pdf,
    const QString &widgetId,
    QByteArrayView expectedSubtype)
{
    return annotationForStringId(
        pdf,
        QByteArrayView("/NM"),
        widgetId,
        expectedSubtype);
}

QByteArray encodedPdfString(const QString &value)
{
    if (value.isEmpty()) {
        return QByteArrayLiteral("<>");
    }
    QByteArray bytes;
    bytes.reserve((value.size() + 1) * 2);
    bytes.append(char(0xfe));
    bytes.append(char(0xff));
    for (QChar character : value) {
        const ushort codeUnit = character.unicode();
        bytes.append(static_cast<char>((codeUnit >> 8) & 0xff));
        bytes.append(static_cast<char>(codeUnit & 0xff));
    }
    return '<' + bytes.toHex().toUpper() + '>';
}

QByteArray encodedPdfStringArray(const QStringList &values)
{
    QByteArray array = QByteArrayLiteral("[");
    for (const QString &value : values) {
        array += encodedPdfString(value);
        array += ' ';
    }
    if (array.endsWith(' ')) {
        array.chop(1);
    }
    array += ']';
    return array;
}

QByteArray encodedPdfIntegerArray(const QVector<int> &values)
{
    QByteArray array = QByteArrayLiteral("[");
    for (int value : values) {
        array += QByteArray::number(value);
        array += ' ';
    }
    if (array.endsWith(' ')) {
        array.chop(1);
    }
    array += ']';
    return array;
}

QByteArray pdfNumber(qreal value)
{
    QByteArray number = QByteArray::number(value, 'f', 4);
    while (number.contains('.') && number.endsWith('0')) {
        number.chop(1);
    }
    if (number.endsWith('.')) {
        number.chop(1);
    }
    return number == "-0" ? QByteArrayLiteral("0") : number;
}

QByteArray uppercaseHex(quint32 value, qsizetype width)
{
    return QByteArray::number(value, 16)
        .rightJustified(width, '0')
        .toUpper();
}

QByteArray unicodeHex(uint codePoint)
{
    if (codePoint <= 0xffffU) {
        return uppercaseHex(codePoint, 4);
    }
    const uint supplementary = codePoint - 0x10000U;
    const quint32 highSurrogate
        = 0xd800U + ((supplementary >> 10U) & 0x3ffU);
    const quint32 lowSurrogate
        = 0xdc00U + (supplementary & 0x3ffU);
    return uppercaseHex(highSurrogate, 4)
        + uppercaseHex(lowSurrogate, 4);
}

QByteArray deflatedStream(const QByteArray &source)
{
    QByteArray compressed = qCompress(source, 9);
    if (compressed.size() <= 4) {
        return {};
    }
    compressed.remove(0, 4);
    return compressed;
}

UnicodeFormFontTemplate buildUnicodeFormFontTemplate()
{
    UnicodeFormFontTemplate result;
    QFile fontFile(QString::fromLatin1(kUnicodeFormFontPath));
    if (!fontFile.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral(
            "The bundled AcroForm font could not be opened.");
        return result;
    }
    const QByteArray fontData = fontFile.readAll();
    if (fontData.isEmpty()
        || fontData.size() > std::numeric_limits<quint32>::max()) {
        result.error = QStringLiteral(
            "The bundled AcroForm font data is invalid.");
        return result;
    }

    constexpr qreal metricPixelSize = 1000.0;
    const QRawFont rawFont(
        fontData,
        metricPixelSize,
        QFont::PreferNoHinting);
    if (!rawFont.isValid()) {
        result.error = QStringLiteral(
            "The bundled AcroForm font could not be decoded.");
        return result;
    }

    // Identity-H uses two-byte Unicode values as CIDs. Keep those CIDs stable
    // and provide an explicit CID-to-glyph map so form engines can resolve
    // Unicode in both directions while regenerating variable text.
    QMap<quint32, quint32> glyphByCodePoint;
    for (uint codePoint = 0x20U; codePoint <= 0xfffdU; ++codePoint) {
        if (codePoint >= 0xd800U && codePoint <= 0xdfffU) {
            continue;
        }
        if (!rawFont.supportsCharacter(codePoint)) {
            continue;
        }
        const char32_t character = static_cast<char32_t>(codePoint);
        const auto glyphs = rawFont.glyphIndexesForString(
            QString::fromUcs4(&character, 1));
        if (glyphs.size() != 1
            || glyphs.constFirst() == 0
            || glyphs.constFirst() > 0xffffU) {
            continue;
        }
        glyphByCodePoint.insert(codePoint, glyphs.constFirst());
    }
    if (!glyphByCodePoint.contains(0x1ecdU)) {
        result.error = QStringLiteral(
            "The bundled AcroForm font does not contain Vietnamese glyphs.");
        return result;
    }

    const QList<quint32> codePoints = glyphByCodePoint.keys();
    QList<quint32> glyphIndexes;
    glyphIndexes.reserve(codePoints.size());
    for (quint32 codePoint : codePoints) {
        glyphIndexes.append(glyphByCodePoint.value(codePoint));
    }
    const QList<QPointF> advances = rawFont.advancesForGlyphIndexes(
        glyphIndexes);
    if (advances.size() != glyphIndexes.size()) {
        result.error = QStringLiteral(
            "The bundled AcroForm font metrics could not be read.");
        return result;
    }

    QByteArray widths = QByteArrayLiteral("[");
    qint64 totalWidth = 0;
    int maximumWidth = 0;
    QRectF fontBounds;
    bool hasFontBounds = false;
    qsizetype runStart = 0;
    while (runStart < codePoints.size()) {
        qsizetype runEnd = runStart + 1;
        while (runEnd < codePoints.size()
               && codePoints.at(runEnd)
                   == codePoints.at(runEnd - 1) + 1U) {
            ++runEnd;
        }
        widths += QByteArray::number(codePoints.at(runStart));
        widths += QByteArrayLiteral(" [");
        for (qsizetype index = runStart; index < runEnd; ++index) {
            const int width = qMax(0, qRound(advances.at(index).x()));
            totalWidth += width;
            maximumWidth = qMax(maximumWidth, width);
            widths += QByteArray::number(width);
            widths += ' ';

            const QRectF glyphBounds = rawFont.boundingRect(
                glyphIndexes.at(index));
            if (!glyphBounds.isEmpty()) {
                fontBounds = hasFontBounds
                    ? fontBounds.united(glyphBounds)
                    : glyphBounds;
                hasFontBounds = true;
            }
        }
        if (widths.endsWith(' ')) {
            widths.chop(1);
        }
        widths += QByteArrayLiteral("] ");
        runStart = runEnd;
    }
    if (widths.endsWith(' ')) {
        widths.chop(1);
    }
    widths += ']';

    QByteArray cidToGidMap(0x10000 * 2, '\0');
    for (qsizetype index = 0; index < codePoints.size(); ++index) {
        const quint32 codePoint = codePoints.at(index);
        const quint32 glyph = glyphIndexes.at(index);
        const qsizetype offset = static_cast<qsizetype>(codePoint) * 2;
        cidToGidMap[offset]
            = static_cast<char>((glyph >> 8U) & 0xffU);
        cidToGidMap[offset + 1]
            = static_cast<char>(glyph & 0xffU);
    }

    QByteArray toUnicode = QByteArrayLiteral(
        "/CIDInit /ProcSet findresource begin\n"
        "12 dict begin\n"
        "begincmap\n"
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) "
        "/Supplement 0 >> def\n"
        "/CMapName /TrePDFArial-UCS def\n"
        "/CMapType 2 def\n"
        "1 begincodespacerange\n"
        "<0000> <FFFF>\n"
        "endcodespacerange\n");
    constexpr qsizetype maximumMappingsPerBlock = 100;
    for (qsizetype start = 0;
         start < codePoints.size();
         start += maximumMappingsPerBlock) {
        const qsizetype count = qMin(
            maximumMappingsPerBlock,
            codePoints.size() - start);
        toUnicode += QByteArray::number(count);
        toUnicode += QByteArrayLiteral(" beginbfchar\n");
        for (qsizetype index = start; index < start + count; ++index) {
            const quint32 codePoint = codePoints.at(index);
            toUnicode += '<';
            toUnicode += uppercaseHex(codePoint, 4);
            toUnicode += QByteArrayLiteral("> <");
            toUnicode += unicodeHex(codePoint);
            toUnicode += QByteArrayLiteral(">\n");
        }
        toUnicode += QByteArrayLiteral("endbfchar\n");
    }
    toUnicode += QByteArrayLiteral(
        "endcmap\n"
        "CMapName currentdict /CMap defineresource pop\n"
        "end\n"
        "end\n");

    const auto glyphHeight = [&rawFont](char32_t character) {
        const auto glyphs = rawFont.glyphIndexesForString(
            QString::fromUcs4(&character, 1));
        return glyphs.isEmpty()
            ? 0
            : qMax(0, qRound(-rawFont.boundingRect(
                  glyphs.constFirst()).top()));
    };
    result.compressedFontData = deflatedStream(fontData);
    result.compressedCidToGidMap = deflatedStream(cidToGidMap);
    result.widths = std::move(widths);
    result.toUnicode = std::move(toUnicode);
    result.uncompressedFontSize = fontData.size();
    result.ascent = qMax(0, qRound(rawFont.ascent()));
    result.descent = -qMax(0, qRound(rawFont.descent()));
    result.capHeight = glyphHeight(U'H');
    result.xHeight = glyphHeight(U'x');
    result.leading = qMax(0, qRound(rawFont.leading()));
    result.averageWidth = codePoints.isEmpty()
        ? 0
        : static_cast<int>(totalWidth / codePoints.size());
    result.maximumWidth = maximumWidth;
    if (hasFontBounds) {
        result.boundingLeft = qFloor(fontBounds.left());
        result.boundingBottom = qFloor(-fontBounds.bottom());
        result.boundingRight = qCeil(fontBounds.right());
        result.boundingTop = qCeil(-fontBounds.top());
    }
    if (!result.isValid()) {
        result.error = QStringLiteral(
            "The bundled AcroForm font resources could not be generated.");
    }
    return result;
}

const UnicodeFormFontTemplate &unicodeFormFontTemplate()
{
    static const UnicodeFormFontTemplate font
        = buildUnicodeFormFontTemplate();
    return font;
}

QByteArray textFieldDefaultAppearance(QByteArrayView fontResourceName)
{
    return QByteArrayLiteral("/")
        + QByteArray(fontResourceName.data(), fontResourceName.size())
        + ' '
        + pdfNumber(PDFFormAppearance::kTextFieldFontPointSize)
        + QByteArrayLiteral(" Tf 0 g");
}

QImage textFieldAppearanceImage(
    const PDFAcroFormCanonicalizer::Field &field,
    bool dropdown = false)
{
    constexpr qreal rasterScale = 2.0;
    constexpr int maximumDimension = 2048;
    const int pixelWidth = qBound(
        2,
        qRound(field.pageRect.width() * rasterScale),
        maximumDimension);
    const int pixelHeight = qBound(
        2,
        qRound(field.pageRect.height() * rasterScale),
        maximumDimension);
    QImage image(pixelWidth, pixelHeight, QImage::Format_RGB888);
    if (image.isNull()) {
        return {};
    }
    image.fill(Qt::white);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const qreal pixelsPerPointX
        = field.pageRect.width() > 0.0
        ? pixelWidth / field.pageRect.width()
        : rasterScale;
    const qreal pixelsPerPointY
        = field.pageRect.height() > 0.0
        ? pixelHeight / field.pageRect.height()
        : rasterScale;
    const qreal borderPixelWidth = qMax(
        1.0,
        PDFFormAppearance::kTextFieldBorderPointWidth
            * qMin(pixelsPerPointX, pixelsPerPointY));
    painter.setPen(QPen(
        QColor::fromRgb(PDFFormAppearance::kTextFieldBorderRgb),
        borderPixelWidth));
    painter.setBrush(Qt::NoBrush);
    const qreal borderInset = qMax(0.5, borderPixelWidth / 2.0);
    painter.drawRect(
        QRectF(
            borderInset,
            borderInset,
            pixelWidth - 2.0 * borderInset,
            pixelHeight - 2.0 * borderInset));

    QFont font(QStringLiteral("Arial"));
    font.setPixelSize(qMax(
        1,
        qRound(
            PDFFormAppearance::kTextFieldFontPointSize
            * pixelsPerPointY)));
    painter.setFont(font);
    painter.setPen(QColor(32, 33, 36));
    const qreal horizontalPadding = qMax(
        PDFFormAppearance::kTextFieldMinimumHorizontalPaddingPoints
            * pixelsPerPointX,
        pixelHeight
            * PDFFormAppearance::kTextFieldHorizontalPaddingRatio);
    const qreal dropdownButtonWidth = dropdown
        ? qMin<qreal>(pixelHeight, pixelWidth / 3.0)
        : 0.0;
    const bool multiline = !dropdown
        && (field.fieldFlags & kTextFieldMultiline);
    const bool comb = !dropdown
        && !multiline
        && field.maxLength > 0
        && (field.fieldFlags & kTextFieldComb);
    const qreal verticalPadding = multiline
        ? qMax(borderPixelWidth + 1.0, horizontalPadding * 0.25)
        : 0.0;
    const QRectF textRect(
        horizontalPadding,
        verticalPadding,
        qMax(
            0.0,
            pixelWidth - 2.0 * horizontalPadding
                - dropdownButtonWidth),
        qMax(0.0, pixelHeight - 2.0 * verticalPadding));
    if (multiline) {
        QTextOption textOption;
        textOption.setAlignment(Qt::AlignLeft | Qt::AlignTop);
        textOption.setWrapMode(
            QTextOption::WrapAtWordBoundaryOrAnywhere);
        painter.drawText(textRect, field.value, textOption);
    } else if (comb) {
        const QRectF combRect(
            borderPixelWidth,
            borderPixelWidth,
            qMax(0.0, pixelWidth - 2.0 * borderPixelWidth),
            qMax(0.0, pixelHeight - 2.0 * borderPixelWidth));
        const qreal slotWidth = combRect.width() / field.maxLength;
        if (slotWidth >= 1.0) {
            painter.setPen(QPen(
                QColor::fromRgb(
                    PDFFormAppearance::kTextFieldBorderRgb),
                borderPixelWidth));
            for (int slot = 1; slot < field.maxLength; ++slot) {
                const qreal x = combRect.left() + slotWidth * slot;
                painter.drawLine(
                    QPointF(x, combRect.top()),
                    QPointF(x, combRect.bottom()));
            }

            painter.setPen(QColor(32, 33, 36));
            int valueOffset = 0;
            for (int slot = 0;
                 slot < field.maxLength
                 && valueOffset < field.value.size();
                 ++slot) {
                int characterLength = 1;
                if (field.value.at(valueOffset).isHighSurrogate()
                    && valueOffset + 1 < field.value.size()
                    && field.value.at(valueOffset + 1).isLowSurrogate()) {
                    characterLength = 2;
                }
                const QRectF slotRect(
                    combRect.left() + slotWidth * slot,
                    combRect.top(),
                    slotWidth,
                    combRect.height());
                painter.drawText(
                    slotRect,
                    Qt::AlignCenter | Qt::TextSingleLine,
                    field.value.mid(valueOffset, characterLength));
                valueOffset += characterLength;
            }
        } else {
            // More slots than visible pixels cannot be represented faithfully.
            // Draw the value once and avoid an unbounded divider/cell loop.
            painter.setPen(QColor(32, 33, 36));
            painter.drawText(
                textRect,
                Qt::AlignLeft | Qt::AlignVCenter
                    | Qt::TextSingleLine,
                field.value);
        }
    } else {
        painter.drawText(
            textRect,
            Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
            field.value);
    }
    if (dropdown) {
        const qreal buttonLeft = pixelWidth - dropdownButtonWidth;
        painter.setPen(QPen(
            QColor::fromRgb(PDFFormAppearance::kTextFieldBorderRgb),
            borderPixelWidth));
        painter.drawLine(
            QPointF(buttonLeft, borderInset),
            QPointF(buttonLeft, pixelHeight - borderInset));
        const qreal arrowHalfWidth = qMax(
            2.0,
            dropdownButtonWidth * 0.18);
        const qreal arrowHalfHeight = qMax(1.5, arrowHalfWidth * 0.55);
        const QPointF center(
            buttonLeft + dropdownButtonWidth / 2.0,
            pixelHeight / 2.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(32, 33, 36));
        painter.drawPolygon(QPolygonF({
            QPointF(center.x() - arrowHalfWidth,
                    center.y() - arrowHalfHeight),
            QPointF(center.x() + arrowHalfWidth,
                    center.y() - arrowHalfHeight),
            QPointF(center.x(), center.y() + arrowHalfHeight),
        }));
    }
    painter.end();
    return image;
}

QImage freeTextAppearanceImage(const QRectF &pageRect,
                               const QString &text)
{
    constexpr qreal rasterScale = 2.0;
    constexpr int maximumDimension = 2048;
    constexpr qreal borderPointWidth = 1.0;
    constexpr qreal paddingPoints = 4.0;
    const int pixelWidth = qBound(
        2,
        qRound(pageRect.width() * rasterScale),
        maximumDimension);
    const int pixelHeight = qBound(
        2,
        qRound(pageRect.height() * rasterScale),
        maximumDimension);
    QImage image(pixelWidth, pixelHeight, QImage::Format_RGB888);
    if (image.isNull()) {
        return {};
    }
    image.fill(Qt::white);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    const qreal pixelsPerPointX = pageRect.width() > 0.0
        ? pixelWidth / pageRect.width()
        : rasterScale;
    const qreal pixelsPerPointY = pageRect.height() > 0.0
        ? pixelHeight / pageRect.height()
        : rasterScale;
    const qreal borderPixelWidth = qMax(
        1.0,
        borderPointWidth * qMin(pixelsPerPointX, pixelsPerPointY));
    const qreal borderInset = qMax(0.5, borderPixelWidth / 2.0);
    painter.setPen(QPen(Qt::black, borderPixelWidth));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(QRectF(
        borderInset,
        borderInset,
        pixelWidth - 2.0 * borderInset,
        pixelHeight - 2.0 * borderInset));

    QFont font(QStringLiteral("Arial"));
    font.setPixelSize(qMax(
        1,
        qRound(
            PDFFormAppearance::kTextFieldFontPointSize
            * pixelsPerPointY)));
    painter.setFont(font);
    painter.setPen(QColor(32, 33, 36));
    const qreal horizontalPadding = qMax(
        borderPixelWidth + 1.0,
        paddingPoints * pixelsPerPointX);
    const qreal verticalPadding = qMax(
        borderPixelWidth + 1.0,
        paddingPoints * pixelsPerPointY);
    const QRectF textRect(
        horizontalPadding,
        verticalPadding,
        qMax(0.0, pixelWidth - 2.0 * horizontalPadding),
        qMax(0.0, pixelHeight - 2.0 * verticalPadding));
    painter.setClipRect(textRect);
    QTextOption textOption;
    textOption.setAlignment(Qt::AlignLeft | Qt::AlignTop);
    textOption.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    painter.drawText(textRect, text, textOption);
    painter.end();
    return image;
}

QImage listBoxAppearanceImage(
    const PDFAcroFormCanonicalizer::Field &field)
{
    constexpr qreal rasterScale = 2.0;
    constexpr int maximumDimension = 2048;
    const int pixelWidth = qBound(
        2,
        qRound(field.pageRect.width() * rasterScale),
        maximumDimension);
    const int pixelHeight = qBound(
        2,
        qRound(field.pageRect.height() * rasterScale),
        maximumDimension);
    QImage image(pixelWidth, pixelHeight, QImage::Format_RGB888);
    if (image.isNull()) {
        return {};
    }
    image.fill(Qt::white);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const qreal pixelsPerPointX
        = field.pageRect.width() > 0.0
        ? pixelWidth / field.pageRect.width()
        : rasterScale;
    const qreal pixelsPerPointY
        = field.pageRect.height() > 0.0
        ? pixelHeight / field.pageRect.height()
        : rasterScale;
    const qreal borderPixelWidth = qMax(
        1.0,
        PDFFormAppearance::kTextFieldBorderPointWidth
            * qMin(pixelsPerPointX, pixelsPerPointY));
    const qreal borderInset = qMax(0.5, borderPixelWidth / 2.0);
    const QRectF borderRect(
        borderInset,
        borderInset,
        pixelWidth - 2.0 * borderInset,
        pixelHeight - 2.0 * borderInset);
    painter.setPen(QPen(
        QColor::fromRgb(PDFFormAppearance::kTextFieldBorderRgb),
        borderPixelWidth));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(borderRect);

    const QRectF contentRect(
        borderPixelWidth,
        borderPixelWidth,
        qMax(0.0, pixelWidth - 2.0 * borderPixelWidth),
        qMax(0.0, pixelHeight - 2.0 * borderPixelWidth));
    QFont font(QStringLiteral("Arial"));
    font.setPixelSize(qMax(
        1,
        qRound(
            PDFFormAppearance::kTextFieldFontPointSize
            * pixelsPerPointY)));
    painter.setFont(font);
    painter.setClipRect(contentRect);

    const QVector<int> selectedIndices = listBoxSelectedIndices(
        field.options,
        field.selectedValues);
    const int firstSelectedIndex = selectedIndices.isEmpty()
        ? 0
        : selectedIndices.constFirst();
    const int topIndex = PDFFormAppearance::listBoxTopIndex(
        field.options.size(),
        firstSelectedIndex,
        field.pageRect.height());
    const qreal rowHeight = qMax(
        1.0,
        PDFFormAppearance::kTextFieldFontPointSize
            * PDFFormAppearance::kListBoxRowHeightRatio
            * pixelsPerPointY);
    const qreal horizontalPadding = qMax(
        PDFFormAppearance::kTextFieldMinimumHorizontalPaddingPoints
            * pixelsPerPointX,
        rowHeight
            * PDFFormAppearance::kTextFieldHorizontalPaddingRatio);
    qreal rowTop = contentRect.top();
    for (int optionIndex = topIndex;
         optionIndex < field.options.size()
             && rowTop < contentRect.bottom();
         ++optionIndex) {
        const QRectF rowRect(
            contentRect.left(),
            rowTop,
            contentRect.width(),
            qMin(rowHeight, contentRect.bottom() - rowTop));
        const bool selected = field.selectedValues.contains(
            field.options.at(optionIndex));
        if (selected) {
            painter.fillRect(rowRect, QColor(51, 122, 183));
        }
        painter.setPen(selected ? Qt::white : QColor(32, 33, 36));
        painter.drawText(
            rowRect.adjusted(horizontalPadding, 0.0, -horizontalPadding, 0.0),
            Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
            field.options.at(optionIndex));
        rowTop += rowHeight;
    }
    painter.end();
    return image;
}

QByteArray compressedRgbPixels(const QImage &source)
{
    const QImage image = source.convertToFormat(QImage::Format_RGB888);
    if (image.isNull()) {
        return {};
    }
    QByteArray pixels;
    const qsizetype rowBytes = static_cast<qsizetype>(image.width()) * 3;
    pixels.reserve(rowBytes * image.height());
    for (int row = 0; row < image.height(); ++row) {
        pixels.append(
            reinterpret_cast<const char *>(image.constScanLine(row)),
            rowBytes);
    }
    QByteArray compressed = qCompress(pixels, 9);
    if (compressed.size() <= 4) {
        return {};
    }
    compressed.remove(0, 4);
    return compressed;
}

QByteArray imageObject(int objectNumber,
                       const QImage &image,
                       const QByteArray &compressedPixels)
{
    QByteArray object = QByteArray::number(objectNumber)
        + QByteArrayLiteral(
            " 0 obj\n"
            "<< /Type /XObject /Subtype /Image /Width ")
        + QByteArray::number(image.width())
        + QByteArrayLiteral(" /Height ")
        + QByteArray::number(image.height())
        + QByteArrayLiteral(
            " /ColorSpace /DeviceRGB /BitsPerComponent 8 "
            "/Filter /FlateDecode /Length ")
        + QByteArray::number(compressedPixels.size())
        + QByteArrayLiteral(" >>\nstream\n");
    object += compressedPixels;
    object += QByteArrayLiteral("\nendstream\nendobj");
    return object;
}

QByteArray appearanceObject(int objectNumber,
                            int imageObjectNumber,
                            const QRectF &pageRect)
{
    const QByteArray width = pdfNumber(pageRect.width());
    const QByteArray height = pdfNumber(pageRect.height());
    const QByteArray content = QByteArrayLiteral("q ")
        + width
        + QByteArrayLiteral(" 0 0 ")
        + height
        + QByteArrayLiteral(" 0 0 cm /Im0 Do Q");
    return QByteArray::number(objectNumber)
        + QByteArrayLiteral(
            " 0 obj\n"
            "<< /Type /XObject /Subtype /Form /FormType 1 /BBox [0 0 ")
        + width
        + ' '
        + height
        + QByteArrayLiteral(
            "] /Resources << /XObject << /Im0 ")
        + QByteArray::number(imageObjectNumber)
        + QByteArrayLiteral(
            " 0 R >> >> /Length ")
        + QByteArray::number(content.size())
        + QByteArrayLiteral(" >>\nstream\n")
        + content
        + QByteArrayLiteral("\nendstream\nendobj");
}

QByteArray textFieldWidgetObject(
    const ObjectReference &reference,
    const PDFAcroFormCanonicalizer::Field &field,
    int appearanceObjectNumber,
    const ObjectReference &pageReference,
    QByteArrayView fontResourceName)
{
    const QByteArray left = pdfNumber(field.pageRect.left());
    const QByteArray bottom = pdfNumber(field.pageRect.top());
    const QByteArray right = pdfNumber(field.pageRect.right());
    const QByteArray top = pdfNumber(field.pageRect.bottom());
    QByteArray object = QByteArray::number(reference.number)
        + ' '
        + QByteArray::number(reference.generation)
        + QByteArrayLiteral(
            " obj\n"
            "<< /Type /Annot /Subtype /Widget /FT /Tx /Rect [")
        + left
        + ' '
        + bottom
        + ' '
        + right
        + ' '
        + top
        + QByteArrayLiteral("] /T ")
        + encodedPdfString(field.fieldName)
        + QByteArrayLiteral(" /TU ")
        + encodedPdfString(field.tooltip)
        + QByteArrayLiteral(" /V ")
        + encodedPdfString(field.value)
        + QByteArrayLiteral(" /DV ")
        + encodedPdfString(field.defaultValue)
        + QByteArrayLiteral(" /NM ")
        + encodedPdfString(field.widgetId)
        + QByteArrayLiteral(" /Ff ")
        + QByteArray::number(field.fieldFlags);
    if (field.maxLength > 0) {
        object += QByteArrayLiteral(" /MaxLen ")
            + QByteArray::number(field.maxLength);
    }
    if (pageReference.number >= 0) {
        object += QByteArrayLiteral(" /P ")
            + QByteArray::number(pageReference.number)
            + ' '
            + QByteArray::number(pageReference.generation)
            + QByteArrayLiteral(" R");
    }
    object += QByteArrayLiteral(" /F 4 /DA (")
        + textFieldDefaultAppearance(fontResourceName)
        + QByteArrayLiteral(
            ") "
            "/MK << /BG [1 1 1] /BC [0.55 0.55 0.55] >> "
            "/BS << /W 1 /S /S >> /AP << /N ")
        + QByteArray::number(appearanceObjectNumber)
        + QByteArrayLiteral(" 0 R >> /")
        + QByteArray(PDFFormAppearance::kVersionKey)
        + ' '
        + QByteArray::number(PDFFormAppearance::kTextFieldVersion)
        + QByteArrayLiteral(" >>\nendobj");
    return object;
}

QByteArray choiceWidgetObject(
    const ObjectReference &reference,
    const PDFAcroFormCanonicalizer::Field &field,
    int appearanceObjectNumber,
    const ObjectReference &pageReference,
    QByteArrayView fontResourceName,
    bool listBox)
{
    const int dropdownSelectedIndex = field.options.indexOf(field.value);
    const QVector<int> selectedIndices = listBox
        ? listBoxSelectedIndices(field.options, field.selectedValues)
        : QVector<int>({dropdownSelectedIndex});
    if ((!listBox && dropdownSelectedIndex < 0)
        || !field.options.contains(field.defaultValue)) {
        return {};
    }
    const QByteArray left = pdfNumber(field.pageRect.left());
    const QByteArray bottom = pdfNumber(field.pageRect.top());
    const QByteArray right = pdfNumber(field.pageRect.right());
    const QByteArray top = pdfNumber(field.pageRect.bottom());
    QByteArray object = QByteArray::number(reference.number)
        + ' '
        + QByteArray::number(reference.generation)
        + QByteArrayLiteral(
            " obj\n"
            "<< /Type /Annot /Subtype /Widget /FT /Ch /Rect [")
        + left
        + ' '
        + bottom
        + ' '
        + right
        + ' '
        + top
        + QByteArrayLiteral("] /T ")
        + encodedPdfString(field.fieldName)
        + QByteArrayLiteral(" /TU ")
        + encodedPdfString(field.tooltip)
        + QByteArrayLiteral(" /Opt ")
        + encodedPdfStringArray(field.options)
        + QByteArrayLiteral(" /V ")
        + (listBox
               ? encodedPdfStringArray(field.selectedValues)
               : encodedPdfString(field.value))
        + QByteArrayLiteral(" /DV ")
        + encodedPdfString(field.defaultValue)
        + QByteArrayLiteral(" /I ")
        + encodedPdfIntegerArray(selectedIndices);
    if (listBox) {
        const int firstSelectedIndex = selectedIndices.isEmpty()
            ? 0
            : selectedIndices.constFirst();
        object += QByteArrayLiteral(" /TI ")
            + QByteArray::number(
                PDFFormAppearance::listBoxTopIndex(
                    field.options.size(),
                    firstSelectedIndex,
                    field.pageRect.height()));
    }
    object += QByteArrayLiteral(" /NM ")
        + encodedPdfString(field.widgetId)
        + QByteArrayLiteral(" /Ff ")
        + QByteArray::number(field.fieldFlags);
    if (pageReference.number >= 0) {
        object += QByteArrayLiteral(" /P ")
            + QByteArray::number(pageReference.number)
            + ' '
            + QByteArray::number(pageReference.generation)
            + QByteArrayLiteral(" R");
    }
    object += QByteArrayLiteral(" /F 4 /DA (")
        + textFieldDefaultAppearance(fontResourceName)
        + QByteArrayLiteral(
            ") "
            "/MK << /BG [1 1 1] /BC [0.55 0.55 0.55] >> "
            "/BS << /W 1 /S /S >> /AP << /N ")
        + QByteArray::number(appearanceObjectNumber)
        + QByteArrayLiteral(" 0 R >> /")
        + QByteArray(PDFFormAppearance::kVersionKey)
        + ' '
        + QByteArray::number(
            listBox
                ? PDFFormAppearance::kListBoxVersion
                : PDFFormAppearance::kDropdownVersion)
        + QByteArrayLiteral(" >>\nendobj");
    return object;
}

bool checkboxChecked(const QString &value)
{
    return value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0
        || value.compare(QStringLiteral("yes"), Qt::CaseInsensitive) == 0
        || value.compare(QStringLiteral("on"), Qt::CaseInsensitive) == 0
        || value == QStringLiteral("1");
}

QByteArray checkboxAppearanceObject(int objectNumber,
                                    const QRectF &pageRect,
                                    bool checked)
{
    const qreal widthValue = pageRect.width();
    const qreal heightValue = pageRect.height();
    const qreal shortestSide = qMin(widthValue, heightValue);
    const qreal borderWidth = qBound(0.5, shortestSide / 16.0, 1.5);
    const qreal borderInset = borderWidth / 2.0;
    const QByteArray width = pdfNumber(widthValue);
    const QByteArray height = pdfNumber(heightValue);
    QByteArray content = QByteArrayLiteral("q 1 1 1 rg 0 0 ")
        + width
        + ' '
        + height
        + QByteArrayLiteral(" re f 0.55 0.55 0.55 RG ")
        + pdfNumber(borderWidth)
        + QByteArrayLiteral(" w ")
        + pdfNumber(borderInset)
        + ' '
        + pdfNumber(borderInset)
        + ' '
        + pdfNumber(qMax(0.0, widthValue - borderWidth))
        + ' '
        + pdfNumber(qMax(0.0, heightValue - borderWidth))
        + QByteArrayLiteral(" re S");
    if (checked) {
        content += QByteArrayLiteral(" 0.12 0.12 0.12 RG ")
            + pdfNumber(qBound(1.0, shortestSide / 9.0, 2.5))
            + QByteArrayLiteral(" w 1 J 1 j ")
            + pdfNumber(widthValue * 0.20)
            + ' '
            + pdfNumber(heightValue * 0.52)
            + QByteArrayLiteral(" m ")
            + pdfNumber(widthValue * 0.43)
            + ' '
            + pdfNumber(heightValue * 0.28)
            + QByteArrayLiteral(" l ")
            + pdfNumber(widthValue * 0.82)
            + ' '
            + pdfNumber(heightValue * 0.76)
            + QByteArrayLiteral(" l S");
    }
    content += QByteArrayLiteral(" Q");

    return QByteArray::number(objectNumber)
        + QByteArrayLiteral(
            " 0 obj\n"
            "<< /Type /XObject /Subtype /Form /FormType 1 /BBox [0 0 ")
        + width
        + ' '
        + height
        + QByteArrayLiteral("] /Resources << >> /Length ")
        + QByteArray::number(content.size())
        + QByteArrayLiteral(" >>\nstream\n")
        + content
        + QByteArrayLiteral("\nendstream\nendobj");
}

QByteArray checkboxWidgetObject(
    const ObjectReference &reference,
    const PDFAcroFormCanonicalizer::Field &field,
    int offAppearanceObjectNumber,
    int onAppearanceObjectNumber,
    const ObjectReference &pageReference)
{
    const QByteArray left = pdfNumber(field.pageRect.left());
    const QByteArray bottom = pdfNumber(field.pageRect.top());
    const QByteArray right = pdfNumber(field.pageRect.right());
    const QByteArray top = pdfNumber(field.pageRect.bottom());
    const bool checked = checkboxChecked(field.value);
    const bool defaultChecked = checkboxChecked(field.defaultValue);
    const QByteArray currentState = checked
        ? QByteArrayLiteral("/Yes")
        : QByteArrayLiteral("/Off");
    const QByteArray defaultState = defaultChecked
        ? QByteArrayLiteral("/Yes")
        : QByteArrayLiteral("/Off");
    QByteArray object = QByteArray::number(reference.number)
        + ' '
        + QByteArray::number(reference.generation)
        + QByteArrayLiteral(
            " obj\n"
            "<< /Type /Annot /Subtype /Widget /FT /Btn /Rect [")
        + left
        + ' '
        + bottom
        + ' '
        + right
        + ' '
        + top
        + QByteArrayLiteral("] /T ")
        + encodedPdfString(field.fieldName)
        + QByteArrayLiteral(" /TU ")
        + encodedPdfString(field.tooltip)
        + QByteArrayLiteral(" /V ")
        + currentState
        + QByteArrayLiteral(" /DV ")
        + defaultState
        + QByteArrayLiteral(" /AS ")
        + currentState
        + QByteArrayLiteral(" /NM ")
        + encodedPdfString(field.widgetId)
        + QByteArrayLiteral(" /Ff ")
        + QByteArray::number(field.fieldFlags);
    if (pageReference.number >= 0) {
        object += QByteArrayLiteral(" /P ")
            + QByteArray::number(pageReference.number)
            + ' '
            + QByteArray::number(pageReference.generation)
            + QByteArrayLiteral(" R");
    }
    object += QByteArrayLiteral(
        " /F 4 /MK << /BG [1 1 1] /BC [0.55 0.55 0.55] >> "
        "/BS << /W 1 /S /S >> /AP << /N << /Off ")
        + QByteArray::number(offAppearanceObjectNumber)
        + QByteArrayLiteral(" 0 R /Yes ")
        + QByteArray::number(onAppearanceObjectNumber)
        + QByteArrayLiteral(" 0 R >> >> /")
        + QByteArray(PDFFormAppearance::kVersionKey)
        + ' '
        + QByteArray::number(PDFFormAppearance::kCheckboxVersion)
        + QByteArrayLiteral(" >>\nendobj");
    return object;
}

QByteArray ellipsePath(qreal centerX,
                       qreal centerY,
                       qreal radiusX,
                       qreal radiusY)
{
    constexpr qreal kBezierCircleControl = 0.5522847498307936;
    const qreal controlX = radiusX * kBezierCircleControl;
    const qreal controlY = radiusY * kBezierCircleControl;
    return pdfNumber(centerX + radiusX)
        + ' '
        + pdfNumber(centerY)
        + QByteArrayLiteral(" m ")
        + pdfNumber(centerX + radiusX)
        + ' '
        + pdfNumber(centerY + controlY)
        + ' '
        + pdfNumber(centerX + controlX)
        + ' '
        + pdfNumber(centerY + radiusY)
        + ' '
        + pdfNumber(centerX)
        + ' '
        + pdfNumber(centerY + radiusY)
        + QByteArrayLiteral(" c ")
        + pdfNumber(centerX - controlX)
        + ' '
        + pdfNumber(centerY + radiusY)
        + ' '
        + pdfNumber(centerX - radiusX)
        + ' '
        + pdfNumber(centerY + controlY)
        + ' '
        + pdfNumber(centerX - radiusX)
        + ' '
        + pdfNumber(centerY)
        + QByteArrayLiteral(" c ")
        + pdfNumber(centerX - radiusX)
        + ' '
        + pdfNumber(centerY - controlY)
        + ' '
        + pdfNumber(centerX - controlX)
        + ' '
        + pdfNumber(centerY - radiusY)
        + ' '
        + pdfNumber(centerX)
        + ' '
        + pdfNumber(centerY - radiusY)
        + QByteArrayLiteral(" c ")
        + pdfNumber(centerX + controlX)
        + ' '
        + pdfNumber(centerY - radiusY)
        + ' '
        + pdfNumber(centerX + radiusX)
        + ' '
        + pdfNumber(centerY - controlY)
        + ' '
        + pdfNumber(centerX + radiusX)
        + ' '
        + pdfNumber(centerY)
        + QByteArrayLiteral(" c");
}

QByteArray radioButtonAppearanceObject(int objectNumber,
                                       const QRectF &pageRect,
                                       bool checked)
{
    const qreal widthValue = pageRect.width();
    const qreal heightValue = pageRect.height();
    const qreal shortestSide = qMin(widthValue, heightValue);
    const qreal visualSide
        = shortestSide * PDFFormAppearance::kRadioButtonVisualScale;
    const qreal borderWidth = qBound(
        PDFFormAppearance::kRadioButtonMinimumBorderPointWidth,
        visualSide / 12.0,
        PDFFormAppearance::kRadioButtonBorderPointWidth);
    const qreal centerX = widthValue / 2.0;
    const qreal centerY = heightValue / 2.0;
    const qreal outerRadius = qMax(
        0.0,
        visualSide / 2.0 - borderWidth / 2.0);
    const QByteArray width = pdfNumber(widthValue);
    const QByteArray height = pdfNumber(heightValue);
    QByteArray content = QByteArrayLiteral(
        "q 1 1 1 rg 0 0 0 RG ")
        + pdfNumber(borderWidth)
        + QByteArrayLiteral(" w ")
        + ellipsePath(
            centerX,
            centerY,
            outerRadius,
            outerRadius)
        + QByteArrayLiteral(" h B");
    if (checked) {
        const qreal dotRadius = outerRadius
            * PDFFormAppearance::kRadioButtonDotRadiusRatio;
        content += QByteArrayLiteral(" 0 0 0 rg ")
            + ellipsePath(
                centerX,
                centerY,
                dotRadius,
                dotRadius)
            + QByteArrayLiteral(" h f");
    }
    content += QByteArrayLiteral(" Q");

    return QByteArray::number(objectNumber)
        + QByteArrayLiteral(
            " 0 obj\n"
            "<< /Type /XObject /Subtype /Form /FormType 1 /BBox [0 0 ")
        + width
        + ' '
        + height
        + QByteArrayLiteral("] /Resources << >> /Length ")
        + QByteArray::number(content.size())
        + QByteArrayLiteral(" >>\nstream\n")
        + content
        + QByteArrayLiteral("\nendstream\nendobj");
}

QByteArray radioButtonFieldObject(
    const ObjectReference &reference,
    const QString &fieldName,
    const QString &tooltip,
    int fieldFlags,
    const QList<ObjectReference> &widgetReferences,
    const QByteArray &currentState,
    const QByteArray &defaultState)
{
    return QByteArray::number(reference.number)
        + ' '
        + QByteArray::number(reference.generation)
        + QByteArrayLiteral(" obj\n<< /FT /Btn /Ff ")
        + QByteArray::number(
            (fieldFlags & ~kButtonNoToggleToOff) | kButtonRadio)
        + QByteArrayLiteral(" /T ")
        + encodedPdfString(fieldName)
        + QByteArrayLiteral(" /TU ")
        + encodedPdfString(tooltip)
        + QByteArrayLiteral(" /V ")
        + currentState
        + QByteArrayLiteral(" /DV ")
        + defaultState
        + QByteArrayLiteral(" /Kids ")
        + referenceArray(widgetReferences)
        + QByteArrayLiteral(" >>\nendobj");
}

QByteArray radioButtonWidgetObject(
    const ObjectReference &reference,
    const ObjectReference &fieldReference,
    const PDFAcroFormCanonicalizer::Field &field,
    int offAppearanceObjectNumber,
    int onAppearanceObjectNumber,
    const ObjectReference &pageReference,
    const QByteArray &exportState)
{
    const QByteArray left = pdfNumber(field.pageRect.left());
    const QByteArray bottom = pdfNumber(field.pageRect.top());
    const QByteArray right = pdfNumber(field.pageRect.right());
    const QByteArray top = pdfNumber(field.pageRect.bottom());
    const bool checked = checkboxChecked(field.value);
    const bool defaultChecked = checkboxChecked(field.defaultValue);
    const QByteArray currentState = checked
        ? exportState
        : QByteArrayLiteral("/Off");
    QByteArray object = QByteArray::number(reference.number)
        + ' '
        + QByteArray::number(reference.generation)
        + QByteArrayLiteral(
            " obj\n"
            "<< /Type /Annot /Subtype /Widget /Rect [")
        + left
        + ' '
        + bottom
        + ' '
        + right
        + ' '
        + top
        + QByteArrayLiteral("] /Parent ")
        + QByteArray::number(fieldReference.number)
        + ' '
        + QByteArray::number(fieldReference.generation)
        + QByteArrayLiteral(" R /AS ")
        + currentState
        + QByteArrayLiteral(" /NM ")
        + encodedPdfString(field.widgetId)
        + QByteArrayLiteral(" /")
        + QByteArray(PDFFormAppearance::kRadioButtonDefaultCheckedKey)
        + ' '
        + (defaultChecked ? '1' : '0');
    if (pageReference.number >= 0) {
        object += QByteArrayLiteral(" /P ")
            + QByteArray::number(pageReference.number)
            + ' '
            + QByteArray::number(pageReference.generation)
            + QByteArrayLiteral(" R");
    }
    object += QByteArrayLiteral(
        " /F 4 /MK << /BG [1 1 1] /BC [0 0 0] >> "
        "/BS << /W 1 /S /S >> /AP << /N << /Off ")
        + QByteArray::number(offAppearanceObjectNumber)
        + QByteArrayLiteral(" 0 R ")
        + exportState
        + ' '
        + QByteArray::number(onAppearanceObjectNumber)
        + QByteArrayLiteral(" 0 R >> >> /")
        + QByteArray(PDFFormAppearance::kVersionKey)
        + ' '
        + QByteArray::number(PDFFormAppearance::kRadioButtonVersion)
        + QByteArrayLiteral(" >>\nendobj");
    return object;
}

bool buttonFieldObjectMatches(const QByteArray &object, bool radioButton)
{
    const qsizetype fieldTypeKey = keyPosition(
        object.constData(),
        object.size(),
        "/FT");
    Cursor fieldTypeCursor(
        object.constData(),
        object.size(),
        fieldTypeKey >= 0
            ? fieldTypeKey + QByteArrayView("/FT").size()
            : object.size());
    qint64 fieldFlags = 0;
    const qsizetype fieldFlagsKey = keyPosition(
        object.constData(),
        object.size(),
        "/Ff");
    if (fieldFlagsKey < 0) {
        return false;
    }
    Cursor fieldFlagsCursor(
        object.constData(),
        object.size(),
        fieldFlagsKey + QByteArrayView("/Ff").size());
    return fieldTypeKey >= 0
        && fieldTypeCursor.consume("/Btn")
        && fieldFlagsCursor.integer(fieldFlags)
        && ((fieldFlags & kButtonRadio) != 0) == radioButton;
}

enum class ChoiceFieldStructure {
    Unsupported,
    Current,
    LegacySingleSelectListBox
};

ChoiceFieldStructure choiceFieldStructure(const QByteArray &object,
                                          bool listBox)
{
    const qsizetype fieldTypeKey = keyPosition(
        object.constData(),
        object.size(),
        "/FT");
    Cursor fieldTypeCursor(
        object.constData(),
        object.size(),
        fieldTypeKey >= 0
            ? fieldTypeKey + QByteArrayView("/FT").size()
            : object.size());
    qint64 fieldFlags = 0;
    const qsizetype fieldFlagsKey = keyPosition(
        object.constData(),
        object.size(),
        "/Ff");
    if (fieldFlagsKey < 0) {
        return ChoiceFieldStructure::Unsupported;
    }
    Cursor fieldFlagsCursor(
        object.constData(),
        object.size(),
        fieldFlagsKey + QByteArrayView("/Ff").size());
    const qint64 unsupportedChoiceFlags = kChoiceEdit
        | kChoiceCommitOnSelectionChange;
    if (fieldTypeKey < 0
        || !fieldTypeCursor.consume("/Ch")
        || !fieldFlagsCursor.integer(fieldFlags)
        || (fieldFlags & unsupportedChoiceFlags)
        || keyPosition(object.constData(), object.size(), "/A") >= 0
        || keyPosition(object.constData(), object.size(), "/AA") >= 0) {
        return ChoiceFieldStructure::Unsupported;
    }
    const bool combo = fieldFlags & kChoiceCombo;
    const bool multiSelect = fieldFlags & kChoiceMultiSelect;
    if (!listBox) {
        return combo && !multiSelect
            ? ChoiceFieldStructure::Current
            : ChoiceFieldStructure::Unsupported;
    }
    if (combo) {
        return ChoiceFieldStructure::Unsupported;
    }
    return multiSelect
        ? ChoiceFieldStructure::Current
        : ChoiceFieldStructure::LegacySingleSelectListBox;
}

bool skipLiteralString(const char *data,
                       qsizetype size,
                       qsizetype &position)
{
    if (position >= size || data[position] != '(') {
        return false;
    }
    int depth = 1;
    ++position;
    while (position < size && depth > 0) {
        const char character = data[position++];
        if (character == '\\') {
            if (position < size) {
                ++position;
            }
        } else if (character == '(') {
            ++depth;
        } else if (character == ')') {
            --depth;
        }
    }
    return depth == 0;
}

bool matchingArrayEnd(const char *data,
                      qsizetype size,
                      qsizetype start,
                      qsizetype &end)
{
    if (start >= size || data[start] != '[') {
        return false;
    }
    int depth = 1;
    qsizetype position = start + 1;
    while (position < size && depth > 0) {
        const char character = data[position];
        if (character == '(') {
            if (!skipLiteralString(data, size, position)) {
                return false;
            }
            continue;
        }
        if (character == '%') {
            while (position < size
                   && data[position] != '\n'
                   && data[position] != '\r') {
                ++position;
            }
            continue;
        }
        if (character == '[') {
            ++depth;
        } else if (character == ']') {
            --depth;
        }
        ++position;
    }
    if (depth != 0) {
        return false;
    }
    end = position;
    return true;
}

bool fieldReferences(const ParsedPdf &pdf,
                     const QByteArray &acroFormObject,
                     QList<ObjectReference> &references,
                     qsizetype &valueStart,
                     qsizetype &valueEnd,
                     QString &error)
{
    const qsizetype fieldsKey = keyPosition(
        acroFormObject.constData(),
        acroFormObject.size(),
        "/Fields");
    if (fieldsKey < 0) {
        valueStart = -1;
        valueEnd = -1;
        return true;
    }
    Cursor cursor(
        acroFormObject.constData(),
        acroFormObject.size(),
        fieldsKey + QByteArrayView("/Fields").size());
    cursor.skipWhitespaceAndComments();
    valueStart = cursor.position();
    if (valueStart >= acroFormObject.size()) {
        error = QStringLiteral("The AcroForm Fields entry is truncated.");
        return false;
    }

    QByteArray arrayStorage;
    const char *arrayData = nullptr;
    qsizetype arraySize = 0;
    qsizetype arrayStart = 0;
    qsizetype arrayEnd = 0;
    if (acroFormObject.at(valueStart) == '[') {
        if (!matchingArrayEnd(
                acroFormObject.constData(),
                acroFormObject.size(),
                valueStart,
                valueEnd)) {
            error = QStringLiteral("The AcroForm Fields array is malformed.");
            return false;
        }
        arrayData = acroFormObject.constData();
        arraySize = acroFormObject.size();
        arrayStart = valueStart;
        arrayEnd = valueEnd;
    } else {
        ObjectReference arrayReference;
        if (!parseReference(
                acroFormObject.constData(),
                acroFormObject.size(),
                valueStart,
                arrayReference,
                &valueEnd)) {
            error = QStringLiteral("The AcroForm Fields value is unsupported.");
            return false;
        }
        arrayStorage = completeObject(pdf, arrayReference.number, error);
        if (arrayStorage.isEmpty()) {
            return false;
        }
        arrayStart = arrayStorage.indexOf('[');
        if (arrayStart < 0
            || !matchingArrayEnd(
                arrayStorage.constData(),
                arrayStorage.size(),
                arrayStart,
                arrayEnd)) {
            error = QStringLiteral("The indirect AcroForm Fields array is malformed.");
            return false;
        }
        arrayData = arrayStorage.constData();
        arraySize = arrayStorage.size();
    }

    Cursor arrayCursor(arrayData, arraySize, arrayStart + 1);
    while (arrayCursor.position() < arrayEnd - 1) {
        arrayCursor.skipWhitespaceAndComments();
        if (arrayCursor.position() >= arrayEnd - 1) {
            break;
        }
        ObjectReference reference;
        qsizetype referenceEnd = 0;
        if (!parseReference(
                arrayData,
                arrayEnd,
                arrayCursor.position(),
                reference,
                &referenceEnd)) {
            error = QStringLiteral("Direct dictionaries in AcroForm Fields are unsupported.");
            return false;
        }
        references.append(reference);
        arrayCursor = Cursor(arrayData, arraySize, referenceEnd);
    }
    return true;
}

bool kidReferences(const ParsedPdf &pdf,
                   const QByteArray &fieldObject,
                   QList<ObjectReference> &references,
                   QString &error)
{
    const qsizetype kidsKey = keyPosition(
        fieldObject.constData(),
        fieldObject.size(),
        "/Kids");
    if (kidsKey < 0) {
        return true;
    }
    Cursor cursor(
        fieldObject.constData(),
        fieldObject.size(),
        kidsKey + QByteArrayView("/Kids").size());
    cursor.skipWhitespaceAndComments();
    const qsizetype valueStart = cursor.position();
    if (valueStart >= fieldObject.size()) {
        error = QStringLiteral("An AcroForm Kids entry is truncated.");
        return false;
    }

    QByteArray arrayStorage;
    const char *arrayData = nullptr;
    qsizetype arraySize = 0;
    qsizetype arrayStart = 0;
    qsizetype arrayEnd = 0;
    if (fieldObject.at(valueStart) == '[') {
        if (!matchingArrayEnd(
                fieldObject.constData(),
                fieldObject.size(),
                valueStart,
                arrayEnd)) {
            error = QStringLiteral("An AcroForm Kids array is malformed.");
            return false;
        }
        arrayData = fieldObject.constData();
        arraySize = fieldObject.size();
        arrayStart = valueStart;
    } else {
        ObjectReference arrayReference;
        if (!parseReference(
                fieldObject.constData(),
                fieldObject.size(),
                valueStart,
                arrayReference)) {
            error = QStringLiteral("An AcroForm Kids value is unsupported.");
            return false;
        }
        arrayStorage = completeObject(pdf, arrayReference.number, error);
        if (arrayStorage.isEmpty()) {
            return false;
        }
        arrayStart = arrayStorage.indexOf('[');
        if (arrayStart < 0
            || !matchingArrayEnd(
                arrayStorage.constData(),
                arrayStorage.size(),
                arrayStart,
                arrayEnd)) {
            error = QStringLiteral(
                "An indirect AcroForm Kids array is malformed.");
            return false;
        }
        arrayData = arrayStorage.constData();
        arraySize = arrayStorage.size();
    }

    Cursor arrayCursor(arrayData, arraySize, arrayStart + 1);
    while (arrayCursor.position() < arrayEnd - 1) {
        arrayCursor.skipWhitespaceAndComments();
        if (arrayCursor.position() >= arrayEnd - 1) {
            break;
        }
        ObjectReference reference;
        qsizetype referenceEnd = 0;
        if (!parseReference(
                arrayData,
                arrayEnd,
                arrayCursor.position(),
                reference,
                &referenceEnd)) {
            error = QStringLiteral(
                "Direct dictionaries in AcroForm Kids are unsupported.");
            return false;
        }
        references.append(reference);
        arrayCursor = Cursor(arrayData, arraySize, referenceEnd);
    }
    return true;
}

bool collectFieldNames(const ParsedPdf &pdf,
                       const QList<ObjectReference> &references,
                       const QString &parentName,
                       QSet<int> &visitedObjects,
                       QSet<QString> &fieldNames,
                       QHash<int, QString> &fieldNamesByObject,
                       QString &error)
{
    for (const ObjectReference &reference : references) {
        if (visitedObjects.contains(reference.number)) {
            error = QStringLiteral("The AcroForm field tree contains a cycle.");
            return false;
        }
        visitedObjects.insert(reference.number);
        const QByteArray object = completeObject(
            pdf,
            reference.number,
            error);
        if (object.isEmpty()) {
            return false;
        }

        QString fullName = parentName;
        const qsizetype nameKey = keyPosition(
            object.constData(),
            object.size(),
            "/T");
        if (nameKey >= 0) {
            QByteArray encodedName;
            if (!parsePdfString(
                    object.constData(),
                    object.size(),
                    nameKey + QByteArrayView("/T").size(),
                    encodedName)) {
                error = QStringLiteral(
                    "An AcroForm field name is malformed.");
                return false;
            }
            const bool hasUnicodeMarker = encodedName.startsWith("\xfe\xff")
                || encodedName.startsWith("\xff\xfe")
                || encodedName.startsWith("\xef\xbb\xbf");
            if (!hasUnicodeMarker
                && std::any_of(
                    encodedName.cbegin(),
                    encodedName.cend(),
                    [](char value) {
                        return static_cast<uchar>(value) >= 0x80;
                    })) {
                error = QStringLiteral(
                    "An existing AcroForm field name uses an unsupported encoding.");
                return false;
            }
            const QString partialName = decodedPdfString(encodedName);
            if (!partialName.isEmpty()) {
                fullName = parentName.isEmpty()
                    ? partialName
                    : parentName + QLatin1Char('.') + partialName;
                fieldNames.insert(fullName);
            }
        }
        if (!fullName.isEmpty()) {
            fieldNamesByObject.insert(reference.number, fullName);
        }

        QList<ObjectReference> kids;
        if (!kidReferences(pdf, object, kids, error)
            || !collectFieldNames(
                pdf,
                kids,
                fullName,
                visitedObjects,
                fieldNames,
                fieldNamesByObject,
                error)) {
            return false;
        }
    }
    return true;
}

QByteArray referenceArray(const QList<ObjectReference> &references)
{
    QByteArray value("[");
    for (const ObjectReference &reference : references) {
        value += QByteArray::number(reference.number);
        value += ' ';
        value += QByteArray::number(reference.generation);
        value += " R ";
    }
    value += ']';
    return value;
}

bool replaceCatalogAcroForm(QByteArray &catalogObject,
                            const ObjectReference &acroForm,
                            QString &error)
{
    const QByteArray replacement = QByteArrayLiteral(" ")
        + QByteArray::number(acroForm.number)
        + ' '
        + QByteArray::number(acroForm.generation)
        + " R";
    const qsizetype key = keyPosition(
        catalogObject.constData(),
        catalogObject.size(),
        "/AcroForm");
    if (key >= 0) {
        const qsizetype valueStart
            = key + QByteArrayView("/AcroForm").size();
        qsizetype valueEnd = 0;
        ObjectReference previous;
        if (!parseReference(
                catalogObject.constData(),
                catalogObject.size(),
                valueStart,
                previous,
                &valueEnd)) {
            error = QStringLiteral("The Catalog AcroForm entry is not indirect.");
            return false;
        }
        catalogObject.replace(valueStart, valueEnd - valueStart, replacement);
        return true;
    }

    const qsizetype dictionaryStart = catalogObject.indexOf("<<");
    if (dictionaryStart < 0) {
        error = QStringLiteral("The PDF Catalog dictionary is malformed.");
        return false;
    }
    catalogObject.insert(
        dictionaryStart + 2,
        QByteArrayLiteral(" /AcroForm ") + replacement);
    return true;
}

bool skipHexString(const char *data,
                   qsizetype size,
                   qsizetype &position)
{
    if (position >= size
        || data[position] != '<'
        || (position + 1 < size && data[position + 1] == '<')) {
        return false;
    }
    ++position;
    while (position < size) {
        if (data[position++] == '>') {
            return true;
        }
    }
    return false;
}

bool matchingDictionaryEnd(const char *data,
                           qsizetype size,
                           qsizetype start,
                           qsizetype &end)
{
    if (start < 0
        || start + 1 >= size
        || data[start] != '<'
        || data[start + 1] != '<') {
        return false;
    }
    int depth = 1;
    qsizetype position = start + 2;
    while (position < size && depth > 0) {
        if (data[position] == '(') {
            if (!skipLiteralString(data, size, position)) {
                return false;
            }
            continue;
        }
        if (data[position] == '<') {
            if (position + 1 < size && data[position + 1] == '<') {
                ++depth;
                position += 2;
                continue;
            }
            if (!skipHexString(data, size, position)) {
                return false;
            }
            continue;
        }
        if (data[position] == '>'
            && position + 1 < size
            && data[position + 1] == '>') {
            --depth;
            position += 2;
            continue;
        }
        if (data[position] == '%') {
            while (position < size
                   && data[position] != '\n'
                   && data[position] != '\r') {
                ++position;
            }
            continue;
        }
        ++position;
    }
    if (depth != 0) {
        return false;
    }
    end = position;
    return true;
}

bool pdfValueEnd(const char *data,
                 qsizetype size,
                 qsizetype start,
                 qsizetype &end)
{
    Cursor cursor(data, size, start);
    cursor.skipWhitespaceAndComments();
    start = cursor.position();
    if (start >= size) {
        return false;
    }
    if (data[start] == '(') {
        end = start;
        return skipLiteralString(data, size, end);
    }
    if (data[start] == '[') {
        return matchingArrayEnd(data, size, start, end);
    }
    if (data[start] == '<') {
        if (start + 1 < size && data[start + 1] == '<') {
            return matchingDictionaryEnd(data, size, start, end);
        }
        end = start;
        return skipHexString(data, size, end);
    }
    if (data[start] == '/') {
        end = start + 1;
        while (end < size && !isDelimiter(data[end])) {
            ++end;
        }
        return end > start + 1;
    }

    ObjectReference reference;
    if (parseReference(data, size, start, reference, &end)) {
        return true;
    }

    end = start;
    while (end < size && !isDelimiter(data[end])) {
        ++end;
    }
    return end > start;
}

bool topLevelDictionaryValueRange(const QByteArray &object,
                                  QByteArrayView key,
                                  qsizetype &valueStart,
                                  qsizetype &valueEnd,
                                  QString &error)
{
    valueStart = -1;
    valueEnd = -1;
    Cursor objectCursor(object.constData(), object.size());
    qint64 objectNumber = -1;
    qint64 generation = -1;
    if (!objectCursor.integer(objectNumber)
        || !objectCursor.integer(generation)
        || !objectCursor.consume("obj")) {
        error = QStringLiteral("A PDF indirect object header is malformed.");
        return false;
    }
    objectCursor.skipWhitespaceAndComments();
    const qsizetype dictionaryStart = objectCursor.position();
    qsizetype dictionaryEnd = -1;
    if (dictionaryStart + 1 >= object.size()
        || object.at(dictionaryStart) != '<'
        || object.at(dictionaryStart + 1) != '<'
        || !matchingDictionaryEnd(
            object.constData(),
            object.size(),
            dictionaryStart,
            dictionaryEnd)) {
        error = QStringLiteral("A PDF dictionary is malformed.");
        return false;
    }

    qsizetype position = dictionaryStart + 2;
    while (position < dictionaryEnd - 2) {
        Cursor cursor(object.constData(), object.size(), position);
        cursor.skipWhitespaceAndComments();
        const qsizetype keyStart = cursor.position();
        if (keyStart >= dictionaryEnd - 2) {
            break;
        }
        if (object.at(keyStart) != '/') {
            error = QStringLiteral("A PDF dictionary entry is malformed.");
            return false;
        }

        qsizetype keyEnd = keyStart + 1;
        while (keyEnd < dictionaryEnd - 2
               && !isDelimiter(object.at(keyEnd))) {
            ++keyEnd;
        }
        Cursor valueCursor(object.constData(), object.size(), keyEnd);
        valueCursor.skipWhitespaceAndComments();
        const qsizetype candidateStart = valueCursor.position();
        qsizetype candidateEnd = -1;
        if (candidateStart >= dictionaryEnd - 2
            || !pdfValueEnd(
                object.constData(),
                dictionaryEnd - 2,
                candidateStart,
                candidateEnd)) {
            error = QStringLiteral("A PDF dictionary value is malformed.");
            return false;
        }

        const QByteArrayView currentKey(
            object.constData() + keyStart,
            keyEnd - keyStart);
        if (currentKey == key) {
            valueStart = candidateStart;
            valueEnd = candidateEnd;
            return true;
        }
        position = candidateEnd;
    }
    return true;
}

bool objectForReference(const ParsedPdf &pdf,
                        const ObjectReference &reference,
                        const QHash<int, QByteArray> &rewrittenObjects,
                        QByteArray &object,
                        QString &error)
{
    const auto entry = pdf.entries.constFind(reference.number);
    if (entry == pdf.entries.cend()
        || entry->generation != reference.generation) {
        error = QStringLiteral(
            "A required PDF object reference is missing or stale.");
        return false;
    }
    const auto rewritten = rewrittenObjects.constFind(reference.number);
    if (rewritten != rewrittenObjects.cend()) {
        object = *rewritten;
        return true;
    }
    object = completeObject(pdf, reference.number, error);
    return !object.isEmpty();
}

struct PageAnnotsArray {
    ObjectReference reference;
    QByteArray object;
    qsizetype start = -1;
    qsizetype end = -1;
};

bool pageAnnotsArray(const ParsedPdf &pdf,
                     const ObjectReference &pageReference,
                     const QHash<int, QByteArray> &rewrittenObjects,
                     PageAnnotsArray &annots,
                     QString &error)
{
    QByteArray pageObject;
    if (!objectForReference(
            pdf,
            pageReference,
            rewrittenObjects,
            pageObject,
            error)) {
        return false;
    }

    qsizetype annotsStart = -1;
    qsizetype annotsEnd = -1;
    if (!topLevelDictionaryValueRange(
            pageObject,
            QByteArrayView("/Annots"),
            annotsStart,
            annotsEnd,
            error)
        || annotsStart < 0) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "The Widget page has no direct Annots entry.");
        }
        return false;
    }

    if (pageObject.at(annotsStart) == '[') {
        annots.reference = pageReference;
        annots.object = std::move(pageObject);
        annots.start = annotsStart;
        annots.end = annotsEnd;
        qsizetype checkedEnd = -1;
        if (!matchingArrayEnd(
                annots.object.constData(),
                annots.object.size(),
                annots.start,
                checkedEnd)
            || checkedEnd != annots.end) {
            error = QStringLiteral(
                "The Widget page Annots array is malformed.");
            return false;
        }
    } else {
        ObjectReference annotsReference;
        qsizetype referenceEnd = -1;
        if (!parseReference(
                pageObject.constData(),
                annotsEnd,
                annotsStart,
                annotsReference,
                &referenceEnd)
            || referenceEnd != annotsEnd
            || !objectForReference(
                pdf,
                annotsReference,
                rewrittenObjects,
                annots.object,
                error)) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "The Widget page Annots entry is unsupported.");
            }
            return false;
        }
        annots.reference = annotsReference;

        Cursor objectCursor(
            annots.object.constData(),
            annots.object.size());
        qint64 objectNumber = -1;
        qint64 generation = -1;
        if (!objectCursor.integer(objectNumber)
            || !objectCursor.integer(generation)
            || !objectCursor.consume("obj")
            || objectNumber != annotsReference.number
            || generation != annotsReference.generation) {
            error = QStringLiteral(
                "The indirect Widget Annots array object is malformed.");
            return false;
        }
        objectCursor.skipWhitespaceAndComments();
        annots.start = objectCursor.position();
        if (annots.start >= annots.object.size()
            || annots.object.at(annots.start) != '['
            || !matchingArrayEnd(
                annots.object.constData(),
                annots.object.size(),
                annots.start,
                annots.end)) {
            error = QStringLiteral(
                "The indirect Widget Annots array is malformed.");
            return false;
        }
        Cursor endCursor(
            annots.object.constData(),
            annots.object.size(),
            annots.end);
        if (!endCursor.consume("endobj")) {
            error = QStringLiteral(
                "The indirect Widget Annots array object is malformed.");
            return false;
        }
    }
    return true;
}

bool annotationReferenceSpans(
    const PageAnnotsArray &annots,
    const QHash<int, ObjectReference> &expectedReferences,
    QHash<int, int> &matchCounts,
    QVector<QPair<qsizetype, qsizetype>> &spans,
    QString &error)
{
    qsizetype position = annots.start + 1;
    while (position < annots.end - 1) {
        Cursor itemCursor(
            annots.object.constData(),
            annots.object.size(),
            position);
        itemCursor.skipWhitespaceAndComments();
        const qsizetype itemStart = itemCursor.position();
        if (itemStart >= annots.end - 1) {
            break;
        }

        ObjectReference candidate;
        qsizetype itemEnd = -1;
        if (parseReference(
                annots.object.constData(),
                annots.end - 1,
                itemStart,
                candidate,
                &itemEnd)) {
            const auto expected = expectedReferences.constFind(
                candidate.number);
            if (expected != expectedReferences.cend()
                && *expected == candidate) {
                ++matchCounts[candidate.number];
                spans.append({itemStart, itemEnd});
            }
            position = itemEnd;
            continue;
        }

        if (itemStart + 1 >= annots.end - 1
            || annots.object.at(itemStart) != '<'
            || annots.object.at(itemStart + 1) != '<'
            || !pdfValueEnd(
                annots.object.constData(),
                annots.end - 1,
                itemStart,
                itemEnd)) {
            error = QStringLiteral(
                "The Widget page Annots array contains an unsupported entry.");
            return false;
        }
        position = itemEnd;
    }
    return true;
}

bool pageForWidgetReference(const ParsedPdf &pdf,
                            const ObjectReference &widgetReference,
                            ObjectReference &pageReference,
                            bool &located,
                            QString &error)
{
    const QHash<int, QByteArray> noRewrites;
    const QHash<int, ObjectReference> expectedReferences({
        {widgetReference.number, widgetReference},
    });
    located = false;
    for (auto entry = pdf.entries.cbegin();
         entry != pdf.entries.cend();
         ++entry) {
        QString objectError;
        const QByteArray object = completeObject(
            pdf,
            entry.key(),
            objectError);
        if (object.isEmpty()) {
            continue;
        }
        Cursor objectCursor(object.constData(), object.size());
        qint64 objectNumber = -1;
        qint64 generation = -1;
        if (!objectCursor.integer(objectNumber)
            || !objectCursor.integer(generation)
            || !objectCursor.consume("obj")
            || objectNumber != entry.key()
            || generation != entry->generation) {
            error = QStringLiteral(
                "A PDF indirect object header is malformed.");
            return false;
        }
        objectCursor.skipWhitespaceAndComments();
        const qsizetype objectValueStart = objectCursor.position();
        if (objectValueStart + 1 >= object.size()
            || object.at(objectValueStart) != '<'
            || object.at(objectValueStart + 1) != '<') {
            continue;
        }
        qsizetype typeStart = -1;
        qsizetype typeEnd = -1;
        if (!topLevelDictionaryValueRange(
                object,
                QByteArrayView("/Type"),
                typeStart,
                typeEnd,
                objectError)) {
            error = objectError;
            return false;
        }
        if (typeStart < 0
            || QByteArrayView(
                   object.constData() + typeStart,
                   typeEnd - typeStart)
                != QByteArrayView("/Page")) {
            continue;
        }

        qsizetype annotsStart = -1;
        qsizetype annotsEnd = -1;
        if (!topLevelDictionaryValueRange(
                object,
                QByteArrayView("/Annots"),
                annotsStart,
                annotsEnd,
                objectError)) {
            error = objectError;
            return false;
        }
        if (annotsStart < 0) {
            continue;
        }

        const ObjectReference candidatePage{
            entry.key(),
            entry->generation};
        PageAnnotsArray annots;
        if (!pageAnnotsArray(
                pdf,
                candidatePage,
                noRewrites,
                annots,
                error)) {
            return false;
        }
        QHash<int, int> matchCounts;
        QVector<QPair<qsizetype, qsizetype>> ignoredSpans;
        if (!annotationReferenceSpans(
                annots,
                expectedReferences,
                matchCounts,
                ignoredSpans,
                error)) {
            return false;
        }
        const int count = matchCounts.value(widgetReference.number);
        if (count > 1 || (count == 1 && located)) {
            error = QStringLiteral(
                "The TrePDF Widget reference appears on multiple pages or more than once.");
            return false;
        }
        if (count == 1) {
            pageReference = candidatePage;
            located = true;
        }
    }
    return true;
}

bool removeWidgetReferencesFromPageAnnots(
    const ParsedPdf &pdf,
    const ObjectReference &pageReference,
    const QVector<ObjectReference> &widgetReferences,
    const QHash<int, QByteArray> &rewrittenObjects,
    ObjectReference &rewrittenReference,
    QByteArray &rewrittenObject,
    QString &error)
{
    PageAnnotsArray annots;
    if (!pageAnnotsArray(
            pdf,
            pageReference,
            rewrittenObjects,
            annots,
            error)) {
        return false;
    }
    QHash<int, ObjectReference> expectedReferences;
    for (const ObjectReference &reference : widgetReferences) {
        if (expectedReferences.contains(reference.number)) {
            error = QStringLiteral(
                "The Widget removal request contains duplicate references.");
            return false;
        }
        expectedReferences.insert(reference.number, reference);
    }
    QHash<int, int> matchCounts;
    QVector<QPair<qsizetype, qsizetype>> removals;
    if (!annotationReferenceSpans(
            annots,
            expectedReferences,
            matchCounts,
            removals,
            error)) {
        return false;
    }

    for (auto expected = expectedReferences.cbegin();
         expected != expectedReferences.cend();
         ++expected) {
        if (matchCounts.value(expected.key()) != 1) {
            error = QStringLiteral(
                "The TrePDF Widget reference is missing or duplicated in its page Annots array.");
            return false;
        }
    }
    std::sort(
        removals.begin(),
        removals.end(),
        [](const auto &left, const auto &right) {
            return left.first > right.first;
        });
    for (const auto &removal : std::as_const(removals)) {
        annots.object.remove(
            removal.first,
            removal.second - removal.first);
    }
    rewrittenReference = annots.reference;
    rewrittenObject = std::move(annots.object);
    return true;
}

bool hasSimpleDirectStringOptions(const QByteArray &object)
{
    const qsizetype optionsKey = keyPosition(
        object.constData(),
        object.size(),
        "/Opt");
    if (optionsKey < 0) {
        return false;
    }
    Cursor cursor(
        object.constData(),
        object.size(),
        optionsKey + QByteArrayView("/Opt").size());
    cursor.skipWhitespaceAndComments();
    const qsizetype arrayStart = cursor.position();
    qsizetype arrayEnd = -1;
    if (arrayStart >= object.size()
        || object.at(arrayStart) != '['
        || !matchingArrayEnd(
            object.constData(),
            object.size(),
            arrayStart,
            arrayEnd)) {
        return false;
    }

    bool hasOption = false;
    qsizetype position = arrayStart + 1;
    while (position < arrayEnd - 1) {
        Cursor optionCursor(object.constData(), object.size(), position);
        optionCursor.skipWhitespaceAndComments();
        position = optionCursor.position();
        if (position >= arrayEnd - 1) {
            break;
        }
        qsizetype optionEnd = position;
        if (object.at(position) == '(') {
            if (!skipLiteralString(
                    object.constData(),
                    arrayEnd,
                    optionEnd)) {
                return false;
            }
        } else if (object.at(position) == '<'
                   && (position + 1 >= arrayEnd
                       || object.at(position + 1) != '<')) {
            if (!skipHexString(
                    object.constData(),
                    arrayEnd,
                    optionEnd)) {
                return false;
            }
        } else {
            // Export/display pairs are nested arrays. TrePDF currently models
            // only one string per option, so reject them instead of silently
            // flattening and losing export values during an unrelated update.
            return false;
        }
        hasOption = true;
        position = optionEnd;
    }
    return hasOption;
}

bool replaceTopLevelDictionaryValue(
    QByteArray &object,
    QByteArrayView key,
    const QByteArray &replacement,
    bool insertIfMissing,
    QString &error)
{
    const qsizetype dictionaryStart = object.indexOf("<<");
    qsizetype dictionaryEnd = -1;
    if (dictionaryStart < 0
        || !matchingDictionaryEnd(
            object.constData(),
            object.size(),
            dictionaryStart,
            dictionaryEnd)) {
        error = QStringLiteral(
            "A TrePDF Widget dictionary is malformed.");
        return false;
    }

    qsizetype position = dictionaryStart + 2;
    while (position < dictionaryEnd - 2) {
        Cursor cursor(object.constData(), object.size(), position);
        cursor.skipWhitespaceAndComments();
        const qsizetype keyStart = cursor.position();
        if (keyStart >= dictionaryEnd - 2) {
            break;
        }
        if (object.at(keyStart) != '/') {
            error = QStringLiteral(
                "A TrePDF Widget dictionary entry is malformed.");
            return false;
        }

        qsizetype keyEnd = keyStart + 1;
        while (keyEnd < dictionaryEnd - 2
               && !isDelimiter(object.at(keyEnd))) {
            ++keyEnd;
        }
        Cursor valueCursor(
            object.constData(),
            object.size(),
            keyEnd);
        valueCursor.skipWhitespaceAndComments();
        const qsizetype valueStart = valueCursor.position();
        qsizetype valueEnd = -1;
        if (valueStart >= dictionaryEnd - 2
            || !pdfValueEnd(
                object.constData(),
                dictionaryEnd - 2,
                valueStart,
                valueEnd)) {
            error = QStringLiteral(
                "A TrePDF Widget dictionary value is malformed.");
            return false;
        }

        const QByteArrayView currentKey(
            object.constData() + keyStart,
            keyEnd - keyStart);
        if (currentKey == key) {
            object.replace(
                valueStart,
                valueEnd - valueStart,
                replacement);
            return true;
        }
        position = valueEnd;
    }

    if (!insertIfMissing) {
        return true;
    }
    object.insert(
        dictionaryEnd - 2,
        QByteArrayLiteral(" ")
            + QByteArray(key.data(), key.size())
            + ' '
            + replacement);
    return true;
}

bool parsePdfReal(const char *data,
                  qsizetype size,
                  qsizetype position,
                  qreal &value,
                  qsizetype &end)
{
    Cursor cursor(data, size, position);
    cursor.skipWhitespaceAndComments();
    const qsizetype start = cursor.position();
    position = start;
    if (position < size
        && (data[position] == '+' || data[position] == '-')) {
        ++position;
    }
    bool hasDigit = false;
    bool hasDecimalPoint = false;
    while (position < size) {
        const char character = data[position];
        if (character >= '0' && character <= '9') {
            hasDigit = true;
            ++position;
            continue;
        }
        if (character == '.' && !hasDecimalPoint) {
            hasDecimalPoint = true;
            ++position;
            continue;
        }
        break;
    }
    if (!hasDigit
        || (position < size && !isDelimiter(data[position]))) {
        return false;
    }
    bool converted = false;
    value = QByteArray(data + start, position - start).toDouble(&converted);
    end = position;
    return converted && std::isfinite(value);
}

struct TextMarkupRect {
    qreal left = 0.0;
    qreal bottom = 0.0;
    qreal right = 0.0;
    qreal top = 0.0;
};

QByteArrayView textMarkupSubtypeName(
    PDFAcroFormCanonicalizer::TextMarkupType type)
{
    switch (type) {
    case PDFAcroFormCanonicalizer::TextMarkupType::Highlight:
        return QByteArrayView("/Highlight");
    case PDFAcroFormCanonicalizer::TextMarkupType::Underline:
        return QByteArrayView("/Underline");
    case PDFAcroFormCanonicalizer::TextMarkupType::Squiggly:
        return QByteArrayView("/Squiggly");
    case PDFAcroFormCanonicalizer::TextMarkupType::StrikeOut:
        return QByteArrayView("/StrikeOut");
    }
    return {};
}

bool directDictionaryValueRange(const QByteArray &dictionary,
                                QByteArrayView key,
                                qsizetype &valueStart,
                                qsizetype &valueEnd,
                                QString &error)
{
    valueStart = -1;
    valueEnd = -1;
    qsizetype dictionaryEnd = -1;
    if (!dictionary.startsWith("<<")
        || !matchingDictionaryEnd(
            dictionary.constData(),
            dictionary.size(),
            0,
            dictionaryEnd)
        || dictionaryEnd != dictionary.size()) {
        error = QStringLiteral(
            "A text markup annotation dictionary value container is malformed.");
        return false;
    }

    qsizetype position = 2;
    while (position < dictionaryEnd - 2) {
        Cursor cursor(dictionary.constData(), dictionary.size(), position);
        cursor.skipWhitespaceAndComments();
        const qsizetype keyStart = cursor.position();
        if (keyStart >= dictionaryEnd - 2) {
            break;
        }
        if (dictionary.at(keyStart) != '/') {
            error = QStringLiteral(
                "A text markup annotation dictionary entry is malformed.");
            return false;
        }

        qsizetype keyEnd = keyStart + 1;
        while (keyEnd < dictionaryEnd - 2
               && !isDelimiter(dictionary.at(keyEnd))) {
            ++keyEnd;
        }
        Cursor valueCursor(
            dictionary.constData(),
            dictionary.size(),
            keyEnd);
        valueCursor.skipWhitespaceAndComments();
        const qsizetype candidateStart = valueCursor.position();
        qsizetype candidateEnd = -1;
        if (candidateStart >= dictionaryEnd - 2
            || !pdfValueEnd(
                dictionary.constData(),
                dictionaryEnd - 2,
                candidateStart,
                candidateEnd)) {
            error = QStringLiteral(
                "A text markup annotation dictionary value is malformed.");
            return false;
        }
        const QByteArrayView currentKey(
            dictionary.constData() + keyStart,
            keyEnd - keyStart);
        if (currentKey == key) {
            valueStart = candidateStart;
            valueEnd = candidateEnd;
            return true;
        }
        position = candidateEnd;
    }
    return true;
}

bool directPdfRealArray(const QByteArray &dictionary,
                        QByteArrayView key,
                        qsizetype maximumValues,
                        QVector<qreal> &values,
                        QString &error)
{
    qsizetype arrayStart = -1;
    qsizetype arrayEnd = -1;
    if (!directDictionaryValueRange(
            dictionary,
            key,
            arrayStart,
            arrayEnd,
            error)) {
        return false;
    }
    Cursor cursor(
        dictionary.constData(),
        arrayEnd >= 0 ? arrayEnd : dictionary.size(),
        arrayStart >= 0 ? arrayStart : dictionary.size());
    if (arrayStart < 0 || !cursor.consume("[")) {
        error = QStringLiteral(
            "A text markup annotation numeric array is missing or indirect.");
        return false;
    }

    values.clear();
    qsizetype position = cursor.position();
    while (true) {
        Cursor valueCursor(
            dictionary.constData(),
            arrayEnd,
            position);
        valueCursor.skipWhitespaceAndComments();
        position = valueCursor.position();
        if (position >= arrayEnd) {
            error = QStringLiteral(
                "A text markup annotation numeric array is unterminated.");
            return false;
        }
        if (dictionary.at(position) == ']') {
            return position + 1 == arrayEnd;
        }
        if (values.size() >= maximumValues) {
            error = QStringLiteral(
                "A text markup annotation numeric array is too large.");
            return false;
        }
        qreal value = 0.0;
        qsizetype end = -1;
        if (!parsePdfReal(
                dictionary.constData(),
                arrayEnd,
                position,
                value,
                end)) {
            error = QStringLiteral(
                "A text markup annotation numeric array is malformed.");
            return false;
        }
        values.append(value);
        position = end;
    }
}

bool textMarkupAppearanceData(
    const QByteArray &dictionary,
    PDFAcroFormCanonicalizer::TextMarkupType type,
    QByteArray &content,
    TextMarkupRect &boundingRect,
    qreal &opacity,
    QString &error)
{
    constexpr qsizetype maximumQuadPointValues = 8 * 4096;
    constexpr qsizetype maximumAppearanceBytes = 8 * 1024 * 1024;
    QVector<qreal> quadPoints;
    if (!directPdfRealArray(
            dictionary,
            QByteArrayView("/QuadPoints"),
            maximumQuadPointValues,
            quadPoints,
            error)
        || quadPoints.isEmpty()
        || quadPoints.size() % 8 != 0) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "A text markup annotation QuadPoints entry is malformed.");
        }
        return false;
    }

    QVector<qreal> color;
    if (!directPdfRealArray(
            dictionary,
            QByteArrayView("/C"),
            4,
            color,
            error)
        || color.size() != 3) {
        if (error.isEmpty()) {
            error = QStringLiteral(
                "A text markup annotation color entry is malformed.");
        }
        return false;
    }
    for (qreal &component : color) {
        if (component < 0.0 || component > 1.0) {
            error = QStringLiteral(
                "A text markup annotation color component is out of range.");
            return false;
        }
    }

    opacity = 1.0;
    qsizetype opacityStart = -1;
    qsizetype opacityValueEnd = -1;
    if (!directDictionaryValueRange(
            dictionary,
            QByteArrayView("/CA"),
            opacityStart,
            opacityValueEnd,
            error)) {
        return false;
    }
    if (opacityStart >= 0) {
        qsizetype opacityEnd = -1;
        if (!parsePdfReal(
                dictionary.constData(),
                opacityValueEnd,
                opacityStart,
                opacity,
                opacityEnd)
            || opacityEnd != opacityValueEnd
            || opacity < 0.0
            || opacity > 1.0) {
            error = QStringLiteral(
                "A text markup annotation opacity is malformed.");
            return false;
        }
    }

    QVector<TextMarkupRect> rectangles;
    rectangles.reserve(quadPoints.size() / 8);
    bool hasBounds = false;
    for (qsizetype index = 0; index < quadPoints.size(); index += 8) {
        TextMarkupRect rect;
        rect.left = rect.right = quadPoints.at(index);
        rect.bottom = rect.top = quadPoints.at(index + 1);
        for (qsizetype coordinate = index + 2;
             coordinate < index + 8;
             coordinate += 2) {
            rect.left = qMin(rect.left, quadPoints.at(coordinate));
            rect.right = qMax(rect.right, quadPoints.at(coordinate));
            rect.bottom = qMin(rect.bottom, quadPoints.at(coordinate + 1));
            rect.top = qMax(rect.top, quadPoints.at(coordinate + 1));
        }
        if (rect.right <= rect.left || rect.top <= rect.bottom) {
            error = QStringLiteral(
                "A text markup annotation QuadPoints rectangle is empty.");
            return false;
        }
        if (!hasBounds) {
            boundingRect = rect;
            hasBounds = true;
        } else {
            boundingRect.left = qMin(boundingRect.left, rect.left);
            boundingRect.bottom = qMin(boundingRect.bottom, rect.bottom);
            boundingRect.right = qMax(boundingRect.right, rect.right);
            boundingRect.top = qMax(boundingRect.top, rect.top);
        }
        rectangles.append(rect);
    }

    content = QByteArrayLiteral("q\n/GS gs ")
        + pdfNumber(color.at(0))
        + ' '
        + pdfNumber(color.at(1))
        + ' '
        + pdfNumber(color.at(2))
        + (type == PDFAcroFormCanonicalizer::TextMarkupType::Highlight
               ? QByteArrayLiteral(" rg\n")
               : QByteArrayLiteral(" RG\n"));
    if (type != PDFAcroFormCanonicalizer::TextMarkupType::Highlight) {
        content += QByteArrayLiteral("1 w 0 J 0 j [] 0 d\n");
    }

    for (const TextMarkupRect &rect : std::as_const(rectangles)) {
        const QByteArray left = pdfNumber(rect.left);
        const QByteArray bottom = pdfNumber(rect.bottom);
        const QByteArray right = pdfNumber(rect.right);
        const QByteArray top = pdfNumber(rect.top);
        switch (type) {
        case PDFAcroFormCanonicalizer::TextMarkupType::Highlight:
            content += left + ' ' + top
                + QByteArrayLiteral(" m ") + right + ' ' + top
                + QByteArrayLiteral(" l ") + right + ' ' + bottom
                + QByteArrayLiteral(" l ") + left + ' ' + bottom
                + QByteArrayLiteral(" l h f\n");
            break;
        case PDFAcroFormCanonicalizer::TextMarkupType::Underline:
            content += left + ' ' + pdfNumber(rect.bottom + 1.0)
                + QByteArrayLiteral(" m ") + right + ' '
                + pdfNumber(rect.bottom + 1.0)
                + QByteArrayLiteral(" l S\n");
            break;
        case PDFAcroFormCanonicalizer::TextMarkupType::StrikeOut:
            content += left + ' '
                + pdfNumber((rect.top + rect.bottom) / 2.0)
                + QByteArrayLiteral(" m ") + right + ' '
                + pdfNumber((rect.top + rect.bottom) / 2.0)
                + QByteArrayLiteral(" l S\n");
            break;
        case PDFAcroFormCanonicalizer::TextMarkupType::Squiggly: {
            constexpr qreal delta = 2.0;
            const qreal waveTop = rect.bottom + delta;
            content += left + ' ' + pdfNumber(waveTop)
                + QByteArrayLiteral(" m ");
            qreal x = rect.left + delta;
            bool upwards = false;
            while (x < rect.right) {
                content += pdfNumber(x) + ' '
                    + pdfNumber(upwards ? waveTop : rect.bottom)
                    + QByteArrayLiteral(" l ");
                if (content.size() > maximumAppearanceBytes) {
                    error = QStringLiteral(
                        "A text markup annotation appearance is too large.");
                    return false;
                }
                x += delta;
                upwards = !upwards;
            }
            const qreal remainder = rect.right - (x - delta);
            content += right + ' '
                + pdfNumber(
                    upwards
                        ? rect.bottom + remainder
                        : waveTop - remainder)
                + QByteArrayLiteral(" l S\n");
            break;
        }
        }
        if (content.size() > maximumAppearanceBytes) {
            error = QStringLiteral(
                "A text markup annotation appearance is too large.");
            return false;
        }
    }
    content += QByteArrayLiteral("Q\n");
    return hasBounds && !content.isEmpty();
}

QByteArray textMarkupAppearanceObject(
    int objectNumber,
    PDFAcroFormCanonicalizer::TextMarkupType type,
    const TextMarkupRect &boundingRect,
    qreal opacity,
    const QByteArray &content)
{
    const QByteArray blendMode
        = type == PDFAcroFormCanonicalizer::TextMarkupType::Highlight
        ? QByteArrayLiteral("Multiply")
        : QByteArrayLiteral("Normal");
    return QByteArray::number(objectNumber)
        + QByteArrayLiteral(
            " 0 obj\n"
            "<< /Type /XObject /Subtype /Form /FormType 1 /BBox [")
        + pdfNumber(boundingRect.left) + ' '
        + pdfNumber(boundingRect.bottom) + ' '
        + pdfNumber(boundingRect.right) + ' '
        + pdfNumber(boundingRect.top)
        + QByteArrayLiteral(
            "] /Matrix [1 0 0 1 0 0] "
            "/Resources << /ExtGState << /GS << /Type /ExtGState /CA ")
        + pdfNumber(opacity)
        + QByteArrayLiteral(" /ca ")
        + pdfNumber(opacity)
        + QByteArrayLiteral(" /AIS false /BM /")
        + blendMode
        + QByteArrayLiteral(
            " >> >> >> /Length ")
        + QByteArray::number(content.size())
        + QByteArrayLiteral(" >>\nstream\n")
        + content
        + QByteArrayLiteral("endstream\nendobj");
}

QByteArrayView drawingSubtypeName(PDFCommentAnnotation::DrawingType type)
{
    switch (type) {
    case PDFCommentAnnotation::DrawingType::Square:
        return QByteArrayView("/Square");
    case PDFCommentAnnotation::DrawingType::Circle:
        return QByteArrayView("/Circle");
    case PDFCommentAnnotation::DrawingType::Line:
        return QByteArrayView("/Line");
    case PDFCommentAnnotation::DrawingType::Polygon:
        return QByteArrayView("/Polygon");
    case PDFCommentAnnotation::DrawingType::PolyLine:
        return QByteArrayView("/PolyLine");
    case PDFCommentAnnotation::DrawingType::Ink:
        return QByteArrayView("/Ink");
    }
    return {};
}

QByteArrayView lineEndingName(PDFCommentAnnotation::LineEnding ending)
{
    switch (ending) {
    case PDFCommentAnnotation::LineEnding::None:
        return QByteArrayView("/None");
    case PDFCommentAnnotation::LineEnding::Square:
        return QByteArrayView("/Square");
    case PDFCommentAnnotation::LineEnding::Circle:
        return QByteArrayView("/Circle");
    case PDFCommentAnnotation::LineEnding::Diamond:
        return QByteArrayView("/Diamond");
    case PDFCommentAnnotation::LineEnding::OpenArrow:
        return QByteArrayView("/OpenArrow");
    case PDFCommentAnnotation::LineEnding::ClosedArrow:
        return QByteArrayView("/ClosedArrow");
    case PDFCommentAnnotation::LineEnding::Butt:
        return QByteArrayView("/Butt");
    case PDFCommentAnnotation::LineEnding::ReverseOpenArrow:
        return QByteArrayView("/ROpenArrow");
    case PDFCommentAnnotation::LineEnding::ReverseClosedArrow:
        return QByteArrayView("/RClosedArrow");
    case PDFCommentAnnotation::LineEnding::Slash:
        return QByteArrayView("/Slash");
    }
    return {};
}

QByteArray pdfColorArray(const QColor &color)
{
    if (!color.isValid()) {
        return QByteArrayLiteral("[]");
    }
    const QColor rgb = color.toRgb();
    return QByteArrayLiteral("[")
        + pdfNumber(rgb.redF()) + ' '
        + pdfNumber(rgb.greenF()) + ' '
        + pdfNumber(rgb.blueF()) + ']';
}

QByteArray pdfPointArray(const QVector<QPointF> &points)
{
    QByteArray value = QByteArrayLiteral("[");
    for (const QPointF &point : points) {
        if (value.size() > 1) {
            value += ' ';
        }
        value += pdfNumber(point.x()) + ' ' + pdfNumber(point.y());
    }
    value += ']';
    return value;
}

QByteArray pdfInkList(const QVector<QVector<QPointF>> &strokes)
{
    QByteArray value = QByteArrayLiteral("[");
    for (const QVector<QPointF> &stroke : strokes) {
        if (value.size() > 1) {
            value += ' ';
        }
        value += pdfPointArray(stroke);
    }
    value += ']';
    return value;
}

bool isSafePdfName(const QString &name)
{
    constexpr qsizetype maximumAcceptedPdfNameCharacters = 64;
    if (name.isEmpty()
        || name.size() > maximumAcceptedPdfNameCharacters) {
        return false;
    }
    return std::all_of(
        name.cbegin(),
        name.cend(),
        [](QChar character) {
            const ushort value = character.unicode();
            return (value >= 'A' && value <= 'Z')
                || (value >= 'a' && value <= 'z')
                || (value >= '0' && value <= '9')
                || value == '-' || value == '_';
        });
}

bool drawingAppearanceData(
    const PDFCommentAnnotation::DrawingAnnotation &annotation,
    QByteArray &content,
    QString &error)
{
    constexpr qsizetype maximumVertices = 4096;
    constexpr qsizetype maximumInkStrokes = 64;
    constexpr qsizetype maximumInkPoints = 32768;
    constexpr qsizetype maximumDashEntries = 16;
    constexpr qsizetype maximumAppearanceBytes = 8 * 1024 * 1024;
    constexpr qreal maximumCoordinateMagnitudePoints = 10'000'000.0;
    constexpr qreal maximumBorderWidthPoints = 1024.0;
    constexpr qreal maximumDashElementLengthPoints = 10'000.0;
    const QRectF bounds = annotation.pageRect.normalized();
    const auto finitePoint = [
        maximumCoordinateMagnitudePoints](const QPointF &point) {
        return std::isfinite(point.x())
            && std::isfinite(point.y())
            && qAbs(point.x()) <= maximumCoordinateMagnitudePoints
            && qAbs(point.y()) <= maximumCoordinateMagnitudePoints;
    };
    const auto &properties = annotation.properties;
    if (!bounds.isValid()
        || bounds.width() <= 0.0
        || bounds.height() <= 0.0
        || !finitePoint(bounds.topLeft())
        || !finitePoint(bounds.bottomRight())
        || !std::isfinite(properties.opacity)
        || properties.opacity < 0.0
        || properties.opacity > 1.0
        || !std::isfinite(properties.borderWidth)
        || properties.borderWidth <= 0.0
        || properties.borderWidth > maximumBorderWidthPoints
        || !properties.strokeColor.isValid()
        || properties.flags < 0
        || (properties.borderStyle
                != PDFCommentAnnotation::BorderStyle::Solid
            && properties.borderStyle
                != PDFCommentAnnotation::BorderStyle::Dashed)
        || !std::isfinite(properties.borderEffectIntensity)
        || properties.borderEffectIntensity < 0.0
        || properties.borderEffectIntensity > 2.0
        || properties.borderEffect != PDFCommentAnnotation::BorderEffect::None
        || properties.startLineEnding != PDFCommentAnnotation::LineEnding::None
        || properties.endLineEnding != PDFCommentAnnotation::LineEnding::None) {
        error = QStringLiteral(
            "A drawing annotation appearance request has unsupported properties.");
        return false;
    }

    const bool rectangular
        = annotation.type == PDFCommentAnnotation::DrawingType::Square
        || annotation.type == PDFCommentAnnotation::DrawingType::Circle;
    const bool ink
        = annotation.type == PDFCommentAnnotation::DrawingType::Ink;
    const qsizetype expectedMinimum
        = annotation.type == PDFCommentAnnotation::DrawingType::Polygon
        ? 3
        : 2;
    bool geometryValid = true;
    if (ink) {
        geometryValid = annotation.pageVertices.isEmpty()
            && !annotation.pageStrokes.isEmpty()
            && annotation.pageStrokes.size() <= maximumInkStrokes
            && !annotation.properties.interiorColor.isValid();
        qsizetype totalInkPoints = 0;
        for (const QVector<QPointF> &stroke : annotation.pageStrokes) {
            geometryValid = geometryValid
                && stroke.size() >= 2
                && stroke.size() <= maximumVertices
                && stroke.size() <= maximumInkPoints - totalInkPoints
                && std::all_of(
                    stroke.cbegin(),
                    stroke.cend(),
                    finitePoint);
            if (!geometryValid) {
                break;
            }
            totalInkPoints += stroke.size();
        }
    } else {
        geometryValid = annotation.pageStrokes.isEmpty()
            && std::all_of(
                annotation.pageVertices.cbegin(),
                annotation.pageVertices.cend(),
                finitePoint);
        if (rectangular) {
            geometryValid = geometryValid
                && annotation.pageVertices.size() == 2;
        } else {
            geometryValid = geometryValid
                && annotation.pageVertices.size() >= expectedMinimum
                && annotation.pageVertices.size() <= maximumVertices;
        }
        if (annotation.type == PDFCommentAnnotation::DrawingType::Line) {
            geometryValid = geometryValid
                && annotation.pageVertices.size() == 2;
        }
    }
    if (!geometryValid) {
        error = QStringLiteral(
            "A drawing annotation geometry request is invalid.");
        return false;
    }

    if (ink) {
        for (const QVector<QPointF> &stroke : annotation.pageStrokes) {
            for (qsizetype index = 1; index < stroke.size(); ++index) {
                const QPointF previous = stroke.at(index - 1);
                const QPointF current = stroke.at(index);
                if (std::hypot(
                        current.x() - previous.x(),
                        current.y() - previous.y()) <= 0.0) {
                    error = QStringLiteral(
                        "An Ink annotation contains a zero-length segment.");
                    return false;
                }
            }
        }
    } else {
        for (qsizetype index = 1;
             index < annotation.pageVertices.size();
             ++index) {
            const QPointF previous = annotation.pageVertices.at(index - 1);
            const QPointF current = annotation.pageVertices.at(index);
            if (std::hypot(
                    current.x() - previous.x(),
                    current.y() - previous.y()) <= 0.0) {
                error = QStringLiteral(
                    "A drawing annotation contains a zero-length segment.");
                return false;
            }
        }
    }
    if (annotation.type == PDFCommentAnnotation::DrawingType::Polygon) {
        const QPointF first = annotation.pageVertices.constFirst();
        const QPointF last = annotation.pageVertices.constLast();
        if (std::hypot(first.x() - last.x(), first.y() - last.y()) <= 0.0) {
            error = QStringLiteral(
                "A Polygon annotation must not repeat its first vertex.");
            return false;
        }
    }

    QByteArray dashCommand = QByteArrayLiteral("[] 0 d\n");
    if (properties.borderStyle == PDFCommentAnnotation::BorderStyle::Dashed) {
        QVector<qreal> dashes = properties.dashPattern;
        if (dashes.isEmpty()) {
            dashes = {3.0, 2.0};
        }
        if (dashes.size() > maximumDashEntries
            || !std::all_of(
                dashes.cbegin(),
                dashes.cend(),
                [maximumDashElementLengthPoints](qreal dash) {
                    return std::isfinite(dash)
                        && dash > 0.0
                        && dash <= maximumDashElementLengthPoints;
                })) {
            error = QStringLiteral(
                "A drawing annotation dash pattern is invalid.");
            return false;
        }
        dashCommand = QByteArrayLiteral("[");
        for (qreal dash : std::as_const(dashes)) {
            if (dashCommand.size() > 1) {
                dashCommand += ' ';
            }
            dashCommand += pdfNumber(dash);
        }
        dashCommand += QByteArrayLiteral("] 0 d\n");
    }

    const QColor stroke = properties.strokeColor.toRgb();
    content = QByteArrayLiteral("q\n/GS gs\n")
        + pdfNumber(stroke.redF()) + ' '
        + pdfNumber(stroke.greenF()) + ' '
        + pdfNumber(stroke.blueF()) + QByteArrayLiteral(" RG\n")
        + pdfNumber(properties.borderWidth)
            + QByteArrayLiteral(" w\n1 J 1 j\n")
        + dashCommand;
    const bool hasInterior = properties.interiorColor.isValid();
    if (hasInterior) {
        const QColor interior = properties.interiorColor.toRgb();
        content += pdfNumber(interior.redF()) + ' '
            + pdfNumber(interior.greenF()) + ' '
            + pdfNumber(interior.blueF()) + QByteArrayLiteral(" rg\n");
    }

    if (ink) {
        for (const QVector<QPointF> &stroke : annotation.pageStrokes) {
            content += pdfNumber(stroke.constFirst().x()) + ' '
                + pdfNumber(stroke.constFirst().y())
                + QByteArrayLiteral(" m\n");
            for (qsizetype index = 1; index < stroke.size(); ++index) {
                content += pdfNumber(stroke.at(index).x()) + ' '
                    + pdfNumber(stroke.at(index).y())
                    + QByteArrayLiteral(" l\n");
                if (content.size() > maximumAppearanceBytes) {
                    error = QStringLiteral(
                        "An Ink annotation appearance is too large.");
                    return false;
                }
            }
            content += QByteArrayLiteral("S\n");
        }
    } else if (annotation.type == PDFCommentAnnotation::DrawingType::Square) {
        const qreal inset = properties.borderWidth / 2.0;
        const qreal left = bounds.left() + inset;
        const qreal right = bounds.right() - inset;
        const qreal bottom = bounds.top() + inset;
        const qreal top = bounds.bottom() - inset;
        if (right <= left || top <= bottom) {
            error = QStringLiteral("A Square annotation rectangle is too small.");
            return false;
        }
        content += pdfNumber(left) + ' ' + pdfNumber(bottom)
            + QByteArrayLiteral(" m ") + pdfNumber(right) + ' '
            + pdfNumber(bottom) + QByteArrayLiteral(" l ")
            + pdfNumber(right) + ' ' + pdfNumber(top)
            + QByteArrayLiteral(" l ") + pdfNumber(left) + ' '
            + pdfNumber(top) + QByteArrayLiteral(" l h ")
            + (hasInterior ? QByteArrayLiteral("B\n")
                           : QByteArrayLiteral("S\n"));
    } else if (annotation.type == PDFCommentAnnotation::DrawingType::Circle) {
        constexpr qreal kappa = 0.5522847498307936;
        const qreal inset = properties.borderWidth / 2.0;
        const qreal left = bounds.left() + inset;
        const qreal right = bounds.right() - inset;
        const qreal bottom = bounds.top() + inset;
        const qreal top = bounds.bottom() - inset;
        const qreal radiusX = (right - left) / 2.0;
        const qreal radiusY = (top - bottom) / 2.0;
        if (radiusX <= 0.0 || radiusY <= 0.0) {
            error = QStringLiteral("A Circle annotation rectangle is too small.");
            return false;
        }
        const qreal centerX = (left + right) / 2.0;
        const qreal centerY = (bottom + top) / 2.0;
        content += pdfNumber(centerX + radiusX) + ' '
            + pdfNumber(centerY) + QByteArrayLiteral(" m\n")
            + pdfNumber(centerX + radiusX) + ' '
            + pdfNumber(centerY + kappa * radiusY) + ' '
            + pdfNumber(centerX + kappa * radiusX) + ' '
            + pdfNumber(centerY + radiusY) + ' '
            + pdfNumber(centerX) + ' '
            + pdfNumber(centerY + radiusY) + QByteArrayLiteral(" c\n")
            + pdfNumber(centerX - kappa * radiusX) + ' '
            + pdfNumber(centerY + radiusY) + ' '
            + pdfNumber(centerX - radiusX) + ' '
            + pdfNumber(centerY + kappa * radiusY) + ' '
            + pdfNumber(centerX - radiusX) + ' '
            + pdfNumber(centerY) + QByteArrayLiteral(" c\n")
            + pdfNumber(centerX - radiusX) + ' '
            + pdfNumber(centerY - kappa * radiusY) + ' '
            + pdfNumber(centerX - kappa * radiusX) + ' '
            + pdfNumber(centerY - radiusY) + ' '
            + pdfNumber(centerX) + ' '
            + pdfNumber(centerY - radiusY) + QByteArrayLiteral(" c\n")
            + pdfNumber(centerX + kappa * radiusX) + ' '
            + pdfNumber(centerY - radiusY) + ' '
            + pdfNumber(centerX + radiusX) + ' '
            + pdfNumber(centerY - kappa * radiusY) + ' '
            + pdfNumber(centerX + radiusX) + ' '
            + pdfNumber(centerY) + QByteArrayLiteral(" c h ")
            + (hasInterior ? QByteArrayLiteral("B\n")
                           : QByteArrayLiteral("S\n"));
    } else {
        const QVector<QPointF> &points = annotation.pageVertices;
        content += pdfNumber(points.constFirst().x()) + ' '
            + pdfNumber(points.constFirst().y()) + QByteArrayLiteral(" m\n");
        for (qsizetype index = 1; index < points.size(); ++index) {
            content += pdfNumber(points.at(index).x()) + ' '
                + pdfNumber(points.at(index).y()) + QByteArrayLiteral(" l\n");
            if (content.size() > maximumAppearanceBytes) {
                error = QStringLiteral(
                    "A drawing annotation appearance is too large.");
                return false;
            }
        }
        if (annotation.type == PDFCommentAnnotation::DrawingType::Polygon) {
            content += hasInterior
                ? QByteArrayLiteral("h B\n")
                : QByteArrayLiteral("h S\n");
        } else {
            content += QByteArrayLiteral("S\n");
        }
    }
    content += QByteArrayLiteral("Q\n");
    if (content.size() > maximumAppearanceBytes) {
        error = QStringLiteral("A drawing annotation appearance is too large.");
        return false;
    }
    return true;
}

QByteArray drawingAppearanceObject(
    int objectNumber,
    const PDFCommentAnnotation::DrawingAnnotation &annotation,
    const QByteArray &content)
{
    const QRectF bounds = annotation.pageRect.normalized();
    return QByteArray::number(objectNumber)
        + QByteArrayLiteral(
            " 0 obj\n"
            "<< /Type /XObject /Subtype /Form /FormType 1 /BBox [")
        + pdfNumber(bounds.left()) + ' '
        + pdfNumber(bounds.top()) + ' '
        + pdfNumber(bounds.right()) + ' '
        + pdfNumber(bounds.bottom())
        + QByteArrayLiteral(
            "] /Matrix [1 0 0 1 0 0] "
            "/Resources << /ExtGState << /GS << /Type /ExtGState /CA ")
        + pdfNumber(annotation.properties.opacity)
        + QByteArrayLiteral(" /ca ")
        + pdfNumber(annotation.properties.opacity)
        + QByteArrayLiteral(
            " /AIS false /BM /Normal >> >> >> /Length ")
        + QByteArray::number(content.size())
        + QByteArrayLiteral(" >>\nstream\n")
        + content
        + QByteArrayLiteral("endstream\nendobj");
}

bool configureDrawingAnnotationDictionary(
    QByteArray &dictionary,
    const PDFCommentAnnotation::DrawingAnnotation &annotation,
    int appearanceObjectNumber,
    QString &error)
{
    const QRectF bounds = annotation.pageRect.normalized();
    const auto replace = [
        &dictionary,
        &error](QByteArrayView key,
                const QByteArray &value,
                bool insertIfMissing = true) {
        const bool replaced = replaceTopLevelDictionaryValue(
            dictionary,
            key,
            value,
            insertIfMissing,
            error);
        if (!replaced) {
            error = QStringLiteral("Drawing annotation key %1: %2")
                .arg(
                    QString::fromLatin1(key.data(), key.size()),
                    error);
        }
        return replaced;
    };
    const QByteArray rect = QByteArrayLiteral("[")
        + pdfNumber(bounds.left()) + ' '
        + pdfNumber(bounds.top()) + ' '
        + pdfNumber(bounds.right()) + ' '
        + pdfNumber(bounds.bottom()) + ']';
    if (!replace(QByteArrayView("/Rect"), rect)
        || !replace(
            QByteArrayView("/Subtype"),
            QByteArray(
                drawingSubtypeName(annotation.type).data(),
                drawingSubtypeName(annotation.type).size()))
        || !replace(
            QByteArrayView("/C"),
            pdfColorArray(annotation.properties.strokeColor))
        || !replace(
            QByteArrayView("/CA"),
            pdfNumber(annotation.properties.opacity))
        || !replace(
            QByteArrayView("/F"),
            QByteArray::number(annotation.properties.flags))
        || !replace(
            QByteArrayView("/Border"),
            QByteArrayLiteral("null"),
            false)) {
        return false;
    }
    if (annotation.type != PDFCommentAnnotation::DrawingType::Ink
        && annotation.properties.interiorColor.isValid()
        && !replace(
            QByteArrayView("/IC"),
            pdfColorArray(annotation.properties.interiorColor))) {
        return false;
    }

    QVector<qreal> dashes = annotation.properties.dashPattern;
    if (annotation.properties.borderStyle
            == PDFCommentAnnotation::BorderStyle::Dashed
        && dashes.isEmpty()) {
        dashes = {3.0, 2.0};
    }
    QByteArray borderStyle = QByteArrayLiteral("<< /Type /Border /W ")
        + pdfNumber(annotation.properties.borderWidth);
    if (annotation.properties.borderStyle
        == PDFCommentAnnotation::BorderStyle::Dashed) {
        borderStyle += QByteArrayLiteral(" /S /D /D [");
        for (qreal dash : std::as_const(dashes)) {
            if (!borderStyle.endsWith('[')) {
                borderStyle += ' ';
            }
            borderStyle += pdfNumber(dash);
        }
        borderStyle += QByteArrayLiteral("] >>");
    } else {
        borderStyle += QByteArrayLiteral(" /S /S >>");
    }
    if (!replace(QByteArrayView("/BS"), borderStyle)) {
        return false;
    }

    if (annotation.type == PDFCommentAnnotation::DrawingType::Ink) {
        if (!replace(
                QByteArrayView("/InkList"),
                pdfInkList(annotation.pageStrokes))) {
            return false;
        }
    } else if (annotation.type == PDFCommentAnnotation::DrawingType::Line) {
        if (!replace(
                QByteArrayView("/L"),
                pdfPointArray(annotation.pageVertices))) {
            return false;
        }
    } else if (annotation.type == PDFCommentAnnotation::DrawingType::Polygon
               || annotation.type
                    == PDFCommentAnnotation::DrawingType::PolyLine) {
        if (!replace(
                QByteArrayView("/Vertices"),
                pdfPointArray(annotation.pageVertices))) {
            return false;
        }
    }

    if (annotation.type == PDFCommentAnnotation::DrawingType::Line
        || annotation.type == PDFCommentAnnotation::DrawingType::PolyLine) {
        const QByteArrayView startEnding = lineEndingName(
            annotation.properties.startLineEnding);
        const QByteArrayView endEnding = lineEndingName(
            annotation.properties.endLineEnding);
        if (startEnding.isEmpty()
            || endEnding.isEmpty()
            || !replace(
                QByteArrayView("/LE"),
                QByteArrayLiteral("[")
                    + QByteArray(startEnding.data(), startEnding.size())
                    + ' '
                    + QByteArray(endEnding.data(), endEnding.size())
                    + ']')) {
            return false;
        }
    }
    if (!annotation.properties.intent.isEmpty()) {
        if (!isSafePdfName(annotation.properties.intent)
            || !replace(
                QByteArrayView("/IT"),
                '/' + annotation.properties.intent.toLatin1())) {
            error = QStringLiteral("A drawing annotation intent is invalid.");
            return false;
        }
    }
    if (!annotation.properties.name.isEmpty()) {
        if (!isSafePdfName(annotation.properties.name)
            || !replace(
                QByteArrayView("/Name"),
                '/' + annotation.properties.name.toLatin1())) {
            error = QStringLiteral("A drawing annotation name is invalid.");
            return false;
        }
    }
    return replace(
        QByteArrayView("/AP"),
        QByteArrayLiteral("<< /N ")
            + QByteArray::number(appearanceObjectNumber)
            + QByteArrayLiteral(" 0 R >>"));
}

bool freeTextAppearanceRect(const QByteArray &dictionary,
                            QRectF &pageRect,
                            QString &error)
{
    const qsizetype rectKey = keyPosition(
        dictionary.constData(),
        dictionary.size(),
        QByteArrayView("/Rect"));
    Cursor cursor(
        dictionary.constData(),
        dictionary.size(),
        rectKey >= 0
            ? rectKey + QByteArrayView("/Rect").size()
            : dictionary.size());
    if (rectKey < 0 || !cursor.consume("[")) {
        error = QStringLiteral(
            "A Free Text annotation Rect entry is missing or malformed.");
        return false;
    }

    std::array<qreal, 4> coordinates{};
    qsizetype position = cursor.position();
    for (qreal &coordinate : coordinates) {
        qsizetype end = -1;
        if (!parsePdfReal(
                dictionary.constData(),
                dictionary.size(),
                position,
                coordinate,
                end)) {
            error = QStringLiteral(
                "A Free Text annotation Rect entry is malformed.");
            return false;
        }
        position = end;
    }
    Cursor closingCursor(
        dictionary.constData(),
        dictionary.size(),
        position);
    if (!closingCursor.consume("]")) {
        error = QStringLiteral(
            "A Free Text annotation Rect entry is malformed.");
        return false;
    }

    const qreal width = qAbs(coordinates[2] - coordinates[0]);
    const qreal height = qAbs(coordinates[3] - coordinates[1]);
    if (width <= 0.0 || height <= 0.0) {
        error = QStringLiteral(
            "A Free Text annotation Rect entry is empty.");
        return false;
    }
    pageRect = QRectF(0.0, 0.0, width, height);
    return true;
}

bool removeTopLevelDictionaryValue(QByteArray &object,
                                   QByteArrayView key,
                                   QString &error)
{
    const qsizetype dictionaryStart = object.indexOf("<<");
    qsizetype dictionaryEnd = -1;
    if (dictionaryStart < 0
        || !matchingDictionaryEnd(
            object.constData(),
            object.size(),
            dictionaryStart,
            dictionaryEnd)) {
        error = QStringLiteral("A PDF dictionary is malformed.");
        return false;
    }

    qsizetype position = dictionaryStart + 2;
    while (position < dictionaryEnd - 2) {
        Cursor cursor(object.constData(), object.size(), position);
        cursor.skipWhitespaceAndComments();
        const qsizetype keyStart = cursor.position();
        if (keyStart >= dictionaryEnd - 2) {
            break;
        }
        if (object.at(keyStart) != '/') {
            error = QStringLiteral("A PDF dictionary entry is malformed.");
            return false;
        }
        qsizetype keyEnd = keyStart + 1;
        while (keyEnd < dictionaryEnd - 2
               && !isDelimiter(object.at(keyEnd))) {
            ++keyEnd;
        }
        Cursor valueCursor(object.constData(), object.size(), keyEnd);
        valueCursor.skipWhitespaceAndComments();
        const qsizetype valueStart = valueCursor.position();
        qsizetype valueEnd = -1;
        if (valueStart >= dictionaryEnd - 2
            || !pdfValueEnd(
                object.constData(),
                dictionaryEnd - 2,
                valueStart,
                valueEnd)) {
            error = QStringLiteral("A PDF dictionary value is malformed.");
            return false;
        }
        const QByteArrayView currentKey(
            object.constData() + keyStart,
            keyEnd - keyStart);
        if (currentKey == key) {
            object.remove(keyStart, valueEnd - keyStart);
            return true;
        }
        position = valueEnd;
    }
    return true;
}

bool updateFreeTextAppearanceDictionary(
    QByteArray &dictionary,
    int appearanceObjectNumber,
    QByteArrayView fontResourceName,
    bool preserveDefaultAppearance,
    QString &error)
{
    const QByteArray appearance
        = QByteArrayLiteral("<< /N ")
        + QByteArray::number(appearanceObjectNumber)
        + QByteArrayLiteral(" 0 R >>");
    const QByteArray defaultAppearance
        = QByteArrayLiteral("(")
        + textFieldDefaultAppearance(fontResourceName)
        + ')';
    if (!replaceTopLevelDictionaryValue(
               dictionary,
               QByteArrayView("/AP"),
               appearance,
               true,
               error)) {
        return false;
    }
    if ((!preserveDefaultAppearance
         || keyPosition(
                dictionary.constData(),
                dictionary.size(),
                QByteArrayView("/DA")) < 0)
        && !replaceTopLevelDictionaryValue(
               dictionary,
               QByteArrayView("/DA"),
               defaultAppearance,
               true,
               error)) {
        return false;
    }
    return replaceTopLevelDictionaryValue(
            dictionary,
            QByteArrayView("/BS"),
            QByteArrayLiteral("<< /W 1 /S /S >>"),
            true,
            error)
        && replaceTopLevelDictionaryValue(
            dictionary,
            QByteArrayView("/TrePDFFreeTextApVersion"),
            QByteArrayLiteral("1"),
            true,
            error)
        && removeTopLevelDictionaryValue(
            dictionary,
            QByteArrayView("/TrePDFFreeTextAppearanceId"),
            error);
}

QByteArray normalizedFieldAppearanceObject(
    const QByteArray &existingObject,
    int appearanceObjectNumber,
    int appearanceVersion,
    QByteArrayView fontResourceName,
    bool replaceDefaultAppearance,
    QString &error)
{
    // Keep field-level extensions such as /MaxLen, /Q and /AA intact. A
    // legacy visual migration owns only the normal appearance and its marker.
    QByteArray object = existingObject;
    const QByteArray appearance
        = QByteArrayLiteral("<< /N ")
        + QByteArray::number(appearanceObjectNumber)
        + QByteArrayLiteral(" 0 R >>");
    const QByteArray version
        = QByteArray::number(appearanceVersion);
    if (!replaceTopLevelDictionaryValue(
            object,
            QByteArrayView("/AP"),
            appearance,
            true,
            error)
        || !replaceTopLevelDictionaryValue(
            object,
            QByteArrayView("/TrePDFApVersion"),
            version,
            true,
            error)) {
        return {};
    }
    if (replaceDefaultAppearance) {
        const QByteArray defaultAppearance
            = QByteArrayLiteral("(")
            + textFieldDefaultAppearance(fontResourceName)
            + ')';
        if (!replaceTopLevelDictionaryValue(
                object,
                QByteArrayView("/DA"),
                defaultAppearance,
                true,
                error)) {
            return {};
        }
    }
    return object;
}

bool dictionaryValue(const ParsedPdf &pdf,
                     const QByteArray &object,
                     qsizetype valueStart,
                     QByteArray &dictionary,
                     qsizetype &valueEnd,
                     QString &error)
{
    Cursor cursor(object.constData(), object.size(), valueStart);
    cursor.skipWhitespaceAndComments();
    valueStart = cursor.position();
    if (valueStart >= object.size()) {
        error = QStringLiteral("A PDF resource dictionary is truncated.");
        return false;
    }
    if (valueStart + 1 < object.size()
        && object.at(valueStart) == '<'
        && object.at(valueStart + 1) == '<') {
        if (!matchingDictionaryEnd(
                object.constData(),
                object.size(),
                valueStart,
                valueEnd)) {
            error = QStringLiteral("A PDF resource dictionary is malformed.");
            return false;
        }
        dictionary = object.mid(valueStart, valueEnd - valueStart);
        return true;
    }

    ObjectReference reference;
    if (!parseReference(
            object.constData(),
            object.size(),
            valueStart,
            reference,
            &valueEnd)) {
        error = QStringLiteral("A PDF resource dictionary value is unsupported.");
        return false;
    }
    const QByteArray indirectObject = completeObject(
        pdf,
        reference.number,
        error);
    if (indirectObject.isEmpty()) {
        return false;
    }
    const qsizetype dictionaryStart = indirectObject.indexOf("<<");
    qsizetype dictionaryEnd = -1;
    if (dictionaryStart < 0
        || !matchingDictionaryEnd(
            indirectObject.constData(),
            indirectObject.size(),
            dictionaryStart,
            dictionaryEnd)) {
        error = QStringLiteral(
            "An indirect PDF resource dictionary is malformed.");
        return false;
    }
    dictionary = indirectObject.mid(
        dictionaryStart,
        dictionaryEnd - dictionaryStart);
    return true;
}

bool ensureFormFontResource(const ParsedPdf &pdf,
                            QByteArray &acroFormDictionary,
                            const ObjectReference &fontReference,
                            QByteArrayView fontResourceName,
                            QString &error)
{
    const QByteArray fontEntry = QByteArrayLiteral(" /")
        + fontResourceName
        + ' '
        + QByteArray::number(fontReference.number)
        + ' '
        + QByteArray::number(fontReference.generation)
        + QByteArrayLiteral(" R");
    const qsizetype drKey = keyPosition(
        acroFormDictionary.constData(),
        acroFormDictionary.size(),
        "/DR");
    if (drKey < 0) {
        const qsizetype dictionaryEnd = acroFormDictionary.lastIndexOf(">>");
        if (dictionaryEnd < 0) {
            error = QStringLiteral("The AcroForm dictionary is malformed.");
            return false;
        }
        acroFormDictionary.insert(
            dictionaryEnd,
            QByteArrayLiteral(" /DR << /Font <<")
                + fontEntry
                + QByteArrayLiteral(" >> >>"));
        return true;
    }

    const qsizetype drValueStart
        = drKey + QByteArrayView("/DR").size();
    QByteArray resources;
    qsizetype drValueEnd = -1;
    if (!dictionaryValue(
            pdf,
            acroFormDictionary,
            drValueStart,
            resources,
            drValueEnd,
            error)) {
        return false;
    }
    const qsizetype fontKey = keyPosition(
        resources.constData(),
        resources.size(),
        "/Font");
    if (fontKey < 0) {
        const qsizetype resourcesEnd = resources.lastIndexOf(">>");
        if (resourcesEnd < 0) {
            error = QStringLiteral(
                "The AcroForm default resources are malformed.");
            return false;
        }
        resources.insert(
            resourcesEnd,
            QByteArrayLiteral(" /Font <<")
                + fontEntry
                + QByteArrayLiteral(" >>"));
    } else {
        const qsizetype fontValueStart
            = fontKey + QByteArrayView("/Font").size();
        QByteArray fontDictionary;
        qsizetype fontValueEnd = -1;
        if (!dictionaryValue(
                pdf,
                resources,
                fontValueStart,
                fontDictionary,
                fontValueEnd,
                error)) {
            return false;
        }
        const QByteArray fontKeyToken = '/'
            + QByteArray(
                fontResourceName.data(),
                fontResourceName.size());
        if (keyPosition(
                fontDictionary.constData(),
                fontDictionary.size(),
                fontKeyToken) < 0) {
            const qsizetype fontDictionaryEnd
                = fontDictionary.lastIndexOf(">>");
            if (fontDictionaryEnd < 0) {
                error = QStringLiteral(
                    "The AcroForm Font resources are malformed.");
                return false;
            }
            fontDictionary.insert(fontDictionaryEnd, fontEntry);
        }
        resources.replace(
            fontValueStart,
            fontValueEnd - fontValueStart,
            fontDictionary);
    }
    acroFormDictionary.replace(
        drValueStart,
        drValueEnd - drValueStart,
        resources);
    return true;
}

struct FormFontResource {
    ObjectReference reference;
    QByteArray name;
};

bool findUnicodeFormFontResource(
    const ParsedPdf &pdf,
    const ObjectReference &acroFormReference,
    FormFontResource &resource,
    QString &error)
{
    resource = {};
    if (acroFormReference.number < 0) {
        return true;
    }
    const QByteArray acroFormObject = completeObject(
        pdf,
        acroFormReference.number,
        error);
    if (acroFormObject.isEmpty()) {
        return false;
    }
    const qsizetype drKey = keyPosition(
        acroFormObject.constData(),
        acroFormObject.size(),
        "/DR");
    if (drKey < 0) {
        return true;
    }
    QByteArray resources;
    qsizetype resourcesEnd = -1;
    if (!dictionaryValue(
            pdf,
            acroFormObject,
            drKey + QByteArrayView("/DR").size(),
            resources,
            resourcesEnd,
            error)) {
        return false;
    }
    const qsizetype fontKey = keyPosition(
        resources.constData(),
        resources.size(),
        "/Font");
    if (fontKey < 0) {
        return true;
    }
    QByteArray fontDictionary;
    qsizetype fontDictionaryEnd = -1;
    if (!dictionaryValue(
            pdf,
            resources,
            fontKey + QByteArrayView("/Font").size(),
            fontDictionary,
            fontDictionaryEnd,
            error)) {
        return false;
    }

    const QByteArray resourcePrefix
        = QByteArrayLiteral("/")
        + QByteArrayLiteral(kUnicodeFormFontResourcePrefix);
    qsizetype position = 0;
    while ((position = findBytes(
                fontDictionary.constData(),
                fontDictionary.size(),
                resourcePrefix,
                position)) >= 0) {
        qsizetype nameEnd = position + resourcePrefix.size();
        while (nameEnd < fontDictionary.size()
               && !isDelimiter(fontDictionary.at(nameEnd))) {
            ++nameEnd;
        }
        ObjectReference candidateReference;
        if (!parseReference(
                fontDictionary.constData(),
                fontDictionary.size(),
                nameEnd,
                candidateReference)) {
            position = nameEnd;
            continue;
        }
        QString candidateError;
        const QByteArray candidateObject = completeObject(
            pdf,
            candidateReference.number,
            candidateError);
        const qsizetype versionKey = keyPosition(
            candidateObject.constData(),
            candidateObject.size(),
            QByteArrayLiteral("/")
                + QByteArray(PDFFormAppearance::kFontVersionKey));
        qint64 version = -1;
        Cursor versionCursor(
            candidateObject.constData(),
            candidateObject.size(),
            versionKey >= 0
                ? versionKey
                    + 1
                    + QByteArrayView(
                          PDFFormAppearance::kFontVersionKey).size()
                : candidateObject.size());
        const qsizetype subtypeKey = keyPosition(
            candidateObject.constData(),
            candidateObject.size(),
            "/Subtype");
        Cursor subtypeCursor(
            candidateObject.constData(),
            candidateObject.size(),
            subtypeKey >= 0
                ? subtypeKey + QByteArrayView("/Subtype").size()
                : candidateObject.size());
        const qsizetype encodingKey = keyPosition(
            candidateObject.constData(),
            candidateObject.size(),
            "/Encoding");
        Cursor encodingCursor(
            candidateObject.constData(),
            candidateObject.size(),
            encodingKey >= 0
                ? encodingKey + QByteArrayView("/Encoding").size()
                : candidateObject.size());
        if (!candidateObject.isEmpty()
            && versionCursor.integer(version)
            && version == PDFFormAppearance::kFontVersion
            && subtypeCursor.consume("/Type0")
            && encodingCursor.consume("/Identity-H")) {
            resource.reference = candidateReference;
            resource.name = fontDictionary.mid(
                position + 1,
                nameEnd - position - 1);
            return true;
        }
        position = nameEnd;
    }
    return true;
}

QByteArray updatedAcroFormObject(const ParsedPdf &pdf,
                                 const QList<ObjectReference> &references,
                                 const ObjectReference &targetReference,
                                 const ObjectReference &defaultFont,
                                 QByteArrayView fontResourceName,
                                 QString &error)
{
    const bool installDefaultFont = defaultFont.number >= 0
        && !fontResourceName.isEmpty();
    QByteArray dictionary;
    const QByteArrayView catalogView = objectView(pdf, pdf.root.number);
    if (catalogView.isEmpty()) {
        error = QStringLiteral("The PDF Catalog object is missing.");
        return {};
    }
    const qsizetype acroKey = keyPosition(
        catalogView.data(),
        catalogView.size(),
        "/AcroForm");
    if (acroKey >= 0) {
        ObjectReference currentAcroForm;
        if (!parseReference(
                catalogView.data(),
                catalogView.size(),
                acroKey + QByteArrayView("/AcroForm").size(),
                currentAcroForm)) {
            error = QStringLiteral("The Catalog AcroForm entry is not indirect.");
            return {};
        }
        QByteArray currentObject = completeObject(
            pdf,
            currentAcroForm.number,
            error);
        if (currentObject.isEmpty()) {
            return {};
        }
        QList<ObjectReference> ignored;
        qsizetype fieldsStart = -1;
        qsizetype fieldsEnd = -1;
        if (!fieldReferences(
                pdf,
                currentObject,
                ignored,
                fieldsStart,
                fieldsEnd,
                error)) {
            return {};
        }
        const qsizetype dictionaryStart = currentObject.indexOf("<<");
        const qsizetype dictionaryEnd = currentObject.lastIndexOf(">>");
        if (dictionaryStart < 0 || dictionaryEnd <= dictionaryStart) {
            error = QStringLiteral("The AcroForm dictionary is malformed.");
            return {};
        }
        dictionary = currentObject.mid(
            dictionaryStart,
            dictionaryEnd + 2 - dictionaryStart);
        if (fieldsStart >= 0) {
            const qsizetype relativeStart = fieldsStart - dictionaryStart;
            dictionary.replace(
                relativeStart,
                fieldsEnd - fieldsStart,
                referenceArray(references));
        } else {
            dictionary.insert(
                dictionary.size() - 2,
                QByteArrayLiteral(" /Fields ") + referenceArray(references));
        }
        if (installDefaultFont
            && keyPosition(
                dictionary.constData(),
                dictionary.size(),
                "/DA") < 0) {
            dictionary.insert(
                dictionary.size() - 2,
                QByteArrayLiteral(" /DA (")
                    + textFieldDefaultAppearance(fontResourceName)
                    + QByteArrayLiteral(")"));
        }
    } else {
        dictionary = QByteArrayLiteral("<< /Fields ")
            + referenceArray(references)
            + QByteArrayLiteral(" /NeedAppearances false /DA (")
            + textFieldDefaultAppearance(fontResourceName)
            + QByteArrayLiteral(") >>");
    }
    if (installDefaultFont
        && !ensureFormFontResource(
            pdf,
            dictionary,
            defaultFont,
            fontResourceName,
            error)) {
        return {};
    }

    return QByteArray::number(targetReference.number)
        + ' '
        + QByteArray::number(targetReference.generation)
        + QByteArrayLiteral(" obj\n")
        + dictionary
        + QByteArrayLiteral("\nendobj");
}

QByteArray standardFormFontObject(int objectNumber)
{
    return QByteArray::number(objectNumber)
        + QByteArrayLiteral(
            " 0 obj\n"
            "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
            "/Encoding /WinAnsiEncoding >>\n"
            "endobj");
}

QByteArray unicodeFormFontObject(int objectNumber,
                                 int descendantFontObjectNumber,
                                 int toUnicodeObjectNumber)
{
    return QByteArray::number(objectNumber)
        + QByteArrayLiteral(
            " 0 obj\n"
            "<< /Type /Font /Subtype /Type0 /BaseFont /")
        + QByteArrayLiteral(kUnicodeFormFontBaseName)
        + QByteArrayLiteral(
            " /Encoding /Identity-H /DescendantFonts [")
        + QByteArray::number(descendantFontObjectNumber)
        + QByteArrayLiteral(" 0 R] /ToUnicode ")
        + QByteArray::number(toUnicodeObjectNumber)
        + QByteArrayLiteral(" 0 R /")
        + QByteArray(PDFFormAppearance::kFontVersionKey)
        + ' '
        + QByteArray::number(PDFFormAppearance::kFontVersion)
        + QByteArrayLiteral(" >>\nendobj");
}

QByteArray unicodeFormDescendantFontObject(
    int objectNumber,
    int widthsObjectNumber,
    int descriptorObjectNumber,
    int cidToGidMapObjectNumber)
{
    return QByteArray::number(objectNumber)
        + QByteArrayLiteral(
            " 0 obj\n"
            "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /")
        + QByteArrayLiteral(kUnicodeFormFontBaseName)
        + QByteArrayLiteral(
            " /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) "
            "/Supplement 0 >> /DW 1000 /W ")
        + QByteArray::number(widthsObjectNumber)
        + QByteArrayLiteral(" 0 R /CIDToGIDMap ")
        + QByteArray::number(cidToGidMapObjectNumber)
        + QByteArrayLiteral(" 0 R /FontDescriptor ")
        + QByteArray::number(descriptorObjectNumber)
        + QByteArrayLiteral(" 0 R >>\nendobj");
}

QByteArray unicodeFormWidthsObject(
    int objectNumber,
    const UnicodeFormFontTemplate &font)
{
    return QByteArray::number(objectNumber)
        + QByteArrayLiteral(" 0 obj\n")
        + font.widths
        + QByteArrayLiteral("\nendobj");
}

QByteArray unicodeFormFontDescriptorObject(
    int objectNumber,
    int fontFileObjectNumber,
    const UnicodeFormFontTemplate &font)
{
    return QByteArray::number(objectNumber)
        + QByteArrayLiteral(
            " 0 obj\n"
            "<< /Type /FontDescriptor /FontName /")
        + QByteArrayLiteral(kUnicodeFormFontBaseName)
        + QByteArrayLiteral(" /Flags 4 /FontBBox [")
        + QByteArray::number(font.boundingLeft)
        + ' '
        + QByteArray::number(font.boundingBottom)
        + ' '
        + QByteArray::number(font.boundingRight)
        + ' '
        + QByteArray::number(font.boundingTop)
        + QByteArrayLiteral("] /ItalicAngle 0 /Ascent ")
        + QByteArray::number(font.ascent)
        + QByteArrayLiteral(" /Descent ")
        + QByteArray::number(font.descent)
        + QByteArrayLiteral(" /CapHeight ")
        + QByteArray::number(font.capHeight)
        + QByteArrayLiteral(" /XHeight ")
        + QByteArray::number(font.xHeight)
        + QByteArrayLiteral(" /Leading ")
        + QByteArray::number(font.leading)
        + QByteArrayLiteral(" /AvgWidth ")
        + QByteArray::number(font.averageWidth)
        + QByteArrayLiteral(" /MaxWidth ")
        + QByteArray::number(font.maximumWidth)
        + QByteArrayLiteral(" /StemV 80 /FontFile2 ")
        + QByteArray::number(fontFileObjectNumber)
        + QByteArrayLiteral(" 0 R >>\nendobj");
}

QByteArray unicodeFormFontFileObject(
    int objectNumber,
    const UnicodeFormFontTemplate &font)
{
    QByteArray object = QByteArray::number(objectNumber)
        + QByteArrayLiteral(
            " 0 obj\n"
            "<< /Length ")
        + QByteArray::number(font.compressedFontData.size())
        + QByteArrayLiteral(" /Filter /FlateDecode /Length1 ")
        + QByteArray::number(font.uncompressedFontSize)
        + QByteArrayLiteral(" >>\nstream\n");
    object += font.compressedFontData;
    object += QByteArrayLiteral("\nendstream\nendobj");
    return object;
}

QByteArray unicodeFormCidToGidMapObject(
    int objectNumber,
    const UnicodeFormFontTemplate &font)
{
    QByteArray object = QByteArray::number(objectNumber)
        + QByteArrayLiteral(" 0 obj\n<< /Length ")
        + QByteArray::number(font.compressedCidToGidMap.size())
        + QByteArrayLiteral(" /Filter /FlateDecode >>\nstream\n");
    object += font.compressedCidToGidMap;
    object += QByteArrayLiteral("\nendstream\nendobj");
    return object;
}

QByteArray unicodeFormToUnicodeObject(
    int objectNumber,
    const UnicodeFormFontTemplate &font)
{
    return QByteArray::number(objectNumber)
        + QByteArrayLiteral(" 0 obj\n<< /Length ")
        + QByteArray::number(font.toUnicode.size())
        + QByteArrayLiteral(" >>\nstream\n")
        + font.toUnicode
        + QByteArrayLiteral("endstream\nendobj");
}

bool existingFields(const ParsedPdf &pdf,
                    QList<ObjectReference> &references,
                    QString &error)
{
    const QByteArrayView catalogView = objectView(pdf, pdf.root.number);
    if (catalogView.isEmpty()) {
        error = QStringLiteral("The PDF Catalog object is missing.");
        return false;
    }
    const qsizetype acroKey = keyPosition(
        catalogView.data(),
        catalogView.size(),
        "/AcroForm");
    if (acroKey < 0) {
        return true;
    }
    ObjectReference acroForm;
    if (!parseReference(
            catalogView.data(),
            catalogView.size(),
            acroKey + QByteArrayView("/AcroForm").size(),
            acroForm)) {
        error = QStringLiteral("The Catalog AcroForm entry is not indirect.");
        return false;
    }
    const QByteArray object = completeObject(pdf, acroForm.number, error);
    if (object.isEmpty()) {
        return false;
    }
    qsizetype valueStart = -1;
    qsizetype valueEnd = -1;
    return fieldReferences(
        pdf,
        object,
        references,
        valueStart,
        valueEnd,
        error);
}

QByteArray xrefEntry(qint64 offset, int generation)
{
    return QByteArray::number(offset).rightJustified(10, '0')
        + ' '
        + QByteArray::number(generation).rightJustified(5, '0')
        + QByteArrayLiteral(" n \n");
}

struct IncrementalObject {
    ObjectReference reference;
    QByteArray data;
    qint64 offset = -1;
};

bool appendIncrementalUpdate(const QString &filePath,
                             const ObjectReference &root,
                             qint64 previousXref,
                             int declaredSize,
                             bool sourceEndsWithNewline,
                             QList<IncrementalObject> objects,
                             QString &error)
{
    constexpr qint64 maximumClassicXrefOffset = 9'999'999'999LL;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadWrite | QIODevice::Append)) {
        error = file.errorString();
        return false;
    }
    const qint64 originalSize = file.size();
    QByteArray update;
    qsizetype objectBytes = 0;
    for (const IncrementalObject &object : std::as_const(objects)) {
        objectBytes += object.data.size();
    }
    update.reserve(objectBytes + objects.size() * 48 + 320);
    if (originalSize > 0 && !sourceEndsWithNewline) {
        update += '\n';
    }
    std::sort(
        objects.begin(),
        objects.end(),
        [](const IncrementalObject &left,
           const IncrementalObject &right) {
            return left.reference.number < right.reference.number;
        });
    for (int index = 0; index < objects.size(); ++index) {
        if (index > 0
            && objects.at(index - 1).reference.number
                == objects.at(index).reference.number) {
            error = QStringLiteral("The incremental PDF update contains duplicate objects.");
            return false;
        }
        IncrementalObject &object = objects[index];
        object.offset = originalSize + update.size();
        if (object.offset > maximumClassicXrefOffset) {
            error = QStringLiteral(
                "The incremental PDF is too large for a classic xref table.");
            return false;
        }
        update += object.data;
        update += '\n';
    }
    const qint64 xrefOffset = originalSize + update.size();
    if (xrefOffset > maximumClassicXrefOffset) {
        error = QStringLiteral(
            "The incremental PDF is too large for a classic xref table.");
        return false;
    }
    update += "xref\n";
    update += "0 1\n0000000000 65535 f \n";
    for (const IncrementalObject &object : std::as_const(objects)) {
        update += QByteArray::number(object.reference.number) + " 1\n";
        update += xrefEntry(
            object.offset,
            object.reference.generation);
    }
    update += "trailer\n<< /Size ";
    update += QByteArray::number(declaredSize);
    update += " /Root ";
    update += QByteArray::number(root.number);
    update += ' ';
    update += QByteArray::number(root.generation);
    update += " R /Prev ";
    update += QByteArray::number(previousXref);
    update += " >>\nstartxref\n";
    update += QByteArray::number(xrefOffset);
    update += "\n%%EOF\n";

    if (file.write(update) != update.size() || !file.flush()) {
        const QString writeError = file.errorString();
        file.resize(originalSize);
        error = writeError;
        return false;
    }
    return true;
}

} // namespace

bool PDFAcroFormCanonicalizer::finalizeFields(
    const QString &filePath,
    const QVector<Field> &formFields,
    QString &error)
{
    if (formFields.isEmpty()) {
        return true;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0) {
        error = file.errorString();
        return false;
    }
    const qint64 fileSize = file.size();
    uchar *mapped = file.map(0, fileSize);
    if (!mapped) {
        error = file.errorString();
        return false;
    }

    ParsedPdf pdf;
    if (!parseXref(
            reinterpret_cast<const char *>(mapped),
            fileSize,
            pdf,
            error)) {
        file.unmap(mapped);
        return false;
    }

    const QByteArray sourceCatalog = completeObject(
        pdf,
        pdf.root.number,
        error);
    if (sourceCatalog.isEmpty()) {
        file.unmap(mapped);
        return false;
    }
    if (keyPosition(
            sourceCatalog.constData(),
            sourceCatalog.size(),
            "/Perms") >= 0) {
        error = QStringLiteral(
            "Adding fields to a PDF with document permissions is not supported.");
        file.unmap(mapped);
        return false;
    }
    ObjectReference sourceAcroFormReference;
    const qsizetype sourceAcroFormKey = keyPosition(
        sourceCatalog.constData(),
        sourceCatalog.size(),
        "/AcroForm");
    if (sourceAcroFormKey >= 0) {
        if (!parseReference(
                sourceCatalog.constData(),
                sourceCatalog.size(),
                sourceAcroFormKey + QByteArrayView("/AcroForm").size(),
                sourceAcroFormReference)) {
            error = QStringLiteral(
                "A direct Catalog AcroForm dictionary is not supported.");
            file.unmap(mapped);
            return false;
        }
        const QByteArray sourceAcroForm = completeObject(
            pdf,
            sourceAcroFormReference.number,
            error);
        if (sourceAcroForm.isEmpty()) {
            file.unmap(mapped);
            return false;
        }
        if (keyPosition(
                sourceAcroForm.constData(),
                sourceAcroForm.size(),
                "/XFA") >= 0
            || keyPosition(
                sourceAcroForm.constData(),
                sourceAcroForm.size(),
                "/SigFlags") >= 0) {
            error = QStringLiteral(
                "Adding fields to XFA or signed PDFs is not supported.");
            file.unmap(mapped);
            return false;
        }
    }

    QList<ObjectReference> fields;
    if (!existingFields(pdf, fields, error)) {
        file.unmap(mapped);
        return false;
    }
    QSet<QString> fieldNames;
    QHash<int, QString> fieldNamesByObject;
    QSet<int> visitedFieldObjects;
    if (!collectFieldNames(
            pdf,
            fields,
            {},
            visitedFieldObjects,
            fieldNames,
            fieldNamesByObject,
            error)) {
        file.unmap(mapped);
        return false;
    }
    for (int objectNumber : std::as_const(visitedFieldObjects)) {
        const QByteArray fieldObject = completeObject(
            pdf,
            objectNumber,
            error);
        if (fieldObject.isEmpty()) {
            file.unmap(mapped);
            return false;
        }
        const qsizetype fieldTypeKey = keyPosition(
            fieldObject.constData(),
            fieldObject.size(),
            "/FT");
        Cursor fieldTypeCursor(
            fieldObject.constData(),
            fieldObject.size(),
            fieldTypeKey >= 0
                ? fieldTypeKey + QByteArrayView("/FT").size()
                : fieldObject.size());
        if (fieldTypeKey >= 0 && fieldTypeCursor.consume("/Sig")) {
            error = QStringLiteral(
                "Adding fields to PDFs with signature fields is not supported.");
            file.unmap(mapped);
            return false;
        }
    }
    int highestObject = 0;
    for (auto entry = pdf.entries.cbegin(); entry != pdf.entries.cend(); ++entry) {
        highestObject = qMax(highestObject, entry.key());
    }
    int nextObjectNumber = qMax(
        pdf.declaredSize,
        highestObject + 1);

    struct ResolvedWidget {
        PlaceholderLocation placeholder;
        ObjectReference reference;
        ObjectReference fieldReference;
        ObjectReference sourceFieldReference;
        QList<ObjectReference> sourceKids;
        QByteArray existingObject;
        bool added = true;
        bool legacySingleSelectListBox = false;
    };
    QVector<ResolvedWidget> resolvedWidgets;
    resolvedWidgets.reserve(formFields.size());
    QList<ObjectReference> widgetReferences;
    widgetReferences.reserve(formFields.size());
    QSet<int> resolvedWidgetObjectNumbers;
    QHash<int, QVector<ObjectReference>> removedWidgetsByPage;
    QHash<int, ObjectReference> removedPageReferences;
    for (const Field &formField : formFields) {
        if (formField.widgetId.isEmpty()) {
            error = QStringLiteral(
                "TrePDF AcroForm operations require a non-empty Widget ID.");
            file.unmap(mapped);
            return false;
        }
        const bool added
            = formField.operation == Operation::Add;
        const bool removed
            = formField.operation == Operation::Remove;
        if (formField.type == FieldType::ListBox
            && !isTrePDFListBoxWidgetId(formField.widgetId)) {
            error = QStringLiteral(
                "The TrePDF List Box Widget ID is invalid.");
            file.unmap(mapped);
            return false;
        }
        if (removed
            && formField.type != FieldType::Text
            && formField.type != FieldType::Dropdown
            && formField.type != FieldType::ListBox
            && formField.type != FieldType::RadioButton) {
            error = QStringLiteral(
                "Only TrePDF Text Field, Dropdown, List Box, and Radio Button fields can currently be removed.");
            file.unmap(mapped);
            return false;
        }
        if (removed
            && ((formField.type == FieldType::Text
                 && !isTrePDFTextFieldWidgetId(formField.widgetId))
                || (formField.type == FieldType::Dropdown
                 && !isTrePDFDropdownWidgetId(formField.widgetId))
                || (formField.type == FieldType::ListBox
                    && !isTrePDFListBoxWidgetId(formField.widgetId))
                || (formField.type == FieldType::RadioButton
                    && !isTrePDFRadioButtonWidgetId(formField.widgetId)))) {
            error = QStringLiteral(
                "The removed Widget ID is not a supported TrePDF form field ID.");
            file.unmap(mapped);
            return false;
        }
        const PlaceholderLocation placeholder = annotationForWidgetId(
            pdf,
            formField.widgetId,
            added
                ? QByteArrayView("/Square")
                : QByteArrayView("/Widget"));
        const bool placeholderLocated = placeholder.widget.number >= 0
            || (placeholder.container.number >= 0
                && placeholder.dictionaryStart >= 0
                && placeholder.dictionaryEnd
                    > placeholder.dictionaryStart);
        if (!placeholderLocated) {
            error = QStringLiteral(
                "A TrePDF Widget annotation could not be located.");
            file.unmap(mapped);
            return false;
        }
        if (!added && placeholder.isDirect()) {
            error = QStringLiteral(
                "An existing TrePDF Widget is not an indirect field object.");
            file.unmap(mapped);
            return false;
        }
        const ObjectReference reference = placeholder.isDirect()
            ? ObjectReference{nextObjectNumber++, 0}
            : placeholder.widget;
        ObjectReference fieldReference = reference;
        ObjectReference sourceFieldReference = added
            ? ObjectReference{}
            : reference;
        QList<ObjectReference> sourceKids;
        QByteArray existingWidget;
        bool legacySingleSelectListBox = false;
        if (!added
            && !visitedFieldObjects.contains(reference.number)) {
            error = QStringLiteral(
                "An existing TrePDF Widget is missing from the AcroForm field tree.");
            file.unmap(mapped);
            return false;
        }
        if (!added) {
            existingWidget = completeObject(
                pdf,
                reference.number,
                error);
            if (existingWidget.isEmpty()) {
                file.unmap(mapped);
                return false;
            }
            if (formField.type == FieldType::RadioButton) {
                const qsizetype parentKey = keyPosition(
                    existingWidget.constData(),
                    existingWidget.size(),
                    "/Parent");
                if (parentKey >= 0) {
                    if (!parseReference(
                            existingWidget.constData(),
                            existingWidget.size(),
                            parentKey + QByteArrayView("/Parent").size(),
                            sourceFieldReference)) {
                        error = QStringLiteral(
                            "The TrePDF Radio Button Parent reference is malformed.");
                        file.unmap(mapped);
                        return false;
                    }
                    const QByteArray existingField = completeObject(
                        pdf,
                        sourceFieldReference.number,
                        error);
                    if (existingField.isEmpty()
                        || !buttonFieldObjectMatches(existingField, true)
                        || keyPosition(
                               existingField.constData(),
                               existingField.size(),
                               "/Parent") >= 0
                        || keyPosition(
                               existingWidget.constData(),
                               existingWidget.size(),
                               "/Kids") >= 0
                        || std::count(
                               fields.cbegin(),
                               fields.cend(),
                               sourceFieldReference) != 1
                        || !kidReferences(
                               pdf,
                               existingField,
                               sourceKids,
                               error)
                        || sourceKids.isEmpty()
                        || !sourceKids.contains(reference)) {
                        if (error.isEmpty()) {
                            error = QStringLiteral(
                                "The existing TrePDF Radio Button field hierarchy is unsupported.");
                        }
                        file.unmap(mapped);
                        return false;
                    }
                    fieldReference = sourceFieldReference;
                } else {
                    const qsizetype topLevelIndex = fields.indexOf(reference);
                    if (!buttonFieldObjectMatches(existingWidget, true)
                        || keyPosition(
                               existingWidget.constData(),
                               existingWidget.size(),
                               "/Kids") >= 0
                        || topLevelIndex < 0
                        || std::count(
                               fields.cbegin(),
                               fields.cend(),
                               reference) != 1) {
                        error = QStringLiteral(
                            "The existing TrePDF Radio Button field structure is unsupported.");
                        file.unmap(mapped);
                        return false;
                    }
                    sourceKids.append(reference);
                }
            } else {
                const qsizetype fieldTypeKey = keyPosition(
                    existingWidget.constData(),
                    existingWidget.size(),
                    "/FT");
                Cursor fieldTypeCursor(
                    existingWidget.constData(),
                    existingWidget.size(),
                    fieldTypeKey >= 0
                        ? fieldTypeKey + QByteArrayView("/FT").size()
                        : existingWidget.size());
                const QByteArrayView expectedFieldType
                    = formField.type == FieldType::Text
                    ? QByteArrayView("/Tx")
                    : isFixedChoiceField(formField.type)
                        ? QByteArrayView("/Ch")
                        : QByteArrayView("/Btn");
                const bool buttonSubtypeMatches
                    = formField.type != FieldType::Checkbox
                    || buttonFieldObjectMatches(existingWidget, false);
                const ChoiceFieldStructure choiceStructure
                    = isFixedChoiceField(formField.type)
                    ? choiceFieldStructure(
                          existingWidget,
                          formField.type == FieldType::ListBox)
                    : ChoiceFieldStructure::Current;
                const bool choiceSubtypeMatches
                    = choiceStructure
                    != ChoiceFieldStructure::Unsupported;
                legacySingleSelectListBox
                    = choiceStructure
                    == ChoiceFieldStructure::LegacySingleSelectListBox;
                if (fieldTypeKey < 0
                    || !fieldTypeCursor.consume(expectedFieldType)
                    || !buttonSubtypeMatches
                    || !choiceSubtypeMatches
                    || (!removed
                        && isFixedChoiceField(formField.type)
                        && !hasSimpleDirectStringOptions(existingWidget))
                    || keyPosition(
                           existingWidget.constData(),
                           existingWidget.size(),
                           "/Kids") >= 0
                    || keyPosition(
                           existingWidget.constData(),
                           existingWidget.size(),
                           "/Parent") >= 0) {
                    error = QStringLiteral(
                        "The existing TrePDF form field structure is unsupported.");
                    file.unmap(mapped);
                    return false;
                }
            }
        }
        if (resolvedWidgetObjectNumbers.contains(reference.number)) {
            error = QStringLiteral(
                "Two form fields resolved to the same Widget placeholder.");
            file.unmap(mapped);
            return false;
        }
        resolvedWidgetObjectNumbers.insert(reference.number);
        if (removed) {
            if (formField.type != FieldType::RadioButton
                && std::count(
                       fields.cbegin(),
                       fields.cend(),
                       reference) != 1) {
                error = QStringLiteral(
                    "Only a top-level single-Widget TrePDF field can be removed.");
                file.unmap(mapped);
                return false;
            }
            ObjectReference pageReference;
            bool pageLocated = false;
            if (!pageForWidgetReference(
                    pdf,
                    reference,
                    pageReference,
                    pageLocated,
                    error)) {
                file.unmap(mapped);
                return false;
            }
            if (pageLocated) {
                const auto previousPage = removedPageReferences.constFind(
                    pageReference.number);
                if (previousPage != removedPageReferences.cend()
                    && !(*previousPage == pageReference)) {
                    error = QStringLiteral(
                        "The Widget removal request resolved a stale page reference.");
                    file.unmap(mapped);
                    return false;
                }
                removedPageReferences.insert(
                    pageReference.number,
                    pageReference);
                removedWidgetsByPage[pageReference.number].append(reference);
            }
        }
        resolvedWidgets.append({
            placeholder,
            reference,
            fieldReference,
            sourceFieldReference,
            std::move(sourceKids),
            std::move(existingWidget),
            added,
            legacySingleSelectListBox});
        widgetReferences.append(reference);
        if (added && formField.type != FieldType::RadioButton) {
            fields.append(fieldReference);
        } else if (removed
                   && formField.type != FieldType::RadioButton) {
            fields.removeOne(reference);
        }
    }

    QHash<int, ObjectReference> sourceRadioParents;
    QHash<int, QList<ObjectReference>> sourceRadioKids;
    QHash<int, QList<ObjectReference>> processedRadioKids;
    for (qsizetype index = 0; index < formFields.size(); ++index) {
        const Field &formField = formFields.at(index);
        const ResolvedWidget &resolved = resolvedWidgets.at(index);
        if (formField.type != FieldType::RadioButton || resolved.added) {
            continue;
        }
        const ObjectReference &sourceParent = resolved.sourceFieldReference;
        if (sourceParent.number < 0 || resolved.sourceKids.isEmpty()) {
            error = QStringLiteral(
                "The existing TrePDF Radio Button group is incomplete.");
            file.unmap(mapped);
            return false;
        }
        const auto previousParent = sourceRadioParents.constFind(
            sourceParent.number);
        const auto previousKids = sourceRadioKids.constFind(
            sourceParent.number);
        if ((previousParent != sourceRadioParents.cend()
             && !(*previousParent == sourceParent))
            || (previousKids != sourceRadioKids.cend()
                && *previousKids != resolved.sourceKids)) {
            error = QStringLiteral(
                "The existing TrePDF Radio Button group references are inconsistent.");
            file.unmap(mapped);
            return false;
        }
        sourceRadioParents.insert(sourceParent.number, sourceParent);
        sourceRadioKids.insert(sourceParent.number, resolved.sourceKids);
        processedRadioKids[sourceParent.number].append(resolved.reference);
    }
    for (auto sourceParent = sourceRadioParents.cbegin();
         sourceParent != sourceRadioParents.cend();
         ++sourceParent) {
        const QList<ObjectReference> expectedKids = sourceRadioKids.value(
            sourceParent.key());
        const QList<ObjectReference> processedKids = processedRadioKids.value(
            sourceParent.key());
        if (expectedKids.size() != processedKids.size()
            || std::any_of(
                expectedKids.cbegin(),
                expectedKids.cend(),
                [&processedKids](const ObjectReference &kid) {
                    return !processedKids.contains(kid);
                })
            || std::count(
                   fields.cbegin(),
                   fields.cend(),
                   *sourceParent) != 1) {
            error = QStringLiteral(
                "The Radio Button save request does not contain the complete source group.");
            file.unmap(mapped);
            return false;
        }
        fields.removeOne(*sourceParent);
    }

    struct RadioGroupPlan {
        QString fieldName;
        QString tooltip;
        int fieldFlags = 0;
        ObjectReference reference;
        QList<ObjectReference> widgets;
        QByteArray currentState = QByteArrayLiteral("/Off");
        QByteArray defaultState = QByteArrayLiteral("/Off");
    };
    QVector<RadioGroupPlan> radioGroups;
    QHash<QString, int> radioGroupIndices;
    for (qsizetype index = 0; index < formFields.size(); ++index) {
        const Field &formField = formFields.at(index);
        if (formField.type != FieldType::RadioButton
            || formField.operation == Operation::Remove) {
            continue;
        }
        if (!isTrePDFRadioButtonWidgetId(formField.widgetId)) {
            error = QStringLiteral(
                "The TrePDF Radio Button Widget ID is invalid.");
            file.unmap(mapped);
            return false;
        }
        int groupIndex = radioGroupIndices.value(formField.fieldName, -1);
        if (groupIndex < 0) {
            groupIndex = radioGroups.size();
            radioGroupIndices.insert(formField.fieldName, groupIndex);
            RadioGroupPlan group;
            group.fieldName = formField.fieldName;
            group.tooltip = formField.tooltip;
            group.fieldFlags = formField.fieldFlags;
            group.reference = ObjectReference{nextObjectNumber++, 0};
            radioGroups.append(std::move(group));
        }
        RadioGroupPlan &group = radioGroups[groupIndex];
        if (group.fieldFlags != formField.fieldFlags
            || group.tooltip != formField.tooltip) {
            error = QStringLiteral(
                "Radio Buttons in one group must share flags and tooltip.");
            file.unmap(mapped);
            return false;
        }
        const QByteArray exportName = radioButtonExportName(
            formField.widgetId);
        if (checkboxChecked(formField.value)) {
            if (group.currentState != QByteArrayLiteral("/Off")) {
                error = QStringLiteral(
                    "A Radio Button group has more than one selected Widget.");
                file.unmap(mapped);
                return false;
            }
            group.currentState = exportName;
        }
        if (checkboxChecked(formField.defaultValue)) {
            if (group.defaultState != QByteArrayLiteral("/Off")) {
                error = QStringLiteral(
                    "A Radio Button group has more than one default Widget.");
                file.unmap(mapped);
                return false;
            }
            group.defaultState = exportName;
        }
        group.widgets.append(widgetReferences.at(index));
        resolvedWidgets[index].fieldReference = group.reference;
    }
    for (const RadioGroupPlan &group : std::as_const(radioGroups)) {
        fields.append(group.reference);
    }

    QSet<QString> occupiedFieldNames = fieldNames;
    for (const ResolvedWidget &resolved : std::as_const(resolvedWidgets)) {
        if (resolved.added) {
            continue;
        }
        occupiedFieldNames.remove(
            fieldNamesByObject.value(resolved.reference.number));
        occupiedFieldNames.remove(
            fieldNamesByObject.value(resolved.sourceFieldReference.number));
    }
    QHash<QString, bool> requestedFieldNames;
    for (const Field &formField : formFields) {
        if (formField.operation == Operation::Remove) {
            continue;
        }
        const bool listBoxSelectionValid
            = formField.type != FieldType::ListBox
            || (validCanonicalListBoxSelection(
                    formField.options,
                    formField.selectedValues)
                && formField.value
                    == (formField.selectedValues.isEmpty()
                            ? QString()
                            : formField.selectedValues.constFirst()));
        if (isFixedChoiceField(formField.type)
            && (!validChoiceOptions(formField.options)
                || !formField.options.contains(formField.defaultValue)
                || (formField.type == FieldType::Dropdown
                    && !formField.options.contains(formField.value))
                || !listBoxSelectionValid)) {
            error = formField.type == FieldType::ListBox
                ? QStringLiteral(
                      "The List Box options or selected values are invalid.")
                : QStringLiteral(
                      "The Dropdown options or selected values are invalid.");
            file.unmap(mapped);
            return false;
        }
        const int unsupportedFixedChoiceFlags = kChoiceEdit
            | kChoiceCommitOnSelectionChange;
        if ((formField.type == FieldType::ListBox
             && ((formField.fieldFlags
                  & (unsupportedFixedChoiceFlags | kChoiceCombo))
                 || !(formField.fieldFlags & kChoiceMultiSelect)))
            || (formField.type == FieldType::Dropdown
                && (!(formField.fieldFlags & kChoiceCombo)
                    || (formField.fieldFlags
                        & (unsupportedFixedChoiceFlags
                           | kChoiceMultiSelect))))) {
            error = formField.type == FieldType::ListBox
                ? QStringLiteral("The List Box field flags are invalid.")
                : QStringLiteral("The Dropdown field flags are invalid.");
            file.unmap(mapped);
            return false;
        }
        if ((formField.type == FieldType::Text
             && (formField.maxLength < 0
                 || formField.maxLength
                     > kMaximumTextFieldLength
                 || ((formField.fieldFlags & kTextFieldComb)
                     && (formField.maxLength <= 0
                         || (formField.fieldFlags
                             & kTextFieldMultiline)))))
            || (formField.type != FieldType::Text
                && (formField.maxLength != 0
                    || (formField.fieldFlags & kTextFieldComb)))) {
            error = QStringLiteral(
                "The Text Field character limit is invalid.");
            file.unmap(mapped);
            return false;
        }
        if (formField.fieldName.isEmpty()
            || formField.fieldName.contains(QLatin1Char('.'))) {
            error = QStringLiteral(
                "TrePDF AcroForm fields require a non-empty flat field name.");
            file.unmap(mapped);
            return false;
        }
        const bool radioButton = formField.type == FieldType::RadioButton;
        const auto requested = requestedFieldNames.constFind(
            formField.fieldName);
        if (occupiedFieldNames.contains(formField.fieldName)
            || (requested != requestedFieldNames.cend()
                && (!radioButton || !*requested))) {
            error = QStringLiteral(
                "An AcroForm field with the same name already exists.");
            file.unmap(mapped);
            return false;
        }
        requestedFieldNames.insert(formField.fieldName, radioButton);
    }

    QByteArray catalogObject = completeObject(pdf, pdf.root.number, error);
    if (catalogObject.isEmpty()) {
        file.unmap(mapped);
        return false;
    }
    const bool hasExistingAcroForm = sourceAcroFormReference.number >= 0;
    const ObjectReference acroFormReference = hasExistingAcroForm
        ? sourceAcroFormReference
        : ObjectReference{nextObjectNumber++, 0};
    const bool requiresFormFont = std::any_of(
        formFields.cbegin(),
        formFields.cend(),
        [](const Field &field) {
            return field.operation != Operation::Remove;
        });
    const bool requiresUnicodeFont = std::any_of(
        formFields.cbegin(),
        formFields.cend(),
        [](const Field &field) {
            return field.operation != Operation::Remove
                && (field.type == FieldType::Text
                    || field.type == FieldType::Dropdown
                    || field.type == FieldType::ListBox);
        });
    FormFontResource defaultFontResource;
    struct UnicodeFontObjects {
        int type0 = -1;
        int descendant = -1;
        int widths = -1;
        int descriptor = -1;
        int fontFile = -1;
        int cidToGidMap = -1;
        int toUnicode = -1;
    } unicodeFontObjects;
    const UnicodeFormFontTemplate *unicodeFont = nullptr;
    int standardFontObjectNumber = -1;
    if (requiresUnicodeFont) {
        if (!findUnicodeFormFontResource(
                pdf,
                sourceAcroFormReference,
                defaultFontResource,
                error)) {
            file.unmap(mapped);
            return false;
        }
        if (defaultFontResource.reference.number < 0) {
            unicodeFont = &unicodeFormFontTemplate();
            if (!unicodeFont->isValid()) {
                error = unicodeFont->error;
                file.unmap(mapped);
                return false;
            }
            unicodeFontObjects.type0 = nextObjectNumber++;
            unicodeFontObjects.descendant = nextObjectNumber++;
            unicodeFontObjects.widths = nextObjectNumber++;
            unicodeFontObjects.descriptor = nextObjectNumber++;
            unicodeFontObjects.fontFile = nextObjectNumber++;
            unicodeFontObjects.cidToGidMap = nextObjectNumber++;
            unicodeFontObjects.toUnicode = nextObjectNumber++;
            defaultFontResource.reference
                = ObjectReference{unicodeFontObjects.type0, 0};
            defaultFontResource.name
                = QByteArrayLiteral(kUnicodeFormFontResourcePrefix)
                + QByteArray::number(unicodeFontObjects.type0);
        }
    } else if (requiresFormFont) {
        standardFontObjectNumber = nextObjectNumber++;
        defaultFontResource.reference
            = ObjectReference{standardFontObjectNumber, 0};
        defaultFontResource.name = QByteArrayLiteral("TrePDFF")
            + QByteArray::number(standardFontObjectNumber);
    }

    QList<IncrementalObject> incrementalObjects;
    incrementalObjects.reserve(formFields.size() * 4 + 9);

    struct PageReplacement {
        qsizetype start = -1;
        qsizetype end = -1;
        ObjectReference widget;
    };
    QHash<int, QVector<PageReplacement>> pageReplacements;
    QHash<int, ObjectReference> pageReferences;
    QHash<int, QByteArray> rewrittenObjects;
    QHash<int, ObjectReference> rewrittenObjectReferences;
    for (const ResolvedWidget &resolved : std::as_const(resolvedWidgets)) {
        if (!resolved.added || !resolved.placeholder.isDirect()) {
            continue;
        }
        pageReplacements[resolved.placeholder.container.number].append({
            resolved.placeholder.dictionaryStart,
            resolved.placeholder.dictionaryEnd,
            resolved.reference});
        pageReferences.insert(
            resolved.placeholder.container.number,
            resolved.placeholder.container);
    }
    for (auto replacements = pageReplacements.begin();
         replacements != pageReplacements.end();
         ++replacements) {
        const ObjectReference pageReference = pageReferences.value(
            replacements.key());
        QByteArray pageObject;
        if (!objectForReference(
            pdf,
            pageReference,
            rewrittenObjects,
            pageObject,
            error)) {
            file.unmap(mapped);
            return false;
        }
        std::sort(
            replacements->begin(),
            replacements->end(),
            [](const PageReplacement &left, const PageReplacement &right) {
                return left.start > right.start;
            });
        for (const PageReplacement &replacement
             : std::as_const(*replacements)) {
            if (replacement.start < 0
                || replacement.end <= replacement.start
                || replacement.end > pageObject.size()) {
                error = QStringLiteral(
                    "A Widget placeholder dictionary is malformed.");
                file.unmap(mapped);
                return false;
            }
            const QByteArray reference = QByteArray::number(
                    replacement.widget.number)
                + ' '
                + QByteArray::number(replacement.widget.generation)
                + QByteArrayLiteral(" R ");
            pageObject.replace(
                replacement.start,
                replacement.end - replacement.start,
                reference);
        }
        rewrittenObjects.insert(
            pageReference.number,
            std::move(pageObject));
        rewrittenObjectReferences.insert(
            pageReference.number,
            pageReference);
    }

    QHash<int, int> annotsObjectOwnerPages;
    for (auto removed = removedWidgetsByPage.cbegin();
         removed != removedWidgetsByPage.cend();
         ++removed) {
        const ObjectReference pageReference
            = removedPageReferences.value(removed.key());
        ObjectReference rewrittenReference;
        QByteArray rewrittenObject;
        if (!removeWidgetReferencesFromPageAnnots(
                pdf,
                pageReference,
                *removed,
                rewrittenObjects,
                rewrittenReference,
                rewrittenObject,
                error)) {
            file.unmap(mapped);
            return false;
        }
        const auto previousOwner = annotsObjectOwnerPages.constFind(
            rewrittenReference.number);
        if (previousOwner != annotsObjectOwnerPages.cend()
            && *previousOwner != pageReference.number) {
            error = QStringLiteral(
                "A shared page Annots array cannot be modified safely.");
            file.unmap(mapped);
            return false;
        }
        const auto previousReference = rewrittenObjectReferences.constFind(
            rewrittenReference.number);
        if (previousReference != rewrittenObjectReferences.cend()
            && !(*previousReference == rewrittenReference)) {
            error = QStringLiteral(
                "An incremental PDF object rewrite has a stale generation.");
            file.unmap(mapped);
            return false;
        }
        annotsObjectOwnerPages.insert(
            rewrittenReference.number,
            pageReference.number);
        rewrittenObjects.insert(
            rewrittenReference.number,
            std::move(rewrittenObject));
        rewrittenObjectReferences.insert(
            rewrittenReference.number,
            rewrittenReference);
    }
    for (auto rewritten = rewrittenObjects.begin();
         rewritten != rewrittenObjects.end();
         ++rewritten) {
        incrementalObjects.append({
            rewrittenObjectReferences.value(rewritten.key()),
            std::move(rewritten.value())});
    }

    constexpr qint64 maximumAppearancePixels = 32 * 1024 * 1024;
    qint64 appearancePixels = 0;
    for (qsizetype index = 0; index < formFields.size(); ++index) {
        const Field &formField = formFields.at(index);
        if (formField.operation == Operation::Remove) {
            continue;
        }
        const ObjectReference &widgetReference = widgetReferences.at(index);
        const ObjectReference &fieldReference
            = resolvedWidgets.at(index).fieldReference;
        const PlaceholderLocation &placeholder
            = resolvedWidgets.at(index).placeholder;
        QByteArray widgetObject;
        if (formField.type == FieldType::Text
            || isFixedChoiceField(formField.type)) {
            const bool dropdown = formField.type == FieldType::Dropdown;
            const bool listBox = formField.type == FieldType::ListBox;
            const QImage appearanceImage = listBox
                ? listBoxAppearanceImage(formField)
                : textFieldAppearanceImage(formField, dropdown);
            appearancePixels += static_cast<qint64>(appearanceImage.width())
                * appearanceImage.height();
            if (appearancePixels > maximumAppearancePixels) {
                error = QStringLiteral(
                    "The combined form field appearances are too large.");
                file.unmap(mapped);
                return false;
            }
            const QByteArray compressedPixels = compressedRgbPixels(
                appearanceImage);
            if (appearanceImage.isNull() || compressedPixels.isEmpty()) {
                error = QStringLiteral(
                    "The form field appearance could not be generated.");
                file.unmap(mapped);
                return false;
            }
            const int appearanceObjectNumber = nextObjectNumber++;
            const int imageObjectNumber = nextObjectNumber++;
            const bool migrateLegacyListBox
                = listBox
                && resolvedWidgets.at(index).legacySingleSelectListBox;
            if (formField.operation == Operation::NormalizeAppearance
                && !migrateLegacyListBox) {
                widgetObject = normalizedFieldAppearanceObject(
                    resolvedWidgets.at(index).existingObject,
                    appearanceObjectNumber,
                    listBox
                        ? PDFFormAppearance::kListBoxVersion
                        : dropdown
                            ? PDFFormAppearance::kDropdownVersion
                            : PDFFormAppearance::kTextFieldVersion,
                    defaultFontResource.name,
                    dropdown || listBox,
                    error);
                if (listBox
                    && !widgetObject.isEmpty()
                    && !replaceTopLevelDictionaryValue(
                        widgetObject,
                        QByteArrayView("/TI"),
                        QByteArray::number(
                            PDFFormAppearance::listBoxTopIndex(
                                formField.options.size(),
                                formField.options.indexOf(formField.value),
                                formField.pageRect.height())),
                        true,
                        error)) {
                    widgetObject.clear();
                }
            } else if (dropdown || listBox) {
                widgetObject = choiceWidgetObject(
                    widgetReference,
                    formField,
                    appearanceObjectNumber,
                    placeholder.isDirect()
                        ? placeholder.container
                        : ObjectReference{},
                    defaultFontResource.name,
                    listBox);
            } else {
                widgetObject = textFieldWidgetObject(
                    widgetReference,
                    formField,
                    appearanceObjectNumber,
                    placeholder.isDirect()
                        ? placeholder.container
                        : ObjectReference{},
                    defaultFontResource.name);
            }
            incrementalObjects.append({
                ObjectReference{appearanceObjectNumber, 0},
                appearanceObject(
                    appearanceObjectNumber,
                    imageObjectNumber,
                    formField.pageRect)});
            incrementalObjects.append({
                ObjectReference{imageObjectNumber, 0},
                imageObject(
                    imageObjectNumber,
                    appearanceImage,
                    compressedPixels)});
        } else if (formField.type == FieldType::Checkbox
                   || formField.type == FieldType::RadioButton) {
            const bool radioButton
                = formField.type == FieldType::RadioButton;
            if (!radioButton
                && formField.operation == Operation::NormalizeAppearance) {
                error = QStringLiteral(
                    "Checkbox appearance-only normalization is unsupported.");
                file.unmap(mapped);
                return false;
            }
            const int offAppearanceObjectNumber = nextObjectNumber++;
            const int onAppearanceObjectNumber = nextObjectNumber++;
            ObjectReference pageReference = placeholder.isDirect()
                ? placeholder.container
                : ObjectReference{};
            if (radioButton && pageReference.number < 0) {
                bool pageLocated = false;
                if (!pageForWidgetReference(
                        pdf,
                        widgetReference,
                        pageReference,
                        pageLocated,
                        error)
                    || !pageLocated) {
                    if (error.isEmpty()) {
                        error = QStringLiteral(
                            "The Radio Button page reference could not be resolved.");
                    }
                    file.unmap(mapped);
                    return false;
                }
            }
            widgetObject = radioButton
                ? radioButtonWidgetObject(
                      widgetReference,
                      fieldReference,
                      formField,
                      offAppearanceObjectNumber,
                      onAppearanceObjectNumber,
                      pageReference,
                      radioButtonExportName(formField.widgetId))
                : checkboxWidgetObject(
                      widgetReference,
                      formField,
                      offAppearanceObjectNumber,
                      onAppearanceObjectNumber,
                      pageReference);
            incrementalObjects.append({
                ObjectReference{offAppearanceObjectNumber, 0},
                radioButton
                    ? radioButtonAppearanceObject(
                          offAppearanceObjectNumber,
                          formField.pageRect,
                          false)
                    : checkboxAppearanceObject(
                          offAppearanceObjectNumber,
                          formField.pageRect,
                          false)});
            incrementalObjects.append({
                ObjectReference{onAppearanceObjectNumber, 0},
                radioButton
                    ? radioButtonAppearanceObject(
                          onAppearanceObjectNumber,
                          formField.pageRect,
                          true)
                    : checkboxAppearanceObject(
                          onAppearanceObjectNumber,
                          formField.pageRect,
                          true)});
        } else {
            error = QStringLiteral(
                "This TrePDF form field type is unsupported.");
            file.unmap(mapped);
            return false;
        }
        if (widgetObject.isEmpty()) {
            if (error.isEmpty()) {
                error = QStringLiteral(
                    "The TrePDF form field dictionary could not be updated.");
            }
            file.unmap(mapped);
            return false;
        }
        incrementalObjects.append({
            widgetReference,
            std::move(widgetObject)});
    }
    for (const RadioGroupPlan &group : std::as_const(radioGroups)) {
        incrementalObjects.append({
            group.reference,
            radioButtonFieldObject(
                group.reference,
                group.fieldName,
                group.tooltip,
                group.fieldFlags,
                group.widgets,
                group.currentState,
                group.defaultState)});
    }

    if (!hasExistingAcroForm) {
        if (!replaceCatalogAcroForm(
                catalogObject,
                acroFormReference,
                error)) {
            file.unmap(mapped);
            return false;
        }
        incrementalObjects.append({
            pdf.root,
            std::move(catalogObject)});
    }
    const QByteArray acroFormObject = updatedAcroFormObject(
        pdf,
        fields,
        acroFormReference,
        defaultFontResource.reference,
        defaultFontResource.name,
        error);
    if (acroFormObject.isEmpty()) {
        file.unmap(mapped);
        return false;
    }
    const bool sourceEndsWithNewline = fileSize > 0
        && reinterpret_cast<const char *>(mapped)[fileSize - 1] == '\n';
    const ObjectReference root = pdf.root;
    const qint64 previousXref = pdf.startXref;
    incrementalObjects.append({
        acroFormReference,
        acroFormObject});
    if (unicodeFont) {
        incrementalObjects.append({
            ObjectReference{unicodeFontObjects.type0, 0},
            unicodeFormFontObject(
                unicodeFontObjects.type0,
                unicodeFontObjects.descendant,
                unicodeFontObjects.toUnicode)});
        incrementalObjects.append({
            ObjectReference{unicodeFontObjects.descendant, 0},
            unicodeFormDescendantFontObject(
                unicodeFontObjects.descendant,
                unicodeFontObjects.widths,
                unicodeFontObjects.descriptor,
                unicodeFontObjects.cidToGidMap)});
        incrementalObjects.append({
            ObjectReference{unicodeFontObjects.widths, 0},
            unicodeFormWidthsObject(
                unicodeFontObjects.widths,
                *unicodeFont)});
        incrementalObjects.append({
            ObjectReference{unicodeFontObjects.descriptor, 0},
            unicodeFormFontDescriptorObject(
                unicodeFontObjects.descriptor,
                unicodeFontObjects.fontFile,
                *unicodeFont)});
        incrementalObjects.append({
            ObjectReference{unicodeFontObjects.cidToGidMap, 0},
            unicodeFormCidToGidMapObject(
                unicodeFontObjects.cidToGidMap,
                *unicodeFont)});
        incrementalObjects.append({
            ObjectReference{unicodeFontObjects.fontFile, 0},
            unicodeFormFontFileObject(
                unicodeFontObjects.fontFile,
                *unicodeFont)});
        incrementalObjects.append({
            ObjectReference{unicodeFontObjects.toUnicode, 0},
            unicodeFormToUnicodeObject(
                unicodeFontObjects.toUnicode,
                *unicodeFont)});
    } else if (standardFontObjectNumber >= 0) {
        incrementalObjects.append({
            ObjectReference{standardFontObjectNumber, 0},
            standardFormFontObject(standardFontObjectNumber)});
    }
    file.unmap(mapped);
    file.close();
    return appendIncrementalUpdate(
        filePath,
        root,
        previousXref,
        nextObjectNumber,
        sourceEndsWithNewline,
        std::move(incrementalObjects),
        error);
}

bool PDFAcroFormCanonicalizer::finalizeFreeTextAnnotations(
    const QString &filePath,
    const QVector<FreeTextAnnotation> &annotations,
    QString &error)
{
    if (annotations.isEmpty()) {
        return true;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0) {
        error = file.errorString();
        return false;
    }
    const qint64 fileSize = file.size();
    uchar *mapped = file.map(0, fileSize);
    if (!mapped) {
        error = file.errorString();
        return false;
    }

    ParsedPdf pdf;
    if (!parseXref(
            reinterpret_cast<const char *>(mapped),
            fileSize,
            pdf,
            error)) {
        file.unmap(mapped);
        return false;
    }

    const auto fail = [&file, mapped, &error](const QString &message) {
        error = message;
        file.unmap(mapped);
        return false;
    };

    QByteArray catalogObject = completeObject(
        pdf,
        pdf.root.number,
        error);
    if (catalogObject.isEmpty()) {
        return fail(error);
    }
    ObjectReference sourceAcroFormReference;
    const qsizetype acroFormKey = keyPosition(
        catalogObject.constData(),
        catalogObject.size(),
        QByteArrayView("/AcroForm"));
    bool directAcroForm = false;
    if (acroFormKey >= 0
        && !parseReference(
            catalogObject.constData(),
            catalogObject.size(),
            acroFormKey + QByteArrayView("/AcroForm").size(),
            sourceAcroFormReference)) {
        directAcroForm = true;
    }

    int highestObject = 0;
    for (auto entry = pdf.entries.cbegin(); entry != pdf.entries.cend(); ++entry) {
        highestObject = qMax(highestObject, entry.key());
    }
    int nextObjectNumber = qMax(
        pdf.declaredSize,
        highestObject + 1);
    const bool requiresUnicodeDefaultAppearance = std::any_of(
        annotations.cbegin(),
        annotations.cend(),
        [](const FreeTextAnnotation &annotation) {
            return !annotation.preserveDefaultAppearance;
        });
    const bool installUnicodeDefaultAppearance
        = requiresUnicodeDefaultAppearance && !directAcroForm;
    const bool hasExistingAcroForm
        = sourceAcroFormReference.number >= 0;
    ObjectReference acroFormReference;
    QList<ObjectReference> fields;
    if (installUnicodeDefaultAppearance) {
        if (!existingFields(pdf, fields, error)) {
            return fail(error);
        }
        acroFormReference = hasExistingAcroForm
            ? sourceAcroFormReference
            : ObjectReference{nextObjectNumber++, 0};
    }

    FormFontResource defaultFontResource;
    defaultFontResource.name = QByteArrayLiteral("Helv");
    if (installUnicodeDefaultAppearance) {
        if (!findUnicodeFormFontResource(
                pdf,
                sourceAcroFormReference,
                defaultFontResource,
                error)) {
            return fail(error);
        }
    }
    struct UnicodeFontObjects {
        int type0 = -1;
        int descendant = -1;
        int widths = -1;
        int descriptor = -1;
        int fontFile = -1;
        int cidToGidMap = -1;
        int toUnicode = -1;
    } unicodeFontObjects;
    const UnicodeFormFontTemplate *unicodeFont = nullptr;
    if (installUnicodeDefaultAppearance
        && defaultFontResource.reference.number < 0) {
        unicodeFont = &unicodeFormFontTemplate();
        if (!unicodeFont->isValid()) {
            return fail(unicodeFont->error);
        }
        unicodeFontObjects.type0 = nextObjectNumber++;
        unicodeFontObjects.descendant = nextObjectNumber++;
        unicodeFontObjects.widths = nextObjectNumber++;
        unicodeFontObjects.descriptor = nextObjectNumber++;
        unicodeFontObjects.fontFile = nextObjectNumber++;
        unicodeFontObjects.cidToGidMap = nextObjectNumber++;
        unicodeFontObjects.toUnicode = nextObjectNumber++;
        defaultFontResource.reference
            = ObjectReference{unicodeFontObjects.type0, 0};
        defaultFontResource.name
            = QByteArrayLiteral(kUnicodeFormFontResourcePrefix)
            + QByteArray::number(unicodeFontObjects.type0);
    }

    struct DictionaryReplacement {
        qsizetype start = -1;
        qsizetype end = -1;
        QByteArray value;
    };
    QHash<int, QVector<DictionaryReplacement>> replacementsByContainer;
    QHash<int, ObjectReference> containerReferences;
    QHash<int, QByteArray> rewrittenObjects;
    QHash<int, ObjectReference> rewrittenReferences;
    QList<IncrementalObject> incrementalObjects;
    incrementalObjects.reserve(annotations.size() * 3 + 10);
    QSet<QString> annotationIds;

    for (const FreeTextAnnotation &annotation : annotations) {
        if (annotation.page < 0
            || annotation.annotationId.isEmpty()
            || annotation.text.size() > kMaximumTextFieldLength
            || annotationIds.contains(annotation.annotationId)) {
            return fail(QStringLiteral(
                "A Free Text appearance request is invalid or duplicated."));
        }
        annotationIds.insert(annotation.annotationId);

        const PlaceholderLocation placeholder = annotationForStringId(
            pdf,
            QByteArrayView("/TrePDFFreeTextAppearanceId"),
            annotation.annotationId,
            QByteArrayView("/FreeText"));
        const bool located = placeholder.widget.number >= 0
            || (placeholder.container.number >= 0
                && placeholder.dictionaryStart >= 0
                && placeholder.dictionaryEnd
                    > placeholder.dictionaryStart);
        if (!located) {
            return fail(QStringLiteral(
                "A Free Text annotation could not be located."));
        }

        QByteArray object;
        QByteArray dictionary;
        qsizetype dictionaryStart = -1;
        qsizetype dictionaryEnd = -1;
        if (placeholder.isDirect()) {
            object = completeObject(
                pdf,
                placeholder.container.number,
                error);
            dictionaryStart = placeholder.dictionaryStart;
            dictionaryEnd = placeholder.dictionaryEnd;
        } else {
            object = completeObject(
                pdf,
                placeholder.widget.number,
                error);
            dictionaryStart = object.indexOf("<<");
            if (dictionaryStart >= 0
                && !matchingDictionaryEnd(
                    object.constData(),
                    object.size(),
                    dictionaryStart,
                    dictionaryEnd)) {
                dictionaryEnd = -1;
            }
        }
        if (object.isEmpty()
            || dictionaryStart < 0
            || dictionaryEnd <= dictionaryStart
            || dictionaryEnd > object.size()) {
            return fail(error.isEmpty()
                    ? QStringLiteral(
                        "A Free Text annotation dictionary is malformed.")
                    : error);
        }
        dictionary = object.mid(
            dictionaryStart,
            dictionaryEnd - dictionaryStart);

        QRectF pageRect;
        if (!freeTextAppearanceRect(dictionary, pageRect, error)) {
            return fail(error);
        }
        const QImage appearanceImage = freeTextAppearanceImage(
            pageRect,
            annotation.text);
        const QByteArray compressedPixels = compressedRgbPixels(
            appearanceImage);
        if (appearanceImage.isNull() || compressedPixels.isEmpty()) {
            return fail(QStringLiteral(
                "A Free Text appearance image could not be generated."));
        }
        if (nextObjectNumber
            > std::numeric_limits<int>::max() - 2) {
            return fail(QStringLiteral(
                "The PDF has no available object number for a Free Text appearance."));
        }
        const int appearanceObjectNumber = nextObjectNumber++;
        const int imageObjectNumber = nextObjectNumber++;
        if (!updateFreeTextAppearanceDictionary(
                dictionary,
                appearanceObjectNumber,
                defaultFontResource.name,
                annotation.preserveDefaultAppearance,
                error)) {
            return fail(error);
        }
        incrementalObjects.append({
            ObjectReference{appearanceObjectNumber, 0},
            appearanceObject(
                appearanceObjectNumber,
                imageObjectNumber,
                pageRect)});
        incrementalObjects.append({
            ObjectReference{imageObjectNumber, 0},
            imageObject(
                imageObjectNumber,
                appearanceImage,
                compressedPixels)});

        if (placeholder.isDirect()) {
            replacementsByContainer[placeholder.container.number].append({
                dictionaryStart,
                dictionaryEnd,
                dictionary});
            containerReferences.insert(
                placeholder.container.number,
                placeholder.container);
            continue;
        }
        if (rewrittenObjects.contains(placeholder.widget.number)) {
            return fail(QStringLiteral(
                "Two Free Text requests resolved to the same annotation."));
        }
        object.replace(
            dictionaryStart,
            dictionaryEnd - dictionaryStart,
            dictionary);
        rewrittenObjects.insert(
            placeholder.widget.number,
            std::move(object));
        rewrittenReferences.insert(
            placeholder.widget.number,
            placeholder.widget);
    }

    for (auto container = replacementsByContainer.begin();
         container != replacementsByContainer.end();
         ++container) {
        QByteArray object = completeObject(pdf, container.key(), error);
        if (object.isEmpty()) {
            return fail(error);
        }
        std::sort(
            container->begin(),
            container->end(),
            [](const DictionaryReplacement &left,
               const DictionaryReplacement &right) {
                return left.start > right.start;
            });
        qsizetype previousStart = object.size();
        for (const DictionaryReplacement &replacement
             : std::as_const(*container)) {
            if (replacement.start < 0
                || replacement.end <= replacement.start
                || replacement.end > previousStart
                || replacement.end > object.size()) {
                return fail(QStringLiteral(
                    "Overlapping Free Text annotation rewrites are not supported."));
            }
            object.replace(
                replacement.start,
                replacement.end - replacement.start,
                replacement.value);
            previousStart = replacement.start;
        }
        rewrittenObjects.insert(container.key(), std::move(object));
        rewrittenReferences.insert(
            container.key(),
            containerReferences.value(container.key()));
    }
    for (auto rewritten = rewrittenObjects.begin();
         rewritten != rewrittenObjects.end();
         ++rewritten) {
        incrementalObjects.append({
            rewrittenReferences.value(rewritten.key()),
            std::move(rewritten.value())});
    }

    if (installUnicodeDefaultAppearance) {
        if (!hasExistingAcroForm) {
            if (!replaceCatalogAcroForm(
                    catalogObject,
                    acroFormReference,
                    error)) {
                return fail(error);
            }
            incrementalObjects.append({
                pdf.root,
                std::move(catalogObject)});
        }
        const QByteArray acroFormObject = updatedAcroFormObject(
            pdf,
            fields,
            acroFormReference,
            defaultFontResource.reference,
            defaultFontResource.name,
            error);
        if (acroFormObject.isEmpty()) {
            return fail(error);
        }
        incrementalObjects.append({
            acroFormReference,
            acroFormObject});
    }
    if (unicodeFont) {
        incrementalObjects.append({
            ObjectReference{unicodeFontObjects.type0, 0},
            unicodeFormFontObject(
                unicodeFontObjects.type0,
                unicodeFontObjects.descendant,
                unicodeFontObjects.toUnicode)});
        incrementalObjects.append({
            ObjectReference{unicodeFontObjects.descendant, 0},
            unicodeFormDescendantFontObject(
                unicodeFontObjects.descendant,
                unicodeFontObjects.widths,
                unicodeFontObjects.descriptor,
                unicodeFontObjects.cidToGidMap)});
        incrementalObjects.append({
            ObjectReference{unicodeFontObjects.widths, 0},
            unicodeFormWidthsObject(
                unicodeFontObjects.widths,
                *unicodeFont)});
        incrementalObjects.append({
            ObjectReference{unicodeFontObjects.descriptor, 0},
            unicodeFormFontDescriptorObject(
                unicodeFontObjects.descriptor,
                unicodeFontObjects.fontFile,
                *unicodeFont)});
        incrementalObjects.append({
            ObjectReference{unicodeFontObjects.cidToGidMap, 0},
            unicodeFormCidToGidMapObject(
                unicodeFontObjects.cidToGidMap,
                *unicodeFont)});
        incrementalObjects.append({
            ObjectReference{unicodeFontObjects.fontFile, 0},
            unicodeFormFontFileObject(
                unicodeFontObjects.fontFile,
                *unicodeFont)});
        incrementalObjects.append({
            ObjectReference{unicodeFontObjects.toUnicode, 0},
            unicodeFormToUnicodeObject(
                unicodeFontObjects.toUnicode,
                *unicodeFont)});
    }

    const bool sourceEndsWithNewline = fileSize > 0
        && reinterpret_cast<const char *>(mapped)[fileSize - 1] == '\n';
    const ObjectReference root = pdf.root;
    const qint64 previousXref = pdf.startXref;
    file.unmap(mapped);
    file.close();
    return appendIncrementalUpdate(
        filePath,
        root,
        previousXref,
        nextObjectNumber,
        sourceEndsWithNewline,
        std::move(incrementalObjects),
        error);
}

bool PDFAcroFormCanonicalizer::finalizeTextMarkupAnnotations(
    const QString &filePath,
    const QVector<TextMarkupAnnotation> &annotations,
    QString &error)
{
    if (annotations.isEmpty()) {
        return true;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0) {
        error = file.errorString();
        return false;
    }
    const qint64 fileSize = file.size();
    uchar *mapped = file.map(0, fileSize);
    if (!mapped) {
        error = file.errorString();
        return false;
    }

    ParsedPdf pdf;
    if (!parseXref(
            reinterpret_cast<const char *>(mapped),
            fileSize,
            pdf,
            error)) {
        file.unmap(mapped);
        return false;
    }

    const auto fail = [&file, mapped, &error](const QString &message) {
        error = message;
        file.unmap(mapped);
        return false;
    };

    int highestObject = 0;
    for (auto entry = pdf.entries.cbegin(); entry != pdf.entries.cend(); ++entry) {
        highestObject = qMax(highestObject, entry.key());
    }
    int nextObjectNumber = qMax(
        pdf.declaredSize,
        highestObject + 1);

    struct DictionaryReplacement {
        qsizetype start = -1;
        qsizetype end = -1;
        QByteArray value;
    };
    QHash<int, QVector<DictionaryReplacement>> replacementsByContainer;
    QHash<int, ObjectReference> containerReferences;
    QHash<int, QByteArray> rewrittenObjects;
    QHash<int, ObjectReference> rewrittenReferences;
    QList<IncrementalObject> incrementalObjects;
    incrementalObjects.reserve(annotations.size() * 2);
    QSet<QString> annotationIds;

    for (const TextMarkupAnnotation &annotation : annotations) {
        const QByteArrayView subtype = textMarkupSubtypeName(annotation.type);
        if (annotation.page < 0
            || annotation.annotationId.isEmpty()
            || subtype.isEmpty()
            || annotationIds.contains(annotation.annotationId)) {
            return fail(QStringLiteral(
                "A text markup appearance request is invalid or duplicated."));
        }
        annotationIds.insert(annotation.annotationId);

        const PlaceholderLocation placeholder = annotationForStringId(
            pdf,
            QByteArrayView("/NM"),
            annotation.annotationId,
            subtype);
        const bool located = placeholder.widget.number >= 0
            || (placeholder.container.number >= 0
                && placeholder.dictionaryStart >= 0
                && placeholder.dictionaryEnd
                    > placeholder.dictionaryStart);
        if (!located) {
            return fail(QStringLiteral(
                "A text markup annotation could not be located."));
        }

        QByteArray object;
        qsizetype dictionaryStart = -1;
        qsizetype dictionaryEnd = -1;
        if (placeholder.isDirect()) {
            object = completeObject(
                pdf,
                placeholder.container.number,
                error);
            dictionaryStart = placeholder.dictionaryStart;
            dictionaryEnd = placeholder.dictionaryEnd;
        } else {
            object = completeObject(
                pdf,
                placeholder.widget.number,
                error);
            dictionaryStart = object.indexOf("<<");
            if (dictionaryStart >= 0
                && !matchingDictionaryEnd(
                    object.constData(),
                    object.size(),
                    dictionaryStart,
                    dictionaryEnd)) {
                dictionaryEnd = -1;
            }
        }
        if (object.isEmpty()
            || dictionaryStart < 0
            || dictionaryEnd <= dictionaryStart
            || dictionaryEnd > object.size()) {
            return fail(error.isEmpty()
                    ? QStringLiteral(
                        "A text markup annotation object dictionary is malformed "
                        "(widget %1, container %2, range %3..%4, size %5).")
                        .arg(placeholder.widget.number)
                        .arg(placeholder.container.number)
                        .arg(dictionaryStart)
                        .arg(dictionaryEnd)
                        .arg(object.size())
                    : error);
        }

        QByteArray dictionary = object.mid(
            dictionaryStart,
            dictionaryEnd - dictionaryStart);
        QByteArray appearanceContent;
        TextMarkupRect appearanceBounds;
        qreal opacity = 1.0;
        if (!textMarkupAppearanceData(
                dictionary,
                annotation.type,
                appearanceContent,
                appearanceBounds,
                opacity,
                error)) {
            return fail(error);
        }
        if (nextObjectNumber >= std::numeric_limits<int>::max()) {
            return fail(QStringLiteral(
                "The PDF has no available object number for a text markup appearance."));
        }
        const int appearanceObjectNumber = nextObjectNumber++;
        if (!replaceTopLevelDictionaryValue(
                dictionary,
                QByteArrayView("/AP"),
                QByteArrayLiteral("<< /N ")
                    + QByteArray::number(appearanceObjectNumber)
                    + QByteArrayLiteral(" 0 R >>"),
                true,
                error)) {
            return fail(error);
        }
        incrementalObjects.append({
            ObjectReference{appearanceObjectNumber, 0},
            textMarkupAppearanceObject(
                appearanceObjectNumber,
                annotation.type,
                appearanceBounds,
                opacity,
                appearanceContent)});

        if (placeholder.isDirect()) {
            replacementsByContainer[placeholder.container.number].append({
                dictionaryStart,
                dictionaryEnd,
                dictionary});
            containerReferences.insert(
                placeholder.container.number,
                placeholder.container);
            continue;
        }
        if (rewrittenObjects.contains(placeholder.widget.number)) {
            return fail(QStringLiteral(
                "Two text markup requests resolved to the same annotation."));
        }
        object.replace(
            dictionaryStart,
            dictionaryEnd - dictionaryStart,
            dictionary);
        rewrittenObjects.insert(
            placeholder.widget.number,
            std::move(object));
        rewrittenReferences.insert(
            placeholder.widget.number,
            placeholder.widget);
    }

    for (auto container = replacementsByContainer.begin();
         container != replacementsByContainer.end();
         ++container) {
        if (rewrittenObjects.contains(container.key())) {
            return fail(QStringLiteral(
                "A text markup annotation container cannot be rewritten safely."));
        }
        QByteArray object = completeObject(pdf, container.key(), error);
        if (object.isEmpty()) {
            return fail(error);
        }
        std::sort(
            container->begin(),
            container->end(),
            [](const DictionaryReplacement &left,
               const DictionaryReplacement &right) {
                return left.start > right.start;
            });
        qsizetype previousStart = object.size();
        for (const DictionaryReplacement &replacement
             : std::as_const(*container)) {
            if (replacement.start < 0
                || replacement.end <= replacement.start
                || replacement.end > previousStart
                || replacement.end > object.size()) {
                return fail(QStringLiteral(
                    "Overlapping text markup annotation rewrites are not supported."));
            }
            object.replace(
                replacement.start,
                replacement.end - replacement.start,
                replacement.value);
            previousStart = replacement.start;
        }
        rewrittenObjects.insert(container.key(), std::move(object));
        rewrittenReferences.insert(
            container.key(),
            containerReferences.value(container.key()));
    }
    for (auto rewritten = rewrittenObjects.begin();
         rewritten != rewrittenObjects.end();
         ++rewritten) {
        incrementalObjects.append({
            rewrittenReferences.value(rewritten.key()),
            std::move(rewritten.value())});
    }

    const bool sourceEndsWithNewline = fileSize > 0
        && reinterpret_cast<const char *>(mapped)[fileSize - 1] == '\n';
    const ObjectReference root = pdf.root;
    const qint64 previousXref = pdf.startXref;
    file.unmap(mapped);
    file.close();
    return appendIncrementalUpdate(
        filePath,
        root,
        previousXref,
        nextObjectNumber,
        sourceEndsWithNewline,
        std::move(incrementalObjects),
        error);
}

bool PDFAcroFormCanonicalizer::finalizeDrawingAnnotations(
    const QString &filePath,
    const QVector<PDFCommentAnnotation::DrawingAnnotation> &annotations,
    QString &error)
{
    constexpr qsizetype maximumAnnotations = 4096;
    constexpr qsizetype maximumTotalVertices = 65536;
    constexpr qsizetype maximumTotalAppearanceBytes = 64 * 1024 * 1024;
    if (annotations.isEmpty()) {
        return true;
    }
    if (annotations.size() > maximumAnnotations) {
        error = QStringLiteral("Too many drawing annotations were requested.");
        return false;
    }

    qsizetype totalVertices = 0;
    QSet<QString> annotationIds;
    for (const auto &annotation : annotations) {
        const QByteArrayView subtype = drawingSubtypeName(annotation.type);
        if (annotation.page < 0
            || annotation.annotationId.isEmpty()
            || subtype.isEmpty()
            || annotationIds.contains(annotation.annotationId)) {
            error = QStringLiteral(
                "A drawing annotation request is invalid or duplicated.");
            return false;
        }
        annotationIds.insert(annotation.annotationId);
        if (annotation.pageVertices.size()
            > maximumTotalVertices - totalVertices) {
            error = QStringLiteral(
                "Drawing annotation geometry exceeds the safety limit.");
            return false;
        }
        totalVertices += annotation.pageVertices.size();
        for (const QVector<QPointF> &stroke : annotation.pageStrokes) {
            if (stroke.size() > maximumTotalVertices - totalVertices) {
                error = QStringLiteral(
                    "Drawing annotation geometry exceeds the safety limit.");
                return false;
            }
            totalVertices += stroke.size();
        }
        if (totalVertices > maximumTotalVertices) {
            error = QStringLiteral(
                "Drawing annotation geometry exceeds the safety limit.");
            return false;
        }
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0) {
        error = file.errorString();
        return false;
    }
    const qint64 fileSize = file.size();
    uchar *mapped = file.map(0, fileSize);
    if (!mapped) {
        error = file.errorString();
        return false;
    }

    ParsedPdf pdf;
    if (!parseXref(
            reinterpret_cast<const char *>(mapped),
            fileSize,
            pdf,
            error)) {
        file.unmap(mapped);
        return false;
    }
    const auto fail = [&file, mapped, &error](const QString &message) {
        error = message;
        file.unmap(mapped);
        return false;
    };

    int highestObject = 0;
    for (auto entry = pdf.entries.cbegin(); entry != pdf.entries.cend(); ++entry) {
        highestObject = (std::max)(highestObject, entry.key());
    }
    int nextObjectNumber = (std::max)(pdf.declaredSize, highestObject + 1);

    struct DictionaryReplacement {
        qsizetype start = -1;
        qsizetype end = -1;
        QByteArray value;
    };
    QHash<int, QVector<DictionaryReplacement>> replacementsByContainer;
    QHash<int, ObjectReference> containerReferences;
    QHash<int, QByteArray> rewrittenObjects;
    QHash<int, ObjectReference> rewrittenReferences;
    QList<IncrementalObject> incrementalObjects;
    incrementalObjects.reserve(annotations.size() * 2);
    qsizetype totalAppearanceBytes = 0;

    for (const PDFCommentAnnotation::DrawingAnnotation &annotation
         : annotations) {
        QByteArray appearanceContent;
        if (!drawingAppearanceData(annotation, appearanceContent, error)) {
            return fail(error);
        }
        if (appearanceContent.size()
                > maximumTotalAppearanceBytes - totalAppearanceBytes) {
            return fail(QStringLiteral(
                "Drawing annotation appearances exceed the safety limit."));
        }
        totalAppearanceBytes += appearanceContent.size();

        const PlaceholderLocation placeholder = annotationForStringId(
            pdf,
            QByteArrayView("/NM"),
            annotation.annotationId,
            QByteArrayView("/Square"));
        const bool located = placeholder.widget.number >= 0
            || (placeholder.container.number >= 0
                && placeholder.dictionaryStart >= 0
                && placeholder.dictionaryEnd > placeholder.dictionaryStart);
        if (!located) {
            return fail(QStringLiteral(
                "A drawing annotation could not be located."));
        }

        QByteArray object;
        qsizetype dictionaryStart = -1;
        qsizetype dictionaryEnd = -1;
        if (placeholder.isDirect()) {
            object = completeObject(pdf, placeholder.container.number, error);
            dictionaryStart = placeholder.dictionaryStart;
            dictionaryEnd = placeholder.dictionaryEnd;
        } else {
            object = completeObject(pdf, placeholder.widget.number, error);
            dictionaryStart = object.indexOf("<<");
            if (dictionaryStart >= 0
                && !matchingDictionaryEnd(
                    object.constData(),
                    object.size(),
                    dictionaryStart,
                    dictionaryEnd)) {
                dictionaryEnd = -1;
            }
        }
        if (object.isEmpty()
            || dictionaryStart < 0
            || dictionaryEnd <= dictionaryStart
            || dictionaryEnd > object.size()) {
            return fail(error.isEmpty()
                    ? QStringLiteral(
                        "A drawing annotation object dictionary is malformed.")
                    : error);
        }

        if (nextObjectNumber >= std::numeric_limits<int>::max()) {
            return fail(QStringLiteral(
                "The PDF has no available object number for a drawing appearance."));
        }
        const int appearanceObjectNumber = nextObjectNumber++;
        QByteArray dictionary = object.mid(
            dictionaryStart,
            dictionaryEnd - dictionaryStart);
        if (!configureDrawingAnnotationDictionary(
                dictionary,
                annotation,
                appearanceObjectNumber,
                error)) {
            return fail(error);
        }
        incrementalObjects.append({
            ObjectReference{appearanceObjectNumber, 0},
            drawingAppearanceObject(
                appearanceObjectNumber,
                annotation,
                appearanceContent)});

        if (placeholder.isDirect()) {
            replacementsByContainer[placeholder.container.number].append({
                dictionaryStart,
                dictionaryEnd,
                dictionary});
            containerReferences.insert(
                placeholder.container.number,
                placeholder.container);
            continue;
        }
        if (rewrittenObjects.contains(placeholder.widget.number)) {
            return fail(QStringLiteral(
                "Two drawing requests resolved to the same annotation."));
        }
        object.replace(
            dictionaryStart,
            dictionaryEnd - dictionaryStart,
            dictionary);
        rewrittenObjects.insert(placeholder.widget.number, std::move(object));
        rewrittenReferences.insert(
            placeholder.widget.number,
            placeholder.widget);
    }

    for (auto container = replacementsByContainer.begin();
         container != replacementsByContainer.end();
         ++container) {
        if (rewrittenObjects.contains(container.key())) {
            return fail(QStringLiteral(
                "A drawing annotation container cannot be rewritten safely."));
        }
        QByteArray object = completeObject(pdf, container.key(), error);
        if (object.isEmpty()) {
            return fail(error);
        }
        std::sort(
            container->begin(),
            container->end(),
            [](const DictionaryReplacement &left,
               const DictionaryReplacement &right) {
                return left.start > right.start;
            });
        qsizetype previousStart = object.size();
        for (const DictionaryReplacement &replacement
             : std::as_const(*container)) {
            if (replacement.start < 0
                || replacement.end <= replacement.start
                || replacement.end > previousStart
                || replacement.end > object.size()) {
                return fail(QStringLiteral(
                    "Overlapping drawing annotation rewrites are not supported."));
            }
            object.replace(
                replacement.start,
                replacement.end - replacement.start,
                replacement.value);
            previousStart = replacement.start;
        }
        rewrittenObjects.insert(container.key(), std::move(object));
        rewrittenReferences.insert(
            container.key(),
            containerReferences.value(container.key()));
    }
    for (auto rewritten = rewrittenObjects.begin();
         rewritten != rewrittenObjects.end();
         ++rewritten) {
        incrementalObjects.append({
            rewrittenReferences.value(rewritten.key()),
            std::move(rewritten.value())});
    }

    const bool sourceEndsWithNewline = fileSize > 0
        && reinterpret_cast<const char *>(mapped)[fileSize - 1] == '\n';
    const ObjectReference root = pdf.root;
    const qint64 previousXref = pdf.startXref;
    file.unmap(mapped);
    file.close();
    return appendIncrementalUpdate(
        filePath,
        root,
        previousXref,
        nextObjectNumber,
        sourceEndsWithNewline,
        std::move(incrementalObjects),
        error);
}

bool PDFAcroFormCanonicalizer::finalizeAnnotationReplies(
    const QString &filePath,
    const QVector<AnnotationReply> &replies,
    QString &error)
{
    if (replies.isEmpty()) {
        return true;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0) {
        error = file.errorString();
        return false;
    }
    const qint64 fileSize = file.size();
    uchar *mapped = file.map(0, fileSize);
    if (!mapped) {
        error = file.errorString();
        return false;
    }

    ParsedPdf pdf;
    if (!parseXref(
            reinterpret_cast<const char *>(mapped),
            fileSize,
            pdf,
            error)) {
        file.unmap(mapped);
        return false;
    }

    struct DictionaryReplacement {
        qsizetype start = -1;
        qsizetype end = -1;
        QByteArray value;
    };
    QHash<int, QVector<DictionaryReplacement>> replacementsByContainer;
    QHash<int, ObjectReference> containerReferences;
    QHash<int, QByteArray> rewrittenObjects;
    QHash<int, ObjectReference> rewrittenReferences;
    QList<IncrementalObject> newObjects;
    QHash<QString, ObjectReference> parentReferences;
    QSet<QString> replyIds;
    int nextObjectNumber = pdf.declaredSize;

    const auto fail = [&file, mapped, &error](const QString &message) {
        error = message;
        file.unmap(mapped);
        return false;
    };
    const auto referenceBytes = [](const ObjectReference &reference) {
        return QByteArray::number(reference.number)
            + ' '
            + QByteArray::number(reference.generation)
            + QByteArrayLiteral(" R");
    };
    const auto linkedDictionary = [
        &error,
        &referenceBytes](QByteArray dictionary,
                         const ObjectReference &parent) {
        if (dictionary.size() < 4
            || !dictionary.startsWith("<<")
            || !dictionary.endsWith(">>")) {
            error = QStringLiteral(
                "A comment reply annotation dictionary is malformed.");
            return QByteArray();
        }
        if (keyPosition(
                dictionary.constData(),
                dictionary.size(),
                QByteArrayView("/IRT"))
            >= 0) {
            error = QStringLiteral(
                "A comment reply already contains an IRT relationship.");
            return QByteArray();
        }
        dictionary.insert(
            dictionary.size() - 2,
            QByteArrayLiteral(" /IRT ")
                + referenceBytes(parent)
                + QByteArrayLiteral(" /RT /R"));
        return dictionary;
    };

    for (const AnnotationReply &reply : replies) {
        if (reply.page < 0
            || reply.parentThreadId.isEmpty()
            || reply.replyId.isEmpty()
            || replyIds.contains(reply.replyId)) {
            return fail(QStringLiteral(
                "A comment reply relationship is invalid or duplicated."));
        }
        replyIds.insert(reply.replyId);

        if (parentReferences.contains(reply.parentThreadId)) {
            continue;
        }
        const PlaceholderLocation parent = annotationForStringId(
            pdf,
            QByteArrayView("/TrePDFThreadId"),
            reply.parentThreadId,
            QByteArrayView());
        const bool parentLocated = parent.widget.number >= 0
            || (parent.container.number >= 0
                && parent.dictionaryStart >= 0
                && parent.dictionaryEnd > parent.dictionaryStart);
        if (!parentLocated) {
            return fail(QStringLiteral(
                "The parent annotation for a comment reply could not be located."));
        }
        if (!parent.isDirect()) {
            parentReferences.insert(
                reply.parentThreadId,
                parent.widget);
            continue;
        }

        const QByteArray containerObject = completeObject(
            pdf,
            parent.container.number,
            error);
        if (containerObject.isEmpty()
            || parent.dictionaryStart < 0
            || parent.dictionaryEnd <= parent.dictionaryStart
            || parent.dictionaryEnd > containerObject.size()) {
            return fail(error.isEmpty()
                    ? QStringLiteral(
                        "A direct parent comment annotation is malformed.")
                    : error);
        }
        if (nextObjectNumber >= std::numeric_limits<int>::max()) {
            return fail(QStringLiteral(
                "The PDF has no available object number for a comment reply."));
        }
        const ObjectReference promotedReference{nextObjectNumber++, 0};
        QByteArray promotedObject = QByteArray::number(
                promotedReference.number)
            + QByteArrayLiteral(" 0 obj\n")
            + containerObject.mid(
                parent.dictionaryStart,
                parent.dictionaryEnd - parent.dictionaryStart)
            + QByteArrayLiteral("\nendobj");
        newObjects.append({promotedReference, std::move(promotedObject)});
        replacementsByContainer[parent.container.number].append({
            parent.dictionaryStart,
            parent.dictionaryEnd,
            referenceBytes(promotedReference)});
        containerReferences.insert(
            parent.container.number,
            parent.container);
        parentReferences.insert(
            reply.parentThreadId,
            promotedReference);
    }

    for (const AnnotationReply &reply : replies) {
        const ObjectReference parentReference = parentReferences.value(
            reply.parentThreadId,
            ObjectReference{});
        if (parentReference.number < 0) {
            return fail(QStringLiteral(
                "The parent annotation reference for a comment reply is missing."));
        }
        const PlaceholderLocation child = annotationForStringId(
            pdf,
            QByteArrayView("/NM"),
            reply.replyId,
            QByteArrayView("/Text"));
        const bool childLocated = child.widget.number >= 0
            || (child.container.number >= 0
                && child.dictionaryStart >= 0
                && child.dictionaryEnd > child.dictionaryStart);
        if (!childLocated) {
            return fail(QStringLiteral(
                "A comment reply annotation could not be located."));
        }

        if (child.isDirect()) {
            const QByteArray containerObject = completeObject(
                pdf,
                child.container.number,
                error);
            if (containerObject.isEmpty()
                || child.dictionaryStart < 0
                || child.dictionaryEnd <= child.dictionaryStart
                || child.dictionaryEnd > containerObject.size()) {
                return fail(error.isEmpty()
                        ? QStringLiteral(
                            "A direct comment reply annotation is malformed.")
                        : error);
            }
            const QByteArray dictionary = linkedDictionary(
                containerObject.mid(
                    child.dictionaryStart,
                    child.dictionaryEnd - child.dictionaryStart),
                parentReference);
            if (dictionary.isEmpty()) {
                return fail(error);
            }
            replacementsByContainer[child.container.number].append({
                child.dictionaryStart,
                child.dictionaryEnd,
                dictionary});
            containerReferences.insert(
                child.container.number,
                child.container);
            continue;
        }

        QByteArray childObject = completeObject(
            pdf,
            child.widget.number,
            error);
        const qsizetype dictionaryStart = findBytes(
            childObject.constData(),
            childObject.size(),
            "<<");
        qsizetype dictionaryEnd = -1;
        if (childObject.isEmpty()
            || dictionaryStart < 0
            || !matchingDictionaryEnd(
                childObject.constData(),
                childObject.size(),
                dictionaryStart,
                dictionaryEnd)) {
            return fail(error.isEmpty()
                    ? QStringLiteral(
                        "An indirect comment reply annotation is malformed.")
                    : error);
        }
        const QByteArray dictionary = linkedDictionary(
            childObject.mid(
                dictionaryStart,
                dictionaryEnd - dictionaryStart),
            parentReference);
        if (dictionary.isEmpty()) {
            return fail(error);
        }
        childObject.replace(
            dictionaryStart,
            dictionaryEnd - dictionaryStart,
            dictionary);
        rewrittenObjects.insert(child.widget.number, std::move(childObject));
        rewrittenReferences.insert(child.widget.number, child.widget);
    }

    for (auto container = replacementsByContainer.begin();
         container != replacementsByContainer.end();
         ++container) {
        if (rewrittenObjects.contains(container.key())) {
            return fail(QStringLiteral(
                "A comment annotation container cannot be rewritten safely."));
        }
        QByteArray object = completeObject(pdf, container.key(), error);
        if (object.isEmpty()) {
            return fail(error);
        }
        std::sort(
            container->begin(),
            container->end(),
            [](const DictionaryReplacement &left,
               const DictionaryReplacement &right) {
                return left.start > right.start;
            });
        qsizetype previousStart = object.size();
        for (const DictionaryReplacement &replacement
             : std::as_const(*container)) {
            if (replacement.start < 0
                || replacement.end <= replacement.start
                || replacement.end > previousStart
                || replacement.end > object.size()) {
                return fail(QStringLiteral(
                    "Overlapping comment annotation rewrites are not supported."));
            }
            object.replace(
                replacement.start,
                replacement.end - replacement.start,
                replacement.value);
            previousStart = replacement.start;
        }
        rewrittenObjects.insert(container.key(), std::move(object));
        rewrittenReferences.insert(
            container.key(),
            containerReferences.value(container.key()));
    }

    QList<IncrementalObject> incrementalObjects = std::move(newObjects);
    for (auto object = rewrittenObjects.begin();
         object != rewrittenObjects.end();
         ++object) {
        incrementalObjects.append({
            rewrittenReferences.value(object.key()),
            std::move(object.value())});
    }
    const bool sourceEndsWithNewline = fileSize > 0
        && reinterpret_cast<const char *>(mapped)[fileSize - 1] == '\n';
    const ObjectReference root = pdf.root;
    const qint64 previousXref = pdf.startXref;
    file.unmap(mapped);
    file.close();
    return appendIncrementalUpdate(
        filePath,
        root,
        previousXref,
        nextObjectNumber,
        sourceEndsWithNewline,
        std::move(incrementalObjects),
        error);
}
