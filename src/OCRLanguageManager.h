#pragma once

#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include <atomic>
#include <memory>

class QCryptographicHash;
class QNetworkAccessManager;
class QNetworkReply;
class QSaveFile;

// Owns language-model preparation only. It never receives a PDF path or document
// contents: the only network requests are GETs for the pinned public models.
class OCRLanguageManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantList languages READ languages NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed)
    Q_PROPERTY(QString errorText READ errorText NOTIFY changed)

public:
    explicit OCRLanguageManager(QObject *parent = nullptr);
    ~OCRLanguageManager() override;

    QVariantList languages() const;
    bool busy() const { return m_busy; }
    double progress() const { return m_progress; }
    QString statusText() const { return m_statusText; }
    QString errorText() const { return m_errorText; }

    // Validates one to four distinct catalog codes, verifies existing models off
    // the GUI thread, and downloads missing optional models in selection order.
    // A true return means preparation started; only ready() means it succeeded.
    Q_INVOKABLE bool prepare(const QStringList &codes);
    // Stops verification/download and discards the active temporary file. Models
    // installed by earlier successful downloads remain reusable offline.
    Q_INVOKABLE void cancel();
    // Revalidates installed models without ever starting a network request.
    Q_INVOKABLE void refresh();

    // These helpers are safe for OCR workers: they do not construct QObjects,
    // start downloads, or alter files. installedModelPath checks type/size only;
    // consumers must hash their private snapshot against modelSha256() before
    // letting Tesseract read it. Hashes are raw 32-byte SHA-256 digests.
    static QString bundledDirectory();
    static QString installedModelPath(const QString &code);
    static QStringList supportedCodes();
    static bool validSelection(const QString &languageExpression);
    static qint64 modelBytes(const QString &code);
    static QByteArray modelSha256(const QString &code);

signals:
    void changed();
    void ready(const QStringList &codes);

private:
    friend class OCRLanguageManagerTest;
    struct Model {
        QString code;
        QString name;
        QString nativeName;
        QByteArray sha256;
        qint64 bytes = 0;
        bool bundled = false;
    };

    // Native-only dependency injection keeps tests deterministic while production
    // QML cannot override the catalog, origin, or cache destination.
    OCRLanguageManager(QNetworkAccessManager *network, const QString &cache,
                       const QString &bundled, const QByteArray &catalog,
                       QObject *parent = nullptr);
    static QList<Model> parseCatalog(const QByteArray &data);
    static const QList<Model> &catalog();
    static QString cacheDirectory();
    static QString modelPath(const Model &model, const QString &cache,
                             const QString &bundled);
    static bool hasExpectedFile(const QString &path, qint64 bytes);
    bool selectionAllowed(const QStringList &codes) const;
    bool verify(const QStringList &codes, bool prepareAfterVerification);
    void nextDownload();
    void consumeReply();
    void finishReply();
    void stopTransfer();
    void fail(const QString &message);

    QList<Model> m_models;
    QString m_cacheDirectory;
    QString m_bundledDirectory;
    QPointer<QNetworkAccessManager> m_network;
    QPointer<QNetworkReply> m_reply;
    QTimer m_deadline;
    std::unique_ptr<QSaveFile> m_file;
    std::unique_ptr<QCryptographicHash> m_hash;
    std::shared_ptr<std::atomic_bool> m_cancellation;
    QStringList m_installed;
    QStringList m_selection;
    QStringList m_pending;
    Model m_current;
    qint64 m_received = 0;
    qint64 m_totalBytes = 0;
    qint64 m_completedBytes = 0;
    quint64 m_generation = 0;
    bool m_busy = false;
    double m_progress = 0;
    QString m_statusText;
    QString m_errorText;
};
