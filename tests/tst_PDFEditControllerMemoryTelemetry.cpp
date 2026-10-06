#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"
#include "PDFMemoryTelemetry.h"
#include "PDFiumDocument.h"
#include <cstring>
#include <thread>
#if TREPDF_ENABLE_MEMORY_TELEMETRY && defined(Q_OS_MACOS)
#include <mach/mach.h>
#include <malloc/malloc.h>
#endif

using namespace PDFEditControllerTestSupport;

namespace {
QVariantMap memoryReading(const QVariantMap &snapshot, const char *name)
{
    const QString key = QString::fromLatin1(name);
    if (key.startsWith(QStringLiteral("rasterBudget.")))
        return snapshot.value(QStringLiteral("resourceEstimates")).toMap()
            .value(QStringLiteral("rasterBudget")).toMap().value(key.mid(13)).toMap();
    for (const auto *group : {"processMemory", "mallocHeap", "resourceEstimates"}) {
        const auto metrics = snapshot.value(QLatin1String(group)).toMap();
        if (metrics.contains(key))
            return metrics.value(key).toMap();
    }
    return {};
}

qint64 snapshotValue(const QVariantMap &snapshot, const char *name, const char *field = "current")
{
    return memoryReading(snapshot, name).value(QLatin1String(field)).toLongLong();
}

qint64 memoryValue(const char *name, const char *field = "current")
{
    return snapshotValue(PDFMemoryTelemetry::snapshot(), name, field);
}
}

void PDFEditControllerTest::reportsMemoryTelemetryPeaksAndSharedImages()
{
#if TREPDF_ENABLE_MEMORY_TELEMETRY
    PDFMemoryTelemetry::resetAllPeaks();
    const auto baseline = memoryValue("pendingImageBytes");
    QImage image(127, 61, QImage::Format_RGB888); // Includes scanline padding.
    const qint64 bytes = qint64(image.bytesPerLine()) * image.height();
    auto ticket = PDFMemoryTelemetry::trackImage(image);
    auto shared = ticket;
    QCOMPARE(memoryValue("pendingImageBytes"), baseline + bytes);
    QCOMPARE(memoryValue("pendingImageBytes", "peak"), baseline + bytes);
    ticket.reset();
    QCOMPARE(memoryValue("pendingImageBytes"), baseline + bytes);
    std::thread release([ticket = std::move(shared)]() mutable { ticket.reset(); });
    release.join();
    QCOMPARE(memoryValue("pendingImageBytes"), baseline);
    QCOMPARE(memoryValue("pendingImageBytes", "peak"), baseline + bytes);

    PDFRasterBudget::Budget budget(1000);
    auto first = budget.tryReserve(100);
    auto firstCopy = first;
    auto second = budget.tryReserve(200);
    QVERIFY(first && second);
    QCOMPARE(budget.currentLeaseBytes(), 600);
    QCOMPARE(budget.peakLeaseBytes(), 600);
    QCOMPARE(budget.leaseCount(), 2);
    QVERIFY(!budget.tryReserve(300));
    first.reset();
    QCOMPARE(budget.leaseCount(), 2);
    firstCopy.reset();
    QCOMPARE(budget.currentLeaseBytes(), 400);
    QCOMPARE(budget.peakLeaseBytes(), 600);
    budget.resetPeaks();
    QCOMPARE(budget.peakLeaseBytes(), 400);
    QCOMPARE(budget.statistics().peakLeaseCount, 1);
    second.reset();
    QCOMPARE(budget.leaseCount(), 0);
    QCOMPARE(budget.peakLeaseBytes(), 400);
    PDFMemoryTelemetry::resetAllPeaks();
    QCOMPARE(memoryValue("pendingImageBytes", "peak"), baseline);
    QCOMPARE(memoryValue("rasterBudget.leaseBytes"), PDFRasterBudget::currentLeaseBytes());
    QCOMPARE(memoryValue("rasterBudget.leaseBytes", "peak"), PDFRasterBudget::peakLeaseBytes());
    QCOMPARE(memoryValue("rasterBudget.leaseCount"), PDFRasterBudget::leaseCount());
#else
    QSKIP("Memory telemetry is compiled out");
#endif
}

void PDFEditControllerTest::reportsMemoryTelemetryGroupsAndLeaseCoverage()
{
#if TREPDF_ENABLE_MEMORY_TELEMETRY
    using M = PDFMemoryTelemetry;
    M::resetAllPeaks();
    const auto baseline = M::snapshot();
    QCOMPARE(baseline.keys(), QStringList({QStringLiteral("mallocHeap"),
        QStringLiteral("processMemory"), QStringLiteral("resourceEstimates")}));
    const auto processGroup = baseline.value(QStringLiteral("processMemory")).toMap();
    const auto heapGroup = baseline.value(QStringLiteral("mallocHeap")).toMap();
    const auto resources = baseline.value(QStringLiteral("resourceEstimates")).toMap();
    QCOMPARE(processGroup.value(QStringLiteral("primaryMetric")).toString(), QStringLiteral("phys_footprint"));
    QVERIFY(resources.value(QStringLiteral("estimatedOnly")).toBool());
    QVERIFY(!processGroup.contains(QStringLiteral("rasterBudget")));
    QVERIFY(!heapGroup.contains(QStringLiteral("phys_footprint")));
    QVERIFY(!resources.contains(QStringLiteral("mallocInUseBytes")));
    for (const auto &group : {baseline, processGroup, heapGroup, resources}) {
        QVERIFY(!group.contains(QStringLiteral("unaccountedBytes")));
        QVERIFY(!group.contains(QStringLiteral("trackedEstimatedBytes")));
        QVERIFY(!group.contains(QStringLiteral("rasterLeaseBytes")));
    }

    // Even estimates exceeding process memory stay in their own group, without
    // generating a cross-group total or remainder.
    struct Source { ~Source() { M::removeSource(this); } } source;
    M::Values values{};
    values[M::CandidateBytes] = 1024LL * 1024 * 1024;
    values[M::TextureCount] = 77;
    M::setSource(&source, values);
    QImage image(127, 61, QImage::Format_RGB888);
    const qint64 imageBytes = qint64(image.bytesPerLine()) * image.height();
    auto reservation = PDFRasterBudget::tryReserve(imageBytes);
    QVERIFY(reservation);
    auto ticket = M::trackImage(image, reservation);
    const auto snapshot = M::snapshot();
    QCOMPARE(snapshotValue(snapshot, "candidateBytes"), snapshotValue(baseline, "candidateBytes") + values[M::CandidateBytes]);
    QCOMPARE(snapshotValue(snapshot, "rasterBudget.leaseBytes"), snapshotValue(baseline, "rasterBudget.leaseBytes") + 2 * imageBytes);
    QCOMPARE(snapshotValue(snapshot, "pendingImageBytes"), snapshotValue(baseline, "pendingImageBytes") + imageBytes);
    QCOMPARE(snapshotValue(snapshot, "pendingImageUnleasedBytes"), snapshotValue(baseline, "pendingImageUnleasedBytes"));
    QCOMPARE(snapshotValue(snapshot, "pendingImageRasterCoveredBytes"), snapshotValue(baseline, "pendingImageRasterCoveredBytes") + imageBytes);
    QVERIFY(!snapshot.value(QStringLiteral("resourceEstimates")).toMap().contains(QStringLiteral("trackedEstimatedBytes")));

    // Coverage remains useful as an overlap diagnostic, not as a byte total.
    {
        QImage extra(64, 32, QImage::Format_ARGB32);
        const qint64 extraBytes = qint64(extra.bytesPerLine()) * extra.height();
        auto extraTicket = M::trackImage(extra);
        QCOMPARE(memoryValue("pendingImageUnleasedBytes"), snapshotValue(baseline, "pendingImageUnleasedBytes") + extraBytes);
        extraTicket.reset();
        PDFRasterBudget::Budget localBudget(extraBytes * 2);
        auto localLease = localBudget.tryReserve(extraBytes);
        QVERIFY(localLease);
        extraTicket = M::trackImage(extra, localLease);
        QCOMPARE(memoryValue("pendingImageUnleasedBytes"), snapshotValue(baseline, "pendingImageUnleasedBytes") + extraBytes);
    }

#if defined(Q_OS_MACOS)
    QVERIFY(processGroup.value(QStringLiteral("available")).toBool());
    const auto measured = M::snapshot();
    task_vm_info_data_t native{};
    mach_msg_type_number_t count = TASK_VM_INFO_REV1_COUNT;
    QCOMPARE(task_info(mach_task_self(), TASK_VM_INFO,
        reinterpret_cast<task_info_t>(&native), &count), KERN_SUCCESS);
    QVERIFY(count >= TASK_VM_INFO_REV1_COUNT);
    const qint64 nativeValues[] = {qint64(native.phys_footprint),
        qint64(native.resident_size), qint64(native.virtual_size)};
    size_t index = 0;
    for (const auto *key : {"phys_footprint", "resident_size", "virtual_size"}) {
        QVERIFY(snapshotValue(measured, key) > 0);
        QVERIFY(snapshotValue(measured, key, "peak") >= snapshotValue(measured, key));
        QVERIFY(qAbs(snapshotValue(measured, key) - nativeValues[index++]) < 1024 * 1024);
    }
#else
    QVERIFY(!processGroup.value(QStringLiteral("available")).toBool());
    for (const auto *key : {"phys_footprint", "resident_size", "virtual_size"})
        QVERIFY(!memoryReading(snapshot, key).value(QStringLiteral("current")).isValid());
#endif

    auto reservationCopy = reservation;
    reservation.reset();
    QCOMPARE(memoryValue("pendingImageRasterCoveredBytes"), snapshotValue(snapshot, "pendingImageRasterCoveredBytes"));
    std::thread release([lease = std::move(reservationCopy)]() mutable { lease.reset(); });
    release.join();
    QCOMPARE(memoryValue("rasterBudget.leaseBytes"), snapshotValue(baseline, "rasterBudget.leaseBytes"));
    QCOMPARE(memoryValue("pendingImageRasterCoveredBytes"), snapshotValue(baseline, "pendingImageRasterCoveredBytes"));
    QCOMPARE(memoryValue("pendingImageUnleasedBytes"), snapshotValue(baseline, "pendingImageUnleasedBytes") + imageBytes);
    auto sharedTicket = ticket;
    ticket.reset();
    QCOMPARE(memoryValue("pendingImageBytes"), snapshotValue(snapshot, "pendingImageBytes"));
    sharedTicket.reset();
    QCOMPARE(memoryValue("pendingImageBytes"), snapshotValue(baseline, "pendingImageBytes"));
    // A texture lease can outlive the CPU image: report its conservative budget
    // independently rather than interpreting it as remaining resident pixels.
    reservation = PDFRasterBudget::tryReserve(imageBytes);
    QVERIFY(reservation);
    QCOMPARE(memoryValue("pendingImageBytes"), snapshotValue(baseline, "pendingImageBytes"));
    QCOMPARE(memoryValue("rasterBudget.leaseBytes"), snapshotValue(snapshot, "rasterBudget.leaseBytes"));
    reservation.reset();
    const qint64 peak = memoryValue("rasterBudget.leaseBytes", "peak");
    M::removeSource(&source);
    QCOMPARE(memoryValue("rasterBudget.leaseBytes", "peak"), peak);
    M::resetAllPeaks();
    QCOMPARE(memoryValue("rasterBudget.leaseBytes", "peak"), snapshotValue(baseline, "rasterBudget.leaseBytes"));
#else
    QSKIP("Memory telemetry is compiled out");
#endif
}

void PDFEditControllerTest::reportsMemoryTelemetryMallocZones()
{
#if TREPDF_ENABLE_MEMORY_TELEMETRY && defined(Q_OS_MACOS)
    using M = PDFMemoryTelemetry;
    auto value = [](const QVariantMap &snapshot, const char *key, const char *field = "current") {
        return snapshotValue(snapshot, key, field);
    };
    // A test-only nondefault zone proves that the API totals all zones. This
    // owns its allocations even if an assertion exits early.
    std::unique_ptr<malloc_zone_t, decltype(&malloc_destroy_zone)> zone(
        malloc_create_zone(0, 0), &malloc_destroy_zone);
    QVERIFY(zone);
    QVERIFY(zone.get() != malloc_default_zone());
    M::resetAllPeaks();
    const auto baseline = M::snapshot();
    QVERIFY(baseline.value(QStringLiteral("mallocHeap")).toMap().value(QStringLiteral("available")).toBool());
    QVERIFY(value(baseline, "mallocInUseBytes") > 0);
    QVERIFY(value(baseline, "mallocReservedBytes") >= value(baseline, "mallocInUseBytes"));

    constexpr qint64 allocationBytes = 16 * 1024 * 1024;
    void *allocation = malloc_zone_malloc(zone.get(), allocationBytes);
    QVERIFY(allocation);
    std::memset(allocation, 0x5a, allocationBytes);
    const auto held = M::snapshot();
    // Leave headroom for unrelated allocator bookkeeping between samples.
    QVERIFY(value(held, "mallocInUseBytes") >= value(baseline, "mallocInUseBytes") + allocationBytes / 2);
    QVERIFY(value(held, "mallocReservedBytes") >= value(held, "mallocInUseBytes"));
    for (const auto *key : {"mallocInUseBytes", "mallocReservedBytes"})
        QVERIFY(value(held, key, "peak") >= value(held, key));
    // Untracked heap activity cannot alter the resource estimates group.
    QCOMPARE(held.value(QStringLiteral("resourceEstimates")), baseline.value(QStringLiteral("resourceEstimates")));
    QVERIFY(!held.contains(QStringLiteral("unaccountedBytes")));
    QVERIFY(!held.contains(QStringLiteral("trackedEstimatedBytes")));

    malloc_zone_free(zone.get(), allocation);
    const auto freed = M::snapshot();
    QVERIFY(value(freed, "mallocInUseBytes") < value(held, "mallocInUseBytes") - allocationBytes / 2);
    for (const auto *key : {"mallocInUseBytes", "mallocReservedBytes"})
        QVERIFY(value(freed, key, "peak") >= value(held, key, "peak"));
    // Reserved memory may remain high after free; telemetry must not trim it.
    M::resetAllPeaks();
    const auto reset = M::snapshot();
    QVERIFY(value(reset, "mallocInUseBytes", "peak") < value(held, "mallocInUseBytes", "peak"));
    QVERIFY(value(reset, "mallocInUseBytes", "peak") >= value(reset, "mallocInUseBytes"));
#elif TREPDF_ENABLE_MEMORY_TELEMETRY
    const auto snapshot = PDFMemoryTelemetry::snapshot();
    QVERIFY(!snapshot.value(QStringLiteral("mallocHeap")).toMap().value(QStringLiteral("available")).toBool());
    for (const auto *key : {"mallocInUseBytes", "mallocReservedBytes"})
        QVERIFY(!memoryReading(snapshot, key).value(QStringLiteral("current")).isValid());
#else
    QSKIP("Memory telemetry is compiled out");
#endif
}

void PDFEditControllerTest::reportsMemoryTelemetrySessionsCachesAndHistory()
{
#if TREPDF_ENABLE_MEMORY_TELEMETRY
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("memory-telemetry.pdf"));
    QVERIFY(createThreePageTextPdf(path));
    const qint64 sessions = memoryValue("sessionCount");
    const qint64 cached = memoryValue("candidatePageCount");
    const qint64 drafts = memoryValue("draftCount");
    auto session = std::make_unique<PDFDocumentSession>();
    QVERIFY(session->document()->open(QUrl::fromLocalFile(path)));
    QCOMPARE(memoryValue("sessionCount"), sessions + 1);
    auto *editor = session->editController();
    editor->requestCandidatesForPage(0);
    QTRY_VERIFY_WITH_TIMEOUT(memoryValue("candidatePageCount") > cached, 5000);
    QVERIFY(memoryValue("candidateBytes") > 0);
    const qint64 candidatePeak = memoryValue("candidateBytes", "peak");
    {
        auto failedSession = std::make_unique<PDFDocumentSession>();
        QVERIFY(!failedSession->document()->open(QUrl::fromLocalFile(path + QStringLiteral(".missing"))));
    }
    QCoreApplication::processEvents();
    QCOMPARE(memoryValue("candidateBytes", "peak"), candidatePeak);

    auto *search = session->searchController();
    search->search(QStringLiteral("fixture page"));
    QTRY_COMPARE_WITH_TIMEOUT(search->state(), PDFSearchController::Ready, 5000);
    QCOMPARE(memoryValue("searchResultCount"), 3);
    QVERIFY(memoryValue("searchRectangleCount") >= 3);
    QVERIFY(memoryValue("searchBytes") > 0);
    const qint64 searchPeak = memoryValue("searchBytes", "peak");
    search->clearSearch();
    QCOMPARE(memoryValue("searchResultCount"), 0);
    QCOMPARE(memoryValue("searchRectangleCount"), 0);
    QCOMPARE(memoryValue("searchBytes", "peak"), searchPeak);

    QImage image(256, 128, QImage::Format_ARGB32);
    image.fill(Qt::blue);
    const qint64 bytes = qint64(image.bytesPerLine()) * image.height();
    const auto added = editor->addImageData(0, 0.2, 0.2, image);
    QVERIFY(!added.isEmpty());
    QCOMPARE(memoryValue("draftCount"), drafts + 1);
    QCOMPARE(memoryValue("undoCommandCount"), 1);
    QCOMPARE(memoryValue("draftImageBytes"), bytes);
    QCOMPARE(memoryValue("undoImageBytes"), 0); // The undo snapshot shares draft pixels.
    QVERIFY(memoryValue("undoBytes") > 0);
    const auto undoPeak = memoryValue("undoBytes", "peak");
    editor->undo();
    QCOMPARE(memoryValue("draftCount"), drafts);
    QCOMPARE(memoryValue("undoCommandCount"), 0);
    QCOMPARE(memoryValue("redoCommandCount"), 1);
    QCOMPARE(memoryValue("redoImageBytes"), bytes);
    QCOMPARE(memoryValue("undoBytes", "peak"), undoPeak);
    editor->redo();
    QCOMPARE(memoryValue("draftImageBytes"), bytes);
    QCOMPARE(memoryValue("redoCommandCount"), 0);
    editor->discardDrafts();
    QCOMPARE(memoryValue("draftCount"), drafts);
    QCOMPARE(memoryValue("draftImageBytes"), 0);
    QCOMPARE(memoryValue("undoCommandCount"), 0);
    QVERIFY(memoryValue("candidateBytes", "peak") >= candidatePeak);
    QVERIFY(memoryValue("draftImageBytes", "peak") >= bytes);
    session->document()->close();
    QCOMPARE(memoryValue("sessionCount"), sessions);
    QCoreApplication::processEvents();
    QCOMPARE(memoryValue("draftImageBytes", "peak"), memoryValue("draftImageBytes"));
    session.reset();
    QCOMPARE(memoryValue("sessionCount"), sessions);
    QCOMPARE(memoryValue("candidatePageCount"), cached);
    // The close boundary rebases peaks after the session's children are gone.
    QCoreApplication::processEvents();
    QCOMPARE(memoryValue("draftImageBytes", "peak"), memoryValue("draftImageBytes"));
#else
    QSKIP("Memory telemetry is compiled out");
#endif
}

void PDFEditControllerTest::reportsMemoryTelemetryRenderLifecycle()
{
#if TREPDF_ENABLE_MEMORY_TELEMETRY
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("memory-raster.pdf"));
    QVERIFY(createThreePageTextPdf(path));
    QTRY_COMPARE_WITH_TIMEOUT(PDFRasterBudget::usedBytes(), 0, 5000);
    PDFMemoryTelemetry::resetAllPeaks();
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    QQuickWindow window;
    window.resize(360, 360);
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
    TestablePDFPageItem item(window.contentItem());
    item.setSize(QSizeF(6000, 8000));
    item.setVisiblePageRect(QRectF(0, 0, 360, 360));
    item.setDocument(&document);
    item.completeForTest();
    QTRY_VERIFY_WITH_TIMEOUT(item.draftTextureCurrent() && !item.m_renderPending, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(memoryValue("pendingImageBytes"), 0, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(memoryValue("textureCount"), item.textureCountForTest(), 5000);
    QVERIFY(memoryValue("textureCount") > 1);
    QCOMPARE(memoryValue("tileCount"), memoryValue("textureCount") - 1);
    QCOMPARE(memoryValue("visiblePageItemCount"), 1);
    QVERIFY(memoryValue("pendingImageBytes", "peak") > 0);
    const qint64 texturePeak = memoryValue("textureCount", "peak");

    item.setDocument(nullptr); // Pooling drops GUI metadata before SG ownership.
    QCOMPARE(memoryValue("visiblePageItemCount"), 0);
    QTRY_COMPARE_WITH_TIMEOUT(memoryValue("textureCount"), 0, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(memoryValue("tileCount"), 0, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(memoryValue("rasterBudget.leaseBytes"), 0, 5000);
    QCOMPARE(memoryValue("textureCount", "peak"), texturePeak);
    for (int i = 0; i < 8; ++i) {
        item.setDocument(&document);
        item.requestRenderForCurrentSize();
        item.setDocument(nullptr);
    }
    QTRY_COMPARE_WITH_TIMEOUT(memoryValue("pendingImageBytes"), 0, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(memoryValue("rasterBudget.leaseCount"), 0, 5000);
    QCOMPARE(memoryValue("textureCount"), 0);
    window.close();
#else
    QSKIP("Memory telemetry is compiled out");
#endif
}

void PDFEditControllerTest::reportsMemoryTelemetryWorkerDocumentMoves()
{
#if TREPDF_ENABLE_MEMORY_TELEMETRY
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("memory-worker.pdf"));
    QVERIFY(createThreePageTextPdf(path));
    // Ensure PDFium runtime is initialized on the GUI thread first.
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    const qint64 baseline = memoryValue("workerDocumentCount");
    std::array<qint64, 6> observed{};
    std::thread worker([&]() {
        PDFiumProcessLock lock;
        auto first = PDFiumDocument::open(path);
        observed[0] = memoryValue("workerDocumentCount");
        auto moved = std::move(first);
        observed[1] = memoryValue("workerDocumentCount");
        auto other = PDFiumDocument::open(path);
        observed[2] = memoryValue("workerDocumentCount");
        other = std::move(moved);
        observed[3] = memoryValue("workerDocumentCount");
        auto invalid = PDFiumDocument::open(path + QStringLiteral(".missing"));
        observed[4] = memoryValue("workerDocumentCount");
        other.reset();
        observed[5] = memoryValue("workerDocumentCount");
    });
    worker.join();
    QCOMPARE(observed[0], baseline + 1);
    QCOMPARE(observed[1], baseline + 1);
    QCOMPARE(observed[2], baseline + 2);
    QCOMPARE(observed[3], baseline + 1);
    QCOMPARE(observed[4], baseline + 1);
    QCOMPARE(observed[5], baseline);
#else
    QSKIP("Memory telemetry is compiled out");
#endif
}

void PDFEditControllerTest::reportsMemoryTelemetryLoggingAndSingleton()
{
#if TREPDF_ENABLE_MEMORY_TELEMETRY
    qmlRegisterTypesAndRevisions<PDFMemoryTelemetry>("TrePDFMemoryTest", 1);
    const int type = qmlTypeId("TrePDFMemoryTest", 1, 0, "PDFMemoryTelemetry");
    QQmlEngine engine;
    auto *telemetry = engine.singletonInstance<PDFMemoryTelemetry *>(type);
    QVERIFY(telemetry && telemetry->enabled());
    auto *timer = QCoreApplication::instance()->findChild<QTimer *>(
        QStringLiteral("trepdfMemoryTelemetryTimer"));
    QVERIFY(timer && timer->isActive());
    QCOMPARE(timer->interval(), 5000);
    PDFMemoryTelemetry::startLogging(QCoreApplication::instance());
    QCOMPARE(QCoreApplication::instance()->findChildren<QTimer *>(
        QStringLiteral("trepdfMemoryTelemetryTimer")).size(), 1);
    // Heap/footprint logging must keep working after the last PDF is closed.
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("memory-log-after-close.pdf"));
    QVERIFY(createThreePageTextPdf(path));
    auto session = std::make_unique<PDFDocumentSession>();
    QVERIFY(session->document()->open(QUrl::fromLocalFile(path)));
    session.reset();
    QCoreApplication::processEvents();
    QCOMPARE(memoryValue("sessionCount"), 0);
    QVERIFY(timer->isActive());
    QSignalSpy changed(telemetry, &PDFMemoryTelemetry::snapshotChanged);
    QTest::ignoreMessage(QtInfoMsg, QRegularExpression(QStringLiteral(
        "^\\[Memory\\] Separate views; do not add or subtract groups\\n"
        "(?![\\s\\S]*(?:Unaccounted|TrackedEst|unaccountedBytes|trackedEstimatedBytes))"
        "\\[Process memory\\]\\nphys_footprint: [^\\n]+\\(primary process-memory KPI\\)\\n"
        "resident_size: [^\\n]+\\nvirtual_size: [^\\n]+\\n"
        "\\[Malloc heap - all zones\\]\\nMallocInUse: [^\\n]+\\(actual malloc heap usage\\)\\n"
        "MallocReserved: [^\\n]+\\(allocator-reserved memory\\)\\n"
        "\\[TrePDF resources - estimates and counts, not process-resident bytes\\]\\n"
        "RasterBudget/LeaseBytes: [^\\n]+\\(conservative CPU \\+ GPU estimate; may outlive CPU QImage\\)\\n"
        "RasterBudget/LeaseCount:")));
    QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), 6500);
#if defined(Q_OS_MACOS)
    QVERIFY(telemetry->snapshot().value(QStringLiteral("mallocHeap")).toMap().value(QStringLiteral("available")).toBool());
    QVERIFY(memoryValue("mallocInUseBytes") > 0);
#endif
    QVERIFY(telemetry->snapshot().value(QStringLiteral("resourceEstimates")).toMap().value(QStringLiteral("estimatedOnly")).toBool());
    QVERIFY(!telemetry->snapshot().value(QStringLiteral("resourceEstimates")).toMap().value(QStringLiteral("thumbnailSeparateCache")).toBool());
    changed.clear();
    telemetry->resetPeaks();
    QCOMPARE(changed.size(), 1);
#else
    QSKIP("Memory telemetry is compiled out");
#endif
}
