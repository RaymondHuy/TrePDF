#include "WindowsSystemFonts.h"

#include <QByteArrayView>
#include <QScopeGuard>
#include <QSet>

#include <algorithm>
#include <array>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <dwrite.h>

namespace {

constexpr quint64 kMaximumFontBytes = 64 * 1024 * 1024;
constexpr quint32 kMaximumTables = 4095;
constexpr quint32 kMaximumCollectionFaces = 4096;

template<typename T>
struct ComRelease
{
    void operator()(T *value) const
    {
        if (value)
            value->Release();
    }
};

template<typename T>
using ComPointer = std::unique_ptr<T, ComRelease<T>>;

IDWriteFactory *fontFactory()
{
    static const ComPointer<IDWriteFactory> factory = [] {
        IDWriteFactory *created = nullptr;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory), reinterpret_cast<IUnknown **>(&created)))) {
            return ComPointer<IDWriteFactory>();
        }
        return ComPointer<IDWriteFactory>(created);
    }();
    return factory.get();
}

quint16 read16(QByteArrayView bytes, qsizetype offset)
{
    return (static_cast<quint16>(static_cast<unsigned char>(bytes[offset])) << 8)
        | static_cast<unsigned char>(bytes[offset + 1]);
}

quint32 read32(QByteArrayView bytes, qsizetype offset)
{
    return (static_cast<quint32>(read16(bytes, offset)) << 16)
        | read16(bytes, offset + 2);
}

QByteArray readFragment(IDWriteFontFileStream *stream, quint64 fileSize,
                        quint64 offset, quint64 size)
{
    if (size == 0 || size > kMaximumFontBytes
        || offset > fileSize || size > fileSize - offset) {
        return {};
    }
    const void *data = nullptr;
    void *context = nullptr;
    if (FAILED(stream->ReadFileFragment(&data, offset, size, &context)))
        return {};
    const auto release = qScopeGuard([&] { stream->ReleaseFileFragment(context); });
    return data ? QByteArray(static_cast<const char *>(data), static_cast<qsizetype>(size))
                : QByteArray();
}

} // namespace
#endif

namespace WindowsSystemFonts {

struct Face::Data
{
#ifdef Q_OS_WIN
    ComPointer<IDWriteFontFace> face;
#endif
};

Face Face::resolve(const QString &family, bool bold, bool italic)
{
    Face result;
#ifdef Q_OS_WIN
    IDWriteFactory *factory = fontFactory();
    if (!factory || family.isEmpty() || family.contains(QChar(u'\0')))
        return result;

    IDWriteFontCollection *collectionValue = nullptr;
    if (FAILED(factory->GetSystemFontCollection(&collectionValue, FALSE)))
        return result;
    const ComPointer<IDWriteFontCollection> collection(collectionValue);
    UINT32 familyIndex = 0;
    BOOL exists = FALSE;
    if (FAILED(collection->FindFamilyName(
            reinterpret_cast<const WCHAR *>(family.utf16()), &familyIndex, &exists))
        || !exists) {
        return result;
    }
    IDWriteFontFamily *familyValue = nullptr;
    if (FAILED(collection->GetFontFamily(familyIndex, &familyValue)))
        return result;
    const ComPointer<IDWriteFontFamily> installedFamily(familyValue);
    IDWriteFont *fontValue = nullptr;
    if (FAILED(installedFamily->GetFirstMatchingFont(
            bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
            &fontValue))) {
        return result;
    }
    const ComPointer<IDWriteFont> font(fontValue);
    // Matching returns the nearest available style, including simulated styles.
    // Accept only a physical face with the requested semantic bold/italic flags.
    if (font->GetSimulations() != DWRITE_FONT_SIMULATIONS_NONE
        || (font->GetWeight() >= DWRITE_FONT_WEIGHT_BOLD) != bold
        || (font->GetStyle() != DWRITE_FONT_STYLE_NORMAL) != italic) {
        return result;
    }
    IDWriteFontFace *faceValue = nullptr;
    if (FAILED(font->CreateFontFace(&faceValue)))
        return result;
    ComPointer<IDWriteFontFace> face(faceValue);
    if (face->GetSimulations() != DWRITE_FONT_SIMULATIONS_NONE)
        return result;
    result.m_data = std::make_shared<Data>();
    result.m_data->face = std::move(face);
#else
    Q_UNUSED(family);
    Q_UNUSED(bold);
    Q_UNUSED(italic);
#endif
    return result;
}

bool Face::isValid() const
{
    return m_data != nullptr;
}

QVector<quint32> Face::tableTags(ApplicationFonts::FontFaceData::Error &error) const
{
    using Error = ApplicationFonts::FontFaceData::Error;
    error = Error::UnsupportedFormat;
#ifdef Q_OS_WIN
    if (!isValid())
        return {};
    IDWriteFontFace *face = m_data->face.get();
    UINT32 fileCount = 0;
    if (FAILED(face->GetFiles(&fileCount, nullptr)) || fileCount != 1)
        return {};
    IDWriteFontFile *fileValue = nullptr;
    if (FAILED(face->GetFiles(&fileCount, &fileValue)))
        return {};
    const ComPointer<IDWriteFontFile> file(fileValue);
    const void *referenceKey = nullptr;
    UINT32 referenceKeySize = 0;
    if (FAILED(file->GetReferenceKey(&referenceKey, &referenceKeySize)))
        return {};
    IDWriteFontFileLoader *loaderValue = nullptr;
    if (FAILED(file->GetLoader(&loaderValue)))
        return {};
    const ComPointer<IDWriteFontFileLoader> loader(loaderValue);
    IDWriteFontFileStream *streamValue = nullptr;
    if (FAILED(loader->CreateStreamFromKey(referenceKey, referenceKeySize, &streamValue)))
        return {};
    const ComPointer<IDWriteFontFileStream> stream(streamValue);
    UINT64 fileSize = 0;
    if (FAILED(stream->GetFileSize(&fileSize)))
        return {};

    QByteArray header = readFragment(stream.get(), fileSize, 0, 12);
    if (header.size() != 12)
        return {};
    quint64 sfntOffset = 0;
    if (read32(header, 0) == 0x74746366U) { // ttcf
        const quint32 version = read32(header, 4);
        const quint32 faceCount = read32(header, 8);
        if ((version != 0x00010000U && version != 0x00020000U)
            || faceCount == 0 || face->GetIndex() >= faceCount) {
            return {};
        }
        if (faceCount > kMaximumCollectionFaces) {
            error = Error::TooLarge;
            return {};
        }
        const quint64 offsetArrayBytes = static_cast<quint64>(faceCount) * 4;
        if (fileSize < 12 + offsetArrayBytes)
            return {};
        const QByteArray faceOffset = readFragment(stream.get(), fileSize,
            12 + static_cast<quint64>(face->GetIndex()) * 4, 4);
        if (faceOffset.size() != 4)
            return {};
        sfntOffset = read32(faceOffset, 0);
        if (sfntOffset < 12 + offsetArrayBytes)
            return {};
        header = readFragment(stream.get(), fileSize, sfntOffset, 12);
        if (header.size() != 12)
            return {};
    } else if (face->GetIndex() != 0) {
        return {};
    }
    const quint32 scaler = read32(header, 0);
    if (scaler != 0x00010000U && scaler != 0x74727565U) // TrueType or true
        return {};
    const quint32 tableCount = read16(header, 4);
    if (tableCount == 0)
        return {};
    if (tableCount > kMaximumTables) {
        error = Error::TooLarge;
        return {};
    }
    const quint64 directoryBytes = static_cast<quint64>(tableCount) * 16;
    const QByteArray directory = readFragment(
        stream.get(), fileSize, sfntOffset + 12, directoryBytes);
    if (static_cast<quint64>(directory.size()) != directoryBytes)
        return {};

    QVector<quint32> tags;
    tags.reserve(tableCount);
    QSet<quint32> seen;
    quint64 totalBytes = 12 + directoryBytes;
    for (quint32 index = 0; index < tableCount; ++index) {
        const qsizetype entry = static_cast<qsizetype>(index) * 16;
        const quint32 tag = read32(directory, entry);
        const quint64 offset = read32(directory, entry + 8);
        const quint64 length = read32(directory, entry + 12);
        if (seen.contains(tag) || offset > fileSize || length > fileSize - offset)
            return {};
        const quint64 paddedLength = (length + 3) & ~quint64(3);
        if (paddedLength > kMaximumFontBytes
            || totalBytes > kMaximumFontBytes - paddedLength) {
            error = Error::TooLarge;
            return {};
        }
        totalBytes += paddedLength;
        seen.insert(tag);
        tags.append(tag);
    }
    error = Error::None;
    return tags;
#else
    return {};
#endif
}

QByteArray Face::table(quint32 bigEndianTag) const
{
#ifdef Q_OS_WIN
    if (!isValid())
        return {};
    // DirectWrite packs its OpenType tags in the opposite byte order to sfnt.
    const UINT32 tag = ((bigEndianTag & 0xffU) << 24)
        | ((bigEndianTag & 0xff00U) << 8)
        | ((bigEndianTag >> 8) & 0xff00U)
        | ((bigEndianTag >> 24) & 0xffU);
    const void *data = nullptr;
    UINT32 size = 0;
    void *context = nullptr;
    BOOL exists = FALSE;
    IDWriteFontFace *face = m_data->face.get();
    if (FAILED(face->TryGetFontTable(tag, &data, &size, &context, &exists)))
        return {};
    if (!exists)
        return {};
    // A successful table can have a null context; it still must be released.
    const auto release = qScopeGuard([&] { face->ReleaseFontTable(context); });
    if (!data || size == 0 || size > kMaximumFontBytes)
        return {};
    return QByteArray(static_cast<const char *>(data), static_cast<qsizetype>(size));
#else
    Q_UNUSED(bigEndianTag);
    return {};
#endif
}

bool Face::supportsCharacters(const QVector<uint> &characters) const
{
#ifdef Q_OS_WIN
    if (!isValid())
        return false;
    constexpr qsizetype chunkSize = 256;
    std::array<UINT32, chunkSize> points = {};
    std::array<UINT16, chunkSize> glyphs = {};
    for (qsizetype offset = 0; offset < characters.size(); offset += chunkSize) {
        const qsizetype count = std::min(chunkSize, characters.size() - offset);
        for (qsizetype index = 0; index < count; ++index) {
            const uint character = characters[offset + index];
            if (character > 0x10ffffU || (character >= 0xd800U && character <= 0xdfffU))
                return false;
            points[static_cast<size_t>(index)] = character;
        }
        if (FAILED(m_data->face->GetGlyphIndices(
                points.data(), static_cast<UINT32>(count), glyphs.data()))) {
            return false;
        }
        for (qsizetype index = 0; index < count; ++index) {
            if (glyphs[static_cast<size_t>(index)] == 0)
                return false;
        }
    }
    return true;
#else
    Q_UNUSED(characters);
    return false;
#endif
}

} // namespace WindowsSystemFonts
