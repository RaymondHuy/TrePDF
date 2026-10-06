#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVector>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <memory>

class PDFDocument;

class PDFSearchController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(PDFDocument *document READ document WRITE setDocument
                   NOTIFY documentChanged FINAL)
    Q_PROPERTY(QString query READ query NOTIFY queryChanged FINAL)
    Q_PROPERTY(SearchState state READ state NOTIFY stateChanged FINAL)
    Q_PROPERTY(int resultCount READ resultCount NOTIFY resultsChanged FINAL)
    Q_PROPERTY(int currentResultIndex READ currentResultIndex
                   NOTIFY resultsChanged FINAL)
    Q_PROPERTY(quint64 resultsRevision READ resultsRevision
                   NOTIFY resultsChanged FINAL)

public:
    enum SearchState {
        Idle,
        Searching,
        Ready,
        Limited,
        Failed
    };
    Q_ENUM(SearchState)

    explicit PDFSearchController(QObject *parent = nullptr);
    ~PDFSearchController() override;

    PDFDocument *document() const;
    QString query() const;
    SearchState state() const;
    int resultCount() const;
    int currentResultIndex() const;
    quint64 resultsRevision() const;

    void setDocument(PDFDocument *document);

    Q_INVOKABLE void prepareSearchText(const QString &query);
    Q_INVOKABLE void scheduleSearch(const QString &query);
    Q_INVOKABLE void search(const QString &query);
    Q_INVOKABLE void clearSearch();
    Q_INVOKABLE int nextResult();
    Q_INVOKABLE int previousResult();
    Q_INVOKABLE QVariantList highlightsForPage(int page) const;

signals:
    void documentChanged();
    void queryChanged();
    void stateChanged();
    void resultsChanged();
    void resultActivated(int page,
                         qreal normalizedX,
                         qreal normalizedY,
                         quint64 resultsRevision);

private:
    friend class PDFMemoryTelemetry;
    struct PageRequest {
        int displayPage = -1;
        int sourcePage = -1;
        QSizeF pageSize;
    };

    struct SearchMatch {
        int page = -1;
        QVector<QRectF> rects;
    };

    struct WorkResult {
        QVector<SearchMatch> matches;
        bool limited = false;
        bool failed = false;
    };

    struct PendingWork {
        QString filePath;
        QByteArray password;
        int revision = -1;
        QString query;
        QVector<PageRequest> requests;
        std::shared_ptr<std::atomic_bool> cancellation;
    };

    static WorkResult performSearch(
        const QString &filePath,
        const QByteArray &password,
        const QString &query,
        const QVector<PageRequest> &requests,
        const std::shared_ptr<std::atomic_bool> &cancellation);

    void prepareSearch(const QString &query);
    void startPreparedSearch();
    void dispatchPendingWork();
    void cancelPendingSearch();
    void resetResults();
    void setState(SearchState state);
    int activateResult(int index);

    QPointer<PDFDocument> m_document;
    QString m_query;
    SearchState m_state = Idle;
    QVector<SearchMatch> m_matches;
    QHash<int, QVector<int>> m_matchIndicesByPage;
    int m_currentResultIndex = -1;
    quint64 m_resultsRevision = 0;
    std::shared_ptr<std::atomic_bool> m_cancellation;
    std::shared_ptr<PendingWork> m_pendingWork;
    QTimer m_searchDelay;
    QTimer m_dispatchRetry;
};
