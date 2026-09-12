#pragma once

#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <memory>

class PDFDocument;

class PDFExcelExportController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool busy READ busy NOTIFY changed FINAL)
    Q_PROPERTY(int completedPages READ completedPages NOTIFY changed FINAL)
    Q_PROPERTY(int totalPages READ totalPages NOTIFY changed FINAL)
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed FINAL)

public:
    explicit PDFExcelExportController(QObject *parent = nullptr);
    ~PDFExcelExportController() override;

    bool busy() const;
    int completedPages() const;
    int totalPages() const;
    QString statusText() const;
    void setDocument(PDFDocument *document);
    // Called only on the GUI thread, before a worker snapshot is created.
    void setExportGuard(std::function<bool()> guard);

    Q_INVOKABLE bool exportTo(const QUrl &target);
    Q_INVOKABLE void cancel();

signals:
    void changed();
    void statusMessage(const QString &message);
    void finished(bool succeeded, const QUrl &target);

private:
    struct Work;
    struct Result;
    static Result performExport(const std::shared_ptr<Work> &work,
                                const std::function<void(int)> &progress);
    void complete(const std::shared_ptr<Work> &work, const Result &result);
    bool reject(const QString &message);

    QPointer<PDFDocument> m_document;
    std::function<bool()> m_exportGuard;
    std::shared_ptr<Work> m_work;
    QString m_statusText;
    int m_completedPages = 0;
    int m_totalPages = 0;
};
