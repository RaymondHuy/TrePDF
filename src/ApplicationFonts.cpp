#include "ApplicationFonts.h"

#include <QByteArrayView>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QRawFont>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QtGlobal>
#include <QVector>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <utility>

#ifdef Q_OS_MACOS
#include <CoreText/CoreText.h>
#endif

namespace {

constexpr qsizetype kMaximumSystemFontBytes = 64 * 1024 * 1024;
constexpr int kFontLanguageCount = 5;

QVector<uint> visibleCodePoints(const QString &text)
{
    const QString normalized = text.normalized(QString::NormalizationForm_C);
    QVector<uint> result;
    QSet<uint> seen;
    for (uint character : normalized.toUcs4()) {
        if (QChar::isSpace(static_cast<char32_t>(character))
            || seen.contains(character)) {
            continue;
        }
        seen.insert(character);
        result.append(character);
    }
    return result;
}

const QString &vietnameseCoverageText()
{
    static const QString coverage = [] {
        QString result = QStringLiteral(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
            "ÀÁÂÃÈÉÊÌÍÒÓÔÕÙÚÝàáâãèéêìíòóôõùúý"
            "ĂăĐđĨĩŨũƠơƯư");
        for (char32_t character = 0x1ea0; character <= 0x1ef9;
             ++character) {
            result.append(QChar(static_cast<ushort>(character)));
        }
        return result;
    }();
    return coverage;
}

const QString &englishCoverageText()
{
    static const QString coverage = QStringLiteral(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
        "0123456789.,;:!?\"'()-");
    return coverage;
}

const QString &simplifiedChineseProbe()
{
    static const QString probe = QStringLiteral("中文汉");
    return probe;
}

const QString &traditionalChineseProbe()
{
    static const QString probe = QStringLiteral("中文漢");
    return probe;
}

const QString &japaneseProbe()
{
    static const QString probe = QStringLiteral("日本語");
    return probe;
}

const QString &koreanProbe()
{
    static const QString probe = QStringLiteral("한국어");
    return probe;
}

const QString &fontStretchProbe()
{
    static const QString probe = QStringLiteral(
        "Hamburgefontsiv AVATAR 0123456789 abcdefghijklmnopqrstuvwxyz");
    return probe;
}

QHash<QString, int> &fontStretchCalibrationCache()
{
    static QHash<QString, int> cache;
    return cache;
}

QMutex &fontStretchCalibrationMutex()
{
    static QMutex mutex;
    return mutex;
}

QString fontStretchCalibrationKey(const QString &family,
                                  int targetStretch,
                                  bool bold,
                                  bool italic,
                                  int pixelSize)
{
    return family.toCaseFolded()
        + QLatin1Char('|')
        + QString::number(targetStretch)
        + QLatin1Char('|')
        + QString::number(bold)
        + QString::number(italic)
        + QLatin1Char('|')
        + QString::number(pixelSize);
}

qreal fontAdvanceForStretch(const QString &family,
                            int stretch,
                            bool bold,
                            bool italic,
                            int pixelSize)
{
    QFont font;
    if (!family.isEmpty()) {
        font.setFamily(family);
    }
    font.setBold(bold);
    font.setItalic(italic);
    font.setPixelSize(pixelSize);
    font.setStretch(stretch);
    return QFontMetricsF(font).horizontalAdvance(fontStretchProbe());
}

quint16 readBigEndian16(QByteArrayView data, qsizetype offset)
{
    return static_cast<quint16>(
        (static_cast<quint16>(static_cast<unsigned char>(data.at(offset))) << 8)
        | static_cast<unsigned char>(data.at(offset + 1)));
}

bool permitsEditableEmbedding(QByteArrayView os2Table)
{
    if (os2Table.size() < 10) {
        return false;
    }

    const quint16 version = readBigEndian16(os2Table, 0);
    const quint16 fsType = readBigEndian16(os2Table, 8);
    constexpr quint16 permanentlyReservedBit = 0x0001U;
    if ((fsType & permanentlyReservedBit) != 0) {
        return false;
    }

    if (version >= 2) {
        constexpr quint16 reservedBits = 0xFCF0U;
        constexpr quint16 noOutlineEmbedding = 0x0200U;
        if ((fsType & reservedBits) != 0
            || (fsType & noOutlineEmbedding) != 0) {
            return false;
        }
    }

    const quint16 permission = fsType & 0x000fU;
    if (permission == 0) {
        return true;
    }
    if (version <= 2) {
        // Early OS/2 versions allowed multiple permission bits and defined the
        // least restrictive bit as authoritative.
        return (permission & 0x0008U) != 0;
    }
    return permission == 0x0008U;
}

#ifdef Q_OS_MACOS

constexpr quint32 kFontChecksumMagic = 0xB1B0AFBAU;
constexpr quint32 kTrueTypeScaler = 0x00010000U;
constexpr quint32 kTagCff = 0x43464620U;
constexpr quint32 kTagCff2 = 0x43464632U;
constexpr quint32 kTagDsig = 0x44534947U;
constexpr quint32 kTagFvar = 0x66766172U;
constexpr quint32 kTagGlyf = 0x676C7966U;
constexpr quint32 kTagHead = 0x68656164U;
constexpr quint32 kTagLoca = 0x6C6F6361U;
constexpr quint32 kTagOs2 = 0x4F532F32U;

template<typename T>
class CfHandle
{
public:
    explicit CfHandle(T value = nullptr)
        : m_value(value)
    {
    }

    ~CfHandle()
    {
        if (m_value) {
            CFRelease(m_value);
        }
    }

    CfHandle(const CfHandle &) = delete;
    CfHandle &operator=(const CfHandle &) = delete;

    CfHandle(CfHandle &&other) noexcept
        : m_value(std::exchange(other.m_value, nullptr))
    {
    }

    CfHandle &operator=(CfHandle &&other) noexcept
    {
        if (this != &other) {
            if (m_value) {
                CFRelease(m_value);
            }
            m_value = std::exchange(other.m_value, nullptr);
        }
        return *this;
    }

    T get() const
    {
        return m_value;
    }

    explicit operator bool() const
    {
        return m_value != nullptr;
    }

private:
    T m_value = nullptr;
};

struct FontTable {
    quint32 tag = 0;
    quint32 checksum = 0;
    quint32 offset = 0;
    QByteArray data;
};

QString fromCfString(CFStringRef value)
{
    if (!value) {
        return {};
    }
    const CFIndex length = CFStringGetLength(value);
    QString result(static_cast<qsizetype>(length), Qt::Uninitialized);
    if (length > 0) {
        CFStringGetCharacters(
            value,
            CFRangeMake(0, length),
            reinterpret_cast<UniChar *>(result.data()));
    }
    return result;
}

CfHandle<CFStringRef> toCfString(const QString &value)
{
    return CfHandle<CFStringRef>(CFStringCreateWithCharacters(
        kCFAllocatorDefault,
        reinterpret_cast<const UniChar *>(value.utf16()),
        static_cast<CFIndex>(value.size())));
}

CfHandle<CTFontRef> exactSystemFont(const QString &family,
                                   bool bold,
                                   bool italic)
{
    const CfHandle<CFStringRef> familyName = toCfString(family);
    if (!familyName) {
        return CfHandle<CTFontRef>();
    }
    const CfHandle<CTFontRef> baseFont(CTFontCreateWithName(
        familyName.get(),
        12.0,
        nullptr));
    if (!baseFont) {
        return CfHandle<CTFontRef>();
    }

    const CTFontSymbolicTraits styleMask = kCTFontTraitBold
        | kCTFontTraitItalic;
    CTFontSymbolicTraits desiredTraits = 0;
    if (bold) {
        desiredTraits |= kCTFontTraitBold;
    }
    if (italic) {
        desiredTraits |= kCTFontTraitItalic;
    }

    CfHandle<CTFontRef> font(CTFontCreateCopyWithSymbolicTraits(
        baseFont.get(),
        12.0,
        nullptr,
        desiredTraits,
        styleMask));
    if (!font
        || (CTFontGetSymbolicTraits(font.get()) & styleMask) != desiredTraits) {
        return CfHandle<CTFontRef>();
    }

    const CfHandle<CFStringRef> resolvedFamily(CTFontCopyFamilyName(font.get()));
    if (fromCfString(resolvedFamily.get()).compare(
            family,
            Qt::CaseInsensitive) != 0) {
        return CfHandle<CTFontRef>();
    }
    return font;
}

CFCharacterSetRef copyExactSystemFontCharacterSet(const QString &family,
                                                  bool bold,
                                                  bool italic)
{
    const CfHandle<CTFontRef> font = exactSystemFont(family, bold, italic);
    return font ? CTFontCopyCharacterSet(font.get()) : nullptr;
}

void appendBigEndian16(QByteArray &data, quint16 value)
{
    data.append(static_cast<char>((value >> 8) & 0xffU));
    data.append(static_cast<char>(value & 0xffU));
}

void appendBigEndian32(QByteArray &data, quint32 value)
{
    data.append(static_cast<char>((value >> 24) & 0xffU));
    data.append(static_cast<char>((value >> 16) & 0xffU));
    data.append(static_cast<char>((value >> 8) & 0xffU));
    data.append(static_cast<char>(value & 0xffU));
}

void writeBigEndian32(QByteArray &data, qsizetype offset, quint32 value)
{
    data[offset] = static_cast<char>((value >> 24) & 0xffU);
    data[offset + 1] = static_cast<char>((value >> 16) & 0xffU);
    data[offset + 2] = static_cast<char>((value >> 8) & 0xffU);
    data[offset + 3] = static_cast<char>(value & 0xffU);
}

quint32 sfntChecksum(const QByteArray &data)
{
    quint32 checksum = 0;
    for (qsizetype offset = 0; offset < data.size(); offset += 4) {
        quint32 word = 0;
        for (qsizetype index = 0; index < 4; ++index) {
            word <<= 8;
            if (offset + index < data.size()) {
                word |= static_cast<unsigned char>(data.at(offset + index));
            }
        }
        checksum += word;
    }
    return checksum;
}

bool containsTable(const QVector<FontTable> &tables, quint32 tag)
{
    return std::any_of(
        tables.cbegin(),
        tables.cend(),
        [tag](const FontTable &table) { return table.tag == tag; });
}

const FontTable *findTable(const QVector<FontTable> &tables, quint32 tag)
{
    const auto table = std::find_if(
        tables.cbegin(),
        tables.cend(),
        [tag](const FontTable &candidate) { return candidate.tag == tag; });
    return table == tables.cend() ? nullptr : &*table;
}

bool isPotentiallyEmbeddableSystemFont(
    CTFontRef font,
    ApplicationFonts::FontFaceData::Error &error)
{
    error = ApplicationFonts::FontFaceData::Error::UnsupportedFormat;
    const CTFontSymbolicTraits traits = CTFontGetSymbolicTraits(font);
    if ((traits & (kCTFontTraitColorGlyphs | kCTFontTraitComposite)) != 0) {
        return false;
    }

    const CfHandle<CFArrayRef> availableTables(CTFontCopyAvailableTables(
        font,
        kCTFontTableOptionNoOptions));
    if (!availableTables) {
        return false;
    }

    QSet<quint32> tags;
    const CFIndex tableCount = CFArrayGetCount(availableTables.get());
    for (CFIndex index = 0; index < tableCount; ++index) {
        tags.insert(static_cast<quint32>(reinterpret_cast<uintptr_t>(
            CFArrayGetValueAtIndex(availableTables.get(), index))));
    }
    if (tags.contains(kTagFvar)
        || tags.contains(kTagCff)
        || tags.contains(kTagCff2)
        || !tags.contains(kTagGlyf)
        || !tags.contains(kTagLoca)
        || !tags.contains(kTagHead)
        || !tags.contains(kTagOs2)) {
        return false;
    }

    const CfHandle<CFDataRef> headTable(CTFontCopyTable(
        font,
        static_cast<CTFontTableTag>(kTagHead),
        kCTFontTableOptionNoOptions));
    if (!headTable || CFDataGetLength(headTable.get()) < 12) {
        return false;
    }

    const CfHandle<CFDataRef> os2Table(CTFontCopyTable(
        font,
        static_cast<CTFontTableTag>(kTagOs2),
        kCTFontTableOptionNoOptions));
    if (!os2Table) {
        error = ApplicationFonts::FontFaceData::Error::EmbeddingRestricted;
        return false;
    }
    const QByteArrayView os2Bytes(
        reinterpret_cast<const char *>(CFDataGetBytePtr(os2Table.get())),
        static_cast<qsizetype>(CFDataGetLength(os2Table.get())));
    if (!permitsEditableEmbedding(os2Bytes)) {
        error = ApplicationFonts::FontFaceData::Error::EmbeddingRestricted;
        return false;
    }

    error = ApplicationFonts::FontFaceData::Error::None;
    return true;
}

QByteArray materializeTrueTypeFont(
    CTFontRef font,
    ApplicationFonts::FontFaceData::Error &error)
{
    if (!isPotentiallyEmbeddableSystemFont(font, error)) {
        return {};
    }
    error = ApplicationFonts::FontFaceData::Error::UnsupportedFormat;

    CfHandle<CFArrayRef> availableTables(CTFontCopyAvailableTables(
        font,
        kCTFontTableOptionNoOptions));
    if (!availableTables) {
        return {};
    }

    QVector<FontTable> tables;
    qsizetype totalTableBytes = 0;
    const CFIndex tableCount = CFArrayGetCount(availableTables.get());
    tables.reserve(static_cast<qsizetype>(tableCount));
    for (CFIndex index = 0; index < tableCount; ++index) {
        const quint32 tag = static_cast<quint32>(reinterpret_cast<uintptr_t>(
            CFArrayGetValueAtIndex(availableTables.get(), index)));
        if (tag == kTagDsig) {
            continue;
        }

        CfHandle<CFDataRef> tableData(CTFontCopyTable(
            font,
            static_cast<CTFontTableTag>(tag),
            kCTFontTableOptionNoOptions));
        if (!tableData) {
            continue;
        }
        const CFIndex byteCount = CFDataGetLength(tableData.get());
        if (byteCount <= 0
            || byteCount > kMaximumSystemFontBytes
            || totalTableBytes > kMaximumSystemFontBytes - byteCount) {
            error = ApplicationFonts::FontFaceData::Error::TooLarge;
            return {};
        }

        QByteArray bytes(
            reinterpret_cast<const char *>(CFDataGetBytePtr(tableData.get())),
            static_cast<qsizetype>(byteCount));
        if (tag == kTagHead) {
            if (bytes.size() < 12) {
                return {};
            }
            writeBigEndian32(bytes, 8, 0);
        }
        totalTableBytes += bytes.size();
        tables.append(FontTable{tag, 0, 0, std::move(bytes)});
    }

    if (!containsTable(tables, kTagGlyf)
        || !containsTable(tables, kTagLoca)
        || !containsTable(tables, kTagHead)) {
        return {};
    }

    std::sort(
        tables.begin(),
        tables.end(),
        [](const FontTable &left, const FontTable &right) {
            return left.tag < right.tag;
        });
    if (tables.isEmpty() || tables.size() > 0xffff) {
        return {};
    }

    const quint16 numTables = static_cast<quint16>(tables.size());
    quint16 maximumPowerOfTwo = 1;
    quint16 entrySelector = 0;
    while (maximumPowerOfTwo <= numTables / 2) {
        maximumPowerOfTwo *= 2;
        ++entrySelector;
    }
    const quint16 searchRange = static_cast<quint16>(maximumPowerOfTwo * 16);
    const quint16 rangeShift = static_cast<quint16>(numTables * 16 - searchRange);
    const qsizetype directoryBytes = 12 + static_cast<qsizetype>(numTables) * 16;
    if (directoryBytes > kMaximumSystemFontBytes
        || totalTableBytes > kMaximumSystemFontBytes - directoryBytes) {
        error = ApplicationFonts::FontFaceData::Error::TooLarge;
        return {};
    }

    quint32 tableOffset = static_cast<quint32>(directoryBytes);
    for (FontTable &table : tables) {
        table.checksum = sfntChecksum(table.data);
        table.offset = tableOffset;
        tableOffset += static_cast<quint32>((table.data.size() + 3) & ~3);
        if (tableOffset > static_cast<quint32>(kMaximumSystemFontBytes)) {
            error = ApplicationFonts::FontFaceData::Error::TooLarge;
            return {};
        }
    }

    QByteArray result;
    result.reserve(static_cast<qsizetype>(tableOffset));
    appendBigEndian32(result, kTrueTypeScaler);
    appendBigEndian16(result, numTables);
    appendBigEndian16(result, searchRange);
    appendBigEndian16(result, entrySelector);
    appendBigEndian16(result, rangeShift);
    for (const FontTable &table : std::as_const(tables)) {
        appendBigEndian32(result, table.tag);
        appendBigEndian32(result, table.checksum);
        appendBigEndian32(result, table.offset);
        appendBigEndian32(result, static_cast<quint32>(table.data.size()));
    }
    for (const FontTable &table : std::as_const(tables)) {
        result.append(table.data);
        while ((result.size() & 3) != 0) {
            result.append('\0');
        }
    }

    const FontTable *headTable = findTable(tables, kTagHead);
    if (!headTable
        || static_cast<qsizetype>(headTable->offset) + 12 > result.size()) {
        return {};
    }
    const quint32 checksumAdjustment = kFontChecksumMagic - sfntChecksum(result);
    writeBigEndian32(result, static_cast<qsizetype>(headTable->offset) + 8,
                     checksumAdjustment);
    error = ApplicationFonts::FontFaceData::Error::None;
    return result;
}

#endif

struct FontRegistry {
    struct BundledFont {
        QString resourcePath;
        QString family;
        bool bold = false;
        bool italic = false;
    };

    struct DownloadedFont {
        QString path;
        QString family;
        QByteArray digest;
        qsizetype size = 0;
        bool bold = false;
        bool italic = false;
        int registrationId = -1;
    };

    QStringList bundledFamilies;
    QStringList downloadedFamilies;
    QStringList systemFamilies;
    QHash<QString, QString> systemFamilyByKey;
    std::array<QStringList, 4> availableFamilies;
    std::array<bool, 4> availableFamiliesReady = {};
    std::array<QStringList, 4 * kFontLanguageCount> languageFamilies;
    std::array<bool, 4 * kFontLanguageCount> languageFamiliesReady = {};
    QHash<QString, bool> selectableSystemFaces;
    QHash<QString, QRawFont> bundledRawFonts;
    QHash<QString, QRawFont> downloadedRawFonts;
#ifdef Q_OS_MACOS
    QHash<QString, CFCharacterSetRef> systemCharacterSets;
#endif
    QVector<BundledFont> bundledFonts;
    QVector<DownloadedFont> downloadedFonts;
    bool initialized = false;

    ~FontRegistry()
    {
#ifdef Q_OS_MACOS
        for (CFCharacterSetRef characterSet : std::as_const(systemCharacterSets)) {
            CFRelease(characterSet);
        }
#endif
    }
};

FontRegistry &fontRegistry()
{
    static FontRegistry registry;
    return registry;
}

int fontStyleIndex(bool bold, bool italic)
{
    return (bold ? 2 : 0) + (italic ? 1 : 0);
}

QString fontFaceCacheKey(const QString &family, bool bold, bool italic)
{
    return family.toCaseFolded()
        + QLatin1Char('|')
        + QString::number(bold)
        + QString::number(italic);
}

constexpr int kMaximumDownloadedFonts = 128;
constexpr qint64 kMaximumDownloadedFontBytes = 256 * 1024 * 1024;

bool fontRegistrationThread()
{
    return QCoreApplication::instance()
        && QThread::currentThread() == QCoreApplication::instance()->thread();
}

struct ValidatedDownloadedFont {
    QString family;
    bool bold = false;
    bool italic = false;
    QRawFont rawFont;
};

bool validateDownloadedFont(const QByteArray &data,
                            ValidatedDownloadedFont &result,
                            QString &error)
{
    if (data.size() < 12 || data.size() > kMaximumSystemFontBytes) {
        error = QStringLiteral("Downloaded font size is invalid.");
        return false;
    }
    const QByteArrayView bytes(data);
    const auto read32 = [&bytes](qsizetype offset) {
        return (quint32(readBigEndian16(bytes, offset)) << 16)
            | readBigEndian16(bytes, offset + 2);
    };
    if (read32(0) != 0x00010000U) {
        error = QStringLiteral("Downloaded font must be a static TrueType font.");
        return false;
    }
    const quint16 count = readBigEndian16(bytes, 4);
    if (count == 0 || count > 256 || 12 + count * 16 > data.size()) {
        error = QStringLiteral("Downloaded font table directory is invalid.");
        return false;
    }
    QHash<QByteArray, QByteArrayView> tables;
    for (int index = 0; index < count; ++index) {
        const qsizetype entry = 12 + index * 16;
        const QByteArray tag = data.mid(entry, 4);
        const quint32 offset = read32(entry + 8);
        const quint32 length = read32(entry + 12);
        if (offset < quint32(12 + count * 16) || quint64(offset) + length > quint64(data.size())
            || tables.contains(tag)) {
            error = QStringLiteral("Downloaded font table bounds are invalid.");
            return false;
        }
        tables.insert(tag, bytes.sliced(offset, length));
    }
    for (const QByteArray &required : {QByteArray("head"), QByteArray("OS/2"),
             QByteArray("glyf"), QByteArray("loca"), QByteArray("cmap"),
             QByteArray("name"), QByteArray("maxp"), QByteArray("hhea"), QByteArray("hmtx")}) {
        if (!tables.contains(required)) {
            error = QStringLiteral("Downloaded font is missing a required TrueType table.");
            return false;
        }
    }
    for (const QByteArray &unsupported : {QByteArray("fvar"), QByteArray("CFF "),
             QByteArray("CFF2"), QByteArray("COLR"), QByteArray("SVG "), QByteArray("CBDT")}) {
        if (tables.contains(unsupported)) {
            error = QStringLiteral("Downloaded font uses an unsupported outline format.");
            return false;
        }
    }
    const QByteArrayView os2 = tables.value(QByteArrayLiteral("OS/2"));
    if (!permitsEditableEmbedding(os2) || (readBigEndian16(os2, 8) & 0x0300U) != 0) {
        error = QStringLiteral("Downloaded font does not permit editable embedding.");
        return false;
    }
    if (os2.size() < 64 || tables.value(QByteArrayLiteral("head")).size() < 54) {
        error = QStringLiteral("Downloaded font metrics are incomplete.");
        return false;
    }
    const quint16 selection = readBigEndian16(os2, 62);
    result.bold = (selection & 0x0020U) != 0;
    result.italic = (selection & 0x0001U) != 0;
    result.rawFont = QRawFont(data, 16.0, QFont::PreferNoHinting);
    result.family = result.rawFont.familyName();
    // PDF subset names are never promoted to a downloadable complete face.
    const auto subsetName = [](const QString &name) {
        if (name.size() <= 7 || name.at(6) != QLatin1Char('+'))
            return false;
        return std::all_of(name.begin(), name.begin() + 6, [](QChar c) {
            return c >= QLatin1Char('A') && c <= QLatin1Char('Z');
        });
    };
    if (!result.rawFont.isValid() || result.family.isEmpty() || result.family.size() > 256
        || subsetName(result.family)
        || (result.rawFont.weight() >= QFont::Bold) != result.bold
        || (result.rawFont.style() != QFont::StyleNormal) != result.italic) {
        error = QStringLiteral("Downloaded font family or style is invalid.");
        return false;
    }
    return true;
}

void clearFontSelectionCaches(FontRegistry &registry)
{
    for (auto &families : registry.availableFamilies)
        families.clear();
    registry.availableFamiliesReady.fill(false);
    for (auto &families : registry.languageFamilies)
        families.clear();
    registry.languageFamiliesReady.fill(false);
    QMutexLocker lock(&fontStretchCalibrationMutex());
    fontStretchCalibrationCache().clear();
}

const FontRegistry::DownloadedFont *findDownloadedFont(
    const FontRegistry &registry, const QString &family, bool bold, bool italic)
{
    const auto found = std::find_if(registry.downloadedFonts.cbegin(), registry.downloadedFonts.cend(),
        [&](const FontRegistry::DownloadedFont &font) {
            return font.family.compare(family, Qt::CaseInsensitive) == 0
                && font.bold == bold && font.italic == italic;
        });
    return found == registry.downloadedFonts.cend() ? nullptr : &*found;
}

bool registerDownloadedFont(FontRegistry &registry, const QString &path,
                            const QByteArray &data, const ValidatedDownloadedFont &font)
{
    if (findDownloadedFont(registry, font.family, font.bold, font.italic))
        return true;
    const int registration = QFontDatabase::addApplicationFontFromData(data);
    if (registration < 0)
        return false;
    registry.downloadedFonts.append({path, font.family,
        QCryptographicHash::hash(data, QCryptographicHash::Sha256), data.size(),
        font.bold, font.italic, registration});
    registry.downloadedRawFonts.insert(fontFaceCacheKey(font.family, font.bold, font.italic), font.rawFont);
    if (!registry.downloadedFamilies.contains(font.family, Qt::CaseInsensitive)) {
        registry.downloadedFamilies.append(font.family);
        registry.downloadedFamilies.sort(Qt::CaseInsensitive);
    }
    return true;
}

void loadDownloadedFonts(FontRegistry &registry)
{
    const QString directory = ApplicationFonts::downloadedFontDirectory();
    if (directory.isEmpty())
        return;
    QDirIterator files(directory, QStringList{QStringLiteral("*.ttf")},
                       QDir::Files | QDir::NoSymLinks);
    qint64 scannedBytes = 0;
    int scannedFiles = 0;
    while (files.hasNext() && scannedFiles++ < kMaximumDownloadedFonts) {
        files.next();
        const QFileInfo info = files.fileInfo();
        if (info.size() <= 0 || info.size() > kMaximumSystemFontBytes
            || info.size() > kMaximumDownloadedFontBytes - scannedBytes)
            continue;
        scannedBytes += info.size();
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QByteArray data = file.read(kMaximumSystemFontBytes + 1);
        const QByteArray digest = QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
        if (info.completeBaseName().toLatin1() != digest)
            continue;
        ValidatedDownloadedFont font;
        QString error;
        if (validateDownloadedFont(data, font, error))
            registerDownloadedFont(registry, info.absoluteFilePath(), data, font);
    }
}

int fontLanguageIndex(ApplicationFonts::FontLanguage language)
{
    switch (language) {
    case ApplicationFonts::FontLanguage::Vietnamese:
        return 0;
    case ApplicationFonts::FontLanguage::English:
        return 1;
    case ApplicationFonts::FontLanguage::Chinese:
        return 2;
    case ApplicationFonts::FontLanguage::Japanese:
        return 3;
    case ApplicationFonts::FontLanguage::Korean:
        return 4;
    }
    Q_UNREACHABLE_RETURN(0);
}

bool bundledFaceIsSelectable(const FontRegistry &registry,
                             const QString &family,
                             bool bold,
                             bool italic)
{
    return std::any_of(
        registry.bundledFonts.cbegin(),
        registry.bundledFonts.cend(),
        [&family, bold, italic](const FontRegistry::BundledFont &font) {
            return font.family.compare(family, Qt::CaseInsensitive) == 0
                && font.bold == bold
                && font.italic == italic;
        });
}

bool systemFaceIsSelectable(FontRegistry &registry,
                            const QString &family,
                            bool bold,
                            bool italic)
{
    const QString key = fontFaceCacheKey(family, bold, italic);
    const auto cached = registry.selectableSystemFaces.constFind(key);
    if (cached != registry.selectableSystemFaces.cend()) {
        return cached.value();
    }

    bool selectable = false;
#ifdef Q_OS_MACOS
    const CfHandle<CTFontRef> font = exactSystemFont(family, bold, italic);
    ApplicationFonts::FontFaceData::Error error;
    selectable = font
        && isPotentiallyEmbeddableSystemFont(font.get(), error);
#else
    Q_UNUSED(family);
    Q_UNUSED(bold);
    Q_UNUSED(italic);
#endif
    registry.selectableSystemFaces.insert(key, selectable);
    return selectable;
}

bool fontFaceSupportsCharacters(FontRegistry &registry,
                                const QString &family,
                                bool bold,
                                bool italic,
                                const QVector<uint> &characters)
{
    const QString key = fontFaceCacheKey(family, bold, italic);
    if (bundledFaceIsSelectable(registry, family, bold, italic)) {
        auto rawFont = registry.bundledRawFonts.constFind(key);
        if (rawFont == registry.bundledRawFonts.cend()) {
            const QByteArray fontData = ApplicationFonts::bundledFontData(
                family,
                bold,
                italic);
            rawFont = registry.bundledRawFonts.insert(
                key,
                QRawFont(fontData, 16.0, QFont::PreferNoHinting));
        }
        if (!rawFont->isValid()) {
            return false;
        }
        return std::all_of(
            characters.cbegin(),
            characters.cend(),
            [&rawFont](uint character) {
                return rawFont->supportsCharacter(character);
            });
    }

    const auto downloaded = registry.downloadedRawFonts.constFind(key);
    if (downloaded != registry.downloadedRawFonts.cend()) {
        return std::all_of(characters.cbegin(), characters.cend(),
            [&downloaded](uint character) { return downloaded->supportsCharacter(character); });
    }

    const auto systemFamily = registry.systemFamilyByKey.constFind(
        family.toCaseFolded());
    if (systemFamily == registry.systemFamilyByKey.cend()
        || !systemFaceIsSelectable(
            registry,
            systemFamily.value(),
            bold,
            italic)) {
        return false;
    }
#ifdef Q_OS_MACOS
    auto characterSet = registry.systemCharacterSets.constFind(key);
    if (characterSet == registry.systemCharacterSets.cend()) {
        CFCharacterSetRef createdCharacterSet = copyExactSystemFontCharacterSet(
            systemFamily.value(),
            bold,
            italic);
        if (!createdCharacterSet) {
            return false;
        }
        characterSet = registry.systemCharacterSets.insert(
            key,
            createdCharacterSet);
    }
    return std::all_of(
        characters.cbegin(),
        characters.cend(),
        [&characterSet](uint character) {
            return CFCharacterSetIsLongCharacterMember(
                characterSet.value(),
                static_cast<UTF32Char>(character));
        });
#else
    return false;
#endif
}

} // namespace

namespace ApplicationFonts {

void registerBundledFonts()
{
    FontRegistry &registry = fontRegistry();
    if (registry.initialized) {
        return;
    }
    registry.initialized = true;
    registry.systemFamilies = QFontDatabase::families();
    for (const QString &family : std::as_const(registry.systemFamilies)) {
        const QString key = family.toCaseFolded();
        if (!registry.systemFamilyByKey.contains(key)) {
            registry.systemFamilyByKey.insert(key, family);
        }
    }

    QDirIterator fontFiles(
        QStringLiteral(":/trepdf/fonts"),
        QStringList{
            QStringLiteral("*.ttf"),
            QStringLiteral("*.TTF"),
            QStringLiteral("*.otf"),
            QStringLiteral("*.OTF"),
            QStringLiteral("*.ttc"),
            QStringLiteral("*.TTC")},
        QDir::Files,
        QDirIterator::Subdirectories);
    while (fontFiles.hasNext()) {
        const QString resourcePath = fontFiles.next();
        const int fontId = QFontDatabase::addApplicationFont(resourcePath);
        if (fontId < 0) {
            continue;
        }
        const QString lowerFileName = QFileInfo(resourcePath).fileName().toLower();
        const bool bold = lowerFileName.contains(QStringLiteral("bold"))
            || lowerFileName.contains(QStringLiteral("black"))
            || lowerFileName.contains(QStringLiteral("heavy"));
        const bool italic = lowerFileName.contains(QStringLiteral("italic"))
            || lowerFileName.contains(QStringLiteral("oblique"))
            || lowerFileName.contains(QStringLiteral("slanted"));
        for (const QString &family : QFontDatabase::applicationFontFamilies(fontId)) {
            if (!registry.bundledFamilies.contains(family, Qt::CaseInsensitive)) {
                registry.bundledFamilies.append(family);
            }
            registry.bundledFonts.append(FontRegistry::BundledFont{
                resourcePath,
                family,
                bold,
                italic});
        }
    }
    registry.bundledFamilies.sort(Qt::CaseInsensitive);
    if (fontRegistrationThread())
        loadDownloadedFonts(registry);
    QMutexLocker lock(&fontStretchCalibrationMutex());
    fontStretchCalibrationCache().clear();
}

const QStringList &bundledFontFamilies()
{
    registerBundledFonts();
    return fontRegistry().bundledFamilies;
}

const QStringList &systemFontFamilies()
{
    registerBundledFonts();
    return fontRegistry().systemFamilies;
}

bool isFontFaceSelectable(const QString &family, bool bold, bool italic)
{
    registerBundledFonts();
    FontRegistry &registry = fontRegistry();
    if (bundledFaceIsSelectable(registry, family, bold, italic)) {
        return true;
    }
    if (findDownloadedFont(registry, family, bold, italic))
        return true;
    const auto systemFamily = registry.systemFamilyByKey.constFind(
        family.toCaseFolded());
    return systemFamily != registry.systemFamilyByKey.cend()
        && systemFaceIsSelectable(
            registry,
            systemFamily.value(),
            bold,
            italic);
}

bool fontFaceSupportsText(const QString &family,
                          bool bold,
                          bool italic,
                          const QString &text)
{
    registerBundledFonts();
    FontRegistry &registry = fontRegistry();
    return fontFaceSupportsCharacters(
        registry,
        family,
        bold,
        italic,
        visibleCodePoints(text));
}

int calibratedFontStretch(const QString &family,
                          int targetStretch,
                          bool bold,
                          bool italic,
                          int pixelSize)
{
    const int boundedTarget = qBound(
        static_cast<int>(QFont::UltraCondensed),
        targetStretch,
        static_cast<int>(QFont::UltraExpanded));
    const int boundedPixelSize = qMax(1, pixelSize);
    if (boundedTarget == QFont::Unstretched) {
        return QFont::Unstretched;
    }

    const QString key = fontStretchCalibrationKey(
        family,
        boundedTarget,
        bold,
        italic,
        boundedPixelSize);
    {
        QMutexLocker lock(&fontStretchCalibrationMutex());
        const auto cached = fontStretchCalibrationCache().constFind(key);
        if (cached != fontStretchCalibrationCache().cend()) {
            return cached.value();
        }
    }

    const qreal normalAdvance = fontAdvanceForStretch(
        family,
        QFont::Unstretched,
        bold,
        italic,
        boundedPixelSize);
    int bestStretch = boundedTarget;
    if (qIsFinite(normalAdvance) && normalAdvance > 0.0) {
        const qreal targetRatio = boundedTarget
            / static_cast<qreal>(QFont::Unstretched);
        qreal bestError = std::numeric_limits<qreal>::max();
        for (int candidate = QFont::UltraCondensed;
             candidate <= QFont::UltraExpanded;
             ++candidate) {
            const qreal candidateAdvance = fontAdvanceForStretch(
                family,
                candidate,
                bold,
                italic,
                boundedPixelSize);
            if (!qIsFinite(candidateAdvance) || candidateAdvance <= 0.0) {
                continue;
            }

            const qreal error = qAbs(candidateAdvance / normalAdvance - targetRatio);
            if (error < bestError
                || (qFuzzyCompare(error, bestError)
                    && qAbs(candidate - boundedTarget)
                        < qAbs(bestStretch - boundedTarget))) {
                bestError = error;
                bestStretch = candidate;
            }
        }
    }

    {
        QMutexLocker lock(&fontStretchCalibrationMutex());
        fontStretchCalibrationCache().insert(key, bestStretch);
    }
    return bestStretch;
}

const QStringList &availableFontFamilies(bool bold, bool italic)
{
    registerBundledFonts();
    FontRegistry &registry = fontRegistry();
    const int styleIndex = fontStyleIndex(bold, italic);
    if (registry.availableFamiliesReady.at(styleIndex)) {
        return registry.availableFamilies.at(styleIndex);
    }

    QStringList &families = registry.availableFamilies.at(styleIndex);
    for (const QString &family : std::as_const(registry.bundledFamilies)) {
        if (bundledFaceIsSelectable(registry, family, bold, italic)) {
            families.append(family);
        }
    }
    for (const QString &family : std::as_const(registry.downloadedFamilies)) {
        if (!families.contains(family, Qt::CaseInsensitive)
            && findDownloadedFont(registry, family, bold, italic))
            families.append(family);
    }

    QStringList systemFamilies;
    for (const QString &family : std::as_const(registry.systemFamilies)) {
        if (!families.contains(family, Qt::CaseInsensitive)
            && systemFaceIsSelectable(registry, family, bold, italic)) {
            systemFamilies.append(family);
        }
    }
    std::sort(
        systemFamilies.begin(),
        systemFamilies.end(),
        [](const QString &left, const QString &right) {
            return left.compare(right, Qt::CaseInsensitive) < 0;
        });
    families.append(systemFamilies);
    registry.availableFamiliesReady.at(styleIndex) = true;
    return families;
}

const QStringList &fontFamiliesForLanguage(FontLanguage language,
                                           bool bold,
                                           bool italic)
{
    registerBundledFonts();
    FontRegistry &registry = fontRegistry();
    const int cacheIndex = fontStyleIndex(bold, italic) * kFontLanguageCount
        + fontLanguageIndex(language);
    if (registry.languageFamiliesReady.at(cacheIndex)) {
        return registry.languageFamilies.at(cacheIndex);
    }

    QStringList &families = registry.languageFamilies.at(cacheIndex);
    for (const QString &family : availableFontFamilies(bold, italic)) {
        bool matches = false;
        const QList<QFontDatabase::WritingSystem> writingSystems =
            QFontDatabase::writingSystems(family);
        switch (language) {
        case FontLanguage::Vietnamese:
            matches = fontFaceSupportsText(
                family,
                bold,
                italic,
                vietnameseCoverageText());
            break;
        case FontLanguage::English:
            matches = writingSystems.contains(QFontDatabase::Latin)
                && fontFaceSupportsText(
                    family,
                    bold,
                    italic,
                    englishCoverageText());
            break;
        case FontLanguage::Chinese: {
            const bool supportsSimplified = writingSystems.contains(
                QFontDatabase::SimplifiedChinese);
            const bool supportsTraditional = writingSystems.contains(
                QFontDatabase::TraditionalChinese);
            matches = (supportsSimplified
                       && fontFaceSupportsText(
                           family,
                           bold,
                           italic,
                           simplifiedChineseProbe()))
                || (supportsTraditional
                    && fontFaceSupportsText(
                        family,
                        bold,
                        italic,
                        traditionalChineseProbe()));
            break;
        }
        case FontLanguage::Japanese:
            matches = writingSystems.contains(QFontDatabase::Japanese)
                && fontFaceSupportsText(
                    family,
                    bold,
                    italic,
                    japaneseProbe());
            break;
        case FontLanguage::Korean:
            matches = writingSystems.contains(QFontDatabase::Korean)
                && fontFaceSupportsText(
                    family,
                    bold,
                    italic,
                    koreanProbe());
            break;
        }
        if (matches) {
            families.append(family);
        }
    }
    registry.languageFamiliesReady.at(cacheIndex) = true;
    return families;
}

QByteArray bundledFontData(const QString &family, bool bold, bool italic)
{
    registerBundledFonts();

    const FontRegistry::BundledFont *bestMatch = nullptr;
    int bestScore = -1;
    for (const FontRegistry::BundledFont &font : std::as_const(fontRegistry().bundledFonts)) {
        if (font.family.compare(family, Qt::CaseInsensitive) != 0) {
            continue;
        }

        const int score = (font.bold == bold ? 2 : 0)
            + (font.italic == italic ? 2 : 0);
        if (score > bestScore) {
            bestMatch = &font;
            bestScore = score;
        }
        if (score == 4) {
            break;
        }
    }
    if (!bestMatch || bestScore != 4) {
        return {};
    }

    QFile file(bestMatch->resourcePath);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

FontFaceData systemFontFace(const QString &family, bool bold, bool italic)
{
    registerBundledFonts();

    FontFaceData result;
    const auto systemFamily = fontRegistry().systemFamilyByKey.constFind(
        family.toCaseFolded());
    if (systemFamily != fontRegistry().systemFamilyByKey.cend()) {
        result.family = systemFamily.value();
    }
    if (result.family.isEmpty()) {
        return result;
    }

#ifdef Q_OS_MACOS
    const CfHandle<CTFontRef> font = exactSystemFont(
        result.family,
        bold,
        italic);
    if (!font) {
        return result;
    }

    result.data = materializeTrueTypeFont(font.get(), result.error);
#else
    Q_UNUSED(bold);
    Q_UNUSED(italic);
#endif
    return result;
}

QString downloadedFontDirectory()
{
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return root.isEmpty() ? QString() : QDir(root).filePath(QStringLiteral("fonts"));
}

const QStringList &downloadedFontFamilies()
{
    registerBundledFonts();
    return fontRegistry().downloadedFamilies;
}

FontFaceData downloadedFontFace(const QString &family, bool bold, bool italic)
{
    registerBundledFonts();
    FontFaceData result;
    const auto *font = findDownloadedFont(fontRegistry(), family, bold, italic);
    if (!font)
        return result;
    QFile file(font->path);
    if (!file.open(QIODevice::ReadOnly) || file.size() != font->size)
        return result;
    const QByteArray data = file.read(kMaximumSystemFontBytes + 1);
    if (data.size() != font->size
        || QCryptographicHash::hash(data, QCryptographicHash::Sha256) != font->digest)
        return result;
    result.data = data;
    result.family = font->family;
    result.error = FontFaceData::Error::None;
    return result;
}

bool installDownloadedFont(const QByteArray &data, const QString &family,
                           bool bold, bool italic, uint requiredCodePoint,
                           QString &error)
{
    error.clear();
    if (!fontRegistrationThread()) {
        error = QStringLiteral("Downloaded fonts must be registered on the application thread.");
        return false;
    }
    registerBundledFonts();
    ValidatedDownloadedFont font;
    if (!validateDownloadedFont(data, font, error))
        return false;
    if (font.family.compare(family, Qt::CaseInsensitive) != 0
        || font.bold != bold || font.italic != italic) {
        error = QStringLiteral("Downloaded font does not match the requested family and style.");
        return false;
    }
    if (requiredCodePoint != 0
        && (requiredCodePoint > 0x10ffffU
            || (requiredCodePoint >= 0xd800U && requiredCodePoint <= 0xdfffU)
            || !font.rawFont.supportsCharacter(requiredCodePoint))) {
        error = QStringLiteral("Downloaded font does not contain the required character.");
        return false;
    }
    FontRegistry &registry = fontRegistry();
    const QByteArray digest = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
    const auto *existing = findDownloadedFont(registry, font.family, bold, italic);
    if (existing) {
        if (existing->digest == digest && downloadedFontFace(family, bold, italic).isValid())
            return true;
        if (existing->digest != digest) {
            error = QStringLiteral("A different downloaded font already provides this exact face.");
            return false;
        }
    }
    const bool alreadyRegistered = existing != nullptr;
    qint64 totalBytes = alreadyRegistered ? 0 : data.size();
    for (const auto &registered : registry.downloadedFonts)
        totalBytes += registered.size;
    if ((!alreadyRegistered && registry.downloadedFonts.size() >= kMaximumDownloadedFonts)
        || totalBytes > kMaximumDownloadedFontBytes) {
        error = QStringLiteral("The private font storage limit has been reached.");
        return false;
    }
    const QString directory = downloadedFontDirectory();
    if (directory.isEmpty() || !QDir().mkpath(directory)) {
        error = QStringLiteral("Could not create the private font directory.");
        return false;
    }
    const QString path = QDir(directory).filePath(
        QString::fromLatin1(digest.toHex()) + QStringLiteral(".ttf"));
    QSaveFile output(path);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly) || output.write(data) != data.size()) {
        error = output.errorString();
        return false;
    }
    if (!alreadyRegistered && !registerDownloadedFont(registry, path, data, font)) {
        error = QStringLiteral("Could not register the private font for this application.");
        return false;
    }
    if (!output.commit()) {
        if (!alreadyRegistered) {
            const auto added = registry.downloadedFonts.takeLast();
            QFontDatabase::removeApplicationFont(added.registrationId);
            registry.downloadedRawFonts.remove(fontFaceCacheKey(font.family, bold, italic));
            if (!std::any_of(registry.downloadedFonts.cbegin(), registry.downloadedFonts.cend(),
                    [&font](const FontRegistry::DownloadedFont &item) { return item.family == font.family; }))
                registry.downloadedFamilies.removeAll(font.family);
        }
        error = output.errorString();
        return false;
    }
    clearFontSelectionCaches(registry);
    return true;
}

} // namespace ApplicationFonts
