#pragma once

#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <memory>

class PDFDocument;

// One session's OCR job. Workers receive immutable values, never live PDF/QML
// objects; their staged PDF reaches the destination only after all checks pass.
class PDFOCRController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool available READ available CONSTANT FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY changed FINAL)
    Q_PROPERTY(int completedPages READ completedPages NOTIFY changed FINAL)
    Q_PROPERTY(int totalPages READ totalPages NOTIFY changed FINAL)
    Q_PROPERTY(int recognizedPages READ recognizedPages NOTIFY changed FINAL)
    Q_PROPERTY(int skippedPages READ skippedPages NOTIFY changed FINAL)
    Q_PROPERTY(int emptyPages READ emptyPages NOTIFY changed FINAL)
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed FINAL)

public:
    explicit PDFOCRController(QObject *parent = nullptr);
    ~PDFOCRController() override;

    bool available() const;
    bool busy() const;
    int completedPages() const;
    int totalPages() const;
    int recognizedPages() const;
    int skippedPages() const;
    int emptyPages() const;
    QString statusText() const;
    QUrl target() const;
    void setDocument(PDFDocument *document);
    // Both guards run only on the GUI thread before dispatch. Session wiring
    // cancels the accepted job as soon as these conditions cease to hold.
    void setRecognitionGuard(std::function<bool()> guard);
    void setDestinationGuard(std::function<bool(const QUrl &)> guard);

    // An empty range means all pages; otherwise accept 1-based comma-separated
    // pages/inclusive ranges, deduplicated into source-document order.
    Q_INVOKABLE bool recognizeTo(const QUrl &target, const QString &language,
                                const QString &pageRange = QString());
    Q_INVOKABLE void cancel();

signals:
    void changed();
    void statusMessage(const QString &message);
    void finished(bool succeeded, const QUrl &target);

private:
    struct Work;
    struct Result;
    static Result performRecognition(const std::shared_ptr<Work> &work,
                                     const std::function<void(int)> &progress);
    void complete(const std::shared_ptr<Work> &work, const Result &result);
    bool reject(const QString &message);

    QPointer<PDFDocument> m_document;
    std::function<bool()> m_recognitionGuard;
    std::function<bool(const QUrl &)> m_destinationGuard;
    std::shared_ptr<Work> m_work;
    QString m_statusText;
    int m_completedPages = 0;
    int m_totalPages = 0;
    int m_recognizedPages = 0;
    int m_skippedPages = 0;
    int m_emptyPages = 0;
};
