#include "UserFontManager.h"

#include "ApplicationFonts.h"
#include "FontDownloadService.h"
#include "PDFFontAliases.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRawFont>
#include <QtTranslation>

#include <algorithm>
#include <array>
#include <utility>

namespace {

QString readFontSnapshot(const QUrl &source, const QString &style, QByteArray &data)
{
    constexpr qint64 maximumFileBytes = 64 * 1024 * 1024;
    if (!source.isLocalFile()) {
        //% "Choose a font file on this device for %1."
        return qtTrId("UserFontLocalFileRequired").arg(style);
    }
    const QFileInfo info(source.toLocalFile());
    const QString suffix = info.suffix().toLower();
    if (suffix != QLatin1String("ttf") && suffix != QLatin1String("otf")) {
        //% "Choose a .ttf or .otf file for %1."
        return qtTrId("UserFontFileTypeRequired").arg(style);
    }
    if (!info.isFile() || !info.isReadable()) {
        //% "Cannot read %1. Choose an accessible font file for %2."
        return qtTrId("UserFontCannotRead").arg(info.fileName(), style);
    }
    if (info.size() < 12 || info.size() > maximumFileBytes) {
        //% "%1 is empty, invalid, or larger than 64 MiB. Choose another font file."
        return qtTrId("UserFontInvalidSize").arg(info.fileName());
    }
    QFile input(info.absoluteFilePath());
    if (!input.open(QIODevice::ReadOnly) || input.size() != info.size()) {
        //% "Cannot read %1. Choose an accessible font file for %2."
        return qtTrId("UserFontCannotRead").arg(info.fileName(), style);
    }
    data = input.read(info.size() + 1);
    if (data.size() != info.size() || input.error() != QFileDevice::NoError) {
        //% "Cannot read %1. Choose an accessible font file for %2."
        return qtTrId("UserFontCannotRead").arg(info.fileName(), style);
    }
    return {};
}

QString validateFontSnapshot(const QByteArray &data, const QString &fileName,
                             const QString &family, bool bold, bool italic)
{
    QString validationError;
    if (ApplicationFonts::validateDownloadedFontData(
            data, family, bold, italic, 0, validationError)) {
        return {};
    }
    qWarning().noquote() << "User font validation failed:" << validationError;
    const QString style = FontDownloadService::styleName(bold, italic);
    // The validator handles canonical family aliases and metadata. Use its
    // mismatch result rather than guessing from the filename.
    if (validationError == QLatin1String(
            "Downloaded font does not match the requested family and style.")) {
        const QRawFont raw(data, 16, QFont::PreferNoHinting);
        const QString actualStyle = FontDownloadService::styleName(
            raw.weight() >= QFont::Bold, raw.style() != QFont::StyleNormal);
        //% "%1 contains %2 (%3), but this slot requires %4 (%5). Check the family name or choose the matching file."
        return qtTrId("UserFontFaceMismatch")
            .arg(fileName, raw.familyName(), actualStyle, family, style);
    }
    //% "%1 cannot be used for %2. Choose a valid static TTF or OTF font that permits editable embedding in PDFs. Variable fonts, font collections and color fonts are not supported."
    return qtTrId("UserFontUnsupportedFile").arg(fileName, style);
}

} // namespace

UserFontManager::UserFontManager(QObject *parent)
    : QObject(parent)
{
}

UserFontManager::~UserFontManager()
{
    if (!m_previewFamily.isEmpty())
        PDFFontAliases::release(m_previewFamily);
}

QString UserFontManager::storageDirectory() const
{
    return QDir::toNativeSeparators(ApplicationFonts::userFontDirectory());
}

QString UserFontManager::fileName(const QUrl &source) const
{
    return source.isLocalFile() ? QFileInfo(source.toLocalFile()).fileName() : QString();
}

void UserFontManager::clearError()
{
    if (m_error.isEmpty())
        return;
    m_error.clear();
    emit errorChanged();
}

bool UserFontManager::fail(const QString &message)
{
    if (m_error != message) {
        m_error = message;
        emit errorChanged();
    }
    return false;
}

void UserFontManager::replacePreview(const QString &family, const QString &error,
                                     bool bold, bool italic)
{
    const QString previousFamily = m_previewFamily;
    QFont font;
    font.setPixelSize(24);
    if (!family.isEmpty()) {
        font.setFamily(family);
        font.setBold(bold);
        font.setItalic(italic);
        // Missing glyphs must not masquerade as coverage from this file.
        font.setStyleStrategy(QFont::NoFontMerging);
        font.setHintingPreference(QFont::PreferNoHinting);
    }
    const bool changed = m_previewFamily != family || m_previewError != error
        || m_previewFont != font;
    m_previewFamily = family;
    m_previewError = error;
    m_previewFont = font;
    if (changed)
        emit previewChanged();
    // Publish the new state before removal emits fontDatabaseChanged. Each
    // nonempty replacement already owns a retain, even for the same alias.
    if (!previousFamily.isEmpty())
        PDFFontAliases::release(previousFamily);
}

void UserFontManager::clearPreview()
{
    ++m_previewRevision;
    replacePreview({}, {});
}

bool UserFontManager::preparePreview(const QUrl &source, bool bold, bool italic)
{
    const quint64 revision = ++m_previewRevision;
    const QString style = FontDownloadService::styleName(bold, italic);
    if (source.isEmpty()) {
        //% "Choose a %1 font file to preview this style."
        replacePreview({}, qtTrId("UserFontPreviewMissing").arg(style));
        return false;
    }
    QByteArray data;
    QString error = readFontSnapshot(source, style, data);
    if (!error.isEmpty()) {
        replacePreview({}, error);
        return false;
    }
    const QRawFont raw(data, 16, QFont::PreferNoHinting);
    error = validateFontSnapshot(data, fileName(source), raw.familyName(), bold, italic);
    if (!error.isEmpty()) {
        replacePreview({}, error);
        return false;
    }

    // Keep previews isolated from installed fonts with the same family name.
    // Only the selected snapshot is registered, without writing to user-font.
    const auto alias = PDFFontAliases::isolate(data, raw.familyName(), true);
    const bool retained = !alias.family.isEmpty() && PDFFontAliases::retain(alias);
    // Qt font registration can synchronously trigger application callbacks.
    // A newer request or cancellation takes precedence over this result.
    if (m_previewRevision != revision) {
        if (retained)
            PDFFontAliases::release(alias.family);
        return false;
    }
    if (!retained) {
        //% "Could not prepare this font for preview. Choose another file or try again."
        replacePreview({}, qtTrId("UserFontPreviewFailed"));
        return false;
    }
    replacePreview(alias.family, {}, bold, italic);
    return m_previewFamily == alias.family && m_previewError.isEmpty();
}

bool UserFontManager::importFonts(const QString &family, const QUrl &regular,
                                 const QUrl &bold, const QUrl &italic,
                                 const QUrl &boldItalic)
{
    clearError();
    const QString name = family.trimmed();
    if (name.isEmpty() || name.size() > 256
        || std::any_of(name.cbegin(), name.cend(), [](QChar c) {
            return c.category() == QChar::Other_Control
                || c == QChar::LineSeparator || c == QChar::ParagraphSeparator;
        })) {
        //% "Enter the font's family name, using at most 256 characters."
        return fail(qtTrId("UserFontInvalidName"));
    }

    // Keep one bounded snapshot per slot; validation and installation use the
    // same bytes even if the selected source changes while the form is open.
    const std::array<QUrl, 4> sources = {regular, bold, italic, boldItalic};
    QList<ApplicationFonts::DownloadedFontData> fonts;
    for (size_t index = 0; index < sources.size(); ++index) {
        const QUrl &source = sources[index];
        if (source.isEmpty())
            continue;
        const bool isBold = index == 1 || index == 3;
        const bool isItalic = index >= 2;
        const QString style = FontDownloadService::styleName(isBold, isItalic);
        QByteArray data;
        QString error = readFontSnapshot(source, style, data);
        if (!error.isEmpty())
            return fail(error);
        error = validateFontSnapshot(data, fileName(source), name, isBold, isItalic);
        if (!error.isEmpty())
            return fail(error);
        const auto existing = ApplicationFonts::downloadedFontFace(name, isBold, isItalic);
        if (existing.isValid() && existing.data != data) {
            //% "A different version of %1 (%2) is already in TrePDF's font library. It has not been replaced."
            return fail(qtTrId("UserFontFaceConflict").arg(name, style));
        }
        fonts.append({std::move(data), name, isBold, isItalic, 0});
    }
    if (fonts.isEmpty()) {
        //% "Choose at least one font file to add."
        return fail(qtTrId("UserFontFilesRequired"));
    }

    QString installError;
    if (!ApplicationFonts::installUserFonts(fonts, installError)) {
        qWarning().noquote() << "User font import failed:" << installError;
        if (installError == QLatin1String("The private font storage limit has been reached.")) {
            //% "TrePDF's font library is full (128 files or 256 MiB). No fonts were added."
            return fail(qtTrId("UserFontStorageFull"));
        }
        //% "Could not add these fonts to %1. Check that the folder is writable and has enough free space, then try again."
        return fail(qtTrId("UserFontInstallFailed").arg(storageDirectory()));
    }
    emit fontsChanged();
    return true;
}
