#pragma once

#include "PDFDocument.h"
#include "PDFEditController.h"
#include "PDFExcelExportController.h"
#include "PDFFormController.h"
#include "PDFFormDesignerController.h"
#include "PDFSearchController.h"

#include <QAbstractListModel>
#include <QUrl>
#include <QVector>
#include <QtQml/qqmlregistration.h>

class PDFViewState : public QObject
{
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(qreal zoom READ zoom WRITE setZoom NOTIFY zoomChanged FINAL)
    Q_PROPERTY(qreal scrollX READ scrollX WRITE setScrollX
                   NOTIFY scrollPositionChanged FINAL)
    Q_PROPERTY(qreal scrollY READ scrollY WRITE setScrollY
                   NOTIFY scrollPositionChanged FINAL)
    Q_PROPERTY(int currentPage READ currentPage WRITE setCurrentPage
                   NOTIFY currentPageChanged FINAL)
    Q_PROPERTY(bool positionInitialized READ positionInitialized
                   NOTIFY positionInitializedChanged FINAL)

public:
    explicit PDFViewState(QObject *parent = nullptr);

    qreal zoom() const;
    qreal scrollX() const;
    qreal scrollY() const;
    int currentPage() const;
    bool positionInitialized() const;

    void setZoom(qreal zoom);
    void setScrollX(qreal scrollX);
    void setScrollY(qreal scrollY);
    void setCurrentPage(int page);
    Q_INVOKABLE void setScrollPosition(qreal scrollX, qreal scrollY);

signals:
    void zoomChanged();
    void scrollPositionChanged();
    void currentPageChanged();
    void positionInitializedChanged();

private:
    qreal m_zoom = 1.0;
    qreal m_scrollX = 0.0;
    qreal m_scrollY = 0.0;
    int m_currentPage = 0;
    bool m_positionInitialized = false;
};

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
    Q_PROPERTY(PDFExcelExportController *excelExportController READ excelExportController
                   CONSTANT FINAL)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged FINAL)
    Q_PROPERTY(QString filePath READ filePath NOTIFY filePathChanged FINAL)
    Q_PROPERTY(qreal zoom READ zoom WRITE setZoom NOTIFY zoomChanged FINAL)
    Q_PROPERTY(qreal scrollX READ scrollX WRITE setScrollX NOTIFY scrollPositionChanged FINAL)
    Q_PROPERTY(qreal scrollY READ scrollY WRITE setScrollY NOTIFY scrollPositionChanged FINAL)
    Q_PROPERTY(int viewRotation READ viewRotation WRITE setViewRotation
                   NOTIFY viewRotationChanged FINAL)
    Q_PROPERTY(PageDisplayMode pageDisplayMode READ pageDisplayMode
                   WRITE setPageDisplayMode NOTIFY pageDisplayModeChanged FINAL)
    Q_PROPERTY(bool separateCoverPage READ separateCoverPage
                   WRITE setSeparateCoverPage NOTIFY separateCoverPageChanged FINAL)
    Q_PROPERTY(SplitViewMode splitViewMode READ splitViewMode
                   WRITE setSplitViewMode NOTIFY splitViewModeChanged FINAL)
    Q_PROPERTY(bool splitViewEnabled READ splitViewEnabled
                   WRITE setSplitViewEnabled NOTIFY splitViewEnabledChanged FINAL)
    Q_PROPERTY(PDFViewState *secondaryViewState READ secondaryViewState CONSTANT FINAL)
    Q_PROPERTY(qreal splitScrollX READ splitScrollX WRITE setSplitScrollX
                   NOTIFY splitScrollPositionChanged FINAL)
    Q_PROPERTY(qreal splitScrollY READ splitScrollY WRITE setSplitScrollY
                   NOTIFY splitScrollPositionChanged FINAL)
    Q_PROPERTY(bool splitScrollPositionInitialized
                   READ splitScrollPositionInitialized
                   NOTIFY splitScrollPositionInitializedChanged FINAL)
    Q_PROPERTY(bool panelCollapsed READ panelCollapsed WRITE setPanelCollapsed
                   NOTIFY panelCollapsedChanged FINAL)
    Q_PROPERTY(bool thumbnailsVisible READ thumbnailsVisible WRITE setThumbnailsVisible
                   NOTIFY thumbnailsVisibleChanged FINAL)
    Q_PROPERTY(bool initialFitPending READ initialFitPending WRITE setInitialFitPending
                   NOTIFY initialFitPendingChanged FINAL)
    Q_PROPERTY(bool hasDrafts READ hasDrafts NOTIFY hasDraftsChanged FINAL)
    Q_PROPERTY(bool saving READ saving NOTIFY savingChanged FINAL)

public:
    enum PageDisplayMode {
        SinglePageMode,
        ContinuousMode,
        FacingMode,
        ContinuousFacingMode
    };
    Q_ENUM(PageDisplayMode)

    enum SplitViewMode {
        NoSplit,
        VerticalSplit,
        HorizontalSplit
    };
    Q_ENUM(SplitViewMode)

    explicit PDFDocumentSession(QObject *parent = nullptr);

    PDFDocument *document() const;
    PDFEditController *editController() const;
    PDFFormController *formController() const;
    PDFFormDesignerController *formDesignerController() const;
    PDFSearchController *searchController() const;
    PDFExcelExportController *excelExportController() const;
    QString title() const;
    QString filePath() const;
    qreal zoom() const;
    qreal scrollX() const;
    qreal scrollY() const;
    int viewRotation() const;
    PageDisplayMode pageDisplayMode() const;
    bool separateCoverPage() const;
    SplitViewMode splitViewMode() const;
    bool splitViewEnabled() const;
    PDFViewState *secondaryViewState() const;
    qreal splitScrollX() const;
    qreal splitScrollY() const;
    bool splitScrollPositionInitialized() const;
    bool panelCollapsed() const;
    bool thumbnailsVisible() const;
    bool initialFitPending() const;
    bool hasDrafts() const;
    bool saving() const;

    void setZoom(qreal zoom);
    void setScrollX(qreal scrollX);
    void setScrollY(qreal scrollY);
    void setViewRotation(int rotation);
    void setPageDisplayMode(PageDisplayMode mode);
    void setSeparateCoverPage(bool separate);
    void setSplitViewMode(SplitViewMode mode);
    void setSplitViewEnabled(bool enabled);
    void setSplitScrollX(qreal scrollX);
    void setSplitScrollY(qreal scrollY);
    Q_INVOKABLE void setSplitScrollPosition(qreal scrollX, qreal scrollY);
    void setPanelCollapsed(bool collapsed);
    void setThumbnailsVisible(bool visible);
    void setInitialFitPending(bool pending);

    bool open(const QUrl &source, const QString &password = QString());
    Q_INVOKABLE void rotateView(int quarterTurns);
    Q_INVOKABLE void prepareForDeactivation();

signals:
    void titleChanged();
    void filePathChanged();
    void zoomChanged();
    void scrollPositionChanged();
    void viewRotationChanged();
    void pageDisplayModeChanged();
    void separateCoverPageChanged();
    void splitViewModeChanged();
    void splitViewEnabledChanged();
    void splitScrollPositionChanged();
    void splitScrollPositionInitializedChanged();
    void panelCollapsedChanged();
    void thumbnailsVisibleChanged();
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
    PDFExcelExportController *m_excelExportController = nullptr;
    PDFViewState *m_secondaryViewState = nullptr;
    QString m_title;
    QString m_filePath;
    qreal m_zoom = 1.0;
    qreal m_scrollX = 0.0;
    qreal m_scrollY = 0.0;
    int m_viewRotation = 0;
    PageDisplayMode m_pageDisplayMode = ContinuousMode;
    bool m_separateCoverPage = false;
    SplitViewMode m_splitViewMode = NoSplit;
    bool m_panelCollapsed = false;
    bool m_thumbnailsVisible = true;
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

    Q_INVOKABLE PDFDocumentSession *open(const QUrl &source,
                                       const QString &password = QString());
    Q_INVOKABLE PDFDocumentSession *sessionAt(int index) const;
    Q_INVOKABLE int indexOf(PDFDocumentSession *session) const;
    Q_INVOKABLE bool closeSession(PDFDocumentSession *session);
    Q_INVOKABLE bool requestSave(PDFDocumentSession *session);
    Q_INVOKABLE bool requestSaveAs(PDFDocumentSession *session, const QUrl &target);
    Q_INVOKABLE bool requestExcelExport(PDFDocumentSession *session, const QUrl &target);
    Q_INVOKABLE QString mergeValidationError(const QList<QUrl> &sources, const QUrl &target) const;
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
    void passwordRequested(const QUrl &source, bool incorrectPassword);
    void fontDownloadRequested(PDFDocumentSession *session);
    void fontDownloadFailed(PDFDocumentSession *session, const QString &message);
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
