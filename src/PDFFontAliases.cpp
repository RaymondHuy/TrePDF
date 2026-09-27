#include "PDFFontAliases.h"

#include <QCryptographicHash>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QRawFont>
#include <QThread>
#include <QVector>

#include <algorithm>

namespace {
// Downloaded faces use the same alias mechanism as embedded PDF faces. The
// controller keeps its own tighter 32 MiB source-font registration budget.
constexpr qsizetype maximumDisplayBytes = 64 * 1024 * 1024;
constexpr qsizetype maximumSaveBytes = 64 * 1024 * 1024;
constexpr qsizetype maximumAliases = 2048;

quint16 read16(const QByteArray &data, qsizetype offset)
{
    return (quint16(quint8(data[offset])) << 8) | quint8(data[offset + 1]);
}

quint32 read32(const QByteArray &data, qsizetype offset)
{
    return (quint32(read16(data, offset)) << 16) | read16(data, offset + 2);
}

void append16(QByteArray &data, quint16 value)
{
    data.append(char(value >> 8));
    data.append(char(value));
}

void append32(QByteArray &data, quint32 value)
{
    append16(data, quint16(value >> 16));
    append16(data, quint16(value));
}

void write32(QByteArray &data, qsizetype offset, quint32 value)
{
    for (int byte = 0; byte < 4; ++byte)
        data[offset + byte] = char(value >> (24 - 8 * byte));
}

quint32 checksum(const QByteArray &data)
{
    quint32 result = 0;
    for (qsizetype offset = 0; offset < data.size(); offset += 4) {
        quint32 word = 0;
        for (int byte = 0; byte < 4; ++byte) {
            word <<= 8;
            if (offset + byte < data.size())
                word |= quint8(data[offset + byte]);
        }
        result += word;
    }
    return result;
}

struct AliasRecord {
    QString semanticFamily;
    QByteArray fingerprint;
    int references = 0;
    bool source = true;
    int registrationId = -1;
    bool bold = false;
    bool italic = false;
};

struct AliasRegistry {
    QMutex mutex;
    QHash<QString, AliasRecord> aliases;
    quint64 generation = 0;
};

AliasRegistry &registry()
{
    // Font requests can outlive other static Qt state during worker teardown.
    static AliasRegistry *value = new AliasRegistry;
    return *value;
}

QString withoutSubsetPrefix(QString family)
{
    if (family.size() > 7 && family[6] == QLatin1Char('+')
        && std::all_of(family.begin(), family.begin() + 6, [](QChar c) {
            return c >= QLatin1Char('A') && c <= QLatin1Char('Z');
        }))
        family.remove(0, 7);
    return family;
}
} // namespace

namespace PDFFontAliases {

static AliasData aliasIdentity(const QByteArray &data, const QString &expectedFamily,
                               bool substitute, const QRawFont &raw,
                               bool displayAlias = true)
{
    AliasData result;
    result.fingerprint = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
    result.source = !substitute;
    const QByteArray aliasFingerprint = substitute
        ? QCryptographicHash::hash(result.fingerprint + '\0' + expectedFamily.toUtf8(),
                                  QCryptographicHash::Sha256)
        : result.fingerprint;
    result.semanticFamily = withoutSubsetPrefix(substitute || raw.familyName().isEmpty()
        ? expectedFamily : raw.familyName());
    if (result.semanticFamily.isEmpty() || result.semanticFamily.size() > 256
        || result.semanticFamily.contains(QChar::Null))
        return {};
    if (displayAlias) {
        auto &state = registry();
        QMutexLocker lock(&state.mutex);
        for (auto entry = state.aliases.cbegin(); entry != state.aliases.cend(); ++entry) {
            if (entry->fingerprint == result.fingerprint
                && entry->semanticFamily == result.semanticFamily
                && entry->source == result.source) {
                result.family = entry.key();
                break;
            }
        }
        if (result.family.isEmpty()) {
            // A pooled Save thread has no event loop to invalidate its old Qt
            // font engine after the last owner unregisters a source face. A
            // new registration must never reuse that engine's family key.
            const QByteArray identity = aliasFingerprint + ':'
                + QByteArray::number(++state.generation);
            result.family = QStringLiteral("TrePDFE") + QString::fromLatin1(
                QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex().left(48));
        }
    }
    return result;
}

static AliasData renamedType1Font(const QByteArray &data, const QString &expectedFamily,
                                  bool substitute)
{
    // PFA has no sfnt name table. Tokenize only its bounded cleartext header;
    // leave encoding, metrics, charstrings and the encrypted tail byte-exact.
    // PFB and unusual executable name definitions keep the existing fallback.
    if (!data.startsWith("%!PS-AdobeFont-1.") && !data.startsWith("%!FontType1-"))
        return {};
    struct Token { qsizetype start; qsizetype length; int depth; };
    QVector<Token> tokens;
    constexpr qsizetype maximumHeader = 64 * 1024;
    const qsizetype limit = qMin(data.size(), maximumHeader);
    const auto whitespace = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
    };
    const auto delimiter = [&](char c) {
        return whitespace(c) || c == '(' || c == ')' || c == '<' || c == '>'
            || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' || c == '%';
    };
    const auto ascii = [&](char c) {
        return (quint8(c) >= 32 && quint8(c) <= 126) || whitespace(c);
    };
    const auto value = [&](const Token &token) {
        return data.mid(token.start, token.length);
    };
    bool foundProgram = false;
    QByteArray containers;
    for (qsizetype offset = 0; offset < limit && tokens.size() < 16384;) {
        const char c = data[offset];
        if (whitespace(c)) { ++offset; continue; }
        if (c == '%') {
            while (offset < limit && data[offset] != '\n' && data[offset] != '\r') {
                if (!ascii(data[offset]))
                    return {};
                ++offset;
            }
            continue;
        }
        if (quint8(c) < 32 || quint8(c) > 126)
            return {};
        const qsizetype start = offset++;
        const int containerDepth = containers.size();
        if (c == '(') {
            int depth = 1;
            while (offset < limit && depth > 0) {
                const char next = data[offset++];
                if (!ascii(next))
                    return {};
                if (next == '\\') {
                    if (offset >= limit || !ascii(data[offset]))
                        return {};
                    ++offset;
                } else if (next == '(') {
                    if (++depth > 16)
                        return {};
                } else if (next == ')') {
                    --depth;
                }
            }
            if (depth != 0)
                return {};
        } else if (c == '[' || c == '{') {
            if (containers.size() >= 16)
                return {};
            containers.append(c);
        } else if (c == ']' || c == '}') {
            if (containers.isEmpty() || containers.back() != (c == ']' ? '[' : '{'))
                return {};
            containers.chop(1);
        } else if (c == '/' || !delimiter(c)) {
            while (offset < limit && !delimiter(data[offset])) {
                if (!ascii(data[offset]))
                    return {};
                ++offset;
            }
            if (c == '/' && offset == start + 1)
                return {};
        } else {
            // Hex strings/dictionaries and unmatched string delimiters are
            // outside the supported declarative PFA header form.
            return {};
        }
        tokens.append({start, offset - start, containerDepth});
        const QByteArray tokenValue = value(tokens.last());
        // Supporting references or computed identity values would require
        // evaluating PostScript. Accept only the literal declarations below.
        if (tokenValue == "FontName" || tokenValue == "FamilyName" || tokenValue == "FullName")
            return {};
        if (tokenValue == "eexec") {
            if (!containers.isEmpty() || tokens.size() < 2
                || value(tokens[tokens.size() - 2]) != "currentfile"
                || offset >= data.size() || !whitespace(data[offset]))
                return {};
            foundProgram = true;
            break;
        }
    }
    if (!foundProgram)
        return {};
    QVector<Token> replacements;
    for (const QByteArray &key : {QByteArray("/FontName"), QByteArray("/FamilyName"),
                                 QByteArray("/FullName"), QByteArray("/FontType")}) {
        bool found = false;
        for (qsizetype i = 0; i < tokens.size(); ++i) {
            if (value(tokens[i]) != key)
                continue;
            if (found || tokens[i].depth != 0 || i + 2 >= tokens.size())
                return {};
            const Token name = tokens[i + 1];
            const QByteArray text = value(name);
            const bool fontType = key == "/FontType";
            if (fontType ? text != "1"
                : text.size() < 3 || (key == "/FontName" ? !text.startsWith('/')
                    : !(text.startsWith('(') && text.endsWith(')'))))
                return {};
            qsizetype end = i + 2;
            if (value(tokens[end]) == "readonly")
                ++end;
            if (end >= tokens.size() || value(tokens[end]) != "def")
                return {};
            if (!fontType)
                replacements.append(name);
            found = true;
        }
        if (!found)
            return {};
    }
    const QRawFont raw(data, 16.0, QFont::PreferNoHinting);
    if (!raw.isValid())
        return {};
    AliasData result = aliasIdentity(data, expectedFamily, substitute, raw);
    if (result.family.isEmpty())
        return {};
    std::sort(replacements.begin(), replacements.end(), [](const Token &a, const Token &b) {
        return a.start > b.start;
    });
    QByteArray renamed = data;
    const QByteArray family = result.family.toLatin1();
    for (const Token &name : replacements) {
        const QByteArray replacement = data[name.start] == '/' ? '/' + family
                                                              : '(' + family + ')';
        renamed.replace(name.start, name.length, replacement);
    }
    if (renamed.size() > maximumDisplayBytes)
        return {};
    const QRawFont check(renamed, 16.0, QFont::PreferNoHinting);
    if (!check.isValid() || check.familyName() != result.family
        || check.weight() != raw.weight() || check.style() != raw.style())
        return {};
    result.data = std::move(renamed);
    return result;
}

static AliasData renamedFont(const QByteArray &data, const QString &expectedFamily,
                             bool substitute, const QByteArray &saveIdentity = {})
{
    const qsizetype maximumBytes = saveIdentity.isEmpty()
        ? maximumDisplayBytes : maximumSaveBytes;
    if (data.size() < 12 || data.size() > maximumBytes)
        return {};
    const quint32 scaler = read32(data, 0);
    if (scaler != 0x00010000U && scaler != 0x74727565U && scaler != 0x4f54544fU)
        return saveIdentity.isEmpty() ? renamedType1Font(data, expectedFamily, substitute)
                                      : AliasData();
    if (!saveIdentity.isEmpty() && scaler == 0x4f54544fU)
        return {}; // CFF has a separate internal name; leave its program intact.
    const quint16 count = read16(data, 4);
    if (!count || count > 256 || 12 + count * 16 > data.size())
        return {};
    struct Table { quint32 tag; QByteArray data; quint32 offset = 0; };
    QVector<Table> tables;
    bool hasName = false;
    bool hasHead = false;
    qsizetype tableBytes = 0;
    for (int index = 0; index < count; ++index) {
        const qsizetype entry = 12 + index * 16;
        const quint32 tag = read32(data, entry);
        const quint32 offset = read32(data, entry + 8);
        const quint32 size = read32(data, entry + 12);
        if (offset < quint32(12 + count * 16)
            || quint64(offset) + size > quint64(data.size())
            || size > maximumBytes - tableBytes
            || std::any_of(tables.cbegin(), tables.cend(), [tag](const Table &t) {
                return t.tag == tag;
            }))
            return {};
        tableBytes += size;
        if (tag == 0x66766172U || tag == 0x43464632U) // fvar, CFF2
            return {};
        if (tag == 0x44534947U) // Renaming invalidates a digital signature.
            continue;
        if (tag == 0x6e616d65U)
            hasName = true;
        if (tag == 0x68656164U) {
            if (size < 54)
                return {};
            hasHead = true;
        }
        tables.append({tag, data.mid(offset, size)});
    }
    if (!hasName || !hasHead)
        return {};
    const QRawFont raw(data, 16.0, QFont::PreferNoHinting);
    if (!raw.isValid())
        return {};
    AliasData result = aliasIdentity(data, expectedFamily, substitute, raw,
                                     saveIdentity.isEmpty());
    if (result.semanticFamily.isEmpty())
        return {};
    const bool bold = raw.weight() >= QFont::Bold;
    const bool italic = raw.style() != QFont::StyleNormal;
    const QString style = bold ? (italic ? QStringLiteral("Bold Italic") : QStringLiteral("Bold"))
                               : (italic ? QStringLiteral("Italic") : QStringLiteral("Regular"));
    const QVector<QPair<quint16, QString>> names{
        {1, result.family}, {2, style}, {3, result.family}, {4, result.family},
        {6, result.family}, {16, result.family}, {17, style},
        {21, result.family}, {22, style}};
    QByteArray name;
    append16(name, 0);
    append16(name, quint16(names.size()));
    append16(name, quint16(6 + names.size() * 12));
    QByteArray strings;
    for (const auto &entry : names) {
        append16(name, 3); // Windows Unicode BMP, English (United States).
        append16(name, 1);
        append16(name, 0x0409);
        append16(name, entry.first);
        append16(name, quint16(entry.second.size() * 2));
        append16(name, quint16(strings.size()));
        for (QChar c : entry.second)
            append16(strings, c.unicode());
    }
    name += strings;
    if (!saveIdentity.isEmpty()) {
        result.family = raw.familyName();
        const auto sourceName = std::find_if(tables.cbegin(), tables.cend(), [](const Table &table) {
            return table.tag == 0x6e616d65U;
        });
        name = sourceName->data;
        if (name.size() < 6 || read16(name, 0) > 1)
            return {};
        const quint16 recordCount = read16(name, 2);
        const quint16 stringOffset = read16(name, 4);
        if (recordCount > 4096 || stringOffset < 6 + recordCount * 12
            || stringOffset > name.size())
            return {};
        const QByteArray styleSuffix = bold
            ? (italic ? QByteArrayLiteral("-BoldItalic") : QByteArrayLiteral("-Bold"))
            : (italic ? QByteArrayLiteral("-Italic") : QByteArrayLiteral("-Regular"));
        const QByteArray postScriptName = QByteArrayLiteral("TrePDFS")
            + QCryptographicHash::hash(saveIdentity, QCryptographicHash::Sha256).toHex().left(40)
            + styleSuffix;
        QByteArray unicodeName;
        for (char c : postScriptName)
            append16(unicodeName, quint8(c));
        struct NameRecord {
            qsizetype entry;
            quint16 id;
            quint16 platform;
            qsizetype offset;
            qsizetype length;
        };
        QVector<NameRecord> records;
        QVector<QPair<qsizetype, qsizetype>> protectedStrings;
        bool needsAscii = false;
        bool needsUnicode = false;
        const auto write16 = [](QByteArray &bytes, qsizetype offset, quint16 value) {
            bytes[offset] = char(value >> 8);
            bytes[offset + 1] = char(value);
        };
        for (int index = 0; index < recordCount; ++index) {
            const qsizetype entry = 6 + index * 12;
            const quint16 platform = read16(name, entry);
            const qsizetype length = read16(name, entry + 8);
            const qsizetype offset = read16(name, entry + 10);
            if (stringOffset + offset + length > name.size())
                return {};
            const quint16 id = read16(name, entry + 6);
            records.append({entry, id, platform, offset, length});
            if (id != 6) {
                protectedStrings.append({offset, length});
                continue;
            }
            if (platform != 0 && platform != 1 && platform != 3)
                return {};
            needsAscii |= platform == 1;
            needsUnicode |= platform != 1;
        }
        if (!needsAscii && !needsUnicode)
            return {};
        // Format 1 language tags also reference the string storage. Preserve them
        // even if a malformed font shares their bytes with a PostScript name.
        if (read16(name, 0) == 1) {
            const qsizetype tagsStart = 6 + recordCount * 12;
            if (tagsStart + 2 > stringOffset)
                return {};
            const quint16 tagCount = read16(name, tagsStart);
            if (tagCount > 4096 || tagsStart + 2 + tagCount * 4 > stringOffset)
                return {};
            for (int index = 0; index < tagCount; ++index) {
                const qsizetype entry = tagsStart + 2 + index * 4;
                const qsizetype length = read16(name, entry);
                const qsizetype offset = read16(name, entry + 2);
                if (stringOffset + offset + length > name.size())
                    return {};
                protectedStrings.append({offset, length});
            }
        }
        const auto reusableOffset = [&](bool unicode) -> qsizetype {
            const qsizetype width = unicode ? 2 : 1;
            for (const NameRecord &record : records) {
                if (record.id != 6 || (record.platform != 1) != unicode
                    || record.length != postScriptName.size() * width)
                    continue;
                const qsizetype start = stringOffset + record.offset;
                bool generatedName = true;
                for (qsizetype index = 0; index < postScriptName.size(); ++index) {
                    const char c = name[start + index * width + width - 1];
                    if ((unicode && name[start + index * width] != '\0')
                        || (index < 7 || index >= 47
                                ? c != postScriptName[index]
                                : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))) {
                        generatedName = false;
                        break;
                    }
                }
                if (!generatedName)
                    continue;
                const bool overlaps = std::any_of(protectedStrings.cbegin(),
                    protectedStrings.cend(), [&](const auto &range) {
                        return range.second > 0
                            && record.offset < range.first + range.second
                            && range.first < record.offset + record.length;
                    });
                if (!overlaps)
                    return record.offset;
            }
            return -1;
        };
        qsizetype asciiOffset = needsAscii ? reusableOffset(false) : -1;
        qsizetype unicodeOffset = needsUnicode ? reusableOffset(true) : -1;
        if (asciiOffset >= 0 && unicodeOffset >= 0
            && asciiOffset < unicodeOffset + unicodeName.size()
            && unicodeOffset < asciiOffset + postScriptName.size())
            unicodeOffset = -1;
        const auto installName = [&](const QByteArray &value, qsizetype &offset) {
            if (offset < 0) {
                offset = name.size() - stringOffset;
                if (offset + value.size() > 65535)
                    return false;
                name += value;
            } else {
                name.replace(stringOffset + offset, value.size(), value);
            }
            return true;
        };
        // Repeated saves overwrite only our fixed-size private name slots. Other
        // name records (including family, copyright and license) stay byte-exact.
        if ((needsAscii && !installName(postScriptName, asciiOffset))
            || (needsUnicode && !installName(unicodeName, unicodeOffset)))
            return {};
        for (const NameRecord &record : records) {
            if (record.id != 6)
                continue;
            const bool unicode = record.platform != 1;
            write16(name, record.entry + 8,
                    quint16(unicode ? unicodeName.size() : postScriptName.size()));
            write16(name, record.entry + 10, quint16(unicode ? unicodeOffset : asciiOffset));
        }
    }
    for (Table &table : tables) {
        if (table.tag == 0x6e616d65U)
            table.data = name;
        if (table.tag == 0x68656164U)
            write32(table.data, 8, 0);
    }
    std::sort(tables.begin(), tables.end(), [](const Table &a, const Table &b) {
        return a.tag < b.tag;
    });
    quint16 power = 1;
    quint16 selector = 0;
    while (power <= tables.size() / 2) { power *= 2; ++selector; }
    quint32 offset = quint32(12 + tables.size() * 16);
    for (Table &table : tables) {
        table.offset = offset;
        if (table.data.size() > maximumBytes - offset - 3)
            return {};
        offset += quint32((table.data.size() + 3) & ~3);
    }
    QByteArray renamed;
    renamed.reserve(offset);
    append32(renamed, scaler);
    append16(renamed, quint16(tables.size()));
    append16(renamed, power * 16);
    append16(renamed, selector);
    append16(renamed, quint16(tables.size() * 16 - power * 16));
    for (const Table &table : tables) {
        append32(renamed, table.tag);
        append32(renamed, checksum(table.data));
        append32(renamed, table.offset);
        append32(renamed, quint32(table.data.size()));
    }
    qsizetype headOffset = 0;
    for (const Table &table : tables) {
        if (table.tag == 0x68656164U)
            headOffset = table.offset;
        renamed += table.data;
        while (renamed.size() % 4)
            renamed.append('\0');
    }
    write32(renamed, headOffset + 8, 0xb1b0afbaU - checksum(renamed));
    const QRawFont check(renamed, 16.0, QFont::PreferNoHinting);
    if (!check.isValid() || check.familyName() != result.family)
        return {};
    result.data = std::move(renamed);
    return result;
}

AliasData isolate(const QByteArray &data, const QString &expectedFamily, bool substitute)
{
    return renamedFont(data, expectedFamily, substitute);
}

QByteArray withSaveResourceName(const QByteArray &data, const QByteArray &identity)
{
    if (identity.isEmpty())
        return {};
    return renamedFont(data, {}, false, identity).data;
}

bool retain(const AliasData &alias)
{
    Q_ASSERT(QThread::currentThread() == qGuiApp->thread());
    auto &state = registry();
    QMutexLocker lock(&state.mutex);
    auto found = state.aliases.find(alias.family);
    if (found != state.aliases.end()) {
        if (found->fingerprint != alias.fingerprint
            || found->semanticFamily != alias.semanticFamily
            || found->source != alias.source
            || found->registrationId < 0)
            return false;
        ++found->references;
        return true;
    }
    if (state.aliases.size() >= maximumAliases || alias.family.isEmpty()
        || alias.fingerprint.size() != 32)
        return false;
    const QRawFont raw(alias.data, 16.0, QFont::PreferNoHinting);
    if (!raw.isValid())
        return false;
    state.aliases.insert(alias.family, {alias.semanticFamily, alias.fingerprint, 1, alias.source,
                                      -1, raw.weight() >= QFont::Bold,
                                      raw.style() != QFont::StyleNormal});
    // Qt registration emits fontDatabaseChanged synchronously. Publish the
    // identity first, but refuse reentrant retains until registration finishes.
    // Never hold the metadata mutex while Qt calls back into the controllers.
    lock.unlock();
    const int id = QFontDatabase::addApplicationFontFromData(alias.data);
    const bool valid = id >= 0
        && QFontDatabase::applicationFontFamilies(id).contains(alias.family);
    lock.relock();
    if (valid) {
        state.aliases[alias.family].registrationId = id;
    } else {
        state.aliases.remove(alias.family);
    }
    lock.unlock();
    if (!valid && id >= 0)
        QFontDatabase::removeApplicationFont(id);
    return valid;
}

void release(const QString &family)
{
    Q_ASSERT(QThread::currentThread() == qGuiApp->thread());
    auto &state = registry();
    QMutexLocker lock(&state.mutex);
    auto found = state.aliases.find(family);
    if (found == state.aliases.end() || --found->references != 0)
        return;
    const int id = found->registrationId;
    state.aliases.erase(found);
    lock.unlock();
    if (id >= 0)
        QFontDatabase::removeApplicationFont(id);
}

QString semanticFamily(const QString &family)
{
    auto &state = registry();
    QMutexLocker lock(&state.mutex);
    const auto found = state.aliases.constFind(family);
    return found == state.aliases.cend() ? family : found->semanticFamily;
}

QByteArray sourceFingerprint(const QString &family)
{
    auto &state = registry();
    QMutexLocker lock(&state.mutex);
    const auto found = state.aliases.constFind(family);
    return found == state.aliases.cend() || !found->source ? QByteArray() : found->fingerprint;
}

bool matchesStyle(const QString &family, bool bold, bool italic)
{
    auto &state = registry();
    QMutexLocker lock(&state.mutex);
    const auto found = state.aliases.constFind(family);
    return found != state.aliases.cend() && found->registrationId >= 0
        && found->bold == bold && found->italic == italic;
}
} // namespace PDFFontAliases
