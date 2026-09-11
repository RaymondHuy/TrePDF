#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

using namespace PDFEditControllerTestSupport;

void PDFEditControllerTest::addsHighlightWithUndoRedoAndDiscard()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("add-highlight-source.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    QCOMPARE(controller.highlightColor(), QColor(255, 235, 59, 96));
    QSignalSpy highlightColorSpy(
        &controller,
        &PDFEditController::highlightColorChanged);
    const QColor chosenColor(255, 128, 171, 96);
    controller.setHighlightColor(chosenColor);
    QCOMPARE(controller.highlightColor(), chosenColor);
    QCOMPARE(highlightColorSpy.count(), 1);
    controller.setHighlightColor(chosenColor);
    QCOMPARE(highlightColorSpy.count(), 1);
    const QVariantMap textCandidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kRegularText));
    QVERIFY(!textCandidate.isEmpty());
    controller.setTool(PDFEditController::HighlightTool);

    const QRectF selectionRect = textCandidate
        .value(QStringLiteral("sourceRect"))
        .toRectF();
    const QVariantList snappedRects
        = controller.textMarkupRectsForSelection(0, selectionRect);
    QVERIFY(!snappedRects.isEmpty());
    const QRectF expectedSourceRect
        = boundingRectForCoordinateMaps(snappedRects);
    const QVariantMap highlight = controller.addHighlight(
        0,
        selectionRect);
    QCOMPARE(
        highlight.value(QStringLiteral("kind")).toString(),
        QStringLiteral("highlight"));
    QCOMPARE(
        highlight.value(QStringLiteral("sourceRect")).toRectF(),
        expectedSourceRect);
    QCOMPARE(
        highlight.value(QStringLiteral("highlightColor")).value<QColor>(),
        chosenColor);
    QCOMPARE(
        highlight.value(QStringLiteral("highlightRects")).toList(),
        snappedRects);
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QCOMPARE(
        candidateForKind(
            controller.candidatesForPage(0),
            QStringLiteral("highlight"))
            .value(QStringLiteral("objectId"))
            .toString(),
        highlight.value(QStringLiteral("objectId")).toString());
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QVERIFY(!controller.canRedo());

    controller.setTool(PDFEditController::SelectTool);
    QCOMPARE(
        controller.candidateAt(
            0,
            selectionRect.center().x(),
            selectionRect.center().y())
            .value(QStringLiteral("kind"))
            .toString(),
        QStringLiteral("highlight"));
    controller.setTool(PDFEditController::EditTextTool);
    QCOMPARE(
        controller.candidateAt(
            0,
            selectionRect.center().x(),
            selectionRect.center().y())
            .value(QStringLiteral("objectId"))
            .toString(),
        textCandidate.value(QStringLiteral("objectId")).toString());

    controller.setHighlightColor(QColor(79, 195, 247, 96));
    QCOMPARE(controller.undo(), -1);
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(candidateForKind(
                controller.candidatesForPage(0),
                QStringLiteral("highlight"))
                .isEmpty());
    QVERIFY(!controller.hasDrafts());
    QVERIFY(controller.canRedo());

    QCOMPARE(controller.redo(), -1);
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QCOMPARE(
        controller.draftsForPage(0)
            .constFirst()
            .toMap()
            .value(QStringLiteral("highlightColor"))
            .value<QColor>(),
        chosenColor);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QVERIFY(!controller.canRedo());

    controller.discardDrafts();
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
}

void PDFEditControllerTest::keepsTextMarkupColorsIndependent()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("independent-markup-colors-source.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap textCandidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kRegularText));
    QVERIFY(!textCandidate.isEmpty());

    const QColor defaultHighlightColor(255, 235, 59, 96);
    const QColor defaultUnderlineColor(198, 40, 40, 192);
    const QColor defaultSquigglyColor(198, 40, 40, 192);
    const QColor defaultStrikeoutColor(198, 40, 40, 192);
    const QColor highlightColor(255, 128, 171, 96);
    const QColor underlineColor(73, 91, 211, 176);
    const QColor squigglyColor(38, 166, 154, 160);
    const QColor strikeoutColor(123, 31, 162, 184);
    QCOMPARE(controller.highlightColor(), defaultHighlightColor);
    QCOMPARE(controller.underlineColor(), defaultUnderlineColor);
    QCOMPARE(controller.squigglyColor(), defaultSquigglyColor);
    QCOMPARE(controller.strikeoutColor(), defaultStrikeoutColor);

    QSignalSpy highlightColorSpy(
        &controller,
        &PDFEditController::highlightColorChanged);
    QSignalSpy underlineColorSpy(
        &controller,
        &PDFEditController::underlineColorChanged);
    QSignalSpy squigglyColorSpy(
        &controller,
        &PDFEditController::squigglyColorChanged);
    QSignalSpy strikeoutColorSpy(
        &controller,
        &PDFEditController::strikeoutColorChanged);

    controller.setHighlightColor(highlightColor);
    QCOMPARE(controller.highlightColor(), highlightColor);
    QCOMPARE(controller.underlineColor(), defaultUnderlineColor);
    QCOMPARE(controller.squigglyColor(), defaultSquigglyColor);
    QCOMPARE(controller.strikeoutColor(), defaultStrikeoutColor);
    QCOMPARE(highlightColorSpy.count(), 1);
    QCOMPARE(underlineColorSpy.count(), 0);
    QCOMPARE(squigglyColorSpy.count(), 0);
    QCOMPARE(strikeoutColorSpy.count(), 0);

    controller.setUnderlineColor(underlineColor);
    QCOMPARE(controller.highlightColor(), highlightColor);
    QCOMPARE(controller.underlineColor(), underlineColor);
    QCOMPARE(controller.squigglyColor(), defaultSquigglyColor);
    QCOMPARE(controller.strikeoutColor(), defaultStrikeoutColor);
    QCOMPARE(highlightColorSpy.count(), 1);
    QCOMPARE(underlineColorSpy.count(), 1);
    QCOMPARE(squigglyColorSpy.count(), 0);
    QCOMPARE(strikeoutColorSpy.count(), 0);
    controller.setUnderlineColor(underlineColor);
    QCOMPARE(underlineColorSpy.count(), 1);

    controller.setSquigglyColor(squigglyColor);
    QCOMPARE(controller.highlightColor(), highlightColor);
    QCOMPARE(controller.underlineColor(), underlineColor);
    QCOMPARE(controller.squigglyColor(), squigglyColor);
    QCOMPARE(controller.strikeoutColor(), defaultStrikeoutColor);
    QCOMPARE(highlightColorSpy.count(), 1);
    QCOMPARE(underlineColorSpy.count(), 1);
    QCOMPARE(squigglyColorSpy.count(), 1);
    QCOMPARE(strikeoutColorSpy.count(), 0);
    controller.setSquigglyColor(squigglyColor);
    QCOMPARE(squigglyColorSpy.count(), 1);

    controller.setStrikeoutColor(strikeoutColor);
    QCOMPARE(controller.highlightColor(), highlightColor);
    QCOMPARE(controller.underlineColor(), underlineColor);
    QCOMPARE(controller.squigglyColor(), squigglyColor);
    QCOMPARE(controller.strikeoutColor(), strikeoutColor);
    QCOMPARE(highlightColorSpy.count(), 1);
    QCOMPARE(underlineColorSpy.count(), 1);
    QCOMPARE(squigglyColorSpy.count(), 1);
    QCOMPARE(strikeoutColorSpy.count(), 1);
    controller.setStrikeoutColor(strikeoutColor);
    QCOMPARE(strikeoutColorSpy.count(), 1);

    const QRectF selectionRect = textCandidate
        .value(QStringLiteral("sourceRect"))
        .toRectF();

    controller.setTool(PDFEditController::HighlightTool);
    const QVariantMap highlight = controller.addHighlight(
        0,
        selectionRect);
    QCOMPARE(
        highlight.value(QStringLiteral("kind")).toString(),
        QStringLiteral("highlight"));
    QCOMPARE(
        highlight.value(QStringLiteral("highlightColor")).value<QColor>(),
        highlightColor);

    controller.setTool(PDFEditController::UnderlineTool);
    const QVariantMap underline = controller.addHighlight(
        0,
        selectionRect);
    QCOMPARE(
        underline.value(QStringLiteral("kind")).toString(),
        QStringLiteral("underline"));
    QCOMPARE(
        underline.value(QStringLiteral("highlightColor")).value<QColor>(),
        underlineColor);

    controller.setTool(PDFEditController::SquigglyTool);
    const QVariantMap squiggly = controller.addHighlight(
        0,
        selectionRect);
    QCOMPARE(
        squiggly.value(QStringLiteral("kind")).toString(),
        QStringLiteral("squiggly"));
    QCOMPARE(
        squiggly.value(QStringLiteral("highlightColor")).value<QColor>(),
        squigglyColor);

    controller.setTool(PDFEditController::StrikeoutTool);
    const QVariantMap strikeout = controller.addHighlight(
        0,
        selectionRect);
    QCOMPARE(
        strikeout.value(QStringLiteral("kind")).toString(),
        QStringLiteral("strikeout"));
    QCOMPARE(
        strikeout.value(QStringLiteral("highlightColor")).value<QColor>(),
        strikeoutColor);
    QCOMPARE(controller.draftsForPage(0).size(), 4);
}

void PDFEditControllerTest::requiresSourceTextForAllMarkupTools_data()
{
    QTest::addColumn<int>("tool");
    QTest::addColumn<QString>("kind");
    QTest::addColumn<QColor>("queuedColor");

    QTest::newRow("highlight")
        << static_cast<int>(PDFEditController::HighlightTool)
        << QStringLiteral("highlight")
        << QColor(255, 128, 171, 151);
    QTest::newRow("underline")
        << static_cast<int>(PDFEditController::UnderlineTool)
        << QStringLiteral("underline")
        << QColor(73, 91, 211, 187);
    QTest::newRow("squiggly")
        << static_cast<int>(PDFEditController::SquigglyTool)
        << QStringLiteral("squiggly")
        << QColor(38, 166, 154, 173);
    QTest::newRow("strikeout")
        << static_cast<int>(PDFEditController::StrikeoutTool)
        << QStringLiteral("strikeout")
        << QColor(123, 31, 162, 181);
}

void PDFEditControllerTest::requiresSourceTextForAllMarkupTools()
{
    QFETCH(int, tool);
    QFETCH(QString, kind);
    QFETCH(QColor, queuedColor);

    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("%1-text-hit-source.pdf").arg(kind));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const auto markupTool = static_cast<PDFEditController::Tool>(tool);
    controller.setTool(markupTool);
    switch (markupTool) {
    case PDFEditController::HighlightTool:
        controller.setHighlightColor(queuedColor);
        break;
    case PDFEditController::UnderlineTool:
        controller.setUnderlineColor(queuedColor);
        break;
    case PDFEditController::SquigglyTool:
        controller.setSquigglyColor(queuedColor);
        break;
    case PDFEditController::StrikeoutTool:
        controller.setStrikeoutColor(queuedColor);
        break;
    default:
        QFAIL("Unexpected text-markup tool");
    }
    QSignalSpy statusSpy(
        &controller,
        &PDFEditController::statusMessage);
    const QVariantMap pendingMarkup = controller.addHighlight(
        0,
        QRectF(0.05, 0.05, 0.90, 0.90));
    QCOMPARE(
        pendingMarkup.value(QStringLiteral("kind")).toString(),
        kind);
    QVERIFY(pendingMarkup.value(QStringLiteral("pending")).toBool());
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(controller.hasDrafts());
    QCOMPARE(statusSpy.count(), 0);

    controller.setTool(PDFEditController::SelectTool);
    const QColor changedColor(17, 29, 43, 109);
    switch (markupTool) {
    case PDFEditController::HighlightTool:
        controller.setHighlightColor(changedColor);
        break;
    case PDFEditController::UnderlineTool:
        controller.setUnderlineColor(changedColor);
        break;
    case PDFEditController::SquigglyTool:
        controller.setSquigglyColor(changedColor);
        break;
    case PDFEditController::StrikeoutTool:
        controller.setStrikeoutColor(changedColor);
        break;
    default:
        QFAIL("Unexpected text-markup tool");
    }

    const QVariantList candidates = waitForCandidates(controller, 0);
    QTRY_COMPARE_WITH_TIMEOUT(controller.draftsForPage(0).size(), 1, 2000);
    const QVariantMap completedMarkup = controller
        .draftsForPage(0)
        .constFirst()
        .toMap();
    QCOMPARE(
        completedMarkup.value(QStringLiteral("kind")).toString(),
        kind);
    QCOMPARE(
        completedMarkup
            .value(QStringLiteral("highlightColor"))
            .value<QColor>(),
        queuedColor);
    QVERIFY(!completedMarkup
        .value(QStringLiteral("highlightRects"))
        .toList()
        .isEmpty());
    QCOMPARE(controller.undo(), -1);
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(!controller.hasDrafts());

    const QVariantMap groupedCandidate = candidateForText(
        candidates,
        QString::fromLatin1(kGroupedText));
    QVERIFY(!groupedCandidate.isEmpty());
    const QVariantList sourceRects = groupedCandidate
        .value(QStringLiteral("sourceRects"))
        .toList();
    QCOMPARE(sourceRects.size(), 2);
    QRectF leftWordRect = rectFromCoordinateMap(sourceRects.at(0));
    QRectF rightWordRect = rectFromCoordinateMap(sourceRects.at(1));
    if (leftWordRect.left() > rightWordRect.left()) {
        std::swap(leftWordRect, rightWordRect);
    }
    const qreal gapWidth = rightWordRect.left() - leftWordRect.right();
    const qreal gapTop = qMax(leftWordRect.top(), rightWordRect.top());
    const qreal gapBottom = qMin(
        leftWordRect.bottom(),
        rightWordRect.bottom());
    QVERIFY(gapWidth > 0.0);
    QVERIFY(gapBottom > gapTop);
    const qreal horizontalInset = gapWidth * 0.20;
    const QRectF whitespaceRect(
        leftWordRect.right() + horizontalInset,
        gapTop,
        gapWidth - 2.0 * horizontalInset,
        gapBottom - gapTop);
    QVERIFY(whitespaceRect.isValid());
    controller.setTool(markupTool);
    QVERIFY(controller
        .textMarkupRectsForSelection(0, whitespaceRect)
        .isEmpty());

    statusSpy.clear();
    QVERIFY(controller.addHighlight(0, whitespaceRect).isEmpty());
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(statusSpy.count(), 1);
    QCOMPARE(
        statusSpy.constFirst().constFirst().toString(),
        qtTrId("NoTextInSelectedArea"));

    const QRectF noTextRect(0.82, 0.82, 0.12, 0.08);
    QVERIFY(controller
        .textMarkupRectsForSelection(0, noTextRect)
        .isEmpty());
    statusSpy.clear();
    QVERIFY(controller.addHighlight(0, noTextRect).isEmpty());
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(statusSpy.count(), 1);
    QCOMPARE(
        statusSpy.constFirst().constFirst().toString(),
        qtTrId("NoTextInSelectedArea"));

    constexpr qreal thinSelectionHeight = 0.00025;
    const QRectF thinTextSelection(
        leftWordRect.left(),
        leftWordRect.center().y() - thinSelectionHeight / 2.0,
        leftWordRect.width(),
        thinSelectionHeight);
    const QVariantList thinMarkupRects
        = controller.textMarkupRectsForSelection(0, thinTextSelection);
    QCOMPARE(thinMarkupRects.size(), 1);
    const QRectF thinMarkupRect = rectFromCoordinateMap(
        thinMarkupRects.constFirst());
    const QRectF helloGlyphBounds = normalizedGlyphBoundsForText(
        sourcePath,
        QStringLiteral("Hello"));
    QVERIFY(helloGlyphBounds.isValid());
    constexpr qreal normalizedTolerance = 0.00001;
    QVERIFY(qAbs(thinMarkupRect.left() - helloGlyphBounds.left())
        < normalizedTolerance);
    QVERIFY(qAbs(thinMarkupRect.top() - helloGlyphBounds.top())
        < normalizedTolerance);
    QVERIFY(qAbs(thinMarkupRect.right() - helloGlyphBounds.right())
        < normalizedTolerance);
    const qreal expectedBottom = markupTool == PDFEditController::SquigglyTool
        ? qMin<qreal>(
              1.0,
              helloGlyphBounds.bottom()
                  + 3.5 / document.pageSize(0).height())
        : helloGlyphBounds.bottom();
    QVERIFY(qAbs(thinMarkupRect.bottom() - expectedBottom)
        < normalizedTolerance);
    const QVariantMap thinMarkup = controller.addHighlight(
        0,
        thinTextSelection);
    QCOMPARE(
        thinMarkup.value(QStringLiteral("kind")).toString(),
        kind);
    QCOMPARE(
        thinMarkup.value(QStringLiteral("highlightRects")).toList(),
        thinMarkupRects);
    QCOMPARE(
        thinMarkup.value(QStringLiteral("sourceRect")).toRectF(),
        boundingRectForCoordinateMaps(thinMarkupRects));
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QCOMPARE(controller.undo(), -1);
    QVERIFY(controller.draftsForPage(0).isEmpty());
}

void PDFEditControllerTest::prioritizesPendingSquigglyCandidateScan()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("pending-squiggly-scan-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("pending-squiggly-scan-output.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SquigglyTool);
    QSignalSpy statusSpy(
        &controller,
        &PDFEditController::statusMessage);

    QVERIFY(controller.candidatesForPage(1).isEmpty());
    const QVariantMap pendingSquiggly = controller.addHighlight(
        0,
        QRectF(0.01, 0.01, 0.98, 0.40));
    QVERIFY(pendingSquiggly.value(QStringLiteral("pending")).toBool());
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(controller.hasDrafts());

    QVERIFY(!controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(statusSpy.count(), 1);
    QCOMPARE(
        statusSpy.constFirst().constFirst().toString(),
        qtTrId("TextGeometryStillLoading"));
    statusSpy.clear();

    controller.candidatesForPage(2);
    controller.candidatesForPage(1);
    QTRY_COMPARE_WITH_TIMEOUT(controller.draftsForPage(0).size(), 1, 5000);
    const QVariantMap completedSquiggly = controller
        .draftsForPage(0)
        .constFirst()
        .toMap();
    QCOMPARE(
        completedSquiggly.value(QStringLiteral("kind")).toString(),
        QStringLiteral("squiggly"));
    QVERIFY(!completedSquiggly
        .value(QStringLiteral("highlightRects"))
        .toList()
        .isEmpty());
    QCOMPARE(statusSpy.count(), 0);
}

void PDFEditControllerTest::resumesNormalCandidateScanAfterPendingSquigglyPriority()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("pending-squiggly-resumes-normal-scan-source.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SquigglyTool);
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

    QVariantList runningNormalCandidates;
    QVariantMap pendingSquiggly;
    QVariantList deferredNormalCandidates;
    if (holderStarted) {
        // Page 1 is guaranteed to remain in flight until the priority and
        // deferred requests below have both been registered.
        runningNormalCandidates = controller.candidatesForPage(1);
        pendingSquiggly = controller.addHighlight(
            0,
            QRectF(0.01, 0.01, 0.98, 0.40));

        // Request page 2 exactly once. The scheduler must defer it while page
        // 0 preempts page 1, then resume it without another caller request.
        deferredNormalCandidates = controller.candidatesForPage(2);
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
    QVERIFY(runningNormalCandidates.isEmpty());
    QVERIFY(pendingSquiggly.value(QStringLiteral("pending")).toBool());
    QVERIFY(deferredNormalCandidates.isEmpty());
    QVERIFY(controller.draftsForPage(0).isEmpty());

    const auto readyPages = [&candidatesReadySpy]() {
        QVector<int> pages;
        pages.reserve(candidatesReadySpy.size());
        for (const QList<QVariant> &arguments : candidatesReadySpy) {
            pages.append(arguments.constFirst().toInt());
        }
        return pages;
    };
    QTRY_VERIFY_WITH_TIMEOUT(
        readyPages().contains(0) && readyPages().contains(2),
        5000);

    const QVector<int> completedPages = readyPages();
    QVERIFY(completedPages.indexOf(0) < completedPages.indexOf(2));
    QVERIFY(!completedPages.contains(1));
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QVERIFY(!controller.candidatesForPage(0).isEmpty());
    QVERIFY(!controller.candidatesForPage(2).isEmpty());
}

void PDFEditControllerTest::discardsPendingSquigglyBeforeCandidateScanCompletes()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("discard-pending-squiggly-source.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SquigglyTool);

    QVERIFY(controller.candidatesForPage(1).isEmpty());
    const QVariantMap pendingSquiggly = controller.addHighlight(
        0,
        QRectF(0.01, 0.01, 0.98, 0.40));
    QVERIFY(pendingSquiggly.value(QStringLiteral("pending")).toBool());
    QVERIFY(controller.hasDrafts());

    controller.discardDrafts();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(!waitForCandidates(controller, 0).isEmpty());
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::preservesTextEditorWhenPendingSquigglyCompletes()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("pending-squiggly-preserves-editor-source.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);

    const QVariantMap editCandidate = candidateForText(
        waitForCandidates(controller, 2),
        QStringLiteral("Third fixture page"));
    QVERIFY(!editCandidate.isEmpty());

    controller.setTool(PDFEditController::SquigglyTool);
    QVERIFY(controller.candidatesForPage(1).isEmpty());
    const QVariantMap pendingSquiggly = controller.addHighlight(
        0,
        QRectF(0.01, 0.01, 0.98, 0.40));
    QVERIFY(pendingSquiggly.value(QStringLiteral("pending")).toBool());
    QVERIFY(controller.draftsForPage(0).isEmpty());

    const QString selectedObjectId = editCandidate
        .value(QStringLiteral("objectId"))
        .toString();
    controller.setTool(PDFEditController::EditTextTool);
    QVERIFY(controller.selectCandidate(2, selectedObjectId));
    controller.beginSelectedTextEdit();
    QVERIFY(controller.textEditActive());
    QCOMPARE(controller.selectedPage(), 2);
    QCOMPARE(controller.selectedObjectId(), selectedObjectId);

    QTRY_COMPARE_WITH_TIMEOUT(controller.draftsForPage(0).size(), 1, 5000);
    QVERIFY(controller.textEditActive());
    QCOMPARE(controller.selectedPage(), 2);
    QCOMPARE(controller.selectedObjectId(), selectedObjectId);
    QCOMPARE(controller.selectedText(), QStringLiteral("Third fixture page"));
}

void PDFEditControllerTest::remapsPendingSquigglyAcrossPageInsertionUndoRedo()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("remap-pending-squiggly-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("remap-pending-squiggly-output.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SquigglyTool);

    // Keep the single candidate-scan worker occupied so page 1 stays pending
    // while the page-structure commands below remap its display index.
    QVERIFY(controller.candidatesForPage(0).isEmpty());
    const QVariantMap pendingSquiggly = controller.addHighlight(
        1,
        QRectF(0.01, 0.01, 0.98, 0.40));
    QVERIFY(pendingSquiggly.value(QStringLiteral("pending")).toBool());
    QVERIFY(controller.draftsForPage(1).isEmpty());
    QVERIFY(controller.hasDrafts());

    document.setCurrentPage(0);
    QCOMPARE(controller.addBlankPage(), 1);
    QCOMPARE(document.displayPageIndexForSourcePage(1), 2);
    QVERIFY(controller.draftsForPage(2).isEmpty());

    QCOMPARE(controller.undo(), 0);
    QCOMPARE(document.displayPageIndexForSourcePage(1), 1);
    QCOMPARE(controller.redo(), 1);
    QCOMPARE(document.displayPageIndexForSourcePage(1), 2);

    QTRY_COMPARE_WITH_TIMEOUT(controller.draftsForPage(2).size(), 1, 5000);
    QVERIFY(controller.draftsForPage(1).isEmpty());
    const QVariantMap completedSquiggly = controller
        .draftsForPage(2)
        .constFirst()
        .toMap();
    QCOMPARE(
        completedSquiggly.value(QStringLiteral("kind")).toString(),
        QStringLiteral("squiggly"));
    QVERIFY(!completedSquiggly
        .value(QStringLiteral("highlightRects"))
        .toList()
        .isEmpty());

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(document.pageCount(), 4);
    QVERIFY(normalizedPageText(outputPath, 2).contains(
        QStringLiteral("Second fixture page")));
}

void PDFEditControllerTest::cancelsPendingSquigglyWhenSourcePageIsDeleted()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("cancel-deleted-page-pending-squiggly-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("cancel-deleted-page-pending-squiggly-output.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SquigglyTool);

    QVERIFY(controller.candidatesForPage(0).isEmpty());
    const QVariantMap pendingSquiggly = controller.addHighlight(
        1,
        QRectF(0.01, 0.01, 0.98, 0.40));
    QVERIFY(pendingSquiggly.value(QStringLiteral("pending")).toBool());
    QVERIFY(controller.hasDrafts());

    document.setCurrentPage(1);
    QCOMPARE(controller.deleteCurrentPage(), 1);
    QCOMPARE(document.displayPageIndexForSourcePage(1), -1);
    QVERIFY(controller.hasDrafts());

    // Undo restores the source page, but the annotation request was explicitly
    // cancelled when that page was deleted and must not be resurrected by a
    // late candidate-scan result.
    QCOMPARE(controller.undo(), 1);
    QCOMPARE(document.displayPageIndexForSourcePage(1), 1);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!waitForCandidates(controller, 1).isEmpty());
    QVERIFY(controller.draftsForPage(1).isEmpty());
    QVERIFY(!controller.hasDrafts());

    QCOMPARE(controller.redo(), 1);
    QCOMPARE(document.displayPageIndexForSourcePage(1), -1);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(document.pageCount(), 2);
    QVERIFY(!normalizedPageText(outputPath, 0).contains(
        QStringLiteral("Second fixture page")));
    QVERIFY(!normalizedPageText(outputPath, 1).contains(
        QStringLiteral("Second fixture page")));
}

void PDFEditControllerTest::keepsSquigglyGapGroupingStableAcrossPageAspectRatios()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("squiggly-aspect-ratio-source.pdf"));
    QVERIFY(createSquigglyAspectRatioPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    QCOMPARE(document.pageCount(), 2);
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SquigglyTool);

    QVector<QVariantList> markupRectsByPage;
    QVector<QRectF> physicalBoundsByPage;
    for (int page = 0; page < document.pageCount(); ++page) {
        QVERIFY(!waitForCandidates(controller, page).isEmpty());
        const QVariantList markupRects
            = controller.textMarkupRectsForSelection(
                page,
                QRectF(0.0, 0.0, 1.0, 1.0));
        markupRectsByPage.append(markupRects);
        if (markupRects.size() != 1) {
            continue;
        }

        const QRectF normalizedBounds = rectFromCoordinateMap(
            markupRects.constFirst());
        const QSizeF pageSize = document.pageSize(page);
        physicalBoundsByPage.append(QRectF(
            normalizedBounds.x() * pageSize.width(),
            (1.0 - normalizedBounds.bottom()) * pageSize.height(),
            normalizedBounds.width() * pageSize.width(),
            normalizedBounds.height() * pageSize.height()));
    }

    QCOMPARE(markupRectsByPage.size(), 2);
    QCOMPARE(markupRectsByPage.at(0).size(), markupRectsByPage.at(1).size());
    QCOMPARE(markupRectsByPage.at(0).size(), 1);
    QCOMPARE(physicalBoundsByPage.size(), 2);
    constexpr qreal physicalTolerancePoints = 0.01;
    const QRectF portraitBounds = physicalBoundsByPage.at(0);
    const QRectF landscapeBounds = physicalBoundsByPage.at(1);
    QVERIFY(qAbs(portraitBounds.left() - landscapeBounds.left())
        < physicalTolerancePoints);
    QVERIFY(qAbs(portraitBounds.top() - landscapeBounds.top())
        < physicalTolerancePoints);
    QVERIFY(qAbs(portraitBounds.width() - landscapeBounds.width())
        < physicalTolerancePoints);
    QVERIFY(qAbs(portraitBounds.height() - landscapeBounds.height())
        < physicalTolerancePoints);
}

void PDFEditControllerTest::snapsSquigglyToSourceTextAndSavesQuadPoints()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("snap-squiggly-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("snap-squiggly-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SquigglyTool);
    const QVariantList candidates = waitForCandidates(controller, 0);
    const QVariantMap firstLine = candidateForText(
        candidates,
        QString::fromLatin1(kRegularText));
    const QVariantMap secondLine = candidateForText(
        candidates,
        QString::fromLatin1(kBoldText));
    QVERIFY(!firstLine.isEmpty());
    QVERIFY(!secondLine.isEmpty());

    const QRectF firstLineRect = firstLine
        .value(QStringLiteral("sourceRect"))
        .toRectF();
    const QRectF secondLineRect = secondLine
        .value(QStringLiteral("sourceRect"))
        .toRectF();
    const QRectF selectionRect = firstLineRect.united(secondLineRect);
    const QVariantList snappedRects
        = controller.textMarkupRectsForSelection(0, selectionRect);
    QCOMPARE(snappedRects.size(), 2);
    const QRectF firstSnappedRect = rectFromCoordinateMap(
        snappedRects.at(0));
    const QRectF secondSnappedRect = rectFromCoordinateMap(
        snappedRects.at(1));
    QVERIFY(firstSnappedRect.isValid());
    QVERIFY(secondSnappedRect.isValid());
    QVERIFY(firstSnappedRect.intersects(firstLineRect));
    QVERIFY(secondSnappedRect.intersects(secondLineRect));
    QVERIFY(firstSnappedRect.bottom() < secondSnappedRect.top());

    const QSizeF pageSize = document.pageSize(0);
    QVERIFY(pageSize.width() > 0.0);
    QVERIFY(pageSize.height() > 0.0);
    const QRectF firstGlyphBounds = normalizedGlyphBoundsForText(
        sourcePath,
        QString::fromLatin1(kRegularText));
    const QRectF secondGlyphBounds = normalizedGlyphBoundsForText(
        sourcePath,
        QString::fromLatin1(kBoldText));
    QVERIFY(firstGlyphBounds.isValid());
    QVERIFY(secondGlyphBounds.isValid());
    constexpr qreal expectedBottomPaddingPoints = 3.5;
    const qreal expectedGapRatio
        = expectedBottomPaddingPoints / pageSize.height();
    constexpr qreal normalizedTolerance = 0.00001;
    QVERIFY(qAbs(firstSnappedRect.left() - firstGlyphBounds.left())
        < normalizedTolerance);
    QVERIFY(qAbs(firstSnappedRect.top() - firstGlyphBounds.top())
        < normalizedTolerance);
    QVERIFY(qAbs(firstSnappedRect.right() - firstGlyphBounds.right())
        < normalizedTolerance);
    QVERIFY(qAbs(firstSnappedRect.bottom()
                 - qMin<qreal>(1.0,
                               firstGlyphBounds.bottom()
                                   + expectedGapRatio))
        < normalizedTolerance);
    QVERIFY(qAbs(secondSnappedRect.left() - secondGlyphBounds.left())
        < normalizedTolerance);
    QVERIFY(qAbs(secondSnappedRect.top() - secondGlyphBounds.top())
        < normalizedTolerance);
    QVERIFY(qAbs(secondSnappedRect.right() - secondGlyphBounds.right())
        < normalizedTolerance);
    QVERIFY(qAbs(secondSnappedRect.bottom()
                 - qMin<qreal>(1.0,
                               secondGlyphBounds.bottom()
                                   + expectedGapRatio))
        < normalizedTolerance);

    controller.setTool(PDFEditController::SquigglyTool);
    const QVariantMap squiggly = controller.addHighlight(
        0,
        selectionRect);
    QVERIFY(!squiggly.isEmpty());
    QCOMPARE(
        squiggly.value(QStringLiteral("kind")).toString(),
        QStringLiteral("squiggly"));
    QCOMPARE(
        squiggly.value(QStringLiteral("highlightRects")).toList(),
        snappedRects);
    QCOMPARE(
        squiggly.value(QStringLiteral("sourceRect")).toRectF(),
        boundingRectForCoordinateMaps(snappedRects));
    QCOMPARE(controller.draftsForPage(0).size(), 1);

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    const HighlightSaveInspection inspection
        = inspectTextMarkupAnnotations(outputPath, FPDF_ANNOT_SQUIGGLY);
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.count, 1);
    QCOMPARE(inspection.quadPoints.size(), snappedRects.size());

    const auto closeEnough = [](qreal actual, qreal expected) {
        return qAbs(actual - expected) < 0.05;
    };
    const QVector<QRectF> sourceGlyphBounds{
        firstGlyphBounds,
        secondGlyphBounds};
    QVector<bool> matchedQuads(inspection.quadPoints.size(), false);
    for (qsizetype rectIndex = 0;
         rectIndex < snappedRects.size();
         ++rectIndex) {
        const QRectF rect = rectFromCoordinateMap(
            snappedRects.at(rectIndex));
        const qreal expectedLeft = rect.left() * pageSize.width();
        const qreal expectedRight = rect.right() * pageSize.width();
        const qreal expectedTop = (1.0 - rect.top()) * pageSize.height();
        const qreal expectedBottom = (1.0 - rect.bottom())
            * pageSize.height();
        bool matched = false;
        FS_QUADPOINTSF matchedQuad{};
        for (qsizetype index = 0;
             index < inspection.quadPoints.size();
             ++index) {
            if (matchedQuads.at(index)) {
                continue;
            }
            const FS_QUADPOINTSF quad = inspection.quadPoints.at(index);
            if (!closeEnough(quad.x1, expectedLeft)
                || !closeEnough(quad.y1, expectedTop)
                || !closeEnough(quad.x2, expectedRight)
                || !closeEnough(quad.y2, expectedTop)
                || !closeEnough(quad.x3, expectedLeft)
                || !closeEnough(quad.y3, expectedBottom)
                || !closeEnough(quad.x4, expectedRight)
                || !closeEnough(quad.y4, expectedBottom)) {
                continue;
            }
            matchedQuads[index] = true;
            matched = true;
            matchedQuad = quad;
            break;
        }
        QVERIFY(matched);
        const qreal unpaddedBottom = (1.0
            - sourceGlyphBounds.at(rectIndex).bottom())
            * pageSize.height();
        QVERIFY(closeEnough(
            matchedQuad.y3,
            unpaddedBottom - expectedBottomPaddingPoints));
        QVERIFY(closeEnough(
            matchedQuad.y4,
            unpaddedBottom - expectedBottomPaddingPoints));
    }
}

void PDFEditControllerTest::clipsSquigglyGapAtPageBottom()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("bottom-edge-squiggly-source.pdf"));
    QVERIFY(createBottomEdgeTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SquigglyTool);
    const QVariantMap textCandidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kBottomEdgeText));
    QVERIFY(!textCandidate.isEmpty());

    const QSizeF pageSize = document.pageSize(0);
    QVERIFY(pageSize.height() > 0.0);
    const QRectF glyphBounds = normalizedGlyphBoundsForText(
        sourcePath,
        QString::fromLatin1(kBottomEdgeText));
    QVERIFY(glyphBounds.isValid());
    constexpr qreal expectedBottomPaddingPoints = 3.5;
    const qreal expectedGapRatio
        = expectedBottomPaddingPoints / pageSize.height();
    QVERIFY(glyphBounds.bottom() < 1.0);
    QVERIFY(glyphBounds.bottom() + expectedGapRatio > 1.0);

    const QRectF selectionRect = textCandidate
        .value(QStringLiteral("sourceRect"))
        .toRectF();
    const QVariantList snappedRects
        = controller.textMarkupRectsForSelection(0, selectionRect);
    QCOMPARE(snappedRects.size(), 1);
    const QRectF snappedRect = rectFromCoordinateMap(
        snappedRects.constFirst());
    constexpr qreal normalizedTolerance = 0.00001;
    QVERIFY(qAbs(snappedRect.left() - glyphBounds.left())
        < normalizedTolerance);
    QVERIFY(qAbs(snappedRect.top() - glyphBounds.top())
        < normalizedTolerance);
    QVERIFY(qAbs(snappedRect.right() - glyphBounds.right())
        < normalizedTolerance);
    QVERIFY(qAbs(snappedRect.bottom() - 1.0) < normalizedTolerance);

    controller.setTool(PDFEditController::SquigglyTool);
    const QVariantMap squiggly = controller.addHighlight(
        0,
        selectionRect);
    QVERIFY(!squiggly.isEmpty());
    QCOMPARE(
        squiggly.value(QStringLiteral("highlightRects")).toList(),
        snappedRects);
}

void PDFEditControllerTest::usesInvisibleOcrTextForSquiggly()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("invisible-ocr-squiggly-source.pdf"));
    QVERIFY(createInvisibleOcrTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SquigglyTool);

    QSignalSpy candidatesReadySpy(
        &controller,
        &PDFEditController::pageCandidatesReady);
    QVariantList candidates = controller.candidatesForPage(0);
    if (candidatesReadySpy.isEmpty()) {
        QVERIFY(candidatesReadySpy.wait(2000));
    }
    QVERIFY(!candidatesReadySpy.isEmpty());
    QCOMPARE(
        candidatesReadySpy.constLast().constFirst().toInt(),
        0);
    candidates = controller.candidatesForPage(0);

    QVERIFY(candidateForText(
        candidates,
        QStringLiteral("Hidden OCR text")).isEmpty());
    const QVariantMap visibleCandidate = candidateForText(
        candidates,
        QStringLiteral("Visible text"));
    QVERIFY(!visibleCandidate.isEmpty());

    const QRectF hiddenSearchRect(0.08, 0.16, 0.62, 0.16);
    const QVariantList hiddenMarkupRects
        = controller.textMarkupRectsForSelection(0, hiddenSearchRect);
    QVERIFY(!hiddenMarkupRects.isEmpty());
    const QRectF firstHiddenGlyph = rectFromCoordinateMap(
        hiddenMarkupRects.constFirst());
    QVERIFY(firstHiddenGlyph.isValid());
    QVERIFY(hiddenSearchRect.intersects(firstHiddenGlyph));
    QVERIFY(controller.candidateAt(
        0,
        firstHiddenGlyph.center().x(),
        firstHiddenGlyph.center().y()).isEmpty());

    controller.setTool(PDFEditController::SquigglyTool);
    const QVariantMap hiddenSquiggly = controller.addHighlight(
        0,
        hiddenSearchRect);
    QVERIFY(!hiddenSquiggly.isEmpty());
    QCOMPARE(
        hiddenSquiggly.value(QStringLiteral("kind")).toString(),
        QStringLiteral("squiggly"));
    QCOMPARE(
        hiddenSquiggly.value(QStringLiteral("highlightRects")).toList(),
        hiddenMarkupRects);
    QCOMPARE(controller.draftsForPage(0).size(), 1);

    const QRectF visibleRect = visibleCandidate
        .value(QStringLiteral("sourceRect"))
        .toRectF();
    const QVariantList visibleMarkupRects
        = controller.textMarkupRectsForSelection(0, visibleRect);
    QVERIFY(!visibleMarkupRects.isEmpty());
    controller.setTool(PDFEditController::SquigglyTool);
    const QVariantMap visibleSquiggly = controller.addHighlight(
        0,
        visibleRect);
    QVERIFY(!visibleSquiggly.isEmpty());
    QCOMPARE(
        visibleSquiggly.value(QStringLiteral("highlightRects")).toList(),
        visibleMarkupRects);
    QCOMPARE(controller.draftsForPage(0).size(), 2);
}

void PDFEditControllerTest::rejectsVerticalTextForSquiggly()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("vertical-text-squiggly-source.pdf"));
    QVERIFY(createVerticalTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SquigglyTool);
    const QVariantList candidates = waitForCandidates(controller, 0);
    const QVariantMap uprightCandidate = candidateForText(
        candidates,
        QStringLiteral("Upright text"));
    const QVariantMap verticalCandidate = candidateForText(
        candidates,
        QStringLiteral("Vertical text"));
    QVERIFY(!uprightCandidate.isEmpty());
    QVERIFY(!verticalCandidate.isEmpty());

    const QRectF verticalRect = verticalCandidate
        .value(QStringLiteral("sourceRect"))
        .toRectF();
    QCOMPARE(
        controller.candidateAt(
            0,
            verticalRect.center().x(),
            verticalRect.center().y())
            .value(QStringLiteral("text"))
            .toString()
            .trimmed(),
        QStringLiteral("Vertical text"));
    QVERIFY(controller
        .textMarkupRectsForSelection(0, verticalRect)
        .isEmpty());

    controller.setTool(PDFEditController::SquigglyTool);
    QSignalSpy statusSpy(
        &controller,
        &PDFEditController::statusMessage);
    QVERIFY(controller.addHighlight(0, verticalRect).isEmpty());
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QCOMPARE(statusSpy.count(), 1);
    QCOMPARE(
        statusSpy.constFirst().constFirst().toString(),
        qtTrId("NoTextInSelectedArea"));

    const QRectF uprightRect = uprightCandidate
        .value(QStringLiteral("sourceRect"))
        .toRectF();
    const QVariantList uprightMarkupRects
        = controller.textMarkupRectsForSelection(0, uprightRect);
    QVERIFY(!uprightMarkupRects.isEmpty());
    controller.setTool(PDFEditController::SquigglyTool);
    const QVariantMap uprightSquiggly = controller.addHighlight(
        0,
        uprightRect);
    QVERIFY(!uprightSquiggly.isEmpty());
    QCOMPARE(
        uprightSquiggly.value(QStringLiteral("highlightRects")).toList(),
        uprightMarkupRects);
    QCOMPARE(controller.draftsForPage(0).size(), 1);
}

void PDFEditControllerTest::matchesSquigglyPreviewGeometryToPdfium()
{
    const QString componentPath = QFINDTESTDATA(
        "../src/Components/SquigglyLine.qml");
    QVERIFY(!componentPath.isEmpty());

    QQmlEngine engine;
    QQmlComponent component(
        &engine,
        QUrl::fromLocalFile(componentPath));
    const QVariantMap initialProperties{
        {QStringLiteral("waveColor"), QColor(Qt::red)},
        {QStringLiteral("pagePointScale"), 2.0},
        {QStringLiteral("width"), 10.0},
        {QStringLiteral("height"), 12.0}};
    std::unique_ptr<QObject> squiggly(
        component.createWithInitialProperties(initialProperties));
    QVERIFY2(squiggly, qPrintable(component.errorString()));

    QCOMPARE(
        squiggly->property("waveStrokeWidth").toReal(),
        2.0);
    QCOMPARE(squiggly->property("waveDelta").toReal(), 4.0);
    QCOMPARE(
        squiggly->property("wavePath").toString(),
        QStringLiteral("M 0 8 L 4 12 L 8 8 L 10 10"));

    QVERIFY(squiggly->setProperty("width", 3.0));
    QCOMPARE(
        squiggly->property("wavePath").toString(),
        QStringLiteral("M 0 8 L 3 11"));

    QVERIFY(squiggly->setProperty("width", 12.0));
    QCOMPARE(
        squiggly->property("wavePath").toString(),
        QStringLiteral("M 0 8 L 4 12 L 8 8 L 12 12"));

    QVERIFY(squiggly->setProperty("pagePointScale", 1.5));
    QVERIFY(squiggly->setProperty("width", 7.5));
    QVERIFY(squiggly->setProperty("height", 20.0));
    QCOMPARE(
        squiggly->property("waveStrokeWidth").toReal(),
        1.5);
    QCOMPARE(squiggly->property("waveDelta").toReal(), 3.0);
    QCOMPARE(
        squiggly->property("wavePath").toString(),
        QStringLiteral("M 0 17 L 3 20 L 6 17 L 7.5 18.5"));
}

void PDFEditControllerTest::matchesSquigglyPreviewRenderingToSavedPdfium()
{
    if (QGuiApplication::screens().isEmpty()) {
        QSKIP("The Squiggly preview needs a Qt Quick render screen.");
    }

    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("squiggly-preview-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("squiggly-preview-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SquigglyTool);
    const QVariantMap textCandidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kRegularText));
    QVERIFY(!textCandidate.isEmpty());
    const QRectF selectionRect = textCandidate
        .value(QStringLiteral("sourceRect"))
        .toRectF();
    const QVariantList snappedRects
        = controller.textMarkupRectsForSelection(0, selectionRect);
    QCOMPARE(snappedRects.size(), 1);
    const QRectF squigglyRect = rectFromCoordinateMap(
        snappedRects.constFirst());

    controller.setTool(PDFEditController::SquigglyTool);
    controller.setSquigglyColor(QColor(230, 30, 30));
    const QVariantMap draft = controller.addHighlight(0, selectionRect);
    QCOMPARE(
        draft.value(QStringLiteral("highlightRects")).toList(),
        snappedRects);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));

    const QString componentPath = QFINDTESTDATA(
        "../src/Components/SquigglyLine.qml");
    QVERIFY(!componentPath.isEmpty());
    QQmlEngine engine;
    QQmlComponent component(
        &engine,
        QUrl::fromLocalFile(componentPath));
    const QSizeF pageSize = document.pageSize(0);
    for (const qreal pagePointScale : {1.0, 2.0}) {
        const QSize renderedPageSize(
            qRound(pageSize.width() * pagePointScale),
            qRound(pageSize.height() * pagePointScale));
        const QImage savedPage = renderFirstPage(
            outputPath,
            renderedPageSize,
            FPDF_ANNOT);
        QVERIFY(!savedPage.isNull());
        const QRect savedSearchArea = QRectF(
            squigglyRect.x() * savedPage.width(),
            squigglyRect.y() * savedPage.height(),
            squigglyRect.width() * savedPage.width(),
            squigglyRect.height() * savedPage.height())
            .toAlignedRect()
            .adjusted(-2, -2, 2, 2);
        const QRect savedBounds = redPixelBounds(
            savedPage,
            savedSearchArea);
        QVERIFY(savedBounds.isValid());

        const qreal previewWidth = squigglyRect.width()
            * pageSize.width()
            * pagePointScale;
        const qreal previewHeight = squigglyRect.height()
            * pageSize.height()
            * pagePointScale;
        const QSize previewImageSize(
            qMax(1, qCeil(previewWidth)),
            qMax(1, qCeil(previewHeight)));
        const QVariantMap initialProperties{
            {QStringLiteral("waveColor"), QColor(230, 30, 30)},
            {QStringLiteral("pagePointScale"), pagePointScale},
            {QStringLiteral("width"), previewWidth},
            {QStringLiteral("height"), previewHeight}};
        std::unique_ptr<QObject> previewObject(
            component.createWithInitialProperties(initialProperties));
        QVERIFY2(previewObject, qPrintable(component.errorString()));
        auto *previewItem = qobject_cast<QQuickItem *>(previewObject.get());
        QVERIFY(previewItem);

        QQuickWindow window;
        window.setColor(Qt::white);
        window.resize(previewImageSize);
        previewItem->setParentItem(window.contentItem());
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        const QSharedPointer<QQuickItemGrabResult> grabResult
            = previewItem->grabToImage(previewImageSize);
        QVERIFY(grabResult);
        QSignalSpy readySpy(
            grabResult.data(),
            &QQuickItemGrabResult::ready);
        if (grabResult->image().isNull()) {
            QTRY_VERIFY_WITH_TIMEOUT(readySpy.count() > 0, 5000);
        }
        const QImage previewImage = grabResult->image();
        QVERIFY(!previewImage.isNull());
        const QRect previewBounds = redPixelBounds(previewImage);
        QVERIFY(previewBounds.isValid());

        const qreal savedBottom = squigglyRect.bottom()
            * savedPage.height();
        const qreal savedTop = squigglyRect.top()
            * savedPage.height();
        const qreal savedWaveTopOffset = savedBounds.top() - savedTop;
        const qreal savedWaveBottomOffset = savedBottom
            - (savedBounds.bottom() + 1);
        const qreal previewWaveTopOffset = previewBounds.top();
        const qreal previewWaveBottomOffset = previewImage.height()
            - (previewBounds.bottom() + 1);
        // Qt Quick and PDFium rasterize the same point geometry independently;
        // allow one antialiased edge pixel on each side of the wave band.
        QVERIFY2(
            qAbs(savedBounds.height() - previewBounds.height()) <= 2,
            qPrintable(QStringLiteral(
                "scale=%1 savedBounds=%2,%3 %4x%5 "
                "previewBounds=%6,%7 %8x%9")
                .arg(pagePointScale)
                .arg(savedBounds.x())
                .arg(savedBounds.y())
                .arg(savedBounds.width())
                .arg(savedBounds.height())
                .arg(previewBounds.x())
                .arg(previewBounds.y())
                .arg(previewBounds.width())
                .arg(previewBounds.height())));
        QVERIFY(qAbs(savedWaveTopOffset - previewWaveTopOffset) <= 2.0);
        QVERIFY(qAbs(savedWaveBottomOffset - previewWaveBottomOffset) <= 2.0);
    }
}

void PDFEditControllerTest::addsTextMarkupWithUndoRedoAndDelete_data()
{
    QTest::addColumn<int>("tool");
    QTest::addColumn<QString>("kind");

    QTest::newRow("highlight")
        << static_cast<int>(PDFEditController::HighlightTool)
        << QStringLiteral("highlight");
    QTest::newRow("underline")
        << static_cast<int>(PDFEditController::UnderlineTool)
        << QStringLiteral("underline");
    QTest::newRow("strikeout")
        << static_cast<int>(PDFEditController::StrikeoutTool)
        << QStringLiteral("strikeout");
    QTest::newRow("squiggly")
        << static_cast<int>(PDFEditController::SquigglyTool)
        << QStringLiteral("squiggly");
}

void PDFEditControllerTest::addsTextMarkupWithUndoRedoAndDelete()
{
    QFETCH(int, tool);
    QFETCH(QString, kind);

    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("add-%1-source.pdf").arg(kind));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);

    QVERIFY(!waitForCandidates(controller, 0).isEmpty());
    controller.setTool(static_cast<PDFEditController::Tool>(tool));
    const QRectF selectionRect(0.05, 0.05, 0.90, 0.65);
    const QVariantList snappedRects
        = controller.textMarkupRectsForSelection(0, selectionRect);
    QVERIFY(snappedRects.size() > 1);
    const QRectF expectedSourceRect
        = boundingRectForCoordinateMaps(snappedRects);
    const QVariantMap annotation = controller.addHighlight(
        0,
        selectionRect);
    QCOMPARE(annotation.value(QStringLiteral("kind")).toString(), kind);
    QCOMPARE(
        annotation.value(QStringLiteral("sourceRect")).toRectF(),
        expectedSourceRect);
    QCOMPARE(
        annotation.value(QStringLiteral("highlightRects")).toList(),
        snappedRects);
    QVERIFY(
        annotation.value(QStringLiteral("objectId")).toString().startsWith(
            kind + QLatin1Char(':')));
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QVERIFY(!candidateForKind(controller.candidatesForPage(0), kind).isEmpty());

    QCOMPARE(controller.undo(), -1);
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(candidateForKind(controller.candidatesForPage(0), kind).isEmpty());
    QCOMPARE(controller.redo(), -1);
    QCOMPARE(controller.draftsForPage(0).size(), 1);

    controller.setTool(PDFEditController::SelectTool);
    const QString objectId = annotation
        .value(QStringLiteral("objectId"))
        .toString();
    QVERIFY(controller.selectCandidate(0, objectId));
    controller.deleteSelectedObject();
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(candidateForKind(controller.candidatesForPage(0), kind).isEmpty());

    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QCOMPARE(
        controller.draftsForPage(0)
            .constFirst()
            .toMap()
            .value(QStringLiteral("kind"))
            .toString(),
        kind);
    QCOMPARE(controller.redo(), -1);
    QVERIFY(controller.draftsForPage(0).isEmpty());
}

void PDFEditControllerTest::savesHighlightAnnotationRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-highlight-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-highlight-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QSizeF pageSize = document.pageSize(0);
    const QVariantMap textCandidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kRegularText));
    QVERIFY(!textCandidate.isEmpty());
    controller.setTool(PDFEditController::HighlightTool);
    const QRectF selectionRect = textCandidate
        .value(QStringLiteral("sourceRect"))
        .toRectF();
    const QVariantList snappedRects
        = controller.textMarkupRectsForSelection(0, selectionRect);
    QCOMPARE(snappedRects.size(), 1);
    const QRectF expectedRect = rectFromCoordinateMap(
        snappedRects.constFirst());
    const QColor highlightColor(139, 195, 74, 96);
    controller.setHighlightColor(highlightColor);
    const QVariantMap addedHighlight = controller.addHighlight(
        0,
        selectionRect);
    QVERIFY(!addedHighlight.isEmpty());
    QCOMPARE(
        addedHighlight.value(QStringLiteral("highlightRects")).toList(),
        snappedRects);
    QVERIFY(!addedHighlight
        .value(QStringLiteral("commentAuthor"))
        .toString()
        .isEmpty());
    const QDateTime creationDate = addedHighlight
        .value(QStringLiteral("commentCreationDate"))
        .toDateTime();
    QVERIFY(creationDate.isValid());
    QVERIFY(!addedHighlight
        .value(QStringLiteral("commentModifiedDate"))
        .toDateTime()
        .isValid());
    QCOMPARE(
        addedHighlight.value(QStringLiteral("commentDate")).toDateTime(),
        creationDate);

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(document.source().toLocalFile(), outputPath);

    const HighlightSaveInspection sourceInspection
        = inspectHighlights(sourcePath);
    QVERIFY(sourceInspection.readable);
    QCOMPARE(sourceInspection.count, 0);

    const HighlightSaveInspection inspection
        = inspectHighlights(outputPath);
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.count, 1);
    QCOMPARE(inspection.color, highlightColor);
    QVERIFY(inspection.flags & FPDF_ANNOT_FLAG_PRINT);
    QCOMPARE(inspection.contents, QString());
    QCOMPARE(inspection.quadPoints.size(), 1);
    QVERIFY(inspection.hasAppearance);
    QVERIFY(!inspection.appearance.isEmpty());
    QVERIFY(inspection.appearanceObjectCount > 0);
    QCOMPARE(
        inspection.appearancePathCount,
        inspection.appearanceObjectCount);
    QCOMPARE(
        inspection.author,
        addedHighlight.value(QStringLiteral("commentAuthor")).toString());
    QVERIFY(!inspection.creationDate.isEmpty());
    QVERIFY(inspection.modificationDate.isEmpty());

    const qreal expectedLeft = expectedRect.left() * pageSize.width();
    const qreal expectedRight = expectedRect.right() * pageSize.width();
    const qreal expectedTop = (1.0 - expectedRect.top())
        * pageSize.height();
    const qreal expectedBottom = (1.0 - expectedRect.bottom())
        * pageSize.height();
    const auto closeEnough = [](qreal actual, qreal expected) {
        return qAbs(actual - expected) < 0.05;
    };
    QVERIFY(closeEnough(inspection.rect.left, expectedLeft));
    QVERIFY(closeEnough(inspection.rect.right, expectedRight));
    QVERIFY(closeEnough(inspection.rect.top, expectedTop));
    QVERIFY(closeEnough(inspection.rect.bottom, expectedBottom));
    const FS_QUADPOINTSF quad = inspection.quadPoints.constFirst();
    QVERIFY(closeEnough(quad.x1, expectedLeft));
    QVERIFY(closeEnough(quad.y1, expectedTop));
    QVERIFY(closeEnough(quad.x2, expectedRight));
    QVERIFY(closeEnough(quad.y2, expectedTop));
    QVERIFY(closeEnough(quad.x3, expectedLeft));
    QVERIFY(closeEnough(quad.y3, expectedBottom));
    QVERIFY(closeEnough(quad.x4, expectedRight));
    QVERIFY(closeEnough(quad.y4, expectedBottom));
}

void PDFEditControllerTest::savesTextMarkupAnnotationRoundTrip_data()
{
    QTest::addColumn<int>("tool");
    QTest::addColumn<int>("subtype");
    QTest::addColumn<QString>("kind");

    QTest::newRow("highlight")
        << static_cast<int>(PDFEditController::HighlightTool)
        << static_cast<int>(FPDF_ANNOT_HIGHLIGHT)
        << QStringLiteral("highlight");
    QTest::newRow("underline")
        << static_cast<int>(PDFEditController::UnderlineTool)
        << static_cast<int>(FPDF_ANNOT_UNDERLINE)
        << QStringLiteral("underline");
    QTest::newRow("strikeout")
        << static_cast<int>(PDFEditController::StrikeoutTool)
        << static_cast<int>(FPDF_ANNOT_STRIKEOUT)
        << QStringLiteral("strikeout");
    QTest::newRow("squiggly")
        << static_cast<int>(PDFEditController::SquigglyTool)
        << static_cast<int>(FPDF_ANNOT_SQUIGGLY)
        << QStringLiteral("squiggly");
}

void PDFEditControllerTest::savesTextMarkupAnnotationRoundTrip()
{
    QFETCH(int, tool);
    QFETCH(int, subtype);
    QFETCH(QString, kind);

    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-%1-source.pdf").arg(kind));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-%1-output.pdf").arg(kind));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QSizeF pageSize = document.pageSize(0);
    QVERIFY(!waitForCandidates(controller, 0).isEmpty());
    controller.setTool(static_cast<PDFEditController::Tool>(tool));
    const QRectF selectionRect(0.05, 0.05, 0.90, 0.65);
    const QVariantList snappedRects
        = controller.textMarkupRectsForSelection(0, selectionRect);
    QVERIFY(snappedRects.size() > 1);
    QVector<QRectF> expectedRects;
    expectedRects.reserve(snappedRects.size());
    for (const QVariant &rectValue : snappedRects) {
        expectedRects.append(rectFromCoordinateMap(rectValue));
    }
    const QColor annotationColor(38, 166, 154, 160);
    const QColor unrelatedHighlightColor(255, 128, 171, 96);
    if (tool == static_cast<int>(PDFEditController::UnderlineTool)) {
        controller.setHighlightColor(unrelatedHighlightColor);
        controller.setUnderlineColor(annotationColor);
        QCOMPARE(controller.highlightColor(), unrelatedHighlightColor);
    } else if (tool == static_cast<int>(PDFEditController::SquigglyTool)) {
        controller.setHighlightColor(unrelatedHighlightColor);
        controller.setSquigglyColor(annotationColor);
        QCOMPARE(controller.highlightColor(), unrelatedHighlightColor);
    } else if (tool == static_cast<int>(PDFEditController::StrikeoutTool)) {
        controller.setHighlightColor(unrelatedHighlightColor);
        controller.setStrikeoutColor(annotationColor);
        QCOMPARE(controller.highlightColor(), unrelatedHighlightColor);
    } else {
        controller.setHighlightColor(annotationColor);
    }
    const QVariantMap addedAnnotation = controller.addHighlight(
        0,
        selectionRect);
    QCOMPARE(
        addedAnnotation.value(QStringLiteral("kind")).toString(),
        kind);
    QCOMPARE(
        addedAnnotation
            .value(QStringLiteral("highlightColor"))
            .value<QColor>(),
        annotationColor);
    QCOMPARE(
        addedAnnotation.value(QStringLiteral("highlightRects")).toList(),
        snappedRects);

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());

    const HighlightSaveInspection sourceInspection
        = inspectTextMarkupAnnotations(
            sourcePath,
            static_cast<FPDF_ANNOTATION_SUBTYPE>(subtype));
    QVERIFY(sourceInspection.readable);
    QCOMPARE(sourceInspection.count, 0);

    const HighlightSaveInspection inspection
        = inspectTextMarkupAnnotations(
            outputPath,
            static_cast<FPDF_ANNOTATION_SUBTYPE>(subtype));
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.count, 1);
    QCOMPARE(inspection.color, annotationColor);
    QVERIFY(inspection.flags & FPDF_ANNOT_FLAG_PRINT);
    QCOMPARE(inspection.contents, QString());
    QCOMPARE(inspection.quadPoints.size(), expectedRects.size());
    QVERIFY(inspection.hasAppearance);
    QVERIFY(!inspection.appearance.isEmpty());
    QVERIFY(inspection.appearanceObjectCount > 0);
    QCOMPARE(
        inspection.appearancePathCount,
        inspection.appearanceObjectCount);

    QRectF expectedAnnotationRect;
    for (const QRectF &rect : std::as_const(expectedRects)) {
        expectedAnnotationRect = expectedAnnotationRect.isEmpty()
            ? rect
            : expectedAnnotationRect.united(rect);
    }
    const qreal expectedLeft = expectedAnnotationRect.left()
        * pageSize.width();
    const qreal expectedRight = expectedAnnotationRect.right()
        * pageSize.width();
    const qreal expectedTop = (1.0 - expectedAnnotationRect.top())
        * pageSize.height();
    const qreal expectedBottom = (1.0 - expectedAnnotationRect.bottom())
        * pageSize.height();
    const auto closeEnough = [](qreal actual, qreal expected) {
        return qAbs(actual - expected) < 0.05;
    };
    QVERIFY(closeEnough(inspection.rect.left, expectedLeft));
    QVERIFY(closeEnough(inspection.rect.right, expectedRight));
    QVERIFY(closeEnough(inspection.rect.top, expectedTop));
    QVERIFY(closeEnough(inspection.rect.bottom, expectedBottom));
    for (qsizetype index = 0; index < expectedRects.size(); ++index) {
        const QRectF rect = expectedRects.at(index);
        const qreal quadLeft = rect.left() * pageSize.width();
        const qreal quadRight = rect.right() * pageSize.width();
        const qreal quadTop = (1.0 - rect.top()) * pageSize.height();
        const qreal quadBottom = (1.0 - rect.bottom()) * pageSize.height();
        const FS_QUADPOINTSF quad = inspection.quadPoints.at(index);
        QVERIFY(closeEnough(quad.x1, quadLeft));
        QVERIFY(closeEnough(quad.y1, quadTop));
        QVERIFY(closeEnough(quad.x2, quadRight));
        QVERIFY(closeEnough(quad.y2, quadTop));
        QVERIFY(closeEnough(quad.x3, quadLeft));
        QVERIFY(closeEnough(quad.y3, quadBottom));
        QVERIFY(closeEnough(quad.x4, quadRight));
        QVERIFY(closeEnough(quad.y4, quadBottom));
    }
}

void PDFEditControllerTest::savesHighlightAlongsideTextEdit()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-highlight-text-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-highlight-text-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap textCandidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kRegularText));
    QVERIFY(!textCandidate.isEmpty());
    controller.setTool(PDFEditController::HighlightTool);
    QVERIFY(!controller.addHighlight(
        0,
        textCandidate.value(QStringLiteral("sourceRect")).toRectF()).isEmpty());
    controller.setTool(PDFEditController::EditTextTool);
    QVERIFY(controller.selectCandidate(
        0,
        textCandidate.value(QStringLiteral("objectId")).toString()));
    const QString editedText = QStringLiteral("Highlighted edited text");
    controller.commitSelectedText(editedText);

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(normalizedPageText(outputPath).contains(editedText));
    const HighlightSaveInspection inspection
        = inspectHighlights(outputPath);
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.count, 1);
    QCOMPARE(inspection.quadPoints.size(), 1);
    QVERIFY(inspection.hasAppearance);
    QVERIFY(!inspection.appearance.isEmpty());
    QVERIFY(inspection.appearanceObjectCount > 0);
    QCOMPARE(
        inspection.appearancePathCount,
        inspection.appearanceObjectCount);
}

void PDFEditControllerTest::preservesPatternGraphicsWhenSavingHighlight()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-highlight-pattern-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-highlight-pattern-output.pdf"));
    QVERIFY(createPatternFillPdf(sourcePath));
    QVERIFY(containsPatternColors(renderFirstPage(sourcePath, QSize(200, 200))));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap textCandidate = candidateForText(
        waitForCandidates(controller, 0),
        QStringLiteral("Pattern edit text"));
    QVERIFY(!textCandidate.isEmpty());
    controller.setTool(PDFEditController::HighlightTool);
    QVERIFY(!controller.addHighlight(
        0,
        textCandidate.value(QStringLiteral("sourceRect")).toRectF()).isEmpty());

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    const HighlightSaveInspection inspection
        = inspectHighlights(outputPath);
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.count, 1);
    QVERIFY(containsPatternColors(renderFirstPage(outputPath, QSize(200, 200))));
}
