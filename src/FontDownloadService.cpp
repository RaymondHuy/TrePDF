#include "FontDownloadService.h"

#include "ApplicationFonts.h"
#include "GoogleFontsProvider.h"
#include "CtanFontProvider.h"

#include <QCoreApplication>
#include <QDebug>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRawFont>
#include <QRegularExpression>
#include <QThread>

#include <unzip.h>
#include <QTemporaryFile>
#include <QScopeGuard>

#include <array>
#include <algorithm>

namespace {
constexpr qint64 maximumFontBytes = 16 * 1024 * 1024;
constexpr auto aptosDownloadUrl =
    "https://download.microsoft.com/download/8/6/0/860a94fa-7feb-44ef-ac79-c072d9113d69/"
    "Microsoft%20Aptos%20Fonts.zip";

QString normalizedRequestedFamily(QString family)
{
    family = family.trimmed();
    if (family.size() > 7 && family.at(6) == QLatin1Char('+')) {
        bool subset = true;
        for (int i = 0; i < 6; ++i) {
            if (family.at(i) < QLatin1Char('A') || family.at(i) > QLatin1Char('Z')) {
                subset = false;
                break;
            }
        }
        if (subset)
            family.remove(0, 7);
    }
    return family;
}

QString incompleteFontMessage(const QString &family)
{
    //% "The downloaded %1 font does not contain all required characters."
    return qtTrId("FontDownloadIncompleteCoverage").arg(family);
}
}

FontDownloadService::FontDownloadService(QObject *parent, QNetworkAccessManager *network)
    : QObject(parent), m_network(network ? network : new QNetworkAccessManager(this))
{
    m_providers.emplace_back(std::make_unique<GoogleFontsProvider>());
    m_providers.emplace_back(std::make_unique<CtanFontProvider>());
    m_deadline.setSingleShot(true);
    m_deadline.setInterval(120000);
    connect(&m_deadline, &QTimer::timeout, this, [this]() {
        //% "Font download timed out."
        complete(false, qtTrId("FontDownloadTimedOut"));
    });
}

FontDownloadService::~FontDownloadService()
{
    cancel();
}

QString FontDownloadService::styleName(bool bold, bool italic)
{
    if (bold && italic) {
        //% "Bold Italic"
        return qtTrId("FontDownloadStyleBoldItalic");
    }
    if (bold) {
        //% "Bold"
        return qtTrId("FontDownloadStyleBold");
    }
    if (italic) {
        //% "Italic"
        return qtTrId("FontDownloadStyleItalic");
    }
    //% "Regular"
    return qtTrId("FontDownloadStyleRegular");
}

void FontDownloadService::start(const QString &family, bool bold, bool italic,
                                uint requiredCodePoint)
{
    Q_ASSERT(thread() == QThread::currentThread());
    cancel();
    m_family = normalizedRequestedFamily(family);
    m_bold = bold;
    m_italic = italic;
    m_errorBold = bold;
    m_errorItalic = italic;
    m_requiredCodePoint = requiredCodePoint;
    m_redirects = 0;
    m_requireVietnamese = false;
    m_active = true;
    if (!m_network || m_family.isEmpty() || m_family.size() > 128
        || m_family.contains(QRegularExpression(QStringLiteral("[\\x00-\\x1f\\x7f]")))
        || requiredCodePoint > 0x10ffffU
        || (requiredCodePoint >= 0xd800U && requiredCodePoint <= 0xdfffU)) {
        failUnavailable();
        return;
    }
    m_deadline.start();
    prepareFaces(true, true, true, true);
    if (hasCachedFace(Face{m_bold, m_italic, {}})) {
        complete(true, {});
        return;
    }
    const QStringList aptosFamilies{
        QStringLiteral("Aptos"), QStringLiteral("Aptos Display"),
        QStringLiteral("Aptos Narrow"), QStringLiteral("Aptos Mono"),
        QStringLiteral("Aptos Serif")};
    for (const QString &aptosFamily : aptosFamilies) {
        if (m_family.compare(aptosFamily, Qt::CaseInsensitive) != 0)
            continue;
        m_family = aptosFamily;
        m_requireVietnamese = true;
        // The published Microsoft Download Center package contains complete TTF
        // faces. Fetch it once for all four styles; never extract archive paths.
        request(QUrl(QString::fromLatin1(aptosDownloadUrl)), Stage::AptosArchive);
        return;
    }
    nextProvider();
}

void FontDownloadService::cancel()
{
    ++m_generation;
    m_active = false;
    m_deadline.stop();
    if (m_reply) {
        QNetworkReply *reply = m_reply;
        m_reply.clear();
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
    m_data.clear();
    m_faces.clear();
    m_faceIndex = 0;
    m_providerIndex = -1;
    m_providerFamily = {};
    m_fontSourceIndex = 0;
}

qint64 FontDownloadService::maximumBytes(Stage stage)
{
    switch (stage) {
    case Stage::Metadata: return 1024 * 1024;
    case Stage::Stylesheet: return 128 * 1024;
    case Stage::Font: return maximumFontBytes;
    case Stage::AptosArchive: return 8 * 1024 * 1024;
    }
    return 0;
}

IFontProvider::Resource FontDownloadService::resourceForStage(Stage stage)
{
    switch (stage) {
    case Stage::Metadata: return IFontProvider::Resource::Metadata;
    case Stage::Stylesheet: return IFontProvider::Resource::Stylesheet;
    default: return IFontProvider::Resource::Font;
    }
}

bool FontDownloadService::allowedUrl(const QUrl &url, Stage stage) const
{
    if (!IFontProvider::isSecureUrl(url))
        return false;
    if (stage == Stage::AptosArchive) {
        return url.host() == QLatin1String("download.microsoft.com")
            && url.path().startsWith(QLatin1String("/download/"))
            && url.path().endsWith(QLatin1String(".zip")) && !url.hasQuery();
    }
    return m_providerIndex >= 0 && size_t(m_providerIndex) < m_providers.size()
        && m_providers[size_t(m_providerIndex)]->allowsUrl(url, resourceForStage(stage));
}

void FontDownloadService::nextProvider()
{
    if (!m_active)
        return;
    ++m_providerIndex;
    m_redirects = 0;
    m_fontSourceIndex = 0;
    if (size_t(m_providerIndex) >= m_providers.size()) {
        // Leave the caller's existing display fallback and save draft intact.
        failUnavailable();
        return;
    }
    const auto url = m_providers[size_t(m_providerIndex)]->metadataUrl(m_family);
    if (url.isEmpty())
        processMetadata({});
    else
        request(url, Stage::Metadata);
}

void FontDownloadService::request(const QUrl &url, Stage stage, bool approvedRedirect)
{
    if (!m_active)
        return;
    if (!m_network || (!approvedRedirect && !allowedUrl(url, stage))) {
        failUnavailable();
        return;
    }
    m_stage = stage;
    m_data.clear();
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setRawHeader("User-Agent", "Mozilla/4.0");
    request.setRawHeader("Accept-Encoding", "identity");
    request.setTransferTimeout(15000);
    QNetworkReply *reply = m_network->get(request);
    m_reply = reply;
    reply->setReadBufferSize(256 * 1024);
    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() { receiveData(reply); });
    connect(reply, &QNetworkReply::finished, this, [this, reply]() { receiveFinished(reply); });
}

void FontDownloadService::receiveData(QNetworkReply *reply)
{
    if (!m_active || reply != m_reply)
        return;
    const qint64 maximum = maximumBytes(m_stage);
    if (reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > maximum
        || reply->bytesAvailable() > maximum - m_data.size()) {
        //% "The font download exceeds the allowed size."
        complete(false, qtTrId("FontDownloadTooLarge"));
        return;
    }
    m_data += reply->readAll();
}

void FontDownloadService::receiveFinished(QNetworkReply *reply)
{
    if (!m_active || reply != m_reply)
        return;
    receiveData(reply);
    if (!m_active || reply != m_reply)
        return;
    const QUrl redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    const QUrl replyUrl = reply->url();
    const QUrl resolvedRedirect = replyUrl.resolved(redirect);
    const auto error = reply->error();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    m_reply.clear();
    disconnect(reply, nullptr, this, nullptr);
    reply->deleteLater();
    if (!redirect.isEmpty()) {
        // Inspect the original Location before QUrl::resolved normalizes away
        // dot segments. Approved providers never need parent traversal.
        const QStringList segments = redirect.path().split(QLatin1Char('/'));
        const bool permitted = m_stage == Stage::AptosArchive
            ? allowedUrl(resolvedRedirect, m_stage)
            : m_providerIndex >= 0 && m_providers[size_t(m_providerIndex)]->allowsRedirect(
                replyUrl, resolvedRedirect, resourceForStage(m_stage));
        if (++m_redirects > 3 || status < 300 || status >= 400 || !permitted
            || segments.contains(QStringLiteral("."))
            || segments.contains(QStringLiteral(".."))) {
            failUnavailable();
            return;
        }
        request(resolvedRedirect, m_stage, true);
        return;
    }
    if (error != QNetworkReply::NoError || status != 200) {
        const bool transportFailure = error > QNetworkReply::NoError
            && error < QNetworkReply::ContentAccessDenied;
        const bool retryable = transportFailure || status == 0 || status == 403 || status == 404
            || status == 408 || status == 429 || (status >= 500 && status <= 599);
        if (retryable && retryFontFromMirror())
            return;
        //% "The font could not be downloaded. Check your connection and try again."
        complete(false, qtTrId("FontDownloadNetworkFailed"));
        return;
    }
    QByteArray data = std::move(m_data);
    switch (m_stage) {
    case Stage::Metadata: processMetadata(data); break;
    case Stage::Stylesheet: processStylesheet(data); break;
    case Stage::Font:
        if (m_faceIndex >= m_faces.size()) {
            failUnavailable();
            return;
        }
        if (stageFont(data, m_faces[m_faceIndex])) {
            ++m_faceIndex;
            requestNextFace();
        }
        break;
    case Stage::AptosArchive: processArchive(data); break;
    }
}

void FontDownloadService::processMetadata(const QByteArray &data)
{
    if (m_providerIndex < 0 || size_t(m_providerIndex) >= m_providers.size()) {
        failUnavailable();
        return;
    }
    m_providerFamily = m_providers[size_t(m_providerIndex)]->resolve(m_family, data);
    if (m_providerFamily.availability == IFontProvider::Availability::NotFound) {
        nextProvider();
        return;
    }
    const int required = (m_bold ? 2 : 0) + (m_italic ? 1 : 0);
    if (m_providerFamily.availability != IFontProvider::Availability::Found
        || m_providerFamily.license.isEmpty()) {
        failUnavailable();
        return;
    }
    m_family = m_providerFamily.name;
    if (!m_providerFamily.styles[required]) {
        QStringList available;
        for (const int style : {0, 2, 1, 3}) {
            if (m_providerFamily.styles[style])
                available.append(styleName(bool(style & 2), bool(style & 1)));
        }
        if (available.isEmpty()) {
            //% "The font source does not offer this style for this font."
            complete(false, qtTrId("FontDownloadStyleNotOffered"));
            return;
        }
        //% "The font source does not offer this style. Available styles: %1."
        complete(false, qtTrId("FontDownloadStyleUnavailable")
                            .arg(available.join(QStringLiteral(", "))));
        return;
    }
    m_requireVietnamese = m_providerFamily.vietnamese;
    const auto &styles = m_providerFamily.styles;
    prepareFaces(styles[0], styles[1], styles[2], styles[3]);
    requestNextFace();
}

void FontDownloadService::prepareFaces(bool regular, bool italic, bool bold, bool boldItalic)
{
    m_faces.clear();
    m_faceIndex = 0;
    const std::array<bool, 4> available{regular, italic, bold, boldItalic};
    const int required = (m_bold ? 2 : 0) + (m_italic ? 1 : 0);
    // Fail the save's required face early. The remaining family faces still
    // finish downloading and validation before any new font is installed.
    if (available[required])
        m_faces.append({m_bold, m_italic, {}});
    for (int index = 0; index < 4; ++index) {
        if (index != required && available[index])
            m_faces.append({bool(index & 2), bool(index & 1), {}});
    }
}

uint FontDownloadService::requiredCharacter(const Face &face) const
{
    return face.bold == m_bold && face.italic == m_italic ? m_requiredCodePoint : 0;
}

bool FontDownloadService::hasCachedFace(const Face &face) const
{
    const auto cached = ApplicationFonts::downloadedFontFace(m_family, face.bold, face.italic);
    return cached.isValid() && (!requiredCharacter(face)
        || QRawFont(cached.data, 16).supportsCharacter(requiredCharacter(face)));
}

void FontDownloadService::requestNextFace()
{
    if (!m_active)
        return;
    while (m_faceIndex < m_faces.size() && hasCachedFace(m_faces.at(m_faceIndex)))
        ++m_faceIndex;
    if (m_faceIndex == m_faces.size()) {
        // Yield before committing so an already queued Cancel can discard the
        // staged family. There is no installation between network requests.
        scheduleInstallation();
        return;
    }
    const Face &face = m_faces.at(m_faceIndex);
    m_errorBold = face.bold;
    m_errorItalic = face.italic;
    const auto &provider = m_providers[size_t(m_providerIndex)];
    const QUrl url = m_fontSourceIndex == 0
        ? provider->faceUrl(m_providerFamily, face.bold, face.italic)
        : provider->fallbackFaceUrls(m_providerFamily, face.bold, face.italic)
              .value(m_fontSourceIndex - 1);
    m_redirects = 0;
    request(url, provider->faceResource() == IFontProvider::Resource::Stylesheet
                     ? Stage::Stylesheet : Stage::Font);
}

bool FontDownloadService::retryFontFromMirror()
{
    if (!m_active || m_stage != Stage::Font || m_providerIndex < 0
        || size_t(m_providerIndex) >= m_providers.size() || m_faceIndex >= m_faces.size())
        return false;
    const auto &provider = m_providers[size_t(m_providerIndex)];
    if (provider->faceResource() != IFontProvider::Resource::Font)
        return false;
    const Face &face = m_faces.at(m_faceIndex);
    const auto mirrors = provider->fallbackFaceUrls(m_providerFamily, face.bold, face.italic);
    // At most two alternatives for the entire family, inside the original
    // deadline. A working mirror also serves the remaining styles. Nothing is
    // installed until all faces pass the normal validation and atomic commit.
    if (m_fontSourceIndex >= qMin(2, mirrors.size()))
        return false;
    ++m_fontSourceIndex;
    requestNextFace();
    return true;
}

void FontDownloadService::scheduleInstallation()
{
    const quint64 generation = m_generation;
    QTimer::singleShot(0, this, [this, generation]() {
        if (m_active && generation == m_generation)
            installFamily();
    });
}

void FontDownloadService::processStylesheet(const QByteArray &data)
{
    const QUrl url = m_providers[size_t(m_providerIndex)]->fontUrl(data);
    if (url.isEmpty()) {
        failUnavailable();
        return;
    }
    m_redirects = 0;
    request(url, Stage::Font);
}

void FontDownloadService::processArchive(const QByteArray &data)
{
    QTemporaryFile archiveFile;
    if (!archiveFile.open() || archiveFile.write(data) != data.size() || !archiveFile.flush()) {
        failUnavailable();
        return;
    }
    const QByteArray path = QFile::encodeName(archiveFile.fileName());
    unzFile input = unzOpen64(path.constData());
    if (!input) {
        failUnavailable();
        return;
    }
    const auto closeArchive = qScopeGuard([&]() { unzClose(input); });
    unz_global_info64 archiveInfo{};
    if (unzGetGlobalInfo64(input, &archiveInfo) != UNZ_OK || archiveInfo.number_entry > 128) {
        failUnavailable();
        return;
    }
    qint64 total = 0;
    QString baseName = m_family;
    baseName.replace(QLatin1Char(' '), QLatin1Char('-'));
    const QStringList names{baseName + QStringLiteral(".ttf"),
                            baseName + QStringLiteral("-Italic.ttf"),
                            baseName + QStringLiteral("-Bold.ttf"),
                            baseName + QStringLiteral("-Bold-Italic.ttf")};
    std::array<QByteArray, 4> fonts;
    std::array<bool, 4> found{};
    int status = unzGoToFirstFile(input);
    for (quint64 index = 0; index < archiveInfo.number_entry && status == UNZ_OK;
         ++index, status = unzGoToNextFile(input)) {
        unz_file_info64 entry{};
        std::array<char, 512> name{};
        if (unzGetCurrentFileInfo64(input, &entry, name.data(), name.size(),
                                   nullptr, 0, nullptr, 0) != UNZ_OK
            || entry.size_filename >= name.size()
            || entry.uncompressed_size > quint64(maximumFontBytes)
            || entry.uncompressed_size > quint64(64 * 1024 * 1024 - total)) {
            failUnavailable();
            return;
        }
        total += qint64(entry.uncompressed_size);
        const int faceIndex = names.indexOf(QString::fromUtf8(name.data(), entry.size_filename));
        if (faceIndex < 0)
            continue;
        if (found[faceIndex] || (entry.flag & 1) || unzOpenCurrentFile(input) != UNZ_OK) {
            failUnavailable();
            return;
        }
        QByteArray fontData;
        fontData.reserve(qsizetype(entry.uncompressed_size));
        std::array<char, 64 * 1024> buffer;
        bool valid = true;
        while (true) {
            const int size = unzReadCurrentFile(input, buffer.data(), buffer.size());
            if (size < 0 || size > maximumFontBytes - fontData.size()) {
                valid = false;
                break;
            }
            if (size == 0)
                break;
            fontData.append(buffer.data(), size);
        }
        valid = unzCloseCurrentFile(input) == UNZ_OK && valid
            && quint64(fontData.size()) == entry.uncompressed_size;
        if (!valid) {
            failUnavailable();
            return;
        }
        fonts[faceIndex] = std::move(fontData);
        found[faceIndex] = true;
    }
    const int required = (m_bold ? 2 : 0) + (m_italic ? 1 : 0);
    if (status != UNZ_END_OF_LIST_OF_FILE || !found[required]) {
        failUnavailable();
        return;
    }
    prepareFaces(found[0], found[1], found[2], found[3]);
    for (Face &face : m_faces) {
        const int index = (face.bold ? 2 : 0) + (face.italic ? 1 : 0);
        if (!hasCachedFace(face) && !stageFont(fonts[index], face))
            return;
    }
    scheduleInstallation();
}

bool FontDownloadService::stageFont(const QByteArray &data, Face &face)
{
    if (!m_active)
        return false;
    m_errorBold = face.bold;
    m_errorItalic = face.italic;
    QString error;
    if (!ApplicationFonts::validateDownloadedFontData(
            data, m_family, face.bold, face.italic, 0, error)) {
        qWarning().noquote() << "Font download validation failed for" << m_family << ':' << error;
        //% "The downloaded file is invalid, does not match the requested font style, or cannot be used for editing."
        complete(false, qtTrId("FontDownloadValidationFailed"));
        return false;
    }
    const QRawFont font(data, 16.0, QFont::PreferNoHinting);
    if (!font.isValid()) {
        //% "The downloaded file is invalid, does not match the requested font style, or cannot be used for editing."
        complete(false, qtTrId("FontDownloadValidationFailed"));
        return false;
    }
    if (requiredCharacter(face) && !font.supportsCharacter(requiredCharacter(face))) {
        complete(false, incompleteFontMessage(m_family));
        return false;
    }
    for (uint character = 32; character <= 126; ++character) {
        if (!font.supportsCharacter(character)) {
            complete(false, incompleteFontMessage(m_family));
            return false;
        }
    }
    if (m_requireVietnamese) {
        const QString accents = QStringLiteral(
            "ÀÁÂÃÈÉÊÌÍÒÓÔÕÙÚÝàáâãèéêìíòóôõùúýĂăĐđĨĩŨũƠơƯư");
        for (QChar character : accents) {
            if (!font.supportsCharacter(character.unicode())) {
                complete(false, incompleteFontMessage(m_family));
                return false;
            }
        }
        for (uint character = 0x1ea0; character <= 0x1ef9; ++character) {
            if (!font.supportsCharacter(character)) {
                complete(false, incompleteFontMessage(m_family));
                return false;
            }
        }
    }
    face.data = data;
    return true;
}

void FontDownloadService::installFamily()
{
    if (!m_active)
        return;
    if (m_faces.isEmpty()) {
        failUnavailable();
        return;
    }
    QList<ApplicationFonts::DownloadedFontData> pending;
    for (const Face &face : std::as_const(m_faces)) {
        m_errorBold = face.bold;
        m_errorItalic = face.italic;
        if (face.data.isEmpty()) {
            if (!hasCachedFace(face)) {
                failUnavailable();
                return;
            }
            continue;
        }
        pending.append({face.data, m_family, face.bold, face.italic, requiredCharacter(face)});
    }
    QString error;
    // Installation is atomic for the whole family; its failure cannot identify
    // a particular sibling face. Keep the user's requested style as context.
    m_errorBold = m_bold;
    m_errorItalic = m_italic;
    if (!pending.isEmpty() && !ApplicationFonts::installDownloadedFonts(pending, error)) {
        qWarning().noquote() << "Font download installation failed for" << m_family << ':' << error;
        //% "The downloaded font could not be validated or stored in TrePDF's font folder."
        complete(false, qtTrId("FontDownloadInstallFailed"));
        return;
    }
    complete(true, {});
}

void FontDownloadService::failUnavailable()
{
    //% "TrePDF could not find a complete downloadable copy of this font style in its font sources."
    complete(false, qtTrId("FontDownloadFaceUnavailable"));
}

void FontDownloadService::complete(bool success, const QString &error)
{
    if (!m_active)
        return;
    // Capture before cancel() clears the active download. Even connection and
    // validation failures must identify the exact face, not just its family.
    //% "Font: %1\nStyle: %2\n\n%3"
    const QString message = success ? error : qtTrId("FontDownloadFaceFailure")
        .arg(m_family, styleName(m_errorBold, m_errorItalic), error);
    cancel();
    emit finished(success, message);
}
