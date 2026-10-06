#include "PDFMemoryTelemetry.h"

#if TREPDF_ENABLE_MEMORY_TELEMETRY
#include "PDFEditController.h"
#include "PDFPageItem.h"
#include "PDFSearchController.h"
#include "PDFTabModel.h"
#include <QCoreApplication>
#include <QSet>
#include <QThread>
#include <QTimer>

namespace {
// Read-only buffer identities prevent counting Qt implicit-sharing copies over
// and over. No data(), bits(), detach(), PDFium calls or resource ownership here.
struct Estimate {
    qint64 bytes = 0;
    qint64 imageBytes = 0;
    QSet<const void *> buffers;

    bool storage(const void *identity, qint64 size)
    {
        if (!identity || size <= 0 || buffers.contains(identity))
            return false;
        buffers.insert(identity);
        bytes += size;
        return true;
    }
    void string(const QString &s) { storage(s.constData(), qint64(s.capacity()) * sizeof(QChar)); }
    void data(const QByteArray &s) { storage(s.constData(), s.capacity()); }
    void image(const QImage &image)
    {
        const qint64 size = qint64(image.bytesPerLine()) * image.height();
        if (storage(image.constBits(), size))
            imageBytes += size;
    }
    template<typename T, typename Visit>
    void vector(const QVector<T> &v, Visit visit)
    {
        if (storage(v.constData(), qint64(v.capacity()) * sizeof(T)))
            for (const auto &entry : v)
                visit(entry);
    }
    template<typename T> void vector(const QVector<T> &v)
    {
        vector(v, [](const auto &) {});
    }
    template<typename K, typename V, typename Visit>
    void hash(const QHash<K, V> &h, Visit visit)
    {
        // QHash has no public buffer address. A const first-entry address is a
        // stable shared-storage identity for this synchronous inspection only.
        const void *identity = h.isEmpty() ? static_cast<const void *>(&h) : &*h.cbegin();
        if (storage(identity, qint64(h.capacity()) * (sizeof(K) + sizeof(V))))
            for (auto it = h.cbegin(); it != h.cend(); ++it)
                visit(it.key(), it.value());
    }
    template<typename T> void set(const QSet<T> &s)
    {
        const void *identity = s.isEmpty() ? static_cast<const void *>(&s) : &*s.cbegin();
        storage(identity, qint64(s.capacity()) * sizeof(T));
    }
    void properties(const PDFCommentAnnotation::CommonProperties &p)
    {
        string(p.subject); string(p.intent); string(p.name); vector(p.dashPattern);
    }
    void watermarks(const QVector<PDFWatermarks::Change> &changes)
    {
        vector(changes, [&](const auto &change) {
            string(change.spec.id); string(change.spec.text); string(change.spec.fontFamily);
            image(change.spec.image);
            set(change.pages);
            hash(change.originals, [&](int, const auto &fingerprint) { data(fingerprint); });
            // QPainterPath does not expose buffer capacity/identity; its opaque
            // allocation is deliberately excluded instead of guessed.
        });
    }
    void links(const QVector<PDFWebLinks::Link> &links)
    {
        vector(links, [&](const auto &link) { string(link.url); vector(link.rects); });
    }
    void links(const QVector<PDFWebLinks::SourceLink> &links)
    {
        vector(links, [&](const auto &link) { data(link.fingerprint); });
    }
    void links(const QVector<PDFWebLinks::ManualLink> &links)
    {
        vector(links, [&](const auto &link) {
            string(link.id); string(link.url); data(link.fingerprint);
        });
    }
};

template<typename T> void removeOnDestruction(T *owner)
{
    QObject::connect(owner, &QObject::destroyed, [owner]() {
        PDFMemoryTelemetry::removeSource(owner);
    });
}
} // namespace

void PDFMemoryTelemetry::track(PDFEditController *c)
{
    auto update = [c]() { updateEditController(c); };
    connect(c, &PDFEditController::historyStateChanged, c, update);
    connect(c, &PDFEditController::textEditActiveChanged, c, update);
    connect(c, &PDFEditController::commentsChanged, c, update);
    connect(c, &PDFEditController::pageCandidatesReady, c, update);
    connect(c, &PDFEditController::documentChanged, c, update);
    connect(c, &PDFEditController::watermarksChanged, c, update);
    connect(c, &PDFEditController::editableLinksChanged, c, update);
    removeOnDestruction(c);
    update();
}

void PDFMemoryTelemetry::updateEditController(PDFEditController *c)
{
    Q_ASSERT(c->thread() == QThread::currentThread());
    using Controller = PDFEditController;
    Values values{};
    Estimate e;
    auto spans = [&](const QVector<Controller::CharacterSpan> &v) {
        e.vector(v, [&](const auto &span) { e.string(span.format.fontFamily); });
    };
    auto candidate = [&](const Controller::Candidate &v) {
        e.vector(v.objectPath);
        e.vector(v.parts, [&](const auto &part) {
            e.string(part.objectId); e.string(part.text); e.string(part.separatorAfter);
            e.vector(part.objectPath); e.vector(part.underlineObjectPath);
        });
        e.vector(v.textGlyphRects); e.vector(v.pendingSourcePositions);
        spans(v.sourceCharacterSpans); spans(v.spacingRuns);
        e.vector(v.sourceParagraphFormats); e.vector(v.sourceParagraphLineBreaks);
        e.vector(v.highlightRects); e.vector(v.textMarkupQuads); e.vector(v.drawingVertices);
        e.vector(v.inkStrokes, [&](const auto &stroke) { e.vector(stroke); });
        e.image(v.image); e.properties(v.commentProperties);
        e.string(v.objectId); e.string(v.sourceWidgetId); e.string(v.kind); e.string(v.label);
        e.string(v.text); e.string(v.paragraphLayoutGroup); e.string(v.paragraphLayoutText);
        e.string(v.spacingGroup); e.string(v.spacingText); e.string(v.sourceAnnotationName);
        e.string(v.commentAuthor); e.string(v.commentThreadId); e.string(v.replyToObjectId);
        e.string(v.replyToThreadId); e.string(v.commentState); e.string(v.commentStateModel);
        e.string(v.commentStateAuthor); e.string(v.fontFamily); e.string(v.fontIdentity);
        e.string(v.stampText); e.string(v.stampDetail); e.string(v.stampArtwork); e.string(v.reason);
        e.string(v.pendingFontExpectedFamily); e.string(v.pendingBundledFontFamily);
        e.string(v.pendingSystemFontFamily);
        e.data(v.paragraphLayoutModel); e.data(v.sourceShapeMetadata);
        e.data(v.sourceDrawingAppearanceFingerprint); e.data(v.pendingEmbeddedFontData);
    };
    auto draft = [&](const Controller::Draft &v) {
        candidate(v.candidate); e.properties(v.commentProperties); e.string(v.text);
        spans(v.characterSpans); e.vector(v.paragraphFormats); e.vector(v.sourceLineBreaks);
        e.string(v.commentStateAuthor);
    };
    auto drafts = [&](const QHash<QString, Controller::Draft> &v) {
        e.hash(v, [&](const auto &key, const auto &value) { e.string(key); draft(value); });
    };
    auto pageState = [&](const Controller::PageState &v) {
        e.set(v.deletedSourcePages); e.vector(v.pageInsertions);
        e.vector(v.addedCandidates, candidate); drafts(v.drafts); candidate(v.selectedCandidate);
    };
    auto command = [&](const Controller::Command &v) {
        e.string(v.key); e.string(v.objectId);
        draft(v.before); draft(v.after);
        pageState(v.pageStateBefore); pageState(v.pageStateAfter);
        e.links(v.webLinksBefore); e.links(v.webLinksAfter);
        e.links(v.removedWebLinksBefore); e.links(v.removedWebLinksAfter);
        e.links(v.manualLinksBefore); e.links(v.manualLinksAfter);
        e.watermarks(v.watermarksBefore); e.watermarks(v.watermarksAfter);
    };

    QSet<int> pages;
    auto candidatePage = [&](int page, const auto &entries) {
        pages.insert(page); e.vector(entries, candidate);
    };
    e.hash(c->m_pageCandidates, candidatePage);
    e.hash(c->m_indexedCommentCandidates, candidatePage);
    e.vector(c->m_pageCandidateLru);
    values[CandidatePageCount] = pages.size();
    values[CandidateBytes] = e.bytes;

    // Attribute shared buffers once, in this order: cache, drafts, undo, redo.
    // In particular a 10-MiB image shared by 100 commands is not 1 GiB of undo.
    qint64 before = e.bytes;
    qint64 imagesBefore = e.imageBytes;
    drafts(c->m_drafts);
    e.vector(c->m_addedCandidates, candidate);
    e.vector(c->m_pageInsertions); e.set(c->m_deletedSourcePages);
    e.links(c->m_autoWebLinks); e.links(c->m_manualLinks); e.links(c->m_removedWebLinks);
    e.watermarks(c->m_watermarkChanges);
    e.image(c->m_watermarkDialogImage);
    if (c->m_textEditActive) {
        draft(c->m_textEditDisplayDraft); command(c->m_textEditCommand);
    }
    values[DraftCount] = c->m_drafts.size() + c->m_pageInsertions.size()
        + c->m_deletedSourcePages.size() + c->m_autoWebLinks.size()
        + c->m_manualLinks.size() + c->m_removedWebLinks.size() + c->m_watermarkChanges.size();
    values[DraftBytes] = e.bytes - before;
    values[DraftImageBytes] = e.imageBytes - imagesBefore;
    before = e.bytes; imagesBefore = e.imageBytes;
    e.vector(c->m_undoStack, command);
    values[UndoCommandCount] = c->m_undoStack.size();
    values[UndoBytes] = e.bytes - before;
    values[UndoImageBytes] = e.imageBytes - imagesBefore;
    before = e.bytes; imagesBefore = e.imageBytes;
    e.vector(c->m_redoStack, command);
    values[RedoCommandCount] = c->m_redoStack.size();
    values[RedoBytes] = e.bytes - before;
    values[RedoImageBytes] = e.imageBytes - imagesBefore;
    setSource(c, values);
}

void PDFMemoryTelemetry::track(PDFSearchController *c)
{
    connect(c, &PDFSearchController::resultsChanged, c, [c]() { updateSearchController(c); });
    connect(c, &PDFSearchController::queryChanged, c, [c]() { updateSearchController(c); });
    removeOnDestruction(c);
    updateSearchController(c);
}

void PDFMemoryTelemetry::updateSearchController(PDFSearchController *c)
{
    Q_ASSERT(c->thread() == QThread::currentThread());
    Values values{};
    Estimate e;
    e.string(c->m_query);
    e.vector(c->m_matches, [&](const auto &match) { e.vector(match.rects); });
    for (const auto &match : std::as_const(c->m_matches))
        values[SearchRectangleCount] += match.rects.size();
    e.hash(c->m_matchIndicesByPage, [&](int, const auto &indices) { e.vector(indices); });
    values[SearchResultCount] = c->m_matches.size();
    values[SearchBytes] = e.bytes;
    setSource(c, values);
}

void PDFMemoryTelemetry::track(PDFDocumentSession *session)
{
    // A failed open creates and discards an empty session; it must not reset
    // the diagnostic history of documents that are already open.
    struct SessionState { bool loaded = false; bool everLoaded = false; };
    auto status = std::make_shared<SessionState>();
    auto resetAfterClose = []() {
        if (auto *app = QCoreApplication::instance())
            QTimer::singleShot(0, app, []() { resetAllPeaks(); });
    };
    auto update = [session, status, resetAfterClose]() {
        Values values{};
        const bool loaded = session->document()->isLoaded();
        values[SessionCount] = loaded ? 1 : 0;
        setSource(session, values);
        if (status->loaded && !loaded)
            resetAfterClose();
        status->loaded = loaded;
        status->everLoaded = status->everLoaded || loaded;
    };
    connect(session->document(), &PDFDocument::loadedChanged, session, update);
    connect(session, &QObject::destroyed, [session, status, resetAfterClose]() {
        removeSource(session);
        // Rebase after QObject children have torn down, to the resources still
        // alive in other sessions/workers/SG nodes. Never reset on Save or undo.
        if (status->everLoaded)
            resetAfterClose();
    });
    update();
}

void PDFMemoryTelemetry::track(PDFPageItem *item)
{
    auto update = [item]() {
        Values values{};
        values[VisiblePageItemCount] = item->isVisible() && item->window()
            && item->document() && item->document()->isLoaded()
            && !item->normalizedVisibleRect().isEmpty() ? 1 : 0;
        setSource(item, values);
    };
    connect(item, &PDFPageItem::documentChanged, item, update);
    connect(item, &PDFPageItem::visiblePageRectChanged, item, update);
    connect(item, &PDFPageItem::visibleChanged, item, update);
    connect(item, &PDFPageItem::widthChanged, item, update);
    connect(item, &PDFPageItem::heightChanged, item, update);
    connect(item, &PDFPageItem::implicitWidthChanged, item, update);
    connect(item, &PDFPageItem::implicitHeightChanged, item, update);
    connect(item, &PDFPageItem::windowChanged, item, update);
    // refreshMetrics follows the document's loaded/page-count state as well.
    connect(item, &PDFPageItem::pageChanged, item, update);
    removeOnDestruction(item);
    update();
}
#endif
