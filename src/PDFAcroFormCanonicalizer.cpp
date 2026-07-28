#include "PDFAcroFormCanonicalizer.h"

#include <QByteArray>
#include <QFile>
#include <QFont>
#include <QHash>
#include <QImage>
#include <QList>
#include <QPainter>
#include <QSet>

#include <algorithm>
#include <limits>
#include <utility>

namespace {

constexpr qreal kTextFieldFontSize = 12.0;

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

struct ParsedPdf {
    const char *data = nullptr;
    qsizetype size = 0;
    qint64 startXref = -1;
    int declaredSize = 0;
    ObjectReference root;
    QHash<int, XrefEntry> entries;
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

    Cursor cursor(data, size, static_cast<qsizetype>(startXref));
    if (!cursor.consume("xref")) {
        error = QStringLiteral("The intermediate PDF did not use a canonical xref table.");
        return false;
    }

    QHash<int, XrefEntry> entries;
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
            error = QStringLiteral("The intermediate PDF has an invalid xref subsection.");
            return false;
        }
        cursor = sectionCursor;
        for (qint64 index = 0; index < objectCount; ++index) {
            qint64 objectOffset = -1;
            qint64 generation = -1;
            if (!cursor.integer(objectOffset)
                || !cursor.integer(generation)) {
                error = QStringLiteral("The intermediate PDF has a truncated xref entry.");
                return false;
            }
            const char state = cursor.nextNonWhitespace();
            if (state != 'n' && state != 'f') {
                error = QStringLiteral("The intermediate PDF has an invalid xref state.");
                return false;
            }
            if (state == 'n') {
                if (objectOffset < 0
                    || objectOffset >= size
                    || generation < 0
                    || generation > std::numeric_limits<int>::max()) {
                    error = QStringLiteral("The intermediate PDF has an invalid object offset.");
                    return false;
                }
                entries.insert(
                    static_cast<int>(firstObject + index),
                    XrefEntry{
                        objectOffset,
                        static_cast<int>(generation)});
            }
        }
    }

    const qsizetype trailerStart = cursor.position();
    const qsizetype trailerEnd = startXrefPosition;
    if (trailerStart >= trailerEnd) {
        error = QStringLiteral("The intermediate PDF has no trailer dictionary.");
        return false;
    }
    const char *trailerData = data + trailerStart;
    const qsizetype trailerSize = trailerEnd - trailerStart;
    if (keyPosition(trailerData, trailerSize, "/Encrypt") >= 0) {
        error = QStringLiteral("Creating fields in encrypted PDFs is not supported.");
        return false;
    }

    const qsizetype rootKey = keyPosition(trailerData, trailerSize, "/Root");
    ObjectReference root;
    if (rootKey < 0
        || !parseReference(
            trailerData,
            trailerSize,
            rootKey + QByteArrayView("/Root").size(),
            root)) {
        error = QStringLiteral("The intermediate PDF has no indirect Catalog reference.");
        return false;
    }
    const qsizetype sizeKey = keyPosition(trailerData, trailerSize, "/Size");
    qint64 declaredSize = 0;
    if (sizeKey < 0) {
        error = QStringLiteral("The intermediate PDF trailer has no Size entry.");
        return false;
    }
    Cursor sizeCursor(
        trailerData,
        trailerSize,
        sizeKey + QByteArrayView("/Size").size());
    if (!sizeCursor.integer(declaredSize)
        || declaredSize <= 0
        || declaredSize > std::numeric_limits<int>::max()) {
        error = QStringLiteral("The intermediate PDF trailer has an invalid Size entry.");
        return false;
    }
    if (!entries.contains(root.number)) {
        error = QStringLiteral("The intermediate PDF Catalog is missing from the xref table.");
        return false;
    }

    pdf.data = data;
    pdf.size = size;
    pdf.startXref = startXref;
    pdf.declaredSize = static_cast<int>(declaredSize);
    pdf.root = root;
    pdf.entries = std::move(entries);
    return true;
}

QByteArrayView objectView(const ParsedPdf &pdf, int objectNumber)
{
    const auto entry = pdf.entries.constFind(objectNumber);
    if (entry == pdf.entries.cend()) {
        return {};
    }
    qint64 endOffset = pdf.startXref;
    for (auto candidate = pdf.entries.cbegin();
         candidate != pdf.entries.cend();
         ++candidate) {
        if (candidate->offset > entry->offset
            && candidate->offset < endOffset) {
            endOffset = candidate->offset;
        }
    }
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

PlaceholderLocation annotationForWidgetId(
    const ParsedPdf &pdf,
    const QString &widgetId,
    QByteArrayView expectedSubtype)
{
    PlaceholderLocation directCandidate;
    for (auto entry = pdf.entries.cbegin(); entry != pdf.entries.cend(); ++entry) {
        const QByteArrayView view = objectView(pdf, entry.key());
        if (view.isEmpty()) {
            continue;
        }
        qsizetype dictionarySize = view.size();
        const qsizetype streamPosition = findBytes(
            view.data(),
            view.size(),
            "stream");
        if (streamPosition >= 0) {
            dictionarySize = streamPosition;
        }
        qsizetype searchPosition = 0;
        while (true) {
            const qsizetype nameKey = keyPositionFrom(
                view.data(),
                dictionarySize,
                "/NM",
                searchPosition);
            if (nameKey < 0) {
                break;
            }
            searchPosition = nameKey + QByteArrayView("/NM").size();
            QByteArray encodedName;
            if (!parsePdfString(
                    view.data(),
                    dictionarySize,
                    searchPosition,
                    encodedName)
                || decodedPdfString(encodedName) != widgetId) {
                continue;
            }

            const qsizetype topLevelDictionary = findBytes(
                view.data(),
                dictionarySize,
                "<<");
            qsizetype dictionaryStart = -1;
            qsizetype dictionaryEnd = -1;
            qsizetype candidateStart = topLevelDictionary;
            while (candidateStart >= 0 && candidateStart < nameKey) {
                qsizetype candidateEnd = -1;
                if (!matchingDictionaryEnd(
                        view.data(),
                        dictionarySize,
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
                    dictionarySize,
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
                || !subtypeCursor.consume(expectedSubtype)) {
                continue;
            }
            const ObjectReference objectReference{
                entry.key(),
                entry->generation};
            if (dictionaryStart == topLevelDictionary) {
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

QByteArray textFieldDefaultAppearance(QByteArrayView fontResourceName)
{
    return QByteArrayLiteral("/")
        + QByteArray(fontResourceName.data(), fontResourceName.size())
        + ' '
        + pdfNumber(kTextFieldFontSize)
        + QByteArrayLiteral(" Tf 0 g");
}

QImage textFieldAppearanceImage(
    const PDFAcroFormCanonicalizer::TextField &field)
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
    painter.setPen(QPen(QColor(140, 140, 140), qMax(1.0, rasterScale)));
    painter.setBrush(Qt::NoBrush);
    const qreal borderInset = qMax(0.5, rasterScale / 2.0);
    painter.drawRect(
        QRectF(
            borderInset,
            borderInset,
            pixelWidth - 2.0 * borderInset,
            pixelHeight - 2.0 * borderInset));

    QFont font(QStringLiteral("Helvetica"));
    const qreal pixelsPerPointY
        = field.pageRect.height() > 0.0
        ? pixelHeight / field.pageRect.height()
        : rasterScale;
    font.setPixelSize(qMax(
        1,
        qRound(kTextFieldFontSize * pixelsPerPointY)));
    painter.setFont(font);
    painter.setPen(QColor(32, 33, 36));
    const qreal horizontalPadding = qMax(4.0, pixelHeight * 0.1);
    painter.drawText(
        QRectF(
            horizontalPadding,
            0,
            pixelWidth - 2.0 * horizontalPadding,
            pixelHeight),
        Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
        field.value);
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
    const PDFAcroFormCanonicalizer::TextField &field,
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
        + QByteArrayLiteral(" 0 R >> >>\nendobj");
    return object;
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

QByteArray updatedAcroFormObject(const ParsedPdf &pdf,
                                 const QList<ObjectReference> &references,
                                 const ObjectReference &targetReference,
                                 const ObjectReference &defaultFont,
                                 QByteArrayView fontResourceName,
                                 QString &error)
{
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
        if (keyPosition(
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
    if (!ensureFormFontResource(
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

bool PDFAcroFormCanonicalizer::finalizeTextFields(
    const QString &filePath,
    const QVector<TextField> &textFields,
    QString &error)
{
    if (textFields.isEmpty()) {
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
        bool added = true;
    };
    QVector<ResolvedWidget> resolvedWidgets;
    resolvedWidgets.reserve(textFields.size());
    QList<ObjectReference> widgetReferences;
    widgetReferences.reserve(textFields.size());
    QSet<int> resolvedWidgetObjectNumbers;
    for (const TextField &textField : textFields) {
        const bool added
            = textField.operation == Operation::Add;
        const PlaceholderLocation placeholder = annotationForWidgetId(
            pdf,
            textField.widgetId,
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
        if (!added
            && !visitedFieldObjects.contains(reference.number)) {
            error = QStringLiteral(
                "An existing TrePDF Widget is missing from the AcroForm field tree.");
            file.unmap(mapped);
            return false;
        }
        if (!added) {
            const QByteArray existingWidget = completeObject(
                pdf,
                reference.number,
                error);
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
            if (existingWidget.isEmpty()
                || fieldTypeKey < 0
                || !fieldTypeCursor.consume("/Tx")
                || keyPosition(
                       existingWidget.constData(),
                       existingWidget.size(),
                       "/Kids") >= 0
                || keyPosition(
                       existingWidget.constData(),
                       existingWidget.size(),
                       "/Parent") >= 0) {
                error = QStringLiteral(
                    "The existing TrePDF Text Field structure is unsupported.");
                file.unmap(mapped);
                return false;
            }
        }
        if (resolvedWidgetObjectNumbers.contains(reference.number)) {
            error = QStringLiteral(
                "Two form fields resolved to the same Widget placeholder.");
            file.unmap(mapped);
            return false;
        }
        resolvedWidgetObjectNumbers.insert(reference.number);
        resolvedWidgets.append({placeholder, reference, added});
        widgetReferences.append(reference);
        if (added) {
            fields.append(reference);
        }
    }

    QSet<QString> occupiedFieldNames = fieldNames;
    for (const ResolvedWidget &resolved : std::as_const(resolvedWidgets)) {
        if (!resolved.added) {
            occupiedFieldNames.remove(
                fieldNamesByObject.value(resolved.reference.number));
        }
    }
    for (const TextField &textField : textFields) {
        if (textField.fieldName.isEmpty()
            || textField.fieldName.contains(QLatin1Char('.'))) {
            error = QStringLiteral(
                "TrePDF AcroForm fields require a non-empty flat field name.");
            file.unmap(mapped);
            return false;
        }
        if (occupiedFieldNames.contains(textField.fieldName)) {
            error = QStringLiteral(
                "An AcroForm field with the same name already exists.");
            file.unmap(mapped);
            return false;
        }
        occupiedFieldNames.insert(textField.fieldName);
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
    const int defaultFontObjectNumber = nextObjectNumber++;
    const QByteArray defaultFontResourceName
        = QByteArrayLiteral("TrePDFF")
        + QByteArray::number(defaultFontObjectNumber);

    QList<IncrementalObject> incrementalObjects;
    incrementalObjects.reserve(textFields.size() * 4 + 3);

    struct PageReplacement {
        qsizetype start = -1;
        qsizetype end = -1;
        ObjectReference widget;
    };
    QHash<int, QVector<PageReplacement>> pageReplacements;
    QHash<int, ObjectReference> pageReferences;
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
        QByteArray pageObject = completeObject(
            pdf,
            pageReference.number,
            error);
        if (pageObject.isEmpty()) {
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
        incrementalObjects.append({
            pageReference,
            std::move(pageObject)});
    }

    constexpr qint64 maximumAppearancePixels = 32 * 1024 * 1024;
    qint64 appearancePixels = 0;
    for (qsizetype index = 0; index < textFields.size(); ++index) {
        const TextField &textField = textFields.at(index);
        const ObjectReference &widgetReference = widgetReferences.at(index);
        const PlaceholderLocation &placeholder
            = resolvedWidgets.at(index).placeholder;
        const QImage appearanceImage = textFieldAppearanceImage(textField);
        appearancePixels += static_cast<qint64>(appearanceImage.width())
            * appearanceImage.height();
        if (appearancePixels > maximumAppearancePixels) {
            error = QStringLiteral(
                "The combined Text Field appearances are too large.");
            file.unmap(mapped);
            return false;
        }
        const QByteArray compressedPixels = compressedRgbPixels(
            appearanceImage);
        if (appearanceImage.isNull() || compressedPixels.isEmpty()) {
            error = QStringLiteral(
                "The Text Field appearance could not be generated.");
            file.unmap(mapped);
            return false;
        }
        const int appearanceObjectNumber = nextObjectNumber++;
        const int imageObjectNumber = nextObjectNumber++;
        incrementalObjects.append({
            widgetReference,
            textFieldWidgetObject(
                widgetReference,
                textField,
                appearanceObjectNumber,
                placeholder.isDirect()
                    ? placeholder.container
                    : ObjectReference{},
                defaultFontResourceName)});
        incrementalObjects.append({
            ObjectReference{appearanceObjectNumber, 0},
            appearanceObject(
                appearanceObjectNumber,
                imageObjectNumber,
                textField.pageRect)});
        incrementalObjects.append({
            ObjectReference{imageObjectNumber, 0},
            imageObject(
                imageObjectNumber,
                appearanceImage,
                compressedPixels)});
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
        ObjectReference{defaultFontObjectNumber, 0},
        defaultFontResourceName,
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
    incrementalObjects.append({
        ObjectReference{defaultFontObjectNumber, 0},
        standardFormFontObject(defaultFontObjectNumber)});
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
