#include "PDFEditController.h"

#include "PDFFormController.h"
#include "PDFFormDesignerController.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QRunnable>
#include <QThreadPool>
#include <QtTranslation>

#include <utility>

namespace {

QThreadPool &webLinkThreadPool()
{
    static QThreadPool pool;
    static const bool configured = [] {
        pool.setMaxThreadCount(1);
        pool.setExpiryTimeout(30000);
        return true;
    }();
    Q_UNUSED(configured);
    return pool;
}

} // namespace

bool PDFEditController::hasNonWebLinkDrafts() const
{
    return !m_drafts.isEmpty() || !m_pendingTextMarkupCreations.isEmpty()
        || !m_pageInsertions.isEmpty() || !m_deletedSourcePages.isEmpty()
        || (m_formController && m_formController->hasDrafts())
        || (m_formDesignerController && m_formDesignerController->hasDrafts());
}

void PDFEditController::cancelWebLinkCreation()
{
    if (!m_webLinkCancellation)
        return;
    m_webLinkCancellation->store(true, std::memory_order_relaxed);
    m_webLinkCancellation.reset();
    emit creatingWebLinksChanged();
}

void PDFEditController::invalidateWebLinks()
{
    if (m_formController && m_pendingWebLinkRequest)
        m_formController->cancelExternalLink(m_pendingWebLinkRequest);
    m_pendingWebLinkRequest = 0;
    ++m_webLinksRevision;
    emit webLinksChanged();
}

bool PDFEditController::autoCreateWebLinks()
{
    if (!m_document || !m_document->isLoaded() || m_saving || creatingWebLinks())
        return false;
    if (!documentAllowsAnnotations()) {
        //% "This PDF does not permit changing annotations."
        emit statusMessage(qtTrId("PdfAnnotationsNotPermitted"));
        return false;
    }
    finishSelectedFrameResize();
    finishSelectedTextEdit();
    if (hasNonWebLinkDrafts()) {
        //% "Save your PDF changes before creating links from URLs."
        emit statusMessage(qtTrId("SaveBeforeCreatingWebLinks"));
        return false;
    }

    const QString path = m_document->renderFilePath();
    const int revision = m_document->renderRevision();
    const QPointer<PDFDocument> document = m_document;
    const QPointer<PDFEditController> controller(this);
    const auto cancellation = std::make_shared<std::atomic_bool>(false);
    m_webLinkCancellation = cancellation;
    auto *work = QRunnable::create([controller, document, path, revision, cancellation] {
        auto result = PDFWebLinks::scan(path, cancellation);
        if (cancellation->load(std::memory_order_relaxed))
            return;
        // Only inspect QObject guards on the GUI thread. The worker owns no
        // controller/document handle and the pool never queues obsolete scans.
        QMetaObject::invokeMethod(QCoreApplication::instance(),
            [controller, document, path, revision, cancellation,
             result = std::move(result)]() mutable {
                if (!controller || controller->m_webLinkCancellation != cancellation
                    || cancellation->load(std::memory_order_relaxed))
                    return;
                controller->m_webLinkCancellation.reset();
                emit controller->creatingWebLinksChanged();
                if (!document || controller->m_document != document
                    || document->renderRevision() != revision
                    || document->renderFilePath() != path
                    || controller->m_saving || controller->m_textEditActive
                    || controller->hasNonWebLinkDrafts())
                    return;
                if (result.failed) {
                    //% "The PDF could not be scanned for web links."
                    emit controller->statusMessage(qtTrId("WebLinkScanFailed"));
                    return;
                }

                Command command;
                command.kind = Command::Kind::WebLinks;
                command.webLinksBefore = controller->m_autoWebLinks;
                command.webLinksAfter = command.webLinksBefore;
                QHash<int, QVector<QRectF>> occupied;
                qsizetype rectangleCount = 0;
                for (const auto &link : std::as_const(command.webLinksBefore)) {
                    occupied[link.sourcePage] += link.rects;
                    rectangleCount += link.rects.size();
                }
                int created = 0;
                int comparisonsLeft = 2'000'000;
                for (const auto &link : std::as_const(result.links)) {
                    bool overlaps = false;
                    for (const QRectF &rect : link.rects) {
                        for (const QRectF &existing : std::as_const(occupied[link.sourcePage])) {
                            if (--comparisonsLeft < 0) {
                                result.limited = true;
                                break;
                            }
                            if (rect.intersects(existing)) {
                                overlaps = true;
                                break;
                            }
                        }
                        if (overlaps || comparisonsLeft < 0)
                            break;
                    }
                    if (comparisonsLeft < 0)
                        break;
                    if (overlaps)
                        continue;
                    if (occupied[link.sourcePage].size() + link.rects.size()
                        > PDFWebLinks::MaximumRectanglesPerPage) {
                        result.limited = true;
                        continue;
                    }
                    if (command.webLinksAfter.size() >= PDFWebLinks::MaximumLinks
                        || rectangleCount + link.rects.size() > PDFWebLinks::MaximumRectangles) {
                        result.limited = true;
                        break;
                    }
                    command.webLinksAfter.append(link);
                    occupied[link.sourcePage] += link.rects;
                    rectangleCount += link.rects.size();
                    ++created;
                }
                if (created) {
                    controller->m_autoWebLinks = command.webLinksAfter;
                    controller->m_undoStack.append(std::move(command));
                    controller->m_redoStack.clear();
                    controller->invalidateWebLinks();
                    emit controller->historyStateChanged();
                }
                if (result.limited) {
                    //% "Created %1 web links. Some content exceeded the scan limits. Save to keep the links."
                    emit controller->statusMessage(qtTrId("WebLinksCreatedLimited").arg(created));
                } else if (created) {
                    //% "Created %1 web links. Switch to View to open them, or Save to keep them."
                    emit controller->statusMessage(qtTrId("WebLinksCreated").arg(created));
                } else {
                    //% "No new web links were found. URLs need a text layer and may already have links."
                    emit controller->statusMessage(qtTrId("NoNewWebLinksFound"));
                }
            }, Qt::QueuedConnection);
    });
    if (!webLinkThreadPool().tryStart(work)) {
        delete work;
        m_webLinkCancellation.reset();
        //% "Another link scan is finishing. Please try again."
        emit statusMessage(qtTrId("WebLinkScannerBusy"));
        return false;
    }
    emit creatingWebLinksChanged();
    //% "Finding URLs in the document…"
    emit statusMessage(qtTrId("FindingWebLinks"));
    return true;
}

QVariantList PDFEditController::webLinkDraftsForPage(int page) const
{
    QVariantList items;
    if (!m_document || !m_document->isLoaded())
        return items;
    const int sourcePage = m_document->sourcePageIndex(page);
    if (sourcePage < 0)
        return items;
    for (qsizetype index = 0; index < m_autoWebLinks.size(); ++index) {
        const auto &link = m_autoWebLinks.at(index);
        if (link.sourcePage != sourcePage)
            continue;
        for (const QRectF &rect : link.rects) {
            items.append(QVariantMap{
                {QStringLiteral("index"), index},
                {QStringLiteral("url"), link.url},
                {QStringLiteral("rect"), rect},
                {QStringLiteral("revision"), QVariant::fromValue(m_webLinksRevision)}});
        }
    }
    return items;
}

bool PDFEditController::activateWebLink(int page, int index, quint64 revision)
{
    if (!m_document || !m_document->isLoaded() || !m_formController
        || m_formController->document() != m_document
        || m_saving || m_tool != BrowseTool || revision != m_webLinksRevision
        || index < 0 || index >= m_autoWebLinks.size())
        return false;
    const auto &link = m_autoWebLinks.at(index);
    if (m_document->sourcePageIndex(page) != link.sourcePage)
        return false;
    const quint64 request = m_formController->requestExternalWebLink(link.url);
    if (!request)
        return false;
    m_pendingWebLinkRequest = request;
    return true;
}
