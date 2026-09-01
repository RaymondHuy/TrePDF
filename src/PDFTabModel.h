#pragma once

#include "PDFDocument.h"
#include "PDFEditController.h"
#include "PDFFormController.h"
#include "PDFFormDesignerController.h"
#include "PDFSearchController.h"

#include <QAbstractListModel>
#include <QUrl>
#include <QVector>
#include <QtQml/qqmlregistration.h>

class PDFDocumentSession : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("PDF document sessions are created by PDFTabModel")
    Q_PROPERTY(PDFDocument *document READ document CONSTANT FINAL)
    Q_PROPERTY(PDFEditController *editController READ editController CONSTANT FINAL)
    Q_PROPERTY(PDFFormController *formController READ formController CONSTANT FINAL)
    Q_PROPERTY(PDFFormDesignerController *formDesignerController
                   READ formDesignerController CONSTANT FINAL)
    Q_PROPERTY(PDFSearchController *searchController READ searchController
                   CONSTANT FINAL)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged FINAL)
    Q_PROPERTY(QString filePath READ filePath NOTIFY filePathChanged FINAL)
    Q_PROPERTY(qreal zoom READ zoom WRITE setZoom NOTIFY zoomChanged FINAL)
    Q_PROPERTY(qreal scrollX READ scrollX WRITE setScrollX NOTIFY scrollPositionChanged FINAL)
    Q_PROPERTY(qreal scrollY READ scrollY WRITE setScrollY NOTIFY scrollPositionChanged FINAL)
    Q_PROPERTY(int viewRotation READ viewRotation WRITE setViewRotation
                   NOTIFY viewRotationChanged FINAL)
    Q_PROPERTY(bool panelCollapsed READ panelCollapsed WRITE setPanelCollapsed
                   NOTIFY panelCollapsedChanged FINAL)
    Q_PROPERTY(bool initialFitPending READ initialFitPending WRITE setInitialFitPending
                   NOTIFY initialFitPendingChanged FINAL)
    Q_PROPERTY(bool hasDrafts READ hasDrafts NOTIFY hasDraftsChanged FINAL)
    Q_PROPERTY(bool saving READ saving NOTIFY savingChanged FINAL)

public:
    explicit PDFDocumentSession(QObject *parent = nullptr);

    PDFDocument *document() const;
    PDFEditController *editController() const;
    PDFFormController *formController() const;
    PDFFormDesignerController *formDesignerController() const;
    PDFSearchController *searchController() const;
    QString title() const;
    QString filePath() const;
    qreal zoom() const;
    qreal scrollX() const;
    qreal scrollY() const;
    int viewRotation() const;
    bool panelCollapsed() const;
    bool initialFitPending() const;
    bool hasDrafts() const;
    bool saving() const;

    void setZoom(qreal zoom);
    void setScrollX(qreal scrollX);
    void setScrollY(qreal scrollY);
    void setViewRotation(int rotation);
    void setPanelCollapsed(bool collapsed);
    void setInitialFitPending(bool pending);

    bool open(const QUrl &source);
    Q_INVOKABLE void rotateView(int quarterTurns);
    Q_INVOKABLE void prepareForDeactivation();

signals:
    void titleChanged();
    void filePathChanged();
    void zoomChanged();
    void scrollPositionChanged();
    void viewRotationChanged();
    void panelCollapsedChanged();
    void initialFitPendingChanged();
    void hasDraftsChanged();
    void savingChanged();
    void statusMessage(const QString &message);
    void saveFinished(bool succeeded);
    void externalLinkRequested(quint64 requestId,
                               const QString &authority,
                               const QString &displayUrl,
                               bool insecureHttp);
    void externalLinkRequestCancelled(quint64 requestId);

private:
    void refreshSourceState();

    PDFDocument *m_document = nullptr;
    PDFEditController *m_editController = nullptr;
    PDFFormController *m_formController = nullptr;
    PDFFormDesignerController *m_formDesignerController = nullptr;
    PDFSearchController *m_searchController = nullptr;
    QString m_title;
    QString m_filePath;
    qreal m_zoom = 1.0;
    qreal m_scrollX = 0.0;
    qreal m_scrollY = 0.0;
    int m_viewRotation = 0;
    bool m_panelCollapsed = false;
    bool m_initialFitPending = true;
};

class PDFTabModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)
    Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex
                   NOTIFY currentIndexChanged FINAL)
    Q_PROPERTY(PDFDocumentSession *currentSession READ currentSession
                   NOTIFY currentSessionChanged FINAL)
    Q_PROPERTY(bool anySaving READ anySaving NOTIFY anySavingChanged FINAL)
    Q_PROPERTY(bool hasDrafts READ hasDrafts NOTIFY hasDraftsChanged FINAL)

public:
    enum Role {
        SessionRole = Qt::UserRole + 1
    };
    Q_ENUM(Role)

    explicit PDFTabModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const;
    int currentIndex() const;
    PDFDocumentSession *currentSession() const;
    bool anySaving() const;
    bool hasDrafts() const;

    Q_INVOKABLE PDFDocumentSession *open(const QUrl &source);
    Q_INVOKABLE PDFDocumentSession *sessionAt(int index) const;
    Q_INVOKABLE int indexOf(PDFDocumentSession *session) const;
    Q_INVOKABLE bool closeSession(PDFDocumentSession *session);
    Q_INVOKABLE bool requestSave(PDFDocumentSession *session);
    Q_INVOKABLE bool requestSaveAs(PDFDocumentSession *session, const QUrl &target);
    Q_INVOKABLE PDFDocumentSession *firstDraftSession() const;
    Q_INVOKABLE void finishAllEdits();

    void setCurrentIndex(int index);

signals:
    void countChanged();
    void currentIndexChanged();
    void currentSessionChanged();
    void anySavingChanged();
    void hasDraftsChanged();
    void statusMessage(const QString &message);
    void sessionSaveFinished(PDFDocumentSession *session, bool succeeded);
    void externalLinkRequested(PDFDocumentSession *session,
                               quint64 requestId,
                               const QString &authority,
                               const QString &displayUrl,
                               bool insecureHttp);
    void externalLinkRequestCancelled(PDFDocumentSession *session,
                                      quint64 requestId);

private:
    static QString fileIdentity(const QUrl &source);
    bool contains(PDFDocumentSession *session) const;
    void connectSession(PDFDocumentSession *session);

    QVector<PDFDocumentSession *> m_sessions;
    int m_currentIndex = -1;
};
