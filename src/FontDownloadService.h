#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

// GUI-thread service. Call start only after the user consents to downloading
// this family. Downloads the available regular/bold/italic/bold-italic faces;
// bold/italic identify the face needed by the pending save. It sends font names
// only, never document text or paths.
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
    struct Face {
        bool bold = false;
        bool italic = false;
        QByteArray data;
    };
    void request(const QUrl &url, Stage stage);
    void receiveData(QNetworkReply *reply);
    void receiveFinished(QNetworkReply *reply);
    void processMetadata(const QByteArray &data);
    void processStylesheet(const QByteArray &data);
    void processArchive(const QByteArray &data);
    void prepareFaces(bool regular, bool italic, bool bold, bool boldItalic);
    void requestNextFace();
    bool stageFont(const QByteArray &data, Face &face);
    bool hasCachedFace(const Face &face) const;
    void scheduleInstallation();
    void installFamily();
    uint requiredCharacter(const Face &face) const;
    void failUnavailable();
    void complete(bool success, const QString &error);
    static bool allowedUrl(const QUrl &url, Stage stage);
    static qint64 maximumBytes(Stage stage);

    QPointer<QNetworkAccessManager> m_network;
    QPointer<QNetworkReply> m_reply;
    QTimer m_deadline;
    QByteArray m_data;
    QString m_family;
    QString m_subsets;
    QList<Face> m_faces;
    qsizetype m_faceIndex = 0;
    quint64 m_generation = 0;
    Stage m_stage = Stage::Metadata;
    uint m_requiredCodePoint = 0;
    int m_redirects = 0;
    bool m_bold = false;
    bool m_italic = false;
    bool m_requireVietnamese = false;
    bool m_active = false;
};
