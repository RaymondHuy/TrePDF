#include "ApplicationFonts.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QtGlobal>
#include <QVector>

#include <algorithm>
#include <cstdint>
#include <utility>

#ifdef Q_OS_MACOS
#include <CoreText/CoreText.h>
#endif

namespace {

constexpr qsizetype kMaximumSystemFontBytes = 32 * 1024 * 1024;

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

quint16 readBigEndian16(const QByteArray &data, qsizetype offset)
{
    return static_cast<quint16>(
        (static_cast<quint16>(static_cast<unsigned char>(data.at(offset))) << 8)
        | static_cast<unsigned char>(data.at(offset + 1)));
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

bool permitsEditableEmbedding(const QByteArray &os2Table)
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

QByteArray materializeTrueTypeFont(
    CTFontRef font,
    ApplicationFonts::FontFaceData::Error &error)
{
    error = ApplicationFonts::FontFaceData::Error::UnsupportedFormat;
    const CTFontSymbolicTraits traits = CTFontGetSymbolicTraits(font);
    if ((traits & (kCTFontTraitColorGlyphs | kCTFontTraitComposite)) != 0) {
        return {};
    }

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

    if (containsTable(tables, kTagFvar)
        || containsTable(tables, kTagCff)
        || containsTable(tables, kTagCff2)
        || !containsTable(tables, kTagGlyf)
        || !containsTable(tables, kTagLoca)
        || !containsTable(tables, kTagHead)) {
        return {};
    }

    const FontTable *os2Table = findTable(tables, kTagOs2);
    if (!os2Table || !permitsEditableEmbedding(os2Table->data)) {
        error = ApplicationFonts::FontFaceData::Error::EmbeddingRestricted;
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

    QStringList bundledFamilies;
    QStringList systemFamilies;
    QStringList availableFamilies;
    QVector<BundledFont> bundledFonts;
    bool initialized = false;
};

FontRegistry &fontRegistry()
{
    static FontRegistry registry;
    return registry;
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

    registry.availableFamilies = registry.bundledFamilies;
    for (const QString &family : std::as_const(registry.systemFamilies)) {
        if (!registry.availableFamilies.contains(family, Qt::CaseInsensitive)) {
            registry.availableFamilies.append(family);
        }
    }
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

const QStringList &availableFontFamilies()
{
    registerBundledFonts();
    return fontRegistry().availableFamilies;
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
    for (const QString &systemFamily : std::as_const(fontRegistry().systemFamilies)) {
        if (systemFamily.compare(family, Qt::CaseInsensitive) == 0) {
            result.family = systemFamily;
            break;
        }
    }
    if (result.family.isEmpty()) {
        return result;
    }

#ifdef Q_OS_MACOS
    const CfHandle<CFStringRef> familyName = toCfString(result.family);
    if (!familyName) {
        return result;
    }
    const CfHandle<CTFontRef> baseFont(CTFontCreateWithName(
        familyName.get(),
        12.0,
        nullptr));
    if (!baseFont) {
        return result;
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
    const CfHandle<CTFontRef> font(CTFontCreateCopyWithSymbolicTraits(
        baseFont.get(),
        12.0,
        nullptr,
        desiredTraits,
        styleMask));
    if (!font || (CTFontGetSymbolicTraits(font.get()) & styleMask) != desiredTraits) {
        return result;
    }

    const CfHandle<CFStringRef> resolvedFamily(CTFontCopyFamilyName(font.get()));
    const QString actualFamily = fromCfString(resolvedFamily.get());
    if (actualFamily.compare(result.family, Qt::CaseInsensitive) != 0) {
        return result;
    }
    result.family = actualFamily;

    result.data = materializeTrueTypeFont(font.get(), result.error);
#else
    Q_UNUSED(bold);
    Q_UNUSED(italic);
#endif
    return result;
}

} // namespace ApplicationFonts
