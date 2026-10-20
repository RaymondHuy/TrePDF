#include "PDFTabModel.h"

#include <QDir>
#include <QFileInfo>
#include <QtTranslation>

#include <filesystem>

PDFViewState::PDFViewState(QObject *parent)
    : QObject(parent)
{
}

qreal PDFViewState::zoom() const
{
    return m_zoom;
}

qreal PDFViewState::scrollX() const
{
    return m_scrollX;
}

qreal PDFViewState::scrollY() const
{
    return m_scrollY;
}

int PDFViewState::currentPage() const
{
    return m_currentPage;
}

bool PDFViewState::positionInitialized() const
{
    return m_positionInitialized;
}

void PDFViewState::setZoom(qreal zoom)
{
    const qreal clampedZoom = qBound(0.1, zoom, 5.0);
    if (qFuzzyCompare(m_zoom, clampedZoom)) {
        return;
    }

    m_zoom = clampedZoom;
    emit zoomChanged();
}

void PDFViewState::setScrollX(qreal scrollX)
{
    setScrollPosition(scrollX, m_scrollY);
}

void PDFViewState::setScrollY(qreal scrollY)
{
    setScrollPosition(m_scrollX, scrollY);
}

void PDFViewState::setCurrentPage(int page)
{
    const int clampedPage = qMax(0, page);
    if (m_currentPage == clampedPage) {
        return;
    }

    m_currentPage = clampedPage;
    emit currentPageChanged();
}

void PDFViewState::setScrollPosition(qreal scrollX, qreal scrollY)
{
    const qreal clampedScrollX = qMax<qreal>(0.0, scrollX);
    const qreal clampedScrollY = qMax<qreal>(0.0, scrollY);
    const bool positionChanged
        = !qFuzzyCompare(m_scrollX + 1.0, clampedScrollX + 1.0)
        || !qFuzzyCompare(m_scrollY + 1.0, clampedScrollY + 1.0);
    const bool initializedChanged = !m_positionInitialized;

    m_scrollX = clampedScrollX;
    m_scrollY = clampedScrollY;
    m_positionInitialized = true;
    if (positionChanged) {
        emit scrollPositionChanged();
    }
    if (initializedChanged) {
        emit positionInitializedChanged();
    }
}

PDFDocumentSession::PDFDocumentSession(QObject *parent)
    : QObject(parent)
    , m_document(new PDFDocument(this))
    , m_editController(new PDFEditController(this))
    , m_formController(new PDFFormController(this))
    , m_formDesignerController(new PDFFormDesignerController(this))
    , m_searchController(new PDFSearchController(this))
    , m_excelExportController(new PDFExcelExportController(this))
    , m_ocrController(new PDFOCRController(this))
    , m_secondaryViewState(new PDFViewState(this))
{
    m_formDesignerController->setDocument(m_document);
    m_formController->setDocument(m_document);
    m_formDesignerController->setFormController(m_formController);
    m_editController->setDocument(m_document);
    m_editController->setFormController(m_formController);
    m_editController->setFormDesignerController(m_formDesignerController);
    m_searchController->setDocument(m_document);
    m_excelExportController->setDocument(m_document);
    m_excelExportController->setExportGuard([this]() {
        prepareForDeactivation();
        return !hasDrafts() && !saving();
    });
    connect(m_editController, &PDFEditController::historyStateChanged,
            m_excelExportController, &PDFExcelExportController::cancel);
    connect(m_editController, &PDFEditController::savingChanged,
            m_excelExportController, &PDFExcelExportController::cancel);
    connect(m_excelExportController, &PDFExcelExportController::statusMessage,
            this, &PDFDocumentSession::statusMessage);
    m_ocrController->setDocument(m_document);
    m_ocrController->setRecognitionGuard([this]() {
        prepareForDeactivation();
        return !hasDrafts() && !saving();
    });
    connect(m_editController, &PDFEditController::historyStateChanged,
            m_ocrController, &PDFOCRController::cancel);
    connect(m_editController, &PDFEditController::savingChanged,
            m_ocrController, &PDFOCRController::cancel);
    connect(m_editController, &PDFEditController::passwordProtectionChanged,
            m_ocrController, &PDFOCRController::cancel);
    connect(m_ocrController, &PDFOCRController::statusMessage,
            this, &PDFDocumentSession::statusMessage);
    connect(this, &PDFDocumentSession::hasDraftsChanged, m_ocrController, [this]() {
        if (hasDrafts())
            m_ocrController->cancel();
    });
    connect(
        m_editController,
        &PDFEditController::historyStateChanged,
        m_searchController,
        &PDFSearchController::clearSearch);

    connect(m_document,
            &PDFDocument::sourceChanged,
            this,
            &PDFDocumentSession::refreshSourceState);
    connect(m_editController,
            &PDFEditController::historyStateChanged,
            this,
            [this]() {
                emit hasDraftsChanged();
            });
    connect(m_editController,
            &PDFEditController::savingChanged,
            this,
            &PDFDocumentSession::savingChanged);
    connect(m_editController,
            &PDFEditController::statusMessage,
            this,
            &PDFDocumentSession::statusMessage);
    connect(m_editController,
            &PDFEditController::saveFinished,
            this,
            &PDFDocumentSession::saveFinished);
    connect(
        m_formController,
        &PDFFormController::externalLinkRequested,
        this,
        &PDFDocumentSession::externalLinkRequested);
    connect(
        m_formController,
        &PDFFormController::externalLinkRequestCancelled,
        this,
        &PDFDocumentSession::externalLinkRequestCancelled);
    connect(
        m_formController,
        &PDFFormController::javascriptEnableFailed,
        this,
        [this]() {
            //% "Some automatic form features are unavailable in “%1”."
            emit statusMessage(
                qtTrId("AutomaticFormFeaturesUnavailableForDocument")
                    .arg(m_title));
        });
    connect(m_secondaryViewState,
            &PDFViewState::scrollPositionChanged,
            this,
            &PDFDocumentSession::splitScrollPositionChanged);
    connect(m_secondaryViewState,
            &PDFViewState::positionInitializedChanged,
            this,
            &PDFDocumentSession::splitScrollPositionInitializedChanged);
}

PDFDocument *PDFDocumentSession::document() const
{
    return m_document;
}

PDFEditController *PDFDocumentSession::editController() const
{
    return m_editController;
}

PDFFormController *PDFDocumentSession::formController() const
{
    return m_formController;
}

PDFFormDesignerController *PDFDocumentSession::formDesignerController() const
{
    return m_formDesignerController;
}

PDFSearchController *PDFDocumentSession::searchController() const
{
    return m_searchController;
}

PDFExcelExportController *PDFDocumentSession::excelExportController() const
{
    return m_excelExportController;
}

PDFOCRController *PDFDocumentSession::ocrController() const
{
    return m_ocrController;
}

QString PDFDocumentSession::title() const
{
    return m_title;
}

QString PDFDocumentSession::filePath() const
{
    return m_filePath;
}

qreal PDFDocumentSession::zoom() const
{
    return m_zoom;
}

qreal PDFDocumentSession::scrollX() const
{
    return m_scrollX;
}

qreal PDFDocumentSession::scrollY() const
{
    return m_scrollY;
}

int PDFDocumentSession::viewRotation() const
{
    return m_viewRotation;
}

PDFDocumentSession::PageDisplayMode PDFDocumentSession::pageDisplayMode() const
{
    return m_pageDisplayMode;
}

bool PDFDocumentSession::separateCoverPage() const
{
    return m_separateCoverPage;
}

PDFDocumentSession::SplitViewMode PDFDocumentSession::splitViewMode() const
{
    return m_splitViewMode;
}

bool PDFDocumentSession::splitViewEnabled() const
{
    return m_splitViewMode != NoSplit;
}

PDFViewState *PDFDocumentSession::secondaryViewState() const
{
    return m_secondaryViewState;
}

qreal PDFDocumentSession::splitScrollX() const
{
    return m_secondaryViewState->scrollX();
}

qreal PDFDocumentSession::splitScrollY() const
{
    return m_secondaryViewState->scrollY();
}

bool PDFDocumentSession::splitScrollPositionInitialized() const
{
    return m_secondaryViewState->positionInitialized();
}

bool PDFDocumentSession::panelCollapsed() const
{
    return m_panelCollapsed;
}

bool PDFDocumentSession::thumbnailsVisible() const
{
    return m_thumbnailsVisible;
}

bool PDFDocumentSession::initialFitPending() const
{
    return m_initialFitPending;
}

bool PDFDocumentSession::hasDrafts() const
{
    return m_editController->hasDrafts();
}

bool PDFDocumentSession::saving() const
{
    return m_editController->saving();
}

void PDFDocumentSession::setZoom(qreal zoom)
{
    const qreal clampedZoom = qBound(0.1, zoom, 5.0);
    if (qFuzzyCompare(m_zoom, clampedZoom)) {
        return;
    }

    m_zoom = clampedZoom;
    emit zoomChanged();
}

void PDFDocumentSession::setScrollX(qreal scrollX)
{
    const qreal clampedScrollX = qMax<qreal>(0.0, scrollX);
    if (qFuzzyCompare(m_scrollX + 1.0, clampedScrollX + 1.0)) {
        return;
    }

    m_scrollX = clampedScrollX;
    emit scrollPositionChanged();
}

void PDFDocumentSession::setScrollY(qreal scrollY)
{
    const qreal clampedScrollY = qMax<qreal>(0.0, scrollY);
    if (qFuzzyCompare(m_scrollY + 1.0, clampedScrollY + 1.0)) {
        return;
    }

    m_scrollY = clampedScrollY;
    emit scrollPositionChanged();
}

void PDFDocumentSession::setViewRotation(int rotation)
{
    const int quarterTurns = qRound(static_cast<qreal>(rotation) / 90.0);
    const int normalizedRotation = ((quarterTurns % 4) + 4) % 4 * 90;
    if (m_viewRotation == normalizedRotation) {
        return;
    }

    m_viewRotation = normalizedRotation;
    emit viewRotationChanged();
}

void PDFDocumentSession::setPageDisplayMode(PageDisplayMode mode)
{
    switch (mode) {
    case SinglePageMode:
    case ContinuousMode:
    case FacingMode:
    case ContinuousFacingMode:
        break;
    default:
        return;
    }

    if (m_pageDisplayMode == mode) {
        return;
    }

    m_pageDisplayMode = mode;
    emit pageDisplayModeChanged();
}

void PDFDocumentSession::setSeparateCoverPage(bool separate)
{
    if (m_separateCoverPage == separate) {
        return;
    }

    m_separateCoverPage = separate;
    emit separateCoverPageChanged();
}

void PDFDocumentSession::setSplitViewMode(SplitViewMode mode)
{
    switch (mode) {
    case NoSplit:
    case VerticalSplit:
    case HorizontalSplit:
        break;
    default:
        return;
    }

    if (m_splitViewMode == mode) {
        return;
    }

    const bool wasEnabled = splitViewEnabled();
    m_splitViewMode = mode;
    emit splitViewModeChanged();
    if (wasEnabled != splitViewEnabled()) {
        emit splitViewEnabledChanged();
    }
}

void PDFDocumentSession::setSplitViewEnabled(bool enabled)
{
    setSplitViewMode(enabled
                         ? (m_splitViewMode == NoSplit
                                ? VerticalSplit
                                : m_splitViewMode)
                         : NoSplit);
}

void PDFDocumentSession::setSplitScrollX(qreal scrollX)
{
    m_secondaryViewState->setScrollX(scrollX);
}

void PDFDocumentSession::setSplitScrollY(qreal scrollY)
{
    m_secondaryViewState->setScrollY(scrollY);
}

void PDFDocumentSession::setSplitScrollPosition(qreal scrollX, qreal scrollY)
{
    m_secondaryViewState->setScrollPosition(scrollX, scrollY);
}

void PDFDocumentSession::setPanelCollapsed(bool collapsed)
{
    if (m_panelCollapsed == collapsed) {
        return;
    }

    m_panelCollapsed = collapsed;
    emit panelCollapsedChanged();
}

void PDFDocumentSession::setInitialFitPending(bool pending)
{
    if (m_initialFitPending == pending) {
        return;
    }

    m_initialFitPending = pending;
    emit initialFitPendingChanged();
}

void PDFDocumentSession::setThumbnailsVisible(bool visible)
{
    if (m_thumbnailsVisible == visible)
        return;
    m_thumbnailsVisible = visible;
    emit thumbnailsVisibleChanged();
}

bool PDFDocumentSession::open(const QUrl &source, const QString &password)
{
    if (!m_document->open(source, password)) {
        return false;
    }

    m_scrollX = 0.0;
    m_scrollY = 0.0;
    setViewRotation(0);
    m_initialFitPending = true;
    emit scrollPositionChanged();
    emit initialFitPendingChanged();
    return true;
}

void PDFDocumentSession::rotateView(int quarterTurns)
{
    setViewRotation(m_viewRotation + quarterTurns * 90);
}

void PDFDocumentSession::prepareForDeactivation()
{
    m_editController->cancelTextEditPreparation();
    m_editController->finishSelectedFrameResize();
    m_editController->finishSelectedTextEdit();
    m_formDesignerController->clearTextFieldFocus();
    m_formDesignerController->clearTextFieldSelection();
    m_formController->killFocus();
}

void PDFDocumentSession::refreshSourceState()
{
    const QString newFilePath = m_document->source().isLocalFile()
        ? QFileInfo(m_document->source().toLocalFile()).absoluteFilePath()
        : QString();
    if (m_filePath != newFilePath) {
        m_filePath = newFilePath;
        m_scrollX = 0.0;
        m_scrollY = 0.0;
        emit filePathChanged();
        emit scrollPositionChanged();
    }

    const QString newTitle = m_filePath.isEmpty()
        ? QString()
        : QFileInfo(m_filePath).fileName();
    if (m_title != newTitle) {
        m_title = newTitle;
        emit titleChanged();
    }
}

PDFTabModel::PDFTabModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int PDFTabModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_sessions.size();
}

QVariant PDFTabModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_sessions.size()) {
        return {};
    }

    return role == SessionRole
        ? QVariant::fromValue(m_sessions.at(index.row()))
        : QVariant();
}

QHash<int, QByteArray> PDFTabModel::roleNames() const
{
    return {{SessionRole, QByteArrayLiteral("session")}};
}

int PDFTabModel::count() const
{
    return m_sessions.size();
}

int PDFTabModel::currentIndex() const
{
    return m_currentIndex;
}

PDFDocumentSession *PDFTabModel::currentSession() const
{
    return sessionAt(m_currentIndex);
}

bool PDFTabModel::anySaving() const
{
    for (PDFDocumentSession *session : m_sessions) {
        if (session->saving()) {
            return true;
        }
    }
    return false;
}

bool PDFTabModel::hasDrafts() const
{
    return firstDraftSession() != nullptr;
}

PDFDocumentSession *PDFTabModel::open(const QUrl &source, const QString &password)
{
    if (anySaving()) {
        //% "Please wait for the PDF to finish saving."
        emit statusMessage(qtTrId("WaitForPdfSave"));
        return nullptr;
    }

    // A destination may have been selected before it was opened in a tab.
    // Cancel under the worker's commit mutex before exposing the old file; if
    // commit already won, opening reads the completed OCR file instead.
    cancelOCRForDestination(source);
    const QString identity = fileIdentity(source);
    if (!identity.isEmpty()) {
        for (int index = 0; index < m_sessions.size(); ++index) {
            if (fileIdentity(m_sessions.at(index)->document()->source()) == identity) {
                setCurrentIndex(index);
                return m_sessions.at(index);
            }
        }
    }

    auto *session = new PDFDocumentSession(this);
    if (!session->open(source, password)) {
        const bool requiresPassword = session->document()->passwordRequired();
        const QString status = session->document()->statusText();
        delete session;
        if (requiresPassword)
            emit passwordRequested(source, !password.isEmpty());
        else
            emit statusMessage(status);
        return nullptr;
    }

    const int insertionIndex = m_sessions.size();
    beginInsertRows(QModelIndex(), insertionIndex, insertionIndex);
    m_sessions.append(session);
    connectSession(session);
    endInsertRows();
    emit countChanged();
    setCurrentIndex(insertionIndex);
    return session;
}

PDFDocumentSession *PDFTabModel::sessionAt(int index) const
{
    return index >= 0 && index < m_sessions.size()
        ? m_sessions.at(index)
        : nullptr;
}

int PDFTabModel::indexOf(PDFDocumentSession *session) const
{
    return m_sessions.indexOf(session);
}

bool PDFTabModel::closeSession(PDFDocumentSession *session)
{
    const int index = indexOf(session);
    if (index < 0 || session->saving() || anySaving()) {
        return false;
    }

    session->prepareForDeactivation();
    if (session->hasDrafts()) {
        return false;
    }
    session->excelExportController()->cancel();
    session->ocrController()->cancel();
    const int oldCurrentIndex = m_currentIndex;
    const bool removingCurrentSession = index == oldCurrentIndex;

    beginRemoveRows(QModelIndex(), index, index);
    m_sessions.removeAt(index);
    if (m_sessions.isEmpty()) {
        m_currentIndex = -1;
    } else if (removingCurrentSession) {
        m_currentIndex = qMin(index, m_sessions.size() - 1);
    } else if (index < oldCurrentIndex) {
        m_currentIndex = oldCurrentIndex - 1;
    }
    endRemoveRows();

    emit countChanged();
    if (m_currentIndex != oldCurrentIndex || removingCurrentSession) {
        emit currentIndexChanged();
    }
    if (removingCurrentSession) {
        emit currentSessionChanged();
    }
    emit hasDraftsChanged();
    session->deleteLater();
    return true;
}

bool PDFTabModel::requestSave(PDFDocumentSession *session)
{
    if (!contains(session) || anySaving()) {
        return false;
    }
    return session->editController()->requestSave();
}

bool PDFTabModel::requestSaveAs(PDFDocumentSession *session, const QUrl &target)
{
    if (!contains(session) || anySaving()) {
        return false;
    }

    const QString targetIdentity = fileIdentity(target);
    if (!targetIdentity.isEmpty()) {
        for (PDFDocumentSession *otherSession : m_sessions) {
            if (otherSession != session
                && fileIdentity(otherSession->document()->source()) == targetIdentity) {
                //% "Another open tab is already using that PDF file."
                emit statusMessage(qtTrId("CannotSaveOverOpenPdf"));
                return false;
            }
        }
    }

    cancelOCRForDestination(target);
    return session->editController()->requestSaveAs(target);
}

bool PDFTabModel::requestExcelExport(PDFDocumentSession *session, const QUrl &target)
{
    if (!contains(session) || anySaving())
        return false;
    const QString targetIdentity = fileIdentity(target);
    for (PDFDocumentSession *other : m_sessions) {
        if (!targetIdentity.isEmpty()
            && targetIdentity == fileIdentity(other->document()->source())) {
            //% "Choose a destination that is not used by an open PDF."
            emit statusMessage(qtTrId("ExcelExportDestinationInUse"));
            return false;
        }
    }
    return session->excelExportController()->exportTo(target);
}

bool PDFTabModel::requestOCR(PDFDocumentSession *session, const QUrl &target,
                            const QString &language, const QString &pageRange)
{
    if (!contains(session) || anySaving())
        return false;
    return session->ocrController()->recognizeTo(target, language, pageRange);
}

QString PDFTabModel::mergeValidationError(const QList<QUrl> &sources, const QUrl &target) const
{
    if (anySaving()) {
        //% "Please wait for the PDF to finish saving."
        return qtTrId("WaitForPdfSave");
    }
    const QString targetIdentity = fileIdentity(target);
    QStringList sourceIdentities;
    for (const QUrl &source : sources)
        sourceIdentities.append(fileIdentity(source));
    for (const PDFDocumentSession *session : m_sessions) {
        const QString identity = fileIdentity(session->document()->source());
        if (identity.isEmpty())
            continue;
        if (identity == targetIdentity) {
            //% "This PDF is open in another tab. Choose a different name for the merged file."
            return qtTrId("MergePdfDestinationOpen");
        }
        if (sourceIdentities.contains(identity) && session->hasDrafts()) {
            //% "Save the changes in “%1” before merging so the new PDF includes your latest edits."
            return qtTrId("MergePdfSaveSourceFirst").arg(session->title());
        }
    }
    return {};
}

QString PDFTabModel::splitValidationError(const QUrl &source) const
{
    if (anySaving()) {
        //% "Please wait for the PDF to finish saving."
        return qtTrId("WaitForPdfSave");
    }
    const QString identity = fileIdentity(source);
    if (identity.isEmpty())
        return {};
    for (const PDFDocumentSession *session : m_sessions) {
        if (!session->hasDrafts())
            continue;
        const QString openIdentity = fileIdentity(session->document()->source());
        bool sameSource = identity == openIdentity;
        if (!sameSource && !openIdentity.isEmpty()) {
            // Canonical paths resolve symlinks; filesystem identity also catches
            // a second hard link to a document with pending form/content input.
            std::error_code error;
            sameSource = std::filesystem::equivalent(
                std::filesystem::u8path(identity.toUtf8().constData()),
                std::filesystem::u8path(openIdentity.toUtf8().constData()), error) && !error;
        }
        if (sameSource) {
            //% "Save the changes in “%1” before splitting so the new PDFs include your latest edits."
            return qtTrId("SplitPdfSaveSourceFirst").arg(session->title());
        }
    }
    return {};
}

PDFDocumentSession *PDFTabModel::firstDraftSession() const
{
    for (PDFDocumentSession *session : m_sessions) {
        if (session->hasDrafts()) {
            return session;
        }
    }
    return nullptr;
}

void PDFTabModel::finishAllEdits()
{
    for (PDFDocumentSession *session : m_sessions) {
        session->prepareForDeactivation();
    }
}

void PDFTabModel::setCurrentIndex(int index)
{
    if (index < 0 || index >= m_sessions.size() || index == m_currentIndex || anySaving()) {
        return;
    }

    if (PDFDocumentSession *oldSession = currentSession()) {
        oldSession->prepareForDeactivation();
    }
    m_currentIndex = index;
    emit currentIndexChanged();
    emit currentSessionChanged();
}

QString PDFTabModel::fileIdentity(const QUrl &source)
{
    if (!source.isLocalFile() || source.toLocalFile().isEmpty()) {
        return {};
    }

    const QFileInfo fileInfo(source.toLocalFile());
    const QString canonicalPath = fileInfo.canonicalFilePath();
    return canonicalPath.isEmpty()
        ? QDir::cleanPath(fileInfo.absoluteFilePath())
        : canonicalPath;
}

bool PDFTabModel::contains(PDFDocumentSession *session) const
{
    return session && m_sessions.contains(session);
}

void PDFTabModel::cancelOCRForDestination(const QUrl &target)
{
    const QString identity = fileIdentity(target);
    if (identity.isEmpty())
        return;
    for (PDFDocumentSession *session : m_sessions) {
        if (session->ocrController()->busy()
            && fileIdentity(session->ocrController()->target()) == identity)
            session->ocrController()->cancel();
    }
}

void PDFTabModel::connectSession(PDFDocumentSession *session)
{
    session->ocrController()->setDestinationGuard([this](const QUrl &target) {
        const QString identity = fileIdentity(target);
        for (PDFDocumentSession *other : m_sessions) {
            if (!identity.isEmpty() && identity == fileIdentity(other->document()->source()))
                return false;
        }
        return !anySaving();
    });
    connect(session->ocrController(), &PDFOCRController::finished,
            this, [this, session](bool succeeded, const QUrl &target) {
                if (contains(session))
                    emit sessionOCRFinished(session, succeeded, target);
            });
    connect(session->editController(), &PDFEditController::fontDownloadFailed,
            this, [this, session](const QString &message) {
                if (contains(session))
                    emit fontDownloadFailed(session, message);
            });
    connect(session->editController(), &PDFEditController::fontDownloadRequested,
            this, [this, session]() {
                if (contains(session))
                    emit fontDownloadRequested(session);
            });
    connect(session,
            &PDFDocumentSession::statusMessage,
            this,
            &PDFTabModel::statusMessage);
    connect(session,
            &PDFDocumentSession::hasDraftsChanged,
            this,
            &PDFTabModel::hasDraftsChanged);
    connect(session,
            &PDFDocumentSession::savingChanged,
            this,
            &PDFTabModel::anySavingChanged);
    connect(session,
            &PDFDocumentSession::saveFinished,
            this,
            [this, session](bool succeeded) {
                emit sessionSaveFinished(session, succeeded);
            });
    connect(
        session,
        &PDFDocumentSession::externalLinkRequested,
        this,
        [this, session](
            quint64 requestId,
            const QString &authority,
            const QString &displayUrl,
            bool insecureHttp) {
            if (!contains(session)) {
                return;
            }
            emit externalLinkRequested(
                session,
                requestId,
                authority,
                displayUrl,
                insecureHttp);
        });
    connect(
        session,
        &PDFDocumentSession::externalLinkRequestCancelled,
        this,
        [this, session](quint64 requestId) {
            if (contains(session)) {
                emit externalLinkRequestCancelled(
                    session,
                    requestId);
            }
        });
}
