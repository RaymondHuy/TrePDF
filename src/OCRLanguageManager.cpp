#include "OCRLanguageManager.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRunnable>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QThreadPool>
#include <QUrl>

#include <algorithm>

static void initializeOCRCatalogResource()
{
    Q_INIT_RESOURCE(trepdf_ocr_languages_catalog);
}

namespace {
constexpr char revision[] = "87416418657359cb625c412a48b6e1d6d41c29bd";
constexpr qint64 maximumModelBytes = 32 * 1024 * 1024;
constexpr qint64 maximumCatalogBytes = 256 * 1024 * 1024;
constexpr qint64 chunkBytes = 64 * 1024;

QThreadPool &verificationPool()
{
    static QThreadPool pool;
    static const bool configured = []() {
        pool.setMaxThreadCount(1);
        pool.setExpiryTimeout(30000);
        return true;
    }();
    Q_UNUSED(configured);
    return pool;
}

QString localizedName(const QString &code)
{
    if (code == QStringLiteral("eng")) {
        //% "English"
        return qtTrId("OCRLanguageEnglish");
    }
    if (code == QStringLiteral("vie")) {
        //% "Vietnamese"
        return qtTrId("OCRLanguageVietnamese");
    }
    if (code == QStringLiteral("tha")) {
        //% "Thai"
        return qtTrId("OCRLanguageThai");
    }
    if (code == QStringLiteral("fra")) {
        //% "French"
        return qtTrId("OCRLanguageFrench");
    }
    if (code == QStringLiteral("hrv")) {
        //% "Croatian"
        return qtTrId("OCRLanguageCroatian");
    }
    if (code == QStringLiteral("deu")) {
        //% "German"
        return qtTrId("OCRLanguageGerman");
    }
    if (code == QStringLiteral("spa")) {
        //% "Spanish"
        return qtTrId("OCRLanguageSpanish");
    }
    if (code == QStringLiteral("ita")) {
        //% "Italian"
        return qtTrId("OCRLanguageItalian");
    }
    if (code == QStringLiteral("por")) {
        //% "Portuguese"
        return qtTrId("OCRLanguagePortuguese");
    }
    if (code == QStringLiteral("chi_sim")) {
        //% "Chinese (Simplified)"
        return qtTrId("OCRLanguageChineseSimplified");
    }
    if (code == QStringLiteral("chi_tra")) {
        //% "Chinese (Traditional)"
        return qtTrId("OCRLanguageChineseTraditional");
    }
    if (code == QStringLiteral("jpn")) {
        //% "Japanese"
        return qtTrId("OCRLanguageJapanese");
    }
    if (code == QStringLiteral("kor")) {
        //% "Korean"
        return qtTrId("OCRLanguageKorean");
    }
    if (code == QStringLiteral("rus")) {
        //% "Russian"
        return qtTrId("OCRLanguageRussian");
    }
    if (code == QStringLiteral("ara")) {
        //% "Arabic"
        return qtTrId("OCRLanguageArabic");
    }
    if (code == QStringLiteral("hin")) {
        //% "Hindi"
        return qtTrId("OCRLanguageHindi");
    }
    if (code == QStringLiteral("ind")) {
        //% "Indonesian"
        return qtTrId("OCRLanguageIndonesian");
    }
    if (code == QStringLiteral("msa")) {
        //% "Malay"
        return qtTrId("OCRLanguageMalay");
    }
    if (code == QStringLiteral("nld")) {
        //% "Dutch"
        return qtTrId("OCRLanguageDutch");
    }
    if (code == QStringLiteral("pol")) {
        //% "Polish"
        return qtTrId("OCRLanguagePolish");
    }
    if (code == QStringLiteral("tur")) {
        //% "Turkish"
        return qtTrId("OCRLanguageTurkish");
    }
    if (code == QStringLiteral("ukr")) {
        //% "Ukrainian"
        return qtTrId("OCRLanguageUkrainian");
    }
    if (code == QStringLiteral("ces")) {
        //% "Czech"
        return qtTrId("OCRLanguageCzech");
    }
    if (code == QStringLiteral("swe")) {
        //% "Swedish"
        return qtTrId("OCRLanguageSwedish");
    }
    if (code == QStringLiteral("ron")) {
        //% "Romanian"
        return qtTrId("OCRLanguageRomanian");
    }
    return code;
}

QByteArray catalogData()
{
    initializeOCRCatalogResource();
    QFile resource(QStringLiteral(":/ocr/ocr-languages.json"));
    return resource.open(QIODevice::ReadOnly) ? resource.readAll() : QByteArray();
}
}

OCRLanguageManager::OCRLanguageManager(QObject *parent)
    : OCRLanguageManager(nullptr, cacheDirectory(), bundledDirectory(), catalogData(), parent)
{
}

OCRLanguageManager::OCRLanguageManager(QNetworkAccessManager *network, const QString &cache,
                                     const QString &bundled, const QByteArray &data,
                                     QObject *parent)
    : QObject(parent), m_models(parseCatalog(data)), m_cacheDirectory(cache),
      m_bundledDirectory(bundled),
      m_network(network ? network : new QNetworkAccessManager(this))
{
    m_deadline.setSingleShot(true);
    m_deadline.setInterval(120000);
    connect(&m_deadline, &QTimer::timeout, this, [this]() {
        //% "The language download took too long. Check your connection and try again."
        fail(qtTrId("OCRLanguageDownloadTimedOut"));
    });
    connect(qApp, &QCoreApplication::aboutToQuit, this, &OCRLanguageManager::cancel);
    // Defer until QML/test clients can connect to changed() and inspect initial
    // state. Refresh performs local verification only, never a download.
    QTimer::singleShot(0, this, &OCRLanguageManager::refresh);
}

OCRLanguageManager::~OCRLanguageManager()
{
    // Worker snapshots contain no manager-owned buffers. Invalidating this token
    // makes them stop at the next 64-KiB read; QPointer protects queued completion.
    if (m_cancellation)
        m_cancellation->store(true);
    stopTransfer();
}

QList<OCRLanguageManager::Model> OCRLanguageManager::parseCatalog(const QByteArray &data)
{
    if (data.size() > 64 * 1024)
        return {};
    const QJsonObject root = QJsonDocument::fromJson(data).object();
    if (root.value(QStringLiteral("revision")).toString() != QString::fromLatin1(revision))
        return {};
    const QJsonArray entries = root.value(QStringLiteral("models")).toArray();
    if (entries.isEmpty() || entries.size() > 32)
        return {};
    QList<Model> models;
    QSet<QString> codes;
    qint64 total = 0;
    for (const QJsonValue &entry : entries) {
        const QJsonObject value = entry.toObject();
        Model model;
        model.code = value.value(QStringLiteral("code")).toString();
        model.name = value.value(QStringLiteral("name")).toString();
        model.nativeName = value.value(QStringLiteral("nativeName")).toString();
        model.bundled = value.value(QStringLiteral("bundled")).toBool();
        model.bytes = value.value(QStringLiteral("bytes")).toInteger();
        const QByteArray hex = value.value(QStringLiteral("sha256")).toString().toLatin1();
        model.sha256 = QByteArray::fromHex(hex);
        const bool safeCode = !model.code.isEmpty() && model.code.size() <= 12
            && std::all_of(model.code.cbegin(), model.code.cend(), [](QChar c) {
                return (c >= QLatin1Char('a') && c <= QLatin1Char('z')) || c == QLatin1Char('_');
            });
        if (!safeCode || codes.contains(model.code) || model.name.isEmpty()
            || model.name.size() > 80 || model.nativeName.isEmpty() || model.nativeName.size() > 80
            || model.bytes <= 0 || model.bytes > maximumModelBytes
            || hex.size() != 64 || model.sha256.size() != 32 || model.sha256.toHex() != hex
            || model.bundled != (model.code == QStringLiteral("eng")
                                  || model.code == QStringLiteral("vie")))
            return {};
        codes.insert(model.code);
        total += model.bytes;
        if (total > maximumCatalogBytes)
            return {};
        models.append(model);
    }
    return models;
}

const QList<OCRLanguageManager::Model> &OCRLanguageManager::catalog()
{
    static const QList<Model> models = parseCatalog(catalogData());
    return models;
}

QString OCRLanguageManager::bundledDirectory()
{
    const QDir application(QCoreApplication::applicationDirPath());
    QStringList candidates{application.filePath(QStringLiteral("../Resources/tessdata")),
                           application.filePath(QStringLiteral("tessdata"))};
#ifdef TREPDF_OCR_TESSDATA_DIR
    candidates.append(QString::fromUtf8(TREPDF_OCR_TESSDATA_DIR));
#endif
    for (const QString &candidate : candidates) {
        if (QDir(candidate).exists())
            return QDir(candidate).absolutePath();
    }
    return {};
}

QString OCRLanguageManager::cacheDirectory()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return base.isEmpty() ? QString() : QDir(base).filePath(
        QStringLiteral("ocr/tessdata-fast-%1").arg(QString::fromLatin1(revision)));
}

QString OCRLanguageManager::modelPath(const Model &model, const QString &cache,
                                      const QString &bundled)
{
    const QString directory = model.bundled ? bundled : cache;
    if (directory.isEmpty() || QFileInfo(directory).isSymLink())
        return {};
    return QDir(directory).filePath(model.code + QStringLiteral(".traineddata"));
}

bool OCRLanguageManager::hasExpectedFile(const QString &path, qint64 bytes)
{
    const QFileInfo info(path);
    return !path.isEmpty() && info.isFile() && !info.isSymLink() && info.isReadable()
        && info.size() == bytes;
}

QString OCRLanguageManager::installedModelPath(const QString &code)
{
    for (const Model &model : catalog()) {
        if (model.code == code) {
            const QString path = modelPath(model, cacheDirectory(), bundledDirectory());
            return hasExpectedFile(path, model.bytes) ? path : QString();
        }
    }
    return {};
}

QStringList OCRLanguageManager::supportedCodes()
{
    QStringList result;
    for (const Model &model : catalog())
        result.append(model.code);
    return result;
}

qint64 OCRLanguageManager::modelBytes(const QString &code)
{
    for (const Model &model : catalog()) {
        if (model.code == code)
            return model.bytes;
    }
    return 0;
}

QByteArray OCRLanguageManager::modelSha256(const QString &code)
{
    for (const Model &model : catalog()) {
        if (model.code == code)
            return model.sha256;
    }
    return {};
}

bool OCRLanguageManager::validSelection(const QString &languageExpression)
{
    if (languageExpression.size() > 64)
        return false;
    const QStringList codes = languageExpression.split(QLatin1Char('+'));
    if (codes.isEmpty() || codes.size() > 4)
        return false;
    const QStringList supported = supportedCodes();
    QSet<QString> seen;
    for (const QString &code : codes) {
        if (!supported.contains(code) || seen.contains(code))
            return false;
        seen.insert(code);
    }
    return true;
}

bool OCRLanguageManager::selectionAllowed(const QStringList &codes) const
{
    if (codes.isEmpty() || codes.size() > 4)
        return false;
    QSet<QString> seen;
    for (const QString &code : codes) {
        const bool found = std::any_of(m_models.cbegin(), m_models.cend(), [&code](const Model &model) {
            return model.code == code;
        });
        if (!found || seen.contains(code))
            return false;
        seen.insert(code);
    }
    return true;
}

QVariantList OCRLanguageManager::languages() const
{
    QVariantList result;
    for (const Model &model : m_models) {
        result.append(QVariantMap{{QStringLiteral("code"), model.code},
                                  {QStringLiteral("name"), localizedName(model.code)},
                                  {QStringLiteral("nativeName"), model.nativeName},
                                  {QStringLiteral("bundled"), model.bundled},
                                  {QStringLiteral("installed"), m_installed.contains(model.code)},
                                  {QStringLiteral("bytes"), model.bytes}});
    }
    return result;
}

bool OCRLanguageManager::prepare(const QStringList &codes)
{
    Q_ASSERT(thread() == QThread::currentThread());
    if (m_busy)
        return false;
    if (!selectionAllowed(codes)) {
        //% "Choose between one and four languages used in this document."
        fail(qtTrId("OCRLanguageSelectionInvalid"));
        return false;
    }
    m_selection = codes;
    return verify(codes, true);
}

void OCRLanguageManager::refresh()
{
    Q_ASSERT(thread() == QThread::currentThread());
    if (m_busy)
        return;
    QStringList codes;
    for (const Model &model : m_models)
        codes.append(model.code);
    if (codes.isEmpty()) {
        //% "The language list could not be loaded. Reinstall TrePDF and try again."
        fail(qtTrId("OCRLanguageCatalogUnavailable"));
        return;
    }
    verify(codes, false);
}

bool OCRLanguageManager::verify(const QStringList &codes, bool prepareAfterVerification)
{
    m_busy = true;
    m_errorText.clear();
    m_progress = 0;
    //% "Checking language files…"
    m_statusText = qtTrId("OCRLanguageChecking");
    const quint64 generation = ++m_generation;
    const auto cancellation = std::make_shared<std::atomic_bool>(false);
    m_cancellation = cancellation;
    QList<Model> models;
    for (const QString &code : codes) {
        for (const Model &model : m_models) {
            if (model.code == code)
                models.append(model);
        }
    }
    const QString cache = m_cacheDirectory;
    const QString bundled = m_bundledDirectory;
    const QPointer<OCRLanguageManager> manager(this);
    auto *runnable = QRunnable::create([models, cache, bundled, codes, cancellation, manager,
                                       generation, prepareAfterVerification]() {
        QStringList verified;
        for (const Model &model : models) {
            if (cancellation->load())
                return;
            const QString path = modelPath(model, cache, bundled);
            if (!hasExpectedFile(path, model.bytes))
                continue;
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                continue;
            QCryptographicHash hash(QCryptographicHash::Sha256);
            qint64 read = 0;
            while (!file.atEnd() && !cancellation->load() && read <= model.bytes) {
                const QByteArray chunk = file.read(chunkBytes);
                if (chunk.isEmpty())
                    break;
                read += chunk.size();
                hash.addData(chunk);
            }
            if (!cancellation->load() && read == model.bytes
                && file.error() == QFileDevice::NoError && hash.result() == model.sha256)
                verified.append(model.code);
        }
        QMetaObject::invokeMethod(qApp, [manager, generation, cancellation, codes, verified,
                                       prepareAfterVerification]() {
            if (!manager || cancellation->load() || manager->m_generation != generation)
                return;
            for (const QString &code : codes)
                manager->m_installed.removeAll(code);
            manager->m_installed.append(verified);
            if (!prepareAfterVerification) {
                manager->m_busy = false;
                manager->m_statusText.clear();
                emit manager->changed();
                return;
            }
            manager->m_pending.clear();
            manager->m_totalBytes = manager->m_completedBytes = 0;
            for (const QString &code : codes) {
                if (verified.contains(code))
                    continue;
                for (const Model &model : manager->m_models) {
                    if (model.code != code)
                        continue;
                    if (model.bundled) {
                        //% "An included language file is missing or damaged. Reinstall TrePDF to restore English and Vietnamese."
                        manager->fail(qtTrId("OCRLanguageBundledMissing"));
                        return;
                    }
                    manager->m_pending.append(code);
                    manager->m_totalBytes += model.bytes;
                }
            }
            manager->nextDownload();
        }, Qt::QueuedConnection);
    });
    // Reject concurrent verification instead of queuing an unbounded collection
    // of stale disk scans when dialogs are repeatedly opened and closed.
    if (!verificationPool().tryStart(runnable)) {
        delete runnable;
        //% "Language files are still being checked. Please try again in a moment."
        fail(qtTrId("OCRLanguageCheckBusy"));
        return false;
    }
    emit changed();
    return true;
}

void OCRLanguageManager::nextDownload()
{
    if (m_pending.isEmpty()) {
        const QStringList prepared = m_selection;
        const quint64 generation = m_generation;
        m_busy = false;
        m_progress = 1;
        m_statusText.clear();
        emit changed();
        // A UI reaction to changed() can close the modal and cancel this
        // generation synchronously. Never apply a now-abandoned selection.
        if (m_generation == generation)
            emit ready(prepared);
        return;
    }
    if (!m_network) {
        //% "The language download could not start. Please try again."
        fail(qtTrId("OCRLanguageDownloadUnavailable"));
        return;
    }
    const QString code = m_pending.takeFirst();
    for (const Model &model : m_models) {
        if (model.code == code) {
            m_current = model;
            break;
        }
    }
    const QString destination = modelPath(m_current, m_cacheDirectory, m_bundledDirectory);
    if (destination.isEmpty() || QFileInfo(destination).isSymLink()
        || !QDir().mkpath(m_cacheDirectory)) {
        //% "TrePDF could not save the language file. Check available disk space and folder access, then try again."
        fail(qtTrId("OCRLanguageStorageFailed"));
        return;
    }
    m_file = std::make_unique<QSaveFile>(destination);
    m_file->setDirectWriteFallback(false);
    if (!m_file->open(QIODevice::WriteOnly)) {
        //% "TrePDF could not save the language file. Check available disk space and folder access, then try again."
        fail(qtTrId("OCRLanguageStorageFailed"));
        return;
    }
    m_hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    m_received = 0;
    const QUrl url(QStringLiteral("https://raw.githubusercontent.com/tesseract-ocr/tessdata_fast/%1/%2.traineddata")
                       .arg(QString::fromLatin1(revision), code));
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setTransferTimeout(30000);
    request.setRawHeader("Accept-Encoding", "identity");
    m_reply = m_network->get(request);
    m_reply->setReadBufferSize(chunkBytes);
    connect(m_reply, &QNetworkReply::readyRead, this, &OCRLanguageManager::consumeReply);
    connect(m_reply, &QNetworkReply::finished, this, &OCRLanguageManager::finishReply);
    m_deadline.start();
    //% "Downloading %1…"
    m_statusText = qtTrId("OCRLanguageDownloading").arg(localizedName(code));
    emit changed();
}

void OCRLanguageManager::consumeReply()
{
    if (!m_reply || !m_file || !m_hash)
        return;
    const QVariant length = m_reply->header(QNetworkRequest::ContentLengthHeader);
    if (length.isValid() && length.toLongLong() != m_current.bytes) {
        //% "The language file could not be verified and was not installed. Please try again."
        fail(qtTrId("OCRLanguageIntegrityFailed"));
        return;
    }
    while (m_reply && m_reply->bytesAvailable() > 0) {
        const QByteArray chunk = m_reply->read(chunkBytes);
        if (chunk.isEmpty())
            break;
        if (m_received + chunk.size() > m_current.bytes) {
            //% "The language file could not be verified and was not installed. Please try again."
            fail(qtTrId("OCRLanguageIntegrityFailed"));
            return;
        }
        if (m_file->write(chunk) != chunk.size()) {
            //% "TrePDF could not save the language file. Check available disk space and folder access, then try again."
            fail(qtTrId("OCRLanguageStorageFailed"));
            return;
        }
        m_hash->addData(chunk);
        m_received += chunk.size();
    }
    m_progress = m_totalBytes > 0
        ? double(m_completedBytes + m_received) / double(m_totalBytes) : 0;
    emit changed();
}

void OCRLanguageManager::finishReply()
{
    if (!m_reply)
        return;
    // Surface HTTP/TLS/network errors as connection failures; model validation is
    // reserved for a successful 200 response with a wrong or truncated payload.
    if (m_reply->error() != QNetworkReply::NoError
        || m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200
        || m_reply->url() != m_reply->request().url()) {
        //% "The language could not be downloaded. Check your internet connection and try again."
        fail(qtTrId("OCRLanguageDownloadFailed"));
        return;
    }
    consumeReply();
    if (!m_reply || !m_file || !m_hash)
        return;
    if (m_received != m_current.bytes || m_hash->result() != m_current.sha256) {
        //% "The language file could not be verified and was not installed. Please try again."
        fail(qtTrId("OCRLanguageIntegrityFailed"));
        return;
    }
    if (!m_file->commit()) {
        //% "TrePDF could not save the language file. Check available disk space and folder access, then try again."
        fail(qtTrId("OCRLanguageStorageFailed"));
        return;
    }
    m_file.reset();
    m_installed.append(m_current.code);
    m_completedBytes += m_current.bytes;
    stopTransfer();
    nextDownload();
}

void OCRLanguageManager::stopTransfer()
{
    m_deadline.stop();
    if (m_reply) {
        QNetworkReply *reply = m_reply;
        m_reply.clear();
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
    m_file.reset();
    m_hash.reset();
}

void OCRLanguageManager::cancel()
{
    ++m_generation;
    if (m_cancellation)
        m_cancellation->store(true);
    stopTransfer();
    m_pending.clear();
    m_selection.clear();
    m_busy = false;
    m_progress = 0;
    m_statusText.clear();
    m_errorText.clear();
    emit changed();
}

void OCRLanguageManager::fail(const QString &message)
{
    ++m_generation;
    if (m_cancellation)
        m_cancellation->store(true);
    stopTransfer();
    m_pending.clear();
    m_busy = false;
    m_progress = 0;
    m_statusText.clear();
    m_errorText = message;
    emit changed();
}
