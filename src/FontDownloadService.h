#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

// GUI-thread service. Call start only after the user consents to downloading
// this family/style. It sends font names only, never document text or paths.
class FontDownloadService final : public QObject
{
    Q_OBJECT

public:
    // An injected manager is for deterministic tests; it must outlive the service.
    explicit FontDownloadService(QObject *parent = nullptr,
                                 QNetworkAccessManager *network = nullptr);
    ~FontDownloadService() override;

    void start(const QString &family, bool bold, bool italic, uint requiredCodePoint);
    // Teardown and user cancellation are silent and never install a late reply.
    void cancel();

signals:
    void finished(bool success, const QString &error);

private:
    enum class Stage { Metadata, Stylesheet, Font, AptosArchive };
    void request(const QUrl &url, Stage stage);
    void receiveData(QNetworkReply *reply);
    void receiveFinished(QNetworkReply *reply);
    void processMetadata(const QByteArray &data);
    void processStylesheet(const QByteArray &data);
    void processArchive(const QByteArray &data);
    void installFont(const QByteArray &data);
    void failUnavailable();
    void complete(bool success, const QString &error);
    static bool allowedUrl(const QUrl &url, Stage stage);
    static qint64 maximumBytes(Stage stage);

    QPointer<QNetworkAccessManager> m_network;
    QPointer<QNetworkReply> m_reply;
    QTimer m_deadline;
    QByteArray m_data;
    QString m_family;
    QString m_archiveEntry;
    Stage m_stage = Stage::Metadata;
    uint m_requiredCodePoint = 0;
    int m_redirects = 0;
    bool m_bold = false;
    bool m_italic = false;
    bool m_requireVietnamese = false;
    bool m_active = false;
};
