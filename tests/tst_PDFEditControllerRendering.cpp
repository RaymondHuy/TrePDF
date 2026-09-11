#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

using namespace PDFEditControllerTestSupport;

void PDFEditControllerTest::serializesPdfiumAcrossThreads()
{
    const auto workerStarted = std::make_shared<QSemaphore>();
    const auto workerAcquired = std::make_shared<QSemaphore>();
    QThread *worker = nullptr;
    bool startedWhileLocked = false;
    bool remainedBlocked = false;
    {
        PDFiumProcessLock mainThreadLock;
        worker = QThread::create([workerStarted, workerAcquired]() {
            workerStarted->release();
            QElapsedTimer deadline;
            deadline.start();
            while (!QThread::currentThread()->isInterruptionRequested()
                   && deadline.elapsed() < 2000) {
                PDFiumProcessLock workerLock(
                    PDFiumProcessLock::Mode::TryLock);
                if (workerLock.isLocked()) {
                    workerAcquired->release();
                    return;
                }
                QThread::msleep(1);
            }
        });
        worker->start();

        startedWhileLocked = workerStarted->tryAcquire(1, 1000);
        remainedBlocked = startedWhileLocked
            && !workerAcquired->tryAcquire(1, 100);
    }

    const bool acquiredAfterUnlock = workerAcquired->tryAcquire(1, 1000);
    const bool finished = worker->wait(2500);
    if (!finished) {
        worker->requestInterruption();
        worker->wait();
    }
    delete worker;

    QVERIFY(startedWhileLocked);
    QVERIFY(remainedBlocked);
    QVERIFY(acquiredAfterUnlock);
    QVERIFY(finished);
}

void PDFEditControllerTest::discoversUncachedHoverOffGuiThread()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("async-hover-source.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    QSignalSpy candidatesReadySpy(
        &controller,
        &PDFEditController::pageCandidatesReady);

    QElapsedTimer hoverTimer;
    hoverTimer.start();
    const QVariantMap initialHover = controller.candidateAt(0, 0.5, 0.5);
    const qint64 hoverMilliseconds = hoverTimer.elapsed();

    QVERIFY(initialHover.isEmpty());
    QVERIFY2(
        hoverMilliseconds < 250,
        qPrintable(QStringLiteral("Uncached hover took %1 ms")
                       .arg(hoverMilliseconds)));
    QTRY_VERIFY_WITH_TIMEOUT(candidatesReadySpy.size() > 0, 2000);
    QVERIFY(!candidateForText(
                 waitForCandidates(controller, 0),
                 QString::fromLatin1(kRegularText))
                 .isEmpty());
}

void PDFEditControllerTest::reschedulesCancelledHoverForSamePage()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("rescheduled-hover-source.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    QSignalSpy candidatesReadySpy(
        &controller,
        &PDFEditController::pageCandidatesReady);

    const auto holderLocked = std::make_shared<QSemaphore>();
    const auto releaseHolder = std::make_shared<QSemaphore>();
    QThread *holder = QThread::create([holderLocked, releaseHolder]() {
        QElapsedTimer deadline;
        deadline.start();
        while (!QThread::currentThread()->isInterruptionRequested()
               && deadline.elapsed() < 2000) {
            PDFiumProcessLock holderLock(PDFiumProcessLock::Mode::TryLock);
            if (holderLock.isLocked()) {
                holderLocked->release();
                releaseHolder->tryAcquire(1, 1000);
                return;
            }
            QThread::msleep(1);
        }
    });
    holder->start();
    const bool holderStarted = holderLocked->tryAcquire(1, 1000);

    QVariantMap initialHover;
    QVariantMap rescheduledHover;
    if (holderStarted) {
        initialHover = controller.candidateAt(0, 0.5, 0.5);
        controller.setDocument(nullptr);
        controller.setDocument(&document);
        rescheduledHover = controller.candidateAt(0, 0.5, 0.5);
    }
    releaseHolder->release();
    const bool holderFinished = holder->wait(2500);
    if (!holderFinished) {
        holder->requestInterruption();
        holder->wait();
    }
    delete holder;

    QVERIFY(holderStarted);
    QVERIFY(holderFinished);
    QVERIFY(initialHover.isEmpty());
    QVERIFY(rescheduledHover.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(candidatesReadySpy.size() > 0, 2000);
    QVERIFY(!candidateForText(
                 waitForCandidates(controller, 0),
                 QString::fromLatin1(kRegularText))
                 .isEmpty());
}

void PDFEditControllerTest::defersUncachedHoverWhilePdfiumIsBusy()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("deferred-hover-source.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    QSignalSpy candidatesReadySpy(
        &controller,
        &PDFEditController::pageCandidatesReady);

    const auto holderLocked = std::make_shared<QSemaphore>();
    const auto releaseHolder = std::make_shared<QSemaphore>();
    QThread *holder = QThread::create([holderLocked, releaseHolder]() {
        QElapsedTimer deadline;
        deadline.start();
        while (!QThread::currentThread()->isInterruptionRequested()
               && deadline.elapsed() < 2000) {
            PDFiumProcessLock holderLock(PDFiumProcessLock::Mode::TryLock);
            if (holderLock.isLocked()) {
                holderLocked->release();
                releaseHolder->tryAcquire(1, 1000);
                return;
            }
            QThread::msleep(1);
        }
    });
    holder->start();
    const bool holderStarted = holderLocked->tryAcquire(1, 1000);

    QElapsedTimer hoverTimer;
    hoverTimer.start();
    const QVariantMap uncachedHover = holderStarted
        ? controller.candidateAt(0, 0.5, 0.5)
        : QVariantMap();
    const qint64 uncachedHoverMilliseconds = hoverTimer.elapsed();
    releaseHolder->release();
    const bool holderFinished = holder->wait(2500);
    if (!holderFinished) {
        holder->requestInterruption();
        holder->wait();
    }
    delete holder;

    QVERIFY(holderStarted);
    QVERIFY(holderFinished);
    QVERIFY(uncachedHover.isEmpty());
    QVERIFY2(
        uncachedHoverMilliseconds < 250,
        qPrintable(QStringLiteral("Uncached hover waited %1 ms")
                       .arg(uncachedHoverMilliseconds)));
    QTRY_VERIFY_WITH_TIMEOUT(candidatesReadySpy.size() > 0, 2000);

    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kRegularText));
    QVERIFY(!candidate.isEmpty());
    const QRectF sourceRect = candidate.value(QStringLiteral("sourceRect")).toRectF();
    QVERIFY(!sourceRect.isEmpty());

    const auto cachedHolderLocked = std::make_shared<QSemaphore>();
    const auto releaseCachedHolder = std::make_shared<QSemaphore>();
    QThread *cachedHolder = QThread::create(
        [cachedHolderLocked, releaseCachedHolder]() {
            QElapsedTimer deadline;
            deadline.start();
            while (!QThread::currentThread()->isInterruptionRequested()
                   && deadline.elapsed() < 2000) {
                PDFiumProcessLock holderLock(PDFiumProcessLock::Mode::TryLock);
                if (holderLock.isLocked()) {
                    cachedHolderLocked->release();
                    releaseCachedHolder->tryAcquire(1, 1000);
                    return;
                }
                QThread::msleep(1);
            }
        });
    cachedHolder->start();
    const bool cachedHolderStarted = cachedHolderLocked->tryAcquire(1, 1000);

    hoverTimer.restart();
    const QVariantMap cachedHover = cachedHolderStarted
        ? controller.candidateAt(
              0,
              sourceRect.center().x(),
              sourceRect.center().y())
        : QVariantMap();
    const qint64 cachedHoverMilliseconds = hoverTimer.elapsed();
    releaseCachedHolder->release();
    const bool cachedHolderFinished = cachedHolder->wait(2500);
    if (!cachedHolderFinished) {
        cachedHolder->requestInterruption();
        cachedHolder->wait();
    }
    delete cachedHolder;

    QVERIFY(cachedHolderStarted);
    QVERIFY(cachedHolderFinished);
    QVERIFY(!cachedHover.isEmpty());
    QVERIFY2(
        cachedHoverMilliseconds < 250,
        qPrintable(QStringLiteral("Cached hover waited %1 ms")
                       .arg(cachedHoverMilliseconds)));
}

void PDFEditControllerTest::dropsAsyncResultsAfterControllerDestruction()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("destroyed-controller-async-source.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    for (int iteration = 0; iteration < 64; ++iteration) {
        auto controller = std::make_unique<PDFEditController>();
        controller->setDocument(&document);
        controller->candidateAt(0, 0.5, 0.5);
        controller->requestCommentIndex();
        controller.reset();
    }

    QTest::qWait(1000);
    QVERIFY(document.isLoaded());
}

void PDFEditControllerTest::choosesHybridRasterPlansWithHysteresis()
{
    using namespace PDFRasterPlan;
    const QRectF visible(0.0, 0.0, 1.0, 1.0);
    const QSize boundary(4096, 2048);
    QCOMPARE(imageBytes(boundary), fullPageLimitBytes);
    QVERIFY(!makePlan(boundary, visible).tiled);
    QVERIFY(makePlan(QSize(4096, 2049), visible).tiled);

    const QSize hysteresisBand(3584, 2048);
    QVERIFY(!makePlan(hysteresisBand, visible, false).tiled);
    QVERIFY(makePlan(hysteresisBand, visible, true).tiled);
    const QSize returnBoundary(3072, 2048);
    QCOMPARE(imageBytes(returnBoundary), fullPageReturnBytes);
    QVERIFY(!makePlan(returnBoundary, visible, true).tiled);
    // A thin page can exceed the texture edge limit without exceeding bytes.
    QVERIFY(makePlan(QSize(maxFullPageEdge + 1, 128), visible).tiled);

    const QSizeF logicalSize(1500, 1500);
    QVERIFY(!makePlan(rasterSize(logicalSize, 1.0), visible).tiled);
    QVERIFY(makePlan(rasterSize(logicalSize, 2.0), visible).tiled);
    QCOMPARE(rasterSize(QSizeF(100.01, 200.01), 1.5), QSize(151, 301));
    QCOMPARE(rasterSize(QSizeF(100, 200), 2.0), QSize(200, 400));
}

void PDFEditControllerTest::boundsHybridRasterGeometryAndCoverage()
{
    using namespace PDFRasterPlan;
    const QSize raster(8192, 8192);
    const QRect visiblePixels(2048, 2048, 1024, 1024);
    const Plan plan = makePlan(raster, QRectF(0.25, 0.25, 0.125, 0.125));
    QVERIFY(plan.tiled);
    QCOMPARE(plan.visibleTileCount, 1);
    QCOMPARE(plan.tiles.size(), 9);
    QCOMPARE(plan.tiles.constFirst().rect, visiblePixels);
    QCOMPARE(plan.tiles.constFirst().renderRect, QRect(2046, 2046, 1028, 1028));
    QVERIFY(imageBytes(plan.previewSize) <= previewLimitBytes);
    QVERIFY(qMax(plan.previewSize.width(), plan.previewSize.height()) <= maxPreviewEdge);
    for (qsizetype i = 0; i < plan.tiles.size(); ++i) {
        const Tile &tile = plan.tiles.at(i);
        QCOMPARE(tile.rect.intersects(visiblePixels), i < plan.visibleTileCount);
        QVERIFY(QRect(QPoint(), raster).contains(tile.renderRect));
        QVERIFY(tile.renderRect.contains(tile.rect));
        QVERIFY(tile.renderRect.width() <= tileEdge + 2 * gutter);
        QVERIFY(tile.renderRect.height() <= tileEdge + 2 * gutter);
    }

    // Partial edge tiles preserve exact page coverage. Only the sampling
    // gutters overlap, never the core rectangles used for scene-graph layout.
    const QSize partialRaster(4097, 4097);
    const Plan entirePage = makePlan(partialRaster, QRectF(0, 0, 1, 1));
    QCOMPARE(entirePage.visibleTileCount, 25);
    QCOMPARE(entirePage.tiles.size(), 25);
    qint64 coveredPixels = 0;
    bool foundBottomRight = false;
    for (qsizetype i = 0; i < entirePage.tiles.size(); ++i) {
        const Tile &tile = entirePage.tiles.at(i);
        QVERIFY(QRect(QPoint(), partialRaster).contains(tile.rect));
        coveredPixels += qint64(tile.rect.width()) * tile.rect.height();
        QCOMPARE(tile.renderRect,
                 tile.rect.adjusted(-gutter, -gutter, gutter, gutter)
                     .intersected(QRect(QPoint(), partialRaster)));
        if (tile.rect.topLeft() == QPoint(4096, 4096)) {
            QCOMPARE(tile.rect.size(), QSize(1, 1));
            QCOMPARE(tile.renderRect, QRect(4094, 4094, 3, 3));
            foundBottomRight = true;
        }
        for (qsizetype j = i + 1; j < entirePage.tiles.size(); ++j) {
            QVERIFY(!tile.rect.intersects(entirePage.tiles.at(j).rect));
        }
    }
    QVERIFY(foundBottomRight);
    QCOMPARE(coveredPixels, qint64(partialRaster.width()) * partialRaster.height());

    const Plan huge = makePlan(
        QSize(maximumRasterDimension, maximumRasterDimension),
        QRectF(0, 0, 1, 1));
    QCOMPARE(huge.tiles.size(), maxTiles);
    QCOMPARE(huge.visibleTileCount, maxTiles);
    QVERIFY(imageBytes(huge.previewSize) <= previewLimitBytes);
    qreal previousDistance = -1;
    const QPointF center(maximumRasterDimension / 2.0,
                         maximumRasterDimension / 2.0);
    for (const Tile &tile : huge.tiles) {
        const QPointF offset = QRectF(tile.rect).center() - center;
        const qreal distance = offset.x() * offset.x() + offset.y() * offset.y();
        QVERIFY(distance >= previousDistance);
        previousDistance = distance;
    }

    QCOMPARE(imageBytes(QSize()), qint64(0));
    QCOMPARE(imageBytes(QSize(-1, 20)), qint64(0));
    QCOMPARE(imageBytes(QSize(std::numeric_limits<int>::max(),
                              std::numeric_limits<int>::max())),
             std::numeric_limits<qint64>::max());
    const qreal infinity = std::numeric_limits<qreal>::infinity();
    const qreal nan = std::numeric_limits<qreal>::quiet_NaN();
    QVERIFY(rasterSize(QSizeF(10, 20), 0).isEmpty());
    QVERIFY(rasterSize(QSizeF(10, 20), infinity).isEmpty());
    QVERIFY(rasterSize(QSizeF(nan, 20), 1).isEmpty());
    QVERIFY(rasterSize(QSizeF(maximumRasterDimension, 20), 2).isEmpty());
    QVERIFY(makePlan(QSize(maximumRasterDimension + 1, 10),
                     QRectF(0, 0, 1, 1)).rasterSize.isEmpty());
    QVERIFY(makePlan(raster, QRectF(nan, 0, 1, 1)).tiles.isEmpty());
    QVERIFY(makePlan(raster, QRectF(2, 2, 1, 1)).tiles.isEmpty());
    QVERIFY(makePlan(raster, QRectF()).tiles.isEmpty());
}

void PDFEditControllerTest::reservesRasterBytesUntilLastOwnerReleases()
{
    PDFRasterBudget::Budget budget(128);
    QVERIFY(!budget.tryReserve(0));
    QVERIFY(!budget.tryReserve(-1));
    QVERIFY(!budget.tryReserve(std::numeric_limits<qint64>::max()));
    QVERIFY(!budget.tryReserve(65));
    QCOMPARE(budget.usedBytes(), qint64(0));
    QCOMPARE(budget.peakBytes(), qint64(0));

    auto first = budget.tryReserve(32);
    QVERIFY(first);
    QCOMPARE(first->bytes(), qint64(64));
    auto sharedOwner = first;
    first.reset();
    QCOMPARE(budget.usedBytes(), qint64(64));
    auto second = budget.tryReserve(32);
    QVERIFY(second);
    QCOMPARE(budget.usedBytes(), qint64(128));
    QVERIFY(!budget.tryReserve(1));

    int releaseCount = 0;
    qint64 usedAtRelease = -1;
    budget.setReleaseCallback([&]() {
        // Calling usedBytes reenters the same mutex: release notification must
        // happen after unlocking, or this callback would deadlock.
        usedAtRelease = budget.usedBytes();
        ++releaseCount;
    });
    sharedOwner.reset();
    QCOMPARE(releaseCount, 1);
    QCOMPARE(usedAtRelease, qint64(64));
    second.reset();
    QCOMPARE(releaseCount, 2);
    QCOMPARE(usedAtRelease, qint64(0));
    QCOMPARE(budget.usedBytes(), qint64(0));
    QCOMPARE(budget.peakBytes(), qint64(128));

    budget.setReleaseCallback({});
    auto disabledNotification = budget.tryReserve(1);
    QVERIFY(disabledNotification);
    disabledNotification.reset();
    QCOMPARE(releaseCount, 2);

    PDFRasterBudget::Lease survivingLease;
    {
        PDFRasterBudget::Budget temporary(10);
        survivingLease = temporary.tryReserve(5);
        QVERIFY(survivingLease);
        temporary.setReleaseCallback([&]() { ++releaseCount; });
    }
    survivingLease.reset();
    QCOMPARE(releaseCount, 3);
    PDFRasterBudget::Budget invalidBudget(-1);
    QVERIFY(!invalidBudget.tryReserve(1));
}

void PDFEditControllerTest::boundsConcurrentRasterReservations()
{
    PDFRasterBudget::Budget budget(128);
    std::atomic<bool> exceededBudget = false;
    std::atomic<int> acceptedReservations = 0;
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([&]() {
            for (int iteration = 0; iteration < 2000; ++iteration) {
                auto lease = budget.tryReserve(32);
                if (lease) {
                    acceptedReservations.fetch_add(1, std::memory_order_relaxed);
                }
                if (budget.usedBytes() > 128) {
                    exceededBudget.store(true, std::memory_order_relaxed);
                }
            }
        });
    }
    for (std::thread &worker : workers) {
        worker.join();
    }
    QVERIFY(!exceededBudget.load(std::memory_order_relaxed));
    QVERIFY(acceptedReservations.load(std::memory_order_relaxed) > 0);
    QCOMPARE(budget.usedBytes(), qint64(0));
    QVERIFY(budget.peakBytes() <= 128);
}

void PDFEditControllerTest::clipsFormSuppressionPatchesToRasterTile()
{
    PDFiumProcessLock pdfiumLock;
    FPDF_DOCUMENT document = FPDF_CreateNewDocument();
    QVERIFY(document);
    const auto closeDocument = qScopeGuard([document]() {
        FPDF_CloseDocument(document);
    });
    FPDF_PAGE page = FPDFPage_New(document, 0, 400, 400);
    QVERIFY(page);
    const auto closePage = qScopeGuard([page]() { FPDF_ClosePage(page); });

    QImage tile(QSize(128, 128), QImage::Format_ARGB32);
    tile.fill(Qt::blue);
    QVector<PDFWidgetRenderPatch> patches;
    QVERIFY(capturePDFWidgetRenderPatches(
        page,
        QSize(4000, 4000),
        {QRectF(105, 181, 4, 9), QRectF(0, 0, 10, 10)},
        tile,
        patches,
        QPoint(1024, 2048)));
    QCOMPARE(patches.size(), 1);
    // Full-page device bounds [1050,1090] x [2100,2190], translated
    // by the tile origin, padded four pixels, then clipped to 128 x 128.
    QCOMPARE(patches.constFirst().deviceRect, QRect(22, 48, 48, 80));
    QCOMPARE(patches.constFirst().pixels.size(), QSize(48, 80));

    tile.fill(Qt::red);
    QVERIFY(restorePDFWidgetRenderPatches(tile, patches));
    QCOMPARE(tile.pixelColor(22, 48), QColor(Qt::blue));
    QCOMPARE(tile.pixelColor(69, 127), QColor(Qt::blue));
    QCOMPARE(tile.pixelColor(21, 48), QColor(Qt::red));
    QCOMPARE(tile.pixelColor(70, 127), QColor(Qt::red));
    QCOMPARE(tile.pixelColor(22, 47), QColor(Qt::red));
    QVERIFY(capturePDFWidgetRenderPatches(
        page, QSize(4000, 4000), {QRectF(105, 181, 4, 9)}, tile,
        patches, QPoint(3000, 3000)));
    QVERIFY(patches.isEmpty());
}

void PDFEditControllerTest::coalescesZoomRasterUntilInputSettles()
{
    if (QGuiApplication::screens().isEmpty()) {
        QSKIP("The PDF viewer regression test needs a Qt Quick render screen.");
    }

    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("viewer-zoom-raster-settle.pdf"));
    QVERIFY(createRootCommentOnlyPdf(sourcePath));

    PDFDocument document;
    PDFEditController editController;
    PDFFormController formController;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    editController.setDocument(&document);
    formController.setDocument(&document);

    QQuickWindow window;
    window.setColor(Qt::white);
    window.resize(360, 360);

    TestablePDFPageItem pageItem(window.contentItem());
    pageItem.setWidth(200);
    pageItem.setHeight(200);
    pageItem.setEditController(&editController);
    pageItem.setFormController(&formController);
    pageItem.setDocument(&document);
    pageItem.setPage(0);
    pageItem.completeForTest();

    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(pageItem.draftTextureCurrent(), 5000);
    const qreal devicePixelRatio = window.devicePixelRatio();
    const QSize initialRasterSize(
        qRound(200 * devicePixelRatio),
        qRound(200 * devicePixelRatio));
    QTRY_COMPARE_WITH_TIMEOUT(
        pageItem.texturePixelSizeForTest(), initialRasterSize, 5000);

    QSignalSpy textureCurrentSpy(
        &pageItem,
        &PDFPageItem::draftTextureCurrentChanged);
    const std::array<std::pair<qreal, int>, 4> zoomSteps = {{
        {1.2, 240},
        {1.5, 300},
        {1.8, 360},
        {2.0, 400},
    }};
    quint64 settledSerial = 0;
    for (const auto &[zoom, edge] : zoomSteps) {
        pageItem.setZoom(zoom);
        pageItem.setWidth(edge);
        pageItem.setHeight(edge);
        settledSerial = pageItem.m_requestSerial;

        // Every input arrives before the 140 ms settle interval. The scene
        // graph must keep scaling the original raster instead of accepting
        // any intermediate PDFium render. requestRender() increments the
        // serial before submitting work, even if its result is later dropped.
        // The entire sequence exceeds 140 ms to exercise timer restarts.
        QTest::qWait(50);
        QCOMPARE(pageItem.m_requestSerial, settledSerial);
        QVERIFY(!pageItem.m_renderPending);
        QVERIFY(pageItem.draftTextureCurrent());
        QCOMPARE(textureCurrentSpy.size(), 0);
        QCOMPARE(pageItem.texturePixelSizeForTest(), initialRasterSize);
    }

    const QSize finalRasterSize(
        qRound(400 * devicePixelRatio),
        qRound(400 * devicePixelRatio));
    QTRY_COMPARE_WITH_TIMEOUT(
        pageItem.texturePixelSizeForTest(), finalRasterSize, 5000);
    QCOMPARE(pageItem.m_requestSerial, settledSerial + 1);
    QCOMPARE(pageItem.m_requestedPixelSize, finalRasterSize);

    // Returning to the accepted raster size during debounce needs no new
    // submission, even though both visual changes cancel stale generations.
    const quint64 acceptedRasterId = pageItem.m_cachedRequestId;
    pageItem.setZoom(1.5);
    pageItem.setSize(QSizeF(300, 300));
    pageItem.setZoom(2.0);
    pageItem.setSize(QSizeF(400, 400));
    settledSerial = pageItem.m_requestSerial;
    QTest::qWait(200);
    QCOMPARE(pageItem.m_requestSerial, settledSerial);
    QCOMPARE(pageItem.m_cachedRequestId, acceptedRasterId);
    QVERIFY(!pageItem.m_renderPending);

    // A zero-sized delegate releases its scene-graph texture. Restoring the
    // previous visual size must not mistake stale cache metadata for a live
    // texture; it must render and upload the raster again.
    pageItem.setWidth(0);
    pageItem.setHeight(0);
    QTRY_VERIFY_WITH_TIMEOUT(
        pageItem.texturePixelSizeForTest().isEmpty(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!pageItem.draftTextureCurrent(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!pageItem.draftTextureReady(), 5000);
    pageItem.setWidth(400);
    pageItem.setHeight(400);
    QTRY_COMPARE_WITH_TIMEOUT(
        pageItem.texturePixelSizeForTest(), finalRasterSize, 5000);
}

void PDFEditControllerTest::rendersVisibleHybridTilesAndReusesThem()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("hybrid-tiles.pdf"));
    const QByteArray content(
        "0.8 0.2 0.1 rg 0 0 100 200 re f\n"
        "0.1 0.3 0.8 rg 100 0 100 200 re f\n"
        "0 1 0 rg 20 130 90 24 re f\n"
        "0 0 0 RG 0.2 w 0 0 m 200 200 l S\n");
    QVERIFY(writePdfObjects(path, {
        QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
        QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
        QByteArray("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                   "/Resources << >> /Contents 4 0 R >>"),
        QByteArray("<< /Length ") + QByteArray::number(content.size())
            + QByteArray(" >>\nstream\n") + content + QByteArray("endstream"),
    }));
    QTRY_COMPARE_WITH_TIMEOUT(PDFRasterBudget::usedBytes(), 0, 5000);
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    QQuickWindow window;
    window.resize(360, 360);
    window.setColor(Qt::white);
    // Cocoa can assign a different-DPR screen when the window is first shown.
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
    TestablePDFPageItem item(window.contentItem());
    const qreal dpr = window.devicePixelRatio();
    const QSize raster(6000, 8000);
    item.setSize(QSizeF(raster.width() / dpr, raster.height() / dpr));
    auto setViewport = [&](QPoint origin) {
        item.setPosition(QPointF(-origin.x() / dpr, -origin.y() / dpr));
        item.setVisiblePageRect(QRectF(origin.x() / dpr, origin.y() / dpr,
                                      window.width(), window.height()));
    };
    setViewport(QPoint(920, 1920));
    item.setDocument(&document);
    item.completeForTest();
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(item.draftTextureCurrent() && !item.m_renderPending, 10000);
    QVERIFY(item.m_cachedTiled);
    QTRY_COMPARE_WITH_TIMEOUT(item.textureCountForTest(), item.m_rasters.size(), 5000);
    QVERIFY(item.textureCountForTest() > 1);
    QVERIFY(item.maximumTextureBytesForTest() <= qint64(1028) * 1028 * 4);
    QVERIFY(item.totalTextureBytesForTest() < PDFRasterPlan::imageBytes(raster) / 4);
    for (const auto &entry : item.m_rasters) {
        QVERIFY(entry.image.isNull());
    }

    // Direct PDFium reference of the visible region only. Crosses tile edges
    // and proves that tiles use the full-page transform, not a tile-sized page.
    const QImage actual = window.grabWindow();
    QVERIFY(!actual.isNull());
    const QImage expected = renderFirstPage(path, raster, 0,
                                            QRect(QPoint(920, 1920), actual.size()));
    QVERIFY(changedPixelCount(actual, expected, actual.rect())
            < actual.width() * actual.height() / 100);

    const quint64 firstBaseId = item.m_rasters.constFirst().id;
    setViewport(QPoint(930, 1930));
    const quint64 reuseSerial = item.m_requestSerial;
    QTest::qWait(200);
    QCOMPARE(item.m_requestSerial, reuseSerial);
    QVERIFY(!item.m_renderPending);
    setViewport(QPoint(2940, 4000));
    QTRY_VERIFY_WITH_TIMEOUT(!item.m_renderPending
                            && item.hasRasterTile(QRect(2048, 3072, 1024, 1024)), 10000);
    QCOMPARE(item.m_rasters.constFirst().id, firstBaseId);
    setViewport(QPoint(920, 1920));
    QTRY_VERIFY_WITH_TIMEOUT(!item.m_renderPending, 10000);
    QCOMPARE(item.m_rasters.constFirst().id, firstBaseId);

    // Keep fallback/tiles while the visual size is changing; only the final
    // settled raster can replace the accepted tile generation.
    for (int edge : {6200, 6600, 7000}) {
        item.setSize(QSizeF(edge / dpr, edge * 4.0 / 3 / dpr));
        const quint64 inputSerial = item.m_requestSerial;
        QTest::qWait(40);
        QCOMPARE(item.m_requestSerial, inputSerial);
        QCOMPARE(item.m_rasters.constFirst().id, firstBaseId);
    }
    QTRY_VERIFY_WITH_TIMEOUT(!item.m_renderPending
                            && item.m_cachedPixelSize.width() == 7000, 10000);
    QVERIFY(item.m_rasters.constFirst().id != firstBaseId);

    // Switching back to a small raster destroys the tiled scene subtree.
    item.setSize(QSizeF(200, 200));
    setViewport(QPoint());
    QTRY_VERIFY_WITH_TIMEOUT(!item.m_cachedTiled && !item.m_renderPending, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(item.textureCountForTest(), 1, 5000);
    QCOMPARE(item.texturePixelSizeForTest(), PDFRasterPlan::rasterSize(item.size(), dpr));

    item.setDocument(nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(item.textureCountForTest(), 0, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(PDFRasterBudget::usedBytes(), 0, 5000);
    // Reopen while tile work is queued; closed-document results must not land.
    item.setSize(QSizeF(6000 / dpr, 8000 / dpr));
    item.setDocument(&document);
    item.requestRenderForCurrentSize();
    item.setDocument(nullptr);
    item.setDocument(&document);
    QTRY_VERIFY_WITH_TIMEOUT(item.draftTextureCurrent() && !item.m_renderPending, 10000);
    item.setDocument(nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(PDFRasterBudget::usedBytes(), 0, 5000);
}

void PDFEditControllerTest::recoversHybridRasterBudgetPressure()
{
    QTRY_COMPARE_WITH_TIMEOUT(PDFRasterBudget::usedBytes(), 0, 5000);
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("hybrid-budget.pdf"));
    QVERIFY(createRootCommentOnlyPdf(path));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    QQuickWindow window;
    window.resize(300, 300);
    TestablePDFPageItem item(window.contentItem());
    item.setSize(QSizeF(6000, 8000));
    item.setVisiblePageRect(QRectF(0, 0, 300, 300));
    // Leave room for the preview, but not for the first detail tile.
    auto pressure = PDFRasterBudget::tryReserve(
        (PDFRasterBudget::limitBytes - 9 * 1024 * 1024) / 2);
    QVERIFY(pressure);
    item.setDocument(&document);
    item.completeForTest();
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(item.m_budgetBlocked, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(item.draftTextureCurrent(), 5000);
    QCOMPARE(item.textureCountForTest(), 1);
    const quint64 blockedSerial = item.m_requestSerial;
    QTest::qWait(200);
    QCOMPARE(item.m_requestSerial, blockedSerial); // No polling/retry storm.
    pressure.reset();
    QTRY_VERIFY_WITH_TIMEOUT(!item.m_budgetBlocked && !item.m_renderPending, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(item.textureCountForTest() > 1, 5000);
    QVERIFY(PDFRasterBudget::peakBytes() <= PDFRasterBudget::limitBytes);
    item.setDocument(nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(PDFRasterBudget::usedBytes(), 0, 5000);
}

void PDFEditControllerTest::replacesHybridCommentSuppressionAtomically()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("hybrid-comment.pdf"));
    const QByteArray appearance("1 0.8 0 rg 0 0 24 24 re f\n");
    QVERIFY(writePdfObjects(path, {
        QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
        QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
        QByteArray("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                   "/Resources << >> /Annots [4 0 R] >>"),
        QByteArray("<< /Type /Annot /Subtype /Square /Rect [24 144 48 168] "
                   "/Contents (Tile comment) /NM (tile-comment) /F 4 /AP << /N 5 0 R >> >>"),
        QByteArray("<< /Type /XObject /Subtype /Form /BBox [0 0 24 24] "
                   "/Resources << >> /Length ") + QByteArray::number(appearance.size())
            + QByteArray(" >>\nstream\n") + appearance + QByteArray("endstream"),
    }));
    PDFDocument document;
    PDFEditController controller;
    PDFFormController forms;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    forms.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);
    const QString commentId = controller.commentItems().constFirst().toMap()
                                  .value(QStringLiteral("objectId")).toString();
    QQuickWindow window;
    window.resize(300, 300);
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
    const qreal dpr = window.devicePixelRatio();
    TestablePDFPageItem item(window.contentItem());
    item.setSize(QSizeF(6000 / dpr, 8000 / dpr));
    item.setPosition(QPointF(-750 / dpr, -1350 / dpr));
    item.setVisiblePageRect(QRectF(750 / dpr, 1350 / dpr, 300, 300));
    item.setEditController(&controller);
    item.setFormController(&forms);
    item.setDocument(&document);
    item.completeForTest();
    QTRY_VERIFY_WITH_TIMEOUT(item.draftTextureReady() && !item.m_renderPending, 10000);
    const quint64 oldBase = item.m_rasters.constFirst().id;
    const QImage original = window.grabWindow();
    QVERIFY(!original.isNull());
    QImage white(original.size(), QImage::Format_RGB32);
    white.fill(Qt::white);
    QVERIFY(changedPixelCount(original, white, original.rect()) > 0);

    QVERIFY(controller.selectComment(0, commentId));
    controller.deleteSelectedObject();
    controller.undo();
    QVERIFY(controller.selectComment(0, commentId));
    controller.deleteSelectedObject();
    const quint64 revision = controller.draftRevisionForPage(0);
    QTRY_VERIFY_WITH_TIMEOUT(item.draftTextureReady() && !item.m_renderPending
                            && item.m_cachedDraftRevision == revision, 10000);
    QVERIFY(item.m_rasters.constFirst().id != oldBase);
    QCOMPARE(item.displayedSuppressedCommentObjectIds(), QStringList{commentId});
    for (const auto &entry : item.m_rasters) {
        QVERIFY(entry.id > oldBase);
    }
    QImage actual = window.grabWindow();
    QVERIFY(!actual.isNull());
    QCOMPARE(changedPixelCount(actual, white, actual.rect()), 0);

    controller.discardDrafts();
    QTRY_VERIFY_WITH_TIMEOUT(item.draftTextureReady() && !item.m_renderPending
                            && item.m_cachedDraftRevision
                                == controller.draftRevisionForPage(0), 10000);
    QVERIFY(item.displayedSuppressedCommentObjectIds().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(([&]() {
        actual = window.grabWindow();
        return changedPixelCount(actual, white, actual.rect()) > 0;
    })(), 5000);
    item.setDocument(nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(PDFRasterBudget::usedBytes(), 0, 5000);
}
