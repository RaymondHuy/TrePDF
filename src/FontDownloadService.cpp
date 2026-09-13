#include "FontDownloadService.h"

#include "ApplicationFonts.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRawFont>
#include <QRegularExpression>
#include <QThread>
#include <QUrlQuery>

#include <unzip.h>
#include <QTemporaryFile>
#include <QScopeGuard>

#include <array>
#include <memory>

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
    m_deadline.setSingleShot(true);
    m_deadline.setInterval(60000);
    connect(&m_deadline, &QTimer::timeout, this, [this]() {
        //% "Font download timed out."
        complete(false, qtTrId("FontDownloadTimedOut"));
    });
}

FontDownloadService::~FontDownloadService()
{
    cancel();
}

void FontDownloadService::start(const QString &family, bool bold, bool italic,
                                uint requiredCodePoint)
{
    Q_ASSERT(thread() == QThread::currentThread());
    cancel();
    m_family = normalizedRequestedFamily(family);
    m_bold = bold;
    m_italic = italic;
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
    const QStringList aptosFamilies{
        QStringLiteral("Aptos"), QStringLiteral("Aptos Display"),
        QStringLiteral("Aptos Narrow"), QStringLiteral("Aptos Mono"),
        QStringLiteral("Aptos Serif")};
    for (const QString &aptosFamily : aptosFamilies) {
        if (m_family.compare(aptosFamily, Qt::CaseInsensitive) != 0)
            continue;
        m_family = aptosFamily;
        m_archiveEntry = aptosFamily;
        m_archiveEntry.replace(QLatin1Char(' '), QLatin1Char('-'));
        if (bold)
            m_archiveEntry += QStringLiteral("-Bold");
        if (italic)
            m_archiveEntry += QStringLiteral("-Italic");
        m_archiveEntry += QStringLiteral(".ttf");
        m_requireVietnamese = true;
        // The published Microsoft Download Center package contains complete TTF
        // faces. Read only the requested filename; archive paths are never extracted.
        request(QUrl(QString::fromLatin1(aptosDownloadUrl)), Stage::AptosArchive);
        return;
    }
    QUrl metadata(QStringLiteral("https://api.fontsource.org/v1/fonts"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("family"), m_family);
    metadata.setQuery(query);
    request(metadata, Stage::Metadata);
}

void FontDownloadService::cancel()
{
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

bool FontDownloadService::allowedUrl(const QUrl &url, Stage stage)
{
    if (!url.isValid() || url.scheme() != QLatin1String("https")
        || !url.userInfo().isEmpty() || url.port(-1) != -1 || url.hasFragment())
        return false;
    switch (stage) {
    case Stage::Metadata:
        return url.host() == QLatin1String("api.fontsource.org")
            && url.path() == QLatin1String("/v1/fonts");
    case Stage::Stylesheet:
        return url.host() == QLatin1String("fonts.googleapis.com")
            && url.path() == QLatin1String("/css");
    case Stage::Font:
        return url.host() == QLatin1String("fonts.gstatic.com")
            && url.path().startsWith(QLatin1String("/s/"))
            && url.path().endsWith(QLatin1String(".ttf")) && !url.hasQuery();
    case Stage::AptosArchive:
        return url.host() == QLatin1String("download.microsoft.com")
            && url.path().startsWith(QLatin1String("/download/"))
            && url.path().endsWith(QLatin1String(".zip")) && !url.hasQuery();
    }
    return false;
}

void FontDownloadService::request(const QUrl &url, Stage stage)
{
    if (!m_active)
        return;
    if (!m_network || !allowedUrl(url, stage)) {
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
    const QUrl resolvedRedirect = reply->url().resolved(redirect);
    const auto error = reply->error();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    m_reply.clear();
    disconnect(reply, nullptr, this, nullptr);
    reply->deleteLater();
    if (!redirect.isEmpty()) {
        if (++m_redirects > 3 || !allowedUrl(resolvedRedirect, m_stage)) {
            failUnavailable();
            return;
        }
        request(resolvedRedirect, m_stage);
        return;
    }
    if (error != QNetworkReply::NoError || status != 200) {
        //% "The font could not be downloaded. Check your connection and try again."
        complete(false, qtTrId("FontDownloadNetworkFailed"));
        return;
    }
    QByteArray data = std::move(m_data);
    switch (m_stage) {
    case Stage::Metadata: processMetadata(data); break;
    case Stage::Stylesheet: processStylesheet(data); break;
    case Stage::Font: installFont(data); break;
    case Stage::AptosArchive: processArchive(data); break;
    }
}

void FontDownloadService::processMetadata(const QByteArray &data)
{
    const QJsonDocument document = QJsonDocument::fromJson(data);
    const QJsonArray fonts = document.array();
    if (!document.isArray() || fonts.size() > 32) {
        failUnavailable();
        return;
    }
    for (const QJsonValue &value : fonts) {
        const QJsonObject font = value.toObject();
        const QString family = font.value(QStringLiteral("family")).toString();
        if (family.compare(m_family, Qt::CaseInsensitive) != 0
            || font.value(QStringLiteral("type")).toString() != QLatin1String("google")
            || !font.value(QStringLiteral("weights")).toArray().contains(m_bold ? 700 : 400)
            || !font.value(QStringLiteral("styles")).toArray().contains(
                m_italic ? QStringLiteral("italic") : QStringLiteral("normal")))
            continue;
        m_family = family;
        const QJsonArray subsets = font.value(QStringLiteral("subsets")).toArray();
        m_requireVietnamese = subsets.contains(QStringLiteral("vietnamese"));
        QStringList requestedSubsets;
        if (subsets.size() > 32) {
            failUnavailable();
            return;
        }
        for (const auto &subsetValue : subsets) {
            const QString subset = subsetValue.toString();
            if (!QRegularExpression(QStringLiteral("^[a-z][a-z0-9-]{0,39}$"))
                     .match(subset).hasMatch()) {
                failUnavailable();
                return;
            }
            requestedSubsets.append(subset);
        }
        QUrl url(QStringLiteral("https://fonts.googleapis.com/css"));
        QUrlQuery query;
        QString face = family + (m_bold ? QStringLiteral(":700") : QStringLiteral(":400"));
        if (m_italic)
            face += QStringLiteral("italic");
        query.addQueryItem(QStringLiteral("family"), face);
        query.addQueryItem(QStringLiteral("subset"), requestedSubsets.join(QLatin1Char(',')));
        url.setQuery(query);
        // A legacy UA requests a single complete static TTF, not disjoint WOFF2
        // web subsets. Never send text=; downloaded coverage is checked below.
        request(url, Stage::Stylesheet);
        return;
    }
    failUnavailable();
}

void FontDownloadService::processStylesheet(const QByteArray &data)
{
    const QString css = QString::fromUtf8(data);
    const QRegularExpression fontUrl(QStringLiteral("url\\(\\s*['\"]?(https://[^\\s)'\"]+)['\"]?\\s*\\)"));
    auto matches = fontUrl.globalMatch(css);
    if (!matches.hasNext()) {
        failUnavailable();
        return;
    }
    const QUrl url(matches.next().captured(1));
    // Multiple files are language subsets. Refuse to install just one subset.
    if (matches.hasNext() || css.contains(QLatin1String("unicode-range"), Qt::CaseInsensitive)
        || !allowedUrl(url, Stage::Font)) {
        failUnavailable();
        return;
    }
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
        if (QString::fromUtf8(name.data(), entry.size_filename) != m_archiveEntry)
            continue;
        if ((entry.flag & 1) || unzOpenCurrentFile(input) != UNZ_OK) {
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
        installFont(fontData);
        return;
    }
    failUnavailable();
}

void FontDownloadService::installFont(const QByteArray &data)
{
    if (!m_active)
        return;
    const QRawFont font(data, 16.0, QFont::PreferNoHinting);
    if (!font.isValid()) {
        failUnavailable();
        return;
    }
    for (uint character = 32; character <= 126; ++character) {
        if (!font.supportsCharacter(character)) {
            complete(false, incompleteFontMessage(m_family));
            return;
        }
    }
    if (m_requireVietnamese) {
        const QString accents = QStringLiteral(
            "ÀÁÂÃÈÉÊÌÍÒÓÔÕÙÚÝàáâãèéêìíòóôõùúýĂăĐđĨĩŨũƠơƯư");
        for (QChar character : accents) {
            if (!font.supportsCharacter(character.unicode())) {
                complete(false, incompleteFontMessage(m_family));
                return;
            }
        }
        for (uint character = 0x1ea0; character <= 0x1ef9; ++character) {
            if (!font.supportsCharacter(character)) {
                complete(false, incompleteFontMessage(m_family));
                return;
            }
        }
    }
    QString error;
    const bool success = ApplicationFonts::installDownloadedFont(
        data, m_family, m_bold, m_italic, m_requiredCodePoint, error);
    if (!success) {
        //% "The downloaded font could not be validated or stored in TrePDF's font folder."
        error = qtTrId("FontDownloadInstallFailed");
    }
    complete(success, error);
}

void FontDownloadService::failUnavailable()
{
    //% "A complete downloadable version of %1 in this style was not found."
    complete(false, qtTrId("FontDownloadUnavailable").arg(m_family));
}

void FontDownloadService::complete(bool success, const QString &error)
{
    if (!m_active)
        return;
    cancel();
    emit finished(success, error);
}
