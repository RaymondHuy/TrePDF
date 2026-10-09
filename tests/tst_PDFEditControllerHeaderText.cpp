#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

#include <limits>

using namespace PDFEditControllerTestSupport;

namespace {

QByteArray headerFileBytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool createHeaderPages(const QString &path, const QList<QSizeF> &sizes,
                       int rotation = 0, bool cropped = false)
{
    QByteArray kids;
    QVector<QByteArray> pages;
    for (int index = 0; index < sizes.size(); ++index) {
        kids += QByteArray::number(index + 3) + " 0 R ";
        const QSizeF size = sizes.at(index);
        pages.append(QByteArray("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 ")
            + QByteArray::number(size.width()) + ' ' + QByteArray::number(size.height())
            + "] /Resources << >> /Rotate " + QByteArray::number(rotation)
            + (cropped ? " /CropBox [20 30 280 170]" : "") + " >>");
    }
    QVector<QByteArray> objects{
        QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
        QByteArray("<< /Type /Pages /Kids [") + kids + "] /Count "
            + QByteArray::number(sizes.size()) + " >>"
    };
    objects += pages;
    return writePdfObjects(path, objects);
}

QRect darkPixelBounds(const QImage &image)
{
    QRect result;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.alpha() > 128 && qGray(color.rgb()) < 128)
                result = result.united(QRect(x, y, 1, 1));
        }
    }
    return result;
}

QVariantList headerDraftSnapshot(const PDFEditController &controller, int pageCount)
{
    QVariantList snapshot;
    for (int page = 0; page < pageCount; ++page)
        snapshot.append(QVariant(controller.draftsForPage(page)));
    return snapshot;
}

QVector<QColor> headerDraftColors(const QVariantMap &draft)
{
    QVector<QColor> result(draft.value(QStringLiteral("text")).toString().size());
    const QVariantList spans = draft.value(QStringLiteral("characterSpans")).toList();
    for (const QVariant &value : spans) {
        const QVariantMap span = value.toMap();
        const int start = span.value(QStringLiteral("start")).toInt();
        const int end = start + span.value(QStringLiteral("length")).toInt();
        if (start < 0 || end > result.size())
            return {};
        for (int index = start; index < end; ++index)
            result[index] = span.value(QStringLiteral("color")).value<QColor>();
    }
    return result;
}

} // namespace

void PDFEditControllerTest::headerTextUsesAtomicHistoryAndIndependentSessions()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("header-history.pdf"));
    QVERIFY(createThreePageTextPdf(path));
    const QByteArray original = headerFileBytes(path);
    PDFDocumentSession session;
    PDFDocumentSession otherSession;
    QVERIFY(session.open(QUrl::fromLocalFile(path)));
    QVERIFY(otherSession.open(QUrl::fromLocalFile(path)));
    auto &controller = *session.editController();
    auto &other = *otherSession.editController();

    QVERIFY(!controller.addText(1, 0.2, 0.6).isEmpty());
    controller.updateSelectedTextPreview(QStringLiteral("Existing body draft"));
    controller.finishSelectedTextEdit();
    controller.setTool(PDFEditController::BrowseTool);
    const QVariantList before = headerDraftSnapshot(controller, 3);
    const QString text = QStringLiteral("Header line one\nHeader line two");
    QVERIFY(controller.addHeaderText(text, QStringLiteral("all")));
    QCOMPARE(controller.tool(), PDFEditController::BrowseTool);
    QVERIFY(!controller.textEditActive());
    QVERIFY(controller.canUndo());
    QVERIFY(!controller.canRedo());
    for (int page = 0; page < 3; ++page) {
        const QVariantList drafts = controller.draftsForPage(page);
        QCOMPARE(drafts.size(), page == 1 ? 2 : 1);
        QVariantMap header;
        for (const QVariant &value : drafts) {
            const QVariantMap draft = value.toMap();
            if (draft.value(QStringLiteral("text")).toString() == text)
                header = draft;
        }
        QVERIFY(!header.isEmpty());
        QCOMPARE(header.value(QStringLiteral("kind")).toString(), QStringLiteral("addedText"));
        const QSizeF size = session.document()->pageSize(page);
        const QRectF frame = header.value(QStringLiteral("frameRect")).toRectF();
        const QRectF content = header.value(QStringLiteral("contentBounds")).toRectF();
        QVERIFY(qAbs(frame.left() * size.width() - 24.0) < 0.01);
        QVERIFY(qAbs(frame.top() * size.height() - 16.0) < 0.01);
        QVERIFY(frame.bottom() * size.height() <= 100.01);
        QVERIFY(content.bottom() * size.height() <= 100.01);
        QVERIFY(qAbs(header.value(QStringLiteral("fontSizeRatio")).toReal()
            * size.height() - 12.0) < 0.01);
        QCOMPARE(header.value(QStringLiteral("color")).value<QColor>(), QColor(Qt::black));
        QVERIFY(other.draftsForPage(page).isEmpty());
    }
    QVERIFY(!other.hasDrafts());
    const QVariantList applied = headerDraftSnapshot(controller, 3);
    controller.undo();
    QCOMPARE(headerDraftSnapshot(controller, 3), before);
    QVERIFY(controller.canUndo()); // The pre-existing body edit remains its own command.
    QVERIFY(controller.canRedo());
    controller.redo();
    QCOMPARE(headerDraftSnapshot(controller, 3), applied);
    controller.discardDrafts();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
    for (int page = 0; page < 3; ++page)
        QVERIFY(controller.draftsForPage(page).isEmpty());
    QCOMPARE(headerFileBytes(path), original);
}

void PDFEditControllerTest::headerTextAppliesCustomPageRangesOnce()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("header-ranges.pdf"));
    QVERIFY(createThreePageTextPdf(path));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    PDFEditController controller;
    controller.setDocument(&document);
    QVERIFY(controller.addHeaderText(QStringLiteral("Selected"), QStringLiteral("3, 1, 3-3, 1")));
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QVERIFY(controller.draftsForPage(1).isEmpty());
    QCOMPARE(controller.draftsForPage(2).size(), 1);
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(controller.addHeaderText(QStringLiteral("Range"), QStringLiteral("3, 1-2, 2, 1")));
    for (int page = 0; page < 3; ++page)
        QCOMPARE(controller.draftsForPage(page).size(), 1);
    QVERIFY(!controller.canRedo());
}

void PDFEditControllerTest::rejectsInvalidHeaderTextWithoutChangingHistory_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<QString>("pages");
    for (const char *range : {"", " ", "0", "4", "2-1", "1-4", "1,4", "1,", ",1",
             "1,,2", "-1", "1-", "1-2-3", "1.5", "all,1", "2147483648"}) {
        QTest::newRow(range[0] ? range : "empty-pages")
            << QStringLiteral("Valid header") << QString::fromLatin1(range);
    }
    QTest::newRow("empty-text") << QString() << QStringLiteral("all");
    QTest::newRow("only-whitespace") << QStringLiteral(" \n\t ") << QStringLiteral("all");
    QTest::newRow("over-700-characters") << QString(701, QLatin1Char('x')) << QStringLiteral("all");
    QTest::newRow("too-many-lines") << QStringLiteral("line\n").repeated(12) << QStringLiteral("all");
    QTest::newRow("embedded-null") << QStringLiteral("a") + QChar(0) + QStringLiteral("b")
        << QStringLiteral("all");
}

void PDFEditControllerTest::rejectsInvalidHeaderTextWithoutChangingHistory()
{
    QFETCH(QString, text);
    QFETCH(QString, pages);
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("header-reject.pdf"));
    QVERIFY(createThreePageTextPdf(path));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    PDFEditController controller;
    controller.setDocument(&document);
    QVERIFY(controller.addHeaderText(QStringLiteral("Retained"), QStringLiteral("1")));
    QVERIFY(controller.addHeaderText(QStringLiteral("Redo me"), QStringLiteral("2")));
    controller.undo();
    const QVariantList before = headerDraftSnapshot(controller, 3);
    QSignalSpy history(&controller, &PDFEditController::historyStateChanged);
    QSignalSpy changed(&controller, &PDFEditController::pageDataChanged);
    QVERIFY(!controller.addHeaderText(text, pages));
    QCOMPARE(headerDraftSnapshot(controller, 3), before);
    QCOMPARE(history.count(), 0);
    QCOMPARE(changed.count(), 0);
    QVERIFY(controller.canUndo());
    QVERIFY(controller.canRedo());
    controller.redo();
    QCOMPARE(controller.draftsForPage(1).size(), 1);
}

void PDFEditControllerTest::rejectsHeaderTextWhenAnyPageCannotFit()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("header-narrow-last-page.pdf"));
    QVERIFY(createHeaderPages(path, {QSizeF(300, 400), QSizeF(48, 400)}));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    PDFEditController controller;
    controller.setDocument(&document);
    QVERIFY(!controller.addHeaderText(QStringLiteral("Header"), QStringLiteral("all")));
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(controller.draftsForPage(1).isEmpty());
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(controller.addHeaderText(QStringLiteral("Header"), QStringLiteral("1")));
    QCOMPARE(controller.draftsForPage(0).size(), 1);
}

void PDFEditControllerTest::savesUnicodeHeaderTextOnSelectedPages()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("header-unicode-source.pdf"));
    const QString target = m_temporaryDirectory.filePath(QStringLiteral("header-unicode-target.pdf"));
    QVERIFY(createThreePageTextPdf(source));
    const QByteArray original = headerFileBytes(source);
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    PDFEditController controller;
    controller.setDocument(&document);
    const QString text = QStringLiteral("Đầu trang\nTiếng Việt");
    QVERIFY(controller.addHeaderText(text, QStringLiteral("1,3")));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(target)));
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QCOMPARE(headerFileBytes(source), original);
    QCOMPARE(document.source(), QUrl::fromLocalFile(target));
    for (int page = 0; page < 3; ++page) {
        const QString saved = normalizedPageText(target, page);
        QCOMPARE(saved.contains(QStringLiteral("Đầu trang")), page != 1);
        QCOMPARE(saved.contains(QStringLiteral("Tiếng Việt")), page != 1);
        QVERIFY(saved.contains(QStringLiteral("fixture page")));
    }
    PDFDocument reopened;
    QVERIFY(reopened.open(QUrl::fromLocalFile(target)));
    PDFEditController reopenedController;
    reopenedController.setDocument(&reopened);
    QVERIFY(!reopenedController.hasDrafts());
    QVERIFY(reopenedController.addHeaderText(QStringLiteral("Next header"), QStringLiteral("2")));
    QVERIFY(reopenedController.save());
    QVERIFY(normalizedPageText(target, 1).contains(QStringLiteral("Next header")));
    QVERIFY(normalizedPageText(target, 0).contains(QStringLiteral("Đầu trang")));
    QCOMPARE(headerFileBytes(source), original);
}

void PDFEditControllerTest::mapsHeaderTextThroughPageChanges()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("header-page-changes-source.pdf"));
    const QString target = m_temporaryDirectory.filePath(QStringLiteral("header-page-changes-target.pdf"));
    QVERIFY(createThreePageTextPdf(source));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    PDFEditController controller;
    controller.setDocument(&document);
    QVERIFY(controller.addHeaderText(QStringLiteral("Original third"), QStringLiteral("3")));
    document.setCurrentPage(0);
    QCOMPARE(controller.addBlankPage(), 1);
    QCOMPARE(document.pageCount(), 4);
    QCOMPARE(controller.draftsForPage(3).size(), 1);
    QVERIFY(controller.addHeaderText(QStringLiteral("Inserted page"), QStringLiteral("2")));
    document.setCurrentPage(0);
    QCOMPARE(controller.deleteCurrentPage(), 0);
    QCOMPARE(document.pageCount(), 3);
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QVERIFY(controller.draftsForPage(1).isEmpty());
    QCOMPARE(controller.draftsForPage(2).size(), 1);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(target)));
    QVERIFY(normalizedPageText(target, 0).contains(QStringLiteral("Inserted page")));
    QVERIFY(normalizedPageText(target, 1).contains(QStringLiteral("Second fixture page")));
    QVERIFY(!normalizedPageText(target, 1).contains(QStringLiteral("Original third")));
    QVERIFY(normalizedPageText(target, 2).contains(QStringLiteral("Original third")));
    QVERIFY(normalizedPageText(target, 2).contains(QStringLiteral("Third fixture page")));
}

void PDFEditControllerTest::savesHeaderTextInsideCroppedRotatedHeader_data()
{
    QTest::addColumn<int>("rotation");
    QTest::addColumn<bool>("cropped");
    for (int rotation : {0, 90, 180, 270}) {
        for (bool cropped : {false, true}) {
            const QByteArray name = QByteArray::number(rotation) + (cropped ? "-cropped" : "-media");
            QTest::newRow(name.constData()) << rotation << cropped;
        }
    }
}

void PDFEditControllerTest::savesHeaderTextInsideCroppedRotatedHeader()
{
    QFETCH(int, rotation);
    QFETCH(bool, cropped);
    const QString tag = QString::fromLatin1(QTest::currentDataTag());
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("header-geometry-%1-source.pdf").arg(tag));
    const QString target = m_temporaryDirectory.filePath(QStringLiteral("header-geometry-%1-target.pdf").arg(tag));
    QVERIFY(createHeaderPages(source, {QSizeF(300, 200)}, rotation, cropped));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    PDFEditController controller;
    controller.setDocument(&document);
    QVERIFY(controller.addHeaderText(QStringLiteral("Header\nSecond line"), QStringLiteral("all")));
    const QSize size = document.pageSize(0).toSize();
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(target)));
    QVERIFY(normalizedPageText(target).contains(QStringLiteral("Header")));
    const QImage image = renderFirstPage(target, size);
    QVERIFY(!image.isNull());
    const QRect bounds = darkPixelBounds(image);
    QVERIFY(!bounds.isEmpty());
    QVERIFY2(bounds.top() >= 15 && bounds.bottom() < 100,
        qPrintable(QStringLiteral("Rendered header bounds: x=%1 y=%2 w=%3 h=%4")
            .arg(bounds.x()).arg(bounds.y()).arg(bounds.width()).arg(bounds.height())));
    QVERIFY(bounds.left() >= 23);
    QVERIFY(bounds.right() <= size.width() - 23);
}

void PDFEditControllerTest::guardsHeaderTextDuringSaveAndWatermarks()
{
    PDFEditController unloaded;
    QVERIFY(!unloaded.addHeaderText(QStringLiteral("Header"), QStringLiteral("all")));
    QVERIFY(!unloaded.hasDrafts());
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("header-guards-source.pdf"));
    const QString target = m_temporaryDirectory.filePath(QStringLiteral("header-guards-target.pdf"));
    QVERIFY(createThreePageTextPdf(source));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    PDFEditController controller;
    controller.setDocument(&document);
    controller.loadWatermarks();
    QTRY_VERIFY_WITH_TIMEOUT(controller.watermarksReady(), 10000);
    const QVariantMap watermark{
        {QStringLiteral("text"), QStringLiteral("DRAFT")},
        {QStringLiteral("fontFamily"), QStringLiteral("Helvetica")},
        {QStringLiteral("pages"), QStringLiteral("all")}
    };
    QVERIFY(controller.applyWatermark(watermark));
    QVERIFY(!controller.addHeaderText(QStringLiteral("Blocked"), QStringLiteral("all")));
    for (int page = 0; page < 3; ++page)
        QVERIFY(controller.draftsForPage(page).isEmpty());
    controller.discardDrafts();
    QVERIFY(controller.addHeaderText(QStringLiteral("Saved header"), QStringLiteral("all")));
    QSignalSpy saved(&controller, &PDFEditController::saveFinished);
    QVERIFY(controller.requestSaveAs(QUrl::fromLocalFile(target)));
    QVERIFY(controller.saving());
    QVERIFY(!controller.addHeaderText(QStringLiteral("Too late"), QStringLiteral("all")));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 15000);
    QVERIFY(saved.constFirst().constFirst().toBool());
    QVERIFY(!normalizedPageText(target).contains(QStringLiteral("Too late")));

    PDFDocument restricted;
    QVERIFY(restricted.open(QUrl::fromLocalFile(QFINDTESTDATA("fixtures/password-3.pdf")),
        QStringLiteral("fixture-open-3")));
    PDFEditController restrictedController;
    restrictedController.setDocument(&restricted);
    QVERIFY(!restrictedController.addHeaderText(QStringLiteral("Blocked"), QStringLiteral("all")));
    QVERIFY(!restrictedController.hasDrafts());
}

void PDFEditControllerTest::keepsHeaderTextDraftsAfterSaveFailure()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("header-save-failure-source.pdf"));
    const QString blocked = m_temporaryDirectory.filePath(QStringLiteral("header-save-failure-directory.pdf"));
    const QString target = m_temporaryDirectory.filePath(QStringLiteral("header-save-failure-retry.pdf"));
    QVERIFY(createThreePageTextPdf(source));
    QVERIFY(QDir().mkpath(blocked));
    const QString markerPath = QDir(blocked).filePath(QStringLiteral("keep.txt"));
    QFile marker(markerPath);
    QVERIFY(marker.open(QIODevice::WriteOnly));
    QCOMPARE(marker.write("keep"), 4);
    marker.close();
    const QByteArray original = headerFileBytes(source);
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    PDFEditController controller;
    controller.setDocument(&document);
    QVERIFY(controller.addHeaderText(QStringLiteral("Retry header"), QStringLiteral("all")));
    const QVariantList before = headerDraftSnapshot(controller, 3);
    QVERIFY(!controller.saveAs(QUrl::fromLocalFile(blocked)));
    QCOMPARE(headerDraftSnapshot(controller, 3), before);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QCOMPARE(document.source(), QUrl::fromLocalFile(source));
    QCOMPARE(headerFileBytes(source), original);
    QCOMPARE(headerFileBytes(markerPath), QByteArray("keep"));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(target)));
    QVERIFY(!controller.hasDrafts());
    for (int page = 0; page < 3; ++page)
        QVERIFY(normalizedPageText(target, page).contains(QStringLiteral("Retry header")));
}

void PDFEditControllerTest::placesHeaderTextAtExplicitPointAcrossDifferentPageSizes()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("header-explicit-pages.pdf"));
    QVERIFY(createHeaderPages(source, {QSizeF(300, 400), QSizeF(180, 240), QSizeF(500, 600)}));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    PDFEditController controller;
    controller.setDocument(&document);
    QVERIFY(!controller.headerTextFontFamily().isEmpty());
    QVERIFY(controller.addHeaderText(QStringLiteral("Positioned"), QStringLiteral("3,1"),
                                     80.0, 40.0, 250.0));
    QVERIFY(controller.draftsForPage(1).isEmpty());
    for (int page : {0, 2}) {
        const QSizeF size = document.pageSize(page);
        const QRectF frame = controller.draftsForPage(page).constFirst().toMap()
                                 .value(QStringLiteral("frameRect")).toRectF();
        QVERIFY(qAbs(frame.x() * size.width() - 80.0) < 0.001);
        QVERIFY(qAbs(frame.y() * size.height() - 40.0) < 0.001);
        QVERIFY(qAbs(frame.width() * size.width() - qMin(250.0, size.width() - 80.0)) < 0.001);
    }
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(controller.addHeaderText(QStringLiteral("Positioned"), QStringLiteral("all"),
                                     80.0, 40.0, 250.0));
    for (int page = 0; page < 3; ++page) {
        const QSizeF size = document.pageSize(page);
        const QVariantMap draft = controller.draftsForPage(page).constFirst().toMap();
        const QRectF frame = draft.value(QStringLiteral("frameRect")).toRectF();
        QVERIFY(qAbs(frame.x() * size.width() - 80.0) < 0.001);
        QVERIFY(qAbs(frame.y() * size.height() - 40.0) < 0.001);
        QVERIFY(qAbs(frame.width() * size.width() - qMin(250.0, size.width() - 80.0)) < 0.001);
        QVERIFY(frame.bottom() * size.height() <= 100.001);
        QVERIFY(qAbs(draft.value(QStringLiteral("fontSizeRatio")).toReal() * size.height() - 12.0) < 0.001);
    }
    const QVariantList applied = headerDraftSnapshot(controller, 3);
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    controller.redo();
    QCOMPARE(headerDraftSnapshot(controller, 3), applied);
}

void PDFEditControllerTest::rejectsInvalidHeaderTextPlacementAtomically_data()
{
    QTest::addColumn<qreal>("x");
    QTest::addColumn<qreal>("y");
    QTest::addColumn<qreal>("width");
    const qreal nan = std::numeric_limits<qreal>::quiet_NaN();
    const qreal infinity = std::numeric_limits<qreal>::infinity();
    QTest::newRow("negative-x") << -1.0 << 20.0 << 120.0;
    QTest::newRow("negative-y") << 20.0 << -1.0 << 120.0;
    QTest::newRow("negative-width") << 20.0 << 20.0 << -1.0;
    QTest::newRow("nan-x") << nan << 20.0 << 120.0;
    QTest::newRow("nan-y") << 20.0 << nan << 120.0;
    QTest::newRow("nan-width") << 20.0 << 20.0 << nan;
    QTest::newRow("infinite-x") << infinity << 20.0 << 120.0;
    QTest::newRow("infinite-y") << 20.0 << infinity << 120.0;
    QTest::newRow("infinite-width") << 20.0 << 20.0 << infinity;
    QTest::newRow("outside-header") << 20.0 << 100.0 << 120.0;
    QTest::newRow("insufficient-header-height") << 20.0 << 99.0 << 120.0;
    QTest::newRow("outside-later-page") << 200.0 << 20.0 << 120.0;
    QTest::newRow("outside-all-pages") << 400.0 << 20.0 << 120.0;
    QTest::newRow("too-narrow") << 20.0 << 20.0 << 0.1;
}

void PDFEditControllerTest::rejectsInvalidHeaderTextPlacementAtomically()
{
    QFETCH(qreal, x);
    QFETCH(qreal, y);
    QFETCH(qreal, width);
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("header-explicit-reject.pdf"));
    QVERIFY(createHeaderPages(source, {QSizeF(300, 400), QSizeF(180, 240)}));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    PDFEditController controller;
    controller.setDocument(&document);
    QVERIFY(controller.addHeaderText(QStringLiteral("Retained"), QStringLiteral("1")));
    QVERIFY(controller.addHeaderText(QStringLiteral("Redo"), QStringLiteral("2")));
    controller.undo();
    const QVariantList before = headerDraftSnapshot(controller, 2);
    QSignalSpy history(&controller, &PDFEditController::historyStateChanged);
    QSignalSpy changed(&controller, &PDFEditController::pageDataChanged);
    QVERIFY(!controller.addHeaderText(QStringLiteral("Positioned"), QStringLiteral("all"), x, y, width));
    QCOMPARE(headerDraftSnapshot(controller, 2), before);
    QCOMPARE(history.count(), 0);
    QCOMPARE(changed.count(), 0);
    QVERIFY(controller.canUndo());
    QVERIFY(controller.canRedo());
    controller.redo();
    QCOMPARE(controller.draftsForPage(1).size(), 1);
}

void PDFEditControllerTest::savesHeaderTextAtExplicitPoint_data()
{
    QTest::addColumn<int>("rotation");
    QTest::addColumn<bool>("cropped");
    for (int rotation : {0, 90, 180, 270}) {
        for (bool cropped : {false, true}) {
            const QByteArray name = QByteArray::number(rotation) + (cropped ? "-cropped" : "-media");
            QTest::newRow(name.constData()) << rotation << cropped;
        }
    }
}

void PDFEditControllerTest::savesHeaderTextAtExplicitPoint()
{
    QFETCH(int, rotation);
    QFETCH(bool, cropped);
    const QString tag = QString::fromLatin1(QTest::currentDataTag());
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("header-explicit-%1-source.pdf").arg(tag));
    const QString target = m_temporaryDirectory.filePath(QStringLiteral("header-explicit-%1-target.pdf").arg(tag));
    QVERIFY(createHeaderPages(source, {QSizeF(300, 200)}, rotation, cropped));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    PDFEditController controller;
    controller.setDocument(&document);
    QVERIFY(controller.addHeaderText(QStringLiteral("Header"), QStringLiteral("all"), 60.0, 40.0, 70.0));
    const QSize size = document.pageSize(0).toSize();
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(target)));
    QVERIFY(normalizedPageText(target).contains(QStringLiteral("Header")));
    const QRect bounds = darkPixelBounds(renderFirstPage(target, size));
    QVERIFY(!bounds.isEmpty());
    QVERIFY2(bounds.left() >= 59 && bounds.left() <= 65
             && bounds.top() >= 39 && bounds.top() <= 50
             && bounds.right() <= 130 && bounds.bottom() < 100,
        qPrintable(QStringLiteral("Rendered explicit header bounds: x=%1 y=%2 w=%3 h=%4")
            .arg(bounds.x()).arg(bounds.y()).arg(bounds.width()).arg(bounds.height())));
}

void PDFEditControllerTest::formatsHeaderTextAcrossPagesAndSave()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("header-formatted-source.pdf"));
    const QString target = m_temporaryDirectory.filePath(QStringLiteral("header-formatted-target.pdf"));
    QVERIFY(createHeaderPages(source, {QSizeF(400, 500), QSizeF(500, 700)}));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    PDFEditController controller;
    controller.setDocument(&document);
    const QString family = QStringLiteral("Times New Roman");
    const QString text = QStringLiteral("Đầu trang");
    const QColor color(QStringLiteral("#1565c0"));
    QVERIFY(controller.headerTextFontFamilies(-1, true, true).contains(family));
    QVERIFY(controller.headerTextFontFamilies(PDFEditController::VietnameseFonts, true, true)
                .contains(family));
    const QVariantMap format{
        {QStringLiteral("fontFamily"), family},
        {QStringLiteral("fontSize"), 24},
        {QStringLiteral("fontBold"), true},
        {QStringLiteral("fontItalic"), true},
        {QStringLiteral("fontUnderline"), true},
        {QStringLiteral("color"), color.name()}
    };
    QVERIFY(controller.addHeaderText(text, QStringLiteral("all"), 32.0, 18.0, 250.0, format));
    for (int page = 0; page < 2; ++page) {
        const QVariantMap draft = controller.draftsForPage(page).constFirst().toMap();
        QCOMPARE(draft.value(QStringLiteral("fontFamily")).toString(), family);
        QCOMPARE(draft.value(QStringLiteral("fontSizeRatio")).toReal()
                     * document.pageSize(page).height(), 24.0);
        QCOMPARE(draft.value(QStringLiteral("color")).value<QColor>(), color);
        QVERIFY(draft.value(QStringLiteral("fontBold")).toBool());
        QVERIFY(draft.value(QStringLiteral("fontItalic")).toBool());
        QVERIFY(draft.value(QStringLiteral("fontUnderline")).toBool());
        QVERIFY(draft.value(QStringLiteral("frameRect")).toRectF().bottom()
                    * document.pageSize(page).height() <= 100.001);
    }
    const QVariantList applied = headerDraftSnapshot(controller, 2);
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    controller.redo();
    QCOMPARE(headerDraftSnapshot(controller, 2), applied);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(target)));
    QVERIFY(!controller.hasDrafts());
    const auto characters = textCharacterRangeInfo(target, text);
    QVERIFY(!characters.isEmpty());
    for (const auto &character : characters) {
        QCOMPARE(character.color, color);
        QVERIFY(qAbs(character.pointSize - 24.0) < 0.01);
        QVERIFY(character.font.weight >= 700);
        QVERIFY(character.font.italic);
    }
    const auto underline = inspectTextUnderlinePaths(target);
    QVERIFY(underline.readable);
    QVERIFY(!underline.underlines.isEmpty());
    PDFDocument reopened;
    QVERIFY(reopened.open(QUrl::fromLocalFile(target)));
    PDFEditController reopenedController;
    reopenedController.setDocument(&reopened);
    for (int page = 0; page < 2; ++page) {
        const QVariantMap candidate = candidateForText(waitForCandidates(reopenedController, page), text);
        QVERIFY(!candidate.isEmpty());
        QVERIFY(reopenedController.selectCandidate(page, candidate.value(QStringLiteral("objectId")).toString()));
        QVERIFY(reopenedController.selectedFontFamily().contains(QStringLiteral("Times"), Qt::CaseInsensitive));
        QVERIFY(qAbs(reopenedController.selectedFontSize() - 24.0) < 0.01);
        QCOMPARE(reopenedController.selectedColor(), color);
        QVERIFY(reopenedController.selectedFontBold());
        QVERIFY(reopenedController.selectedFontItalic());
        QVERIFY(reopenedController.selectedFontUnderline());
    }
}

void PDFEditControllerTest::rejectsInvalidHeaderTextFormatAtomically_data()
{
    QTest::addColumn<QVariantMap>("format");
    QTest::addColumn<QString>("text");
    const QString text = QStringLiteral("Header");
    const auto add = [&text](const char *name, const QString &key, const QVariant &value) {
        QTest::newRow(name) << QVariantMap{{key, value}} << text;
    };
    add("empty-family", QStringLiteral("fontFamily"), QString());
    add("unknown-family", QStringLiteral("fontFamily"), QStringLiteral("TrePDF missing header face"));
    add("non-string-family", QStringLiteral("fontFamily"), 123);
    add("zero-size", QStringLiteral("fontSize"), 0);
    add("negative-size", QStringLiteral("fontSize"), -1);
    add("fractional-size", QStringLiteral("fontSize"), 12.5);
    add("oversized-size", QStringLiteral("fontSize"), 513);
    add("non-finite-size", QStringLiteral("fontSize"), std::numeric_limits<qreal>::infinity());
    add("nan-size", QStringLiteral("fontSize"), std::numeric_limits<qreal>::quiet_NaN());
    add("non-numeric-size", QStringLiteral("fontSize"), QStringLiteral("large"));
    add("boolean-size", QStringLiteral("fontSize"), true);
    add("invalid-color", QStringLiteral("color"), QStringLiteral("invalid-color"));
    add("transparent-color", QStringLiteral("color"), QColor(20, 60, 80, 100));
    add("non-list-colors", QStringLiteral("colors"), QStringLiteral("#ff0000"));
    add("null-colors", QStringLiteral("colors"), QVariant());
    add("short-colors", QStringLiteral("colors"), QVariantList{QColor(Qt::red)});
    add("long-colors", QStringLiteral("colors"), QVariantList(7, QColor(Qt::red)));
    QVariantList colors(text.size(), QColor(Qt::black));
    colors[2] = QStringLiteral("invalid-color");
    add("invalid-character-color", QStringLiteral("colors"), colors);
    colors[2] = QColor(20, 60, 80, 100);
    add("transparent-character-color", QStringLiteral("colors"), colors);
    colors[2] = 123;
    add("non-color-character", QStringLiteral("colors"), colors);
    QTest::newRow("normalized-color-count")
        << QVariantMap{{QStringLiteral("colors"), QVariantList{QColor(Qt::red)}}}
        << QStringLiteral("a\u0301");
    add("non-boolean-style", QStringLiteral("fontBold"), QStringLiteral("true"));
    add("size-does-not-fit", QStringLiteral("fontSize"), 100);
    QTest::newRow("unavailable-glyph") << QVariantMap{} << QString::fromUcs4(U"\U0010ffff");
}

void PDFEditControllerTest::rejectsInvalidHeaderTextFormatAtomically()
{
    QFETCH(QVariantMap, format);
    QFETCH(QString, text);
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("header-invalid-format.pdf"));
    QVERIFY(createHeaderPages(source, {QSizeF(300, 400), QSizeF(180, 240)}));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    PDFEditController controller;
    controller.setDocument(&document);
    QVERIFY(controller.addHeaderText(QStringLiteral("Retained"), QStringLiteral("1")));
    QVERIFY(controller.addHeaderText(QStringLiteral("Redo"), QStringLiteral("2")));
    controller.undo();
    const QVariantList before = headerDraftSnapshot(controller, 2);
    QSignalSpy history(&controller, &PDFEditController::historyStateChanged);
    QSignalSpy changed(&controller, &PDFEditController::pageDataChanged);
    QVERIFY(!controller.addHeaderText(text, QStringLiteral("all"), 24.0, 16.0, 150.0, format));
    QCOMPARE(headerDraftSnapshot(controller, 2), before);
    QCOMPARE(history.count(), 0);
    QCOMPARE(changed.count(), 0);
    QVERIFY(controller.canUndo());
    QVERIFY(controller.canRedo());
    controller.redo();
    QCOMPARE(controller.draftsForPage(1).size(), 1);
}

void PDFEditControllerTest::preservesHeaderCharacterColorsAcrossPagesAndSave_data()
{
    QTest::addColumn<QString>("pages");
    QTest::newRow("all-pages") << QStringLiteral("all");
    QTest::newRow("custom-pages") << QStringLiteral("1, 3");
}

void PDFEditControllerTest::preservesHeaderCharacterColorsAcrossPagesAndSave()
{
    QFETCH(QString, pages);
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("header-colors-source.pdf"));
    const QString target = m_temporaryDirectory.filePath(QStringLiteral("header-colors-target.pdf"));
    QVERIFY(createHeaderPages(source, {QSizeF(400, 500), QSizeF(500, 700), QSizeF(450, 600)}));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    PDFEditController controller;
    controller.setDocument(&document);
    const QString text = QStringLiteral("BlackRedBlue");
    const QVector<QColor> expected{
        Qt::black, Qt::black, Qt::black, Qt::black, Qt::black,
        Qt::red, Qt::red, Qt::red, Qt::blue, Qt::blue, Qt::blue, Qt::blue
    };
    QVariantList colors;
    for (const QColor &color : expected)
        colors.append(color.name()); // Match the QML array's string representation.
    const QVariantMap format{
        {QStringLiteral("fontSize"), 18},
        {QStringLiteral("fontBold"), true},
        {QStringLiteral("color"), QColor(Qt::blue)}, // Typing color is not the paragraph color.
        {QStringLiteral("colors"), colors}
    };
    QVERIFY(controller.addHeaderText(text, pages, 24.0, 16.0, 300.0, format));
    for (int page = 0; page < 3; ++page) {
        const bool included = pages == QLatin1String("all") || page != 1;
        const QVariantList drafts = controller.draftsForPage(page);
        QCOMPARE(drafts.size(), included ? 1 : 0);
        if (!included)
            continue;
        const QVariantMap draft = drafts.constFirst().toMap();
        QCOMPARE(headerDraftColors(draft), expected);
        const QVariantList spans = draft.value(QStringLiteral("characterSpans")).toList();
        QCOMPARE(spans.size(), 3);
        for (const QVariant &value : spans) {
            const QVariantMap span = value.toMap();
            QVERIFY(span.value(QStringLiteral("fontBold")).toBool());
            QCOMPARE(span.value(QStringLiteral("fontSizeRatio")).toReal()
                         * document.pageSize(page).height(), 18.0);
        }
    }
    const QVariantList applied = headerDraftSnapshot(controller, 3);
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    controller.redo();
    QCOMPARE(headerDraftSnapshot(controller, 3), applied);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(target)));
    QVERIFY(!controller.hasDrafts());
    const auto characters = textCharacterRangeInfo(target, text);
    QCOMPARE(characters.size(), text.size());
    for (int index = 0; index < characters.size(); ++index) {
        QCOMPARE(characters.at(index).color, expected.at(index));
        QVERIFY(qAbs(characters.at(index).pointSize - 18.0) < 0.01);
        QVERIFY(characters.at(index).font.weight >= 700);
    }
    PDFDocument reopened;
    QVERIFY(reopened.open(QUrl::fromLocalFile(target)));
    QCOMPARE(reopened.pageCount(), 3);
    for (int page = 0; page < 3; ++page)
        QCOMPARE(normalizedPageText(target, page).contains(text),
                 pages == QLatin1String("all") || page != 1);
}

void PDFEditControllerTest::normalizesHeaderCharacterColorsWithUnicode()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("header-colors-unicode.pdf"));
    QVERIFY(createHeaderPages(source, {QSizeF(400, 500)}));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    PDFEditController controller;
    controller.setDocument(&document);
    const QString raw = QStringLiteral("A\u0323\u0302B\r\nC\rD\u2028E\u2029F");
    const QString normalized = QStringLiteral("ẬB\nC\nD\nE\nF");
    QVariantList colors;
    for (int index = 0; index < raw.size(); ++index)
        colors.append(QColor(index * 10, 100, 200));
    const QVariantMap format{
        {QStringLiteral("fontSize"), 10},
        {QStringLiteral("colors"), colors}
    };
    QVERIFY(controller.addHeaderText(raw, QStringLiteral("all"), 24.0, 0.0, 300.0, format));
    const QVariantMap draft = controller.draftsForPage(0).constFirst().toMap();
    QCOMPARE(draft.value(QStringLiteral("text")).toString(), normalized);
    QVector<QColor> expected;
    for (int rawIndex : {0, 3, 4, 6, 7, 8, 9, 10, 11, 12})
        expected.append(colors.at(rawIndex).value<QColor>());
    QCOMPARE(headerDraftColors(draft), expected);

    controller.discardDrafts();
    const QColor uniform(Qt::darkGreen);
    QVERIFY(controller.addHeaderText(QStringLiteral("Legacy"), QStringLiteral("all"),
        24.0, 0.0, 300.0, {{QStringLiteral("color"), uniform},
                          {QStringLiteral("colors"), QVariantList{}}}));
    QCOMPARE(headerDraftColors(controller.draftsForPage(0).constFirst().toMap()),
             QVector<QColor>(6, uniform));
}
