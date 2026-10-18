#include "OCRLanguageManager.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <cstring>
#include <functional>

namespace {
struct Response {
    QByteArray payload;
    int status = 200;
    qint64 declaredBytes = -1;
    int chunkBytes = 4096;
    bool holdAfterFirstChunk = false;
    QNetworkReply::NetworkError error = QNetworkReply::NoError;
};

// Exercise the real manager's QNetworkRequest, streaming, limits, and commit
// pipeline with asynchronous in-memory replies. Tests never contact the internet.
class FakeReply final : public QNetworkReply
{
public:
    FakeReply(const QNetworkRequest &request, const Response &response,
              std::function<void()> aborted, QObject *parent)
        : QNetworkReply(parent), m_response(response), m_aborted(std::move(aborted))
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, response.status);
        if (response.declaredBytes >= 0)
            setHeader(QNetworkRequest::ContentLengthHeader, response.declaredBytes);
        open(QIODevice::ReadOnly);
        QTimer::singleShot(0, this, [this]() { deliver(); });
    }

    void abort() override
    {
        if (isFinished())
            return;
        m_aborted();
        setError(OperationCanceledError, QStringLiteral("Cancelled"));
        setFinished(true);
        emit finished();
    }

    qint64 bytesAvailable() const override
    {
        return m_available - m_read + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char *data, qint64 maximum) override
    {
        const qint64 count = qMin(maximum, m_available - m_read);
        if (count <= 0)
            return isFinished() ? -1 : 0;
        std::memcpy(data, m_response.payload.constData() + m_read, size_t(count));
        m_read += count;
        return count;
    }

private:
    void deliver()
    {
        if (isFinished())
            return;
        m_available = qMin(qint64(m_response.payload.size()),
                           m_available + m_response.chunkBytes);
        if (m_available > m_read)
            emit readyRead();
        if (isFinished() || m_response.holdAfterFirstChunk)
            return;
        if (m_available == m_response.payload.size()) {
            if (m_response.error != NoError)
                setError(m_response.error, QStringLiteral("Simulated network failure"));
            setFinished(true);
            emit finished();
            return;
        }
        QTimer::singleShot(0, this, [this]() { deliver(); });
    }

    Response m_response;
    std::function<void()> m_aborted;
    qint64 m_available = 0;
    qint64 m_read = 0;
};

class FakeNetwork final : public QNetworkAccessManager
{
public:
    Response response;
    QList<QNetworkRequest> requests;
    int abortCount = 0;
    bool onlyGets = true;

protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request,
                                 QIODevice *outgoingData) override
    {
        requests.append(request);
        onlyGets = onlyGets && operation == GetOperation && outgoingData == nullptr;
        return new FakeReply(request, response, [this]() { ++abortCount; }, this);
    }
};

QByteArray makeCatalog(const QList<QPair<QString, QByteArray>> &models)
{
    QJsonArray entries;
    for (const auto &item : models) {
        entries.append(QJsonObject{
            {QStringLiteral("code"), item.first},
            {QStringLiteral("name"), item.first},
            {QStringLiteral("nativeName"), item.first},
            {QStringLiteral("bundled"), item.first == QStringLiteral("eng")
                                       || item.first == QStringLiteral("vie")},
            {QStringLiteral("bytes"), item.second.size()},
            {QStringLiteral("sha256"), QString::fromLatin1(
                 QCryptographicHash::hash(item.second, QCryptographicHash::Sha256).toHex())}
        });
    }
    return QJsonDocument(QJsonObject{
        {QStringLiteral("revision"), QStringLiteral("87416418657359cb625c412a48b6e1d6d41c29bd")},
        {QStringLiteral("models"), entries}}).toJson();
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
}

class OCRLanguageManagerTest : public QObject
{
    Q_OBJECT
private slots:
    void catalogRejectsUnknownAndUnsafeSelections();
    void bundledLanguagesWorkOffline();
    void missingBundledModelIsNotDownloaded();
    void downloadIsVerifiedAndReusedAcrossManagers();
    void corruptedCacheIsDownloadedAgain();
    void failedPayloadNeverInstalls_data();
    void failedPayloadNeverInstalls();
    void cancellationDiscardsPartialDownloadAndAllowsRetry();
    void completedLanguagesSurviveLaterFailure();
    void timeoutDiscardsPartialDownload();
    void failedReplacementPreservesExistingBytes();
    void refusesSymlinkDestination();
    void synchronousCancellationSuppressesReady();
};

void OCRLanguageManagerTest::catalogRejectsUnknownAndUnsafeSelections()
{
    const QStringList supported = OCRLanguageManager::supportedCodes();
    QCOMPARE(supported.size(), 25);
    for (const QString &code : supported) {
        QVERIFY(OCRLanguageManager::validSelection(code));
        QVERIFY(OCRLanguageManager::modelBytes(code) > 0);
        QCOMPARE(OCRLanguageManager::modelSha256(code).size(), 32);
    }
    QVERIFY(OCRLanguageManager::validSelection(QStringLiteral("eng+vie+fra+tha")));
    QVERIFY(OCRLanguageManager::validSelection(QStringLiteral("chi_sim+chi_tra")));
    const QStringList invalid{QString(), QStringLiteral("en"), QStringLiteral("eng+eng"),
                              QStringLiteral("eng+"), QStringLiteral("+eng"),
                              QStringLiteral(" eng"), QStringLiteral("ENG"),
                              QStringLiteral("../../eng"), QStringLiteral("eng+script/Latin"),
                              QStringLiteral("eng+vie+fra+tha+hrv")};
    for (const QString &selection : invalid)
        QVERIFY2(!OCRLanguageManager::validSelection(selection), qPrintable(selection));
    QVERIFY(OCRLanguageManager::installedModelPath(QStringLiteral("../eng")).isEmpty());
    QVERIFY(OCRLanguageManager::modelSha256(QStringLiteral("unknown")).isEmpty());
    QCOMPARE(OCRLanguageManager::modelBytes(QStringLiteral("unknown")), 0);
}

void OCRLanguageManagerTest::bundledLanguagesWorkOffline()
{
    QTemporaryDir cache;
    QTemporaryDir bundled;
    QVERIFY(cache.isValid() && bundled.isValid());
    const QByteArray english("bundled English"), vietnamese("bundled Vietnamese");
    QVERIFY(writeFile(bundled.filePath(QStringLiteral("eng.traineddata")), english));
    QVERIFY(writeFile(bundled.filePath(QStringLiteral("vie.traineddata")), vietnamese));
    FakeNetwork network;
    OCRLanguageManager manager(&network, cache.path(), bundled.path(),
        makeCatalog({{QStringLiteral("eng"), english}, {QStringLiteral("vie"), vietnamese}}));
    QSignalSpy ready(&manager, &OCRLanguageManager::ready);
    QVERIFY(manager.prepare({QStringLiteral("eng"), QStringLiteral("vie")}));
    QTRY_COMPARE(ready.size(), 1);
    QVERIFY(!manager.busy());
    QCOMPARE(ready.constFirst().constFirst().toStringList(),
             (QStringList{QStringLiteral("eng"), QStringLiteral("vie")}));
    QCOMPARE(network.requests.size(), 0);
    QVERIFY(manager.errorText().isEmpty());
    for (const QVariant &entry : manager.languages())
        QVERIFY(entry.toMap().value(QStringLiteral("installed")).toBool());
    QCOMPARE(QDir(cache.path()).entryList(QDir::Files).size(), 0);
}

void OCRLanguageManagerTest::missingBundledModelIsNotDownloaded()
{
    QTemporaryDir cache;
    QTemporaryDir bundled;
    FakeNetwork network;
    OCRLanguageManager manager(&network, cache.path(), bundled.path(),
        makeCatalog({{QStringLiteral("eng"), QByteArray("missing bundled model")}}));
    QSignalSpy ready(&manager, &OCRLanguageManager::ready);
    QVERIFY(manager.prepare({QStringLiteral("eng")}));
    QTRY_VERIFY(!manager.busy());
    QVERIFY(!manager.errorText().isEmpty());
    QCOMPARE(ready.size(), 0);
    QCOMPARE(network.requests.size(), 0);
}

void OCRLanguageManagerTest::downloadIsVerifiedAndReusedAcrossManagers()
{
    QTemporaryDir cache;
    QTemporaryDir bundled;
    const QByteArray french(150000, 'f');
    const QByteArray catalog = makeCatalog({{QStringLiteral("fra"), french}});
    FakeNetwork network;
    network.response.payload = french;
    network.response.declaredBytes = french.size();
    {
        OCRLanguageManager manager(&network, cache.path(), bundled.path(), catalog);
        QSignalSpy ready(&manager, &OCRLanguageManager::ready);
        QVERIFY(manager.prepare({QStringLiteral("fra")}));
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(manager.progress(), 1.0);
        QVERIFY(manager.errorText().isEmpty());
        QCOMPARE(readFile(cache.filePath(QStringLiteral("fra.traineddata"))), french);
        QCOMPARE(network.requests.size(), 1);
        QVERIFY(network.onlyGets);
        const QNetworkRequest request = network.requests.constFirst();
        QCOMPARE(request.url(), QUrl(QStringLiteral(
            "https://raw.githubusercontent.com/tesseract-ocr/tessdata_fast/"
            "87416418657359cb625c412a48b6e1d6d41c29bd/fra.traineddata")));
        QCOMPARE(request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(),
                 int(QNetworkRequest::ManualRedirectPolicy));
    }
    // A new manager must discover/revalidate the previous model from disk even
    // with a network that would fail every request.
    network.response.error = QNetworkReply::ConnectionRefusedError;
    OCRLanguageManager reopened(&network, cache.path(), bundled.path(), catalog);
    QSignalSpy ready(&reopened, &OCRLanguageManager::ready);
    QVERIFY(reopened.prepare({QStringLiteral("fra")}));
    QTRY_COMPARE(ready.size(), 1);
    QCOMPARE(network.requests.size(), 1);
    QVERIFY(reopened.languages().constFirst().toMap().value(QStringLiteral("installed")).toBool());
}

void OCRLanguageManagerTest::corruptedCacheIsDownloadedAgain()
{
    QTemporaryDir cache;
    QTemporaryDir bundled;
    const QByteArray good("known correct French model bytes");
    const QString path = cache.filePath(QStringLiteral("fra.traineddata"));
    QVERIFY(writeFile(path, QByteArray(good.size(), '?')));
    FakeNetwork network;
    network.response.payload = good;
    OCRLanguageManager manager(&network, cache.path(), bundled.path(),
                               makeCatalog({{QStringLiteral("fra"), good}}));
    QSignalSpy ready(&manager, &OCRLanguageManager::ready);
    QVERIFY(manager.prepare({QStringLiteral("fra")}));
    QTRY_COMPARE(ready.size(), 1);
    QCOMPARE(network.requests.size(), 1);
    QCOMPARE(readFile(path), good);
}

void OCRLanguageManagerTest::failedPayloadNeverInstalls_data()
{
    QTest::addColumn<int>("failure");
    QTest::newRow("wrong SHA256 with correct size") << 0;
    QTest::newRow("truncated model") << 1;
    QTest::newRow("oversized streamed model") << 2;
    QTest::newRow("unexpected Content-Length") << 3;
    QTest::newRow("network failure") << 4;
    QTest::newRow("redirect is not followed") << 5;
}

void OCRLanguageManagerTest::failedPayloadNeverInstalls()
{
    QFETCH(int, failure);
    QTemporaryDir cache;
    QTemporaryDir bundled;
    const QByteArray good("known correct French model bytes");
    FakeNetwork network;
    network.response.payload = good;
    if (failure == 0)
        network.response.payload[0] = '?';
    else if (failure == 1)
        network.response.payload.chop(1);
    else if (failure == 2)
        network.response.payload.append('?');
    else if (failure == 3)
        network.response.declaredBytes = good.size() + 1;
    else if (failure == 4)
        network.response.error = QNetworkReply::RemoteHostClosedError;
    else if (failure == 5)
        network.response.status = 302;
    OCRLanguageManager manager(&network, cache.path(), bundled.path(),
                               makeCatalog({{QStringLiteral("fra"), good}}));
    QSignalSpy ready(&manager, &OCRLanguageManager::ready);
    QVERIFY(manager.prepare({QStringLiteral("fra")}));
    QTRY_VERIFY(!manager.busy());
    QVERIFY(!manager.errorText().isEmpty());
    QCOMPARE(ready.size(), 0);
    QCOMPARE(network.requests.size(), 1);
    QVERIFY(!QFileInfo::exists(cache.filePath(QStringLiteral("fra.traineddata"))));
    QCOMPARE(QDir(cache.path()).entryList(QDir::Files | QDir::Hidden).size(), 0);
}

void OCRLanguageManagerTest::cancellationDiscardsPartialDownloadAndAllowsRetry()
{
    QTemporaryDir cache;
    QTemporaryDir bundled;
    const QByteArray good(150000, 'f');
    FakeNetwork network;
    network.response.payload = good;
    network.response.holdAfterFirstChunk = true;
    OCRLanguageManager manager(&network, cache.path(), bundled.path(),
                               makeCatalog({{QStringLiteral("fra"), good}}));
    QSignalSpy ready(&manager, &OCRLanguageManager::ready);
    QVERIFY(manager.prepare({QStringLiteral("fra")}));
    QTRY_VERIFY(manager.progress() > 0 && manager.progress() < 1);
    manager.cancel();
    QVERIFY(!manager.busy());
    QVERIFY(manager.errorText().isEmpty());
    QCOMPARE(network.abortCount, 1);
    QCOMPARE(ready.size(), 0);
    QCOMPARE(QDir(cache.path()).entryList(QDir::Files | QDir::Hidden).size(), 0);
    network.response.holdAfterFirstChunk = false;
    QVERIFY(manager.prepare({QStringLiteral("fra")}));
    QTRY_COMPARE(ready.size(), 1);
    QCOMPARE(network.requests.size(), 2);
    QCOMPARE(readFile(cache.filePath(QStringLiteral("fra.traineddata"))), good);
}

void OCRLanguageManagerTest::completedLanguagesSurviveLaterFailure()
{
    QTemporaryDir cache;
    QTemporaryDir bundled;
    const QByteArray french("French bytes");
    const QByteArray thai("Thai bytes that differ");
    FakeNetwork network;
    // The same response is valid for fra but invalid for tha. This verifies
    // that partial multi-language success remains reusable, but cannot emit ready.
    network.response.payload = french;
    OCRLanguageManager manager(&network, cache.path(), bundled.path(), makeCatalog({
        {QStringLiteral("fra"), french}, {QStringLiteral("tha"), thai}}));
    QSignalSpy ready(&manager, &OCRLanguageManager::ready);
    QVERIFY(manager.prepare({QStringLiteral("fra"), QStringLiteral("tha")}));
    QTRY_VERIFY(!manager.busy());
    QCOMPARE(ready.size(), 0);
    QCOMPARE(network.requests.size(), 2);
    QCOMPARE(readFile(cache.filePath(QStringLiteral("fra.traineddata"))), french);
    QVERIFY(!QFileInfo::exists(cache.filePath(QStringLiteral("tha.traineddata"))));
    network.response.payload = thai;
    QVERIFY(manager.prepare({QStringLiteral("fra"), QStringLiteral("tha")}));
    QTRY_COMPARE(ready.size(), 1);
    QCOMPARE(network.requests.size(), 3);
    QCOMPARE(readFile(cache.filePath(QStringLiteral("tha.traineddata"))), thai);
}

void OCRLanguageManagerTest::timeoutDiscardsPartialDownload()
{
    QTemporaryDir cache;
    QTemporaryDir bundled;
    const QByteArray good(150000, 'f');
    FakeNetwork network;
    network.response.payload = good;
    network.response.holdAfterFirstChunk = true;
    OCRLanguageManager manager(&network, cache.path(), bundled.path(),
                               makeCatalog({{QStringLiteral("fra"), good}}));
    manager.m_deadline.setInterval(10);
    QSignalSpy ready(&manager, &OCRLanguageManager::ready);
    QVERIFY(manager.prepare({QStringLiteral("fra")}));
    QTRY_VERIFY(!manager.busy());
    QVERIFY(!manager.errorText().isEmpty());
    QCOMPARE(ready.size(), 0);
    QCOMPARE(network.abortCount, 1);
    QCOMPARE(QDir(cache.path()).entryList(QDir::Files | QDir::Hidden).size(), 0);
}

void OCRLanguageManagerTest::failedReplacementPreservesExistingBytes()
{
    QTemporaryDir cache;
    QTemporaryDir bundled;
    const QByteArray good("known correct French model bytes");
    const QByteArray old("existing model is left intact until a replacement verifies");
    const QString path = cache.filePath(QStringLiteral("fra.traineddata"));
    QVERIFY(writeFile(path, old));
    FakeNetwork network;
    network.response.payload = QByteArray(good.size(), '?');
    OCRLanguageManager manager(&network, cache.path(), bundled.path(),
                               makeCatalog({{QStringLiteral("fra"), good}}));
    QVERIFY(manager.prepare({QStringLiteral("fra")}));
    QTRY_VERIFY(!manager.busy());
    QVERIFY(!manager.errorText().isEmpty());
    QCOMPARE(readFile(path), old);
}

void OCRLanguageManagerTest::refusesSymlinkDestination()
{
#ifdef Q_OS_WIN
    QSKIP("QFile::link creates Windows shortcuts rather than Unix symbolic links.");
#else
    QTemporaryDir cache;
    QTemporaryDir outside;
    const QByteArray good("known correct French model bytes");
    const QString target = outside.filePath(QStringLiteral("untouched.txt"));
    const QByteArray old("external file must not be overwritten");
    QVERIFY(writeFile(target, old));
    QVERIFY(QFile::link(target, cache.filePath(QStringLiteral("fra.traineddata"))));
    FakeNetwork network;
    network.response.payload = good;
    OCRLanguageManager manager(&network, cache.path(), outside.path(),
                               makeCatalog({{QStringLiteral("fra"), good}}));
    QVERIFY(manager.prepare({QStringLiteral("fra")}));
    QTRY_VERIFY(!manager.busy());
    QVERIFY(!manager.errorText().isEmpty());
    QCOMPARE(network.requests.size(), 0);
    QCOMPARE(readFile(target), old);
#endif
}

void OCRLanguageManagerTest::synchronousCancellationSuppressesReady()
{
    QTemporaryDir cache;
    QTemporaryDir bundled;
    const QByteArray english("bundled English");
    QVERIFY(writeFile(bundled.filePath(QStringLiteral("eng.traineddata")), english));
    FakeNetwork network;
    OCRLanguageManager manager(&network, cache.path(), bundled.path(),
                               makeCatalog({{QStringLiteral("eng"), english}}));
    QSignalSpy ready(&manager, &OCRLanguageManager::ready);
    bool cancelOnCompletion = true;
    connect(&manager, &OCRLanguageManager::changed, &manager, [&]() {
        if (cancelOnCompletion && !manager.busy() && manager.progress() == 1)
            manager.cancel();
    });
    QVERIFY(manager.prepare({QStringLiteral("eng")}));
    QTRY_VERIFY(!manager.busy());
    QCOMPARE(ready.size(), 0);
    cancelOnCompletion = false;
    QVERIFY(manager.prepare({QStringLiteral("eng")}));
    QTRY_COMPARE(ready.size(), 1);
    QCOMPARE(network.requests.size(), 0);
}

QTEST_GUILESS_MAIN(OCRLanguageManagerTest)
#include "tst_OCRLanguageManager.moc"
