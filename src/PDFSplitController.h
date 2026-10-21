#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QObject>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <memory>

// Independent of document sessions. Reads an immutable source snapshot and
// publishes a complete batch in a new directory; never modifies the source.
class PDFSplitController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QUrl source READ source NOTIFY changed FINAL)
    Q_PROPERTY(QString fileName READ fileName NOTIFY changed FINAL)
    Q_PROPERTY(int pageCount READ pageCount NOTIFY changed FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY changed FINAL)
    Q_PROPERTY(bool canSplit READ canSplit NOTIFY changed FINAL)
    Q_PROPERTY(bool passwordRequired READ passwordRequired NOTIFY changed FINAL)
    Q_PROPERTY(bool encryptedSource READ encryptedSource NOTIFY changed FINAL)
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed FINAL)
    Q_PROPERTY(int outputCount READ outputCount NOTIFY changed FINAL)
    Q_PROPERTY(qreal progress READ progress NOTIFY changed FINAL)
    Q_PROPERTY(int mode READ mode WRITE setMode NOTIFY changed FINAL)
    Q_PROPERTY(int pagesPerFile READ pagesPerFile WRITE setPagesPerFile NOTIFY changed FINAL)
    Q_PROPERTY(QString pageRanges READ pageRanges WRITE setPageRanges NOTIFY changed FINAL)

public:
    explicit PDFSplitController(QObject *parent = nullptr);
    ~PDFSplitController() override;

    QUrl source() const;
    QString fileName() const;
    int pageCount() const;
    bool busy() const;
    bool canSplit() const;
    bool passwordRequired() const;
    bool encryptedSource() const;
    QString statusText() const;
    int outputCount() const;
    qreal progress() const;
    int mode() const;
    int pagesPerFile() const;
    QString pageRanges() const;
    void setMode(int value);
    void setPagesPerFile(int value);
    void setPageRanges(const QString &value);

    Q_INVOKABLE bool setSource(const QUrl &url, const QString &password = {});
    Q_INVOKABLE bool unlockSource(const QString &password);
    Q_INVOKABLE void clear();
    Q_INVOKABLE bool splitTo(const QUrl &parentFolder);
    Q_INVOKABLE void cancel();

signals:
    void changed();
    void statusMessage(const QString &message);
    void completed(const QUrl &outputFolder, const QList<QUrl> &outputs);
    void finished(bool succeeded, const QUrl &outputFolder);

private:
    struct Entry {
        QString path;
        int pageCount = 0;
        qint64 size = 0;
        QDateTime modified;
        QByteArray fingerprint;
        QByteArray password;
        bool encrypted = false;
    };
    struct Work;
    struct Result;
    static Result perform(const std::shared_ptr<Work> &work,
                          const std::function<void(qreal)> &progress);
    bool start(const std::shared_ptr<Work> &work);
    void complete(const std::shared_ptr<Work> &work, const Result &result);
    bool reject(const QString &message);
    void optionsChanged();

    Entry m_source;
    std::shared_ptr<Work> m_work;
    QString m_statusText;
    QString m_pageRanges;
    int m_mode = 0;
    int m_pagesPerFile = 1;
    int m_outputCount = 0;
    qreal m_progress = 0;
    bool m_clearWhenFinished = false;
    bool m_passwordRequired = false;
};
