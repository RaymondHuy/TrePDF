#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QObject>
#include <QUrl>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <memory>

// Independent of the open PDF sessions: inputs are immutable on-disk snapshots,
// and a successful operation produces a new document through atomic replacement.
class PDFMergeController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantList files READ files NOTIFY changed FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY changed FINAL)
    Q_PROPERTY(bool canMerge READ canMerge NOTIFY changed FINAL)
    Q_PROPERTY(int totalPages READ totalPages NOTIFY changed FINAL)
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed FINAL)

public:
    explicit PDFMergeController(QObject *parent = nullptr);
    ~PDFMergeController() override;

    QVariantList files() const;
    bool busy() const;
    bool canMerge() const;
    int totalPages() const;
    QString statusText() const;

    Q_INVOKABLE bool addFiles(const QList<QUrl> &urls);
    Q_INVOKABLE void moveFile(int from, int to);
    Q_INVOKABLE void removeFile(int index);
    Q_INVOKABLE void clear();
    Q_INVOKABLE bool mergeTo(const QUrl &target);
    Q_INVOKABLE void cancel();

signals:
    void changed();
    void statusMessage(const QString &message);
    void completed(const QUrl &target);
    void finished(bool succeeded, const QUrl &target);

private:
    struct Entry {
        QString path;
        int pageCount = 0;
        qint64 size = 0;
        QDateTime modified;
        QByteArray fingerprint;
    };
    struct Work;
    struct Result;
    static Result perform(const std::shared_ptr<Work> &work);
    bool start(const std::shared_ptr<Work> &work);
    void complete(const std::shared_ptr<Work> &work, const Result &result);
    bool reject(const QString &message);

    QList<Entry> m_files;
    std::shared_ptr<Work> m_work;
    QString m_statusText;
};
