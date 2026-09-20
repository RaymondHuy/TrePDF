#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

#include <fpdf_doc.h>

using namespace PDFEditControllerTestSupport;

namespace {

bool createWebLinkPdf(const QString &filePath,
                      const QList<QStringList> &pageLines,
                      bool existingFirstLink = false,
                      int rotation = 0,
                      bool crop = false)
{
    QVector<QByteArray> objects{
        "<< /Type /Catalog /Pages 2 0 R >>",
        {},
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    };
    QByteArray kids;
    const int annotationObject = 4 + 2 * pageLines.size();
    for (qsizetype page = 0; page < pageLines.size(); ++page) {
        const int pageObject = 4 + 2 * page;
        kids += QByteArray::number(pageObject) + " 0 R ";
        QByteArray content("% web-link-source-content\n");
        const QStringList &lines = pageLines.at(page);
        for (qsizetype line = 0; line < lines.size(); ++line) {
            QByteArray literal = lines.at(line).toLatin1();
            literal.replace("\\", "\\\\");
            literal.replace("(", "\\(");
            literal.replace(")", "\\)");
            content += "BT /F1 12 Tf 40 " + QByteArray::number(730 - 30 * line)
                + " Td (" + literal + ") Tj ET\n";
        }
        QByteArray dictionary(
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 800] "
            "/Resources << /Font << /F1 3 0 R >> >> /Contents ");
        dictionary += QByteArray::number(pageObject + 1) + " 0 R";
        dictionary += " /Rotate " + QByteArray::number(rotation);
        if (crop) {
            dictionary += " /CropBox [25 50 550 780]";
        }
        if (existingFirstLink && page == 0) {
            dictionary += " /Annots [" + QByteArray::number(annotationObject)
                + " 0 R]";
        }
        dictionary += " >>";
        objects.append(dictionary);
        objects.append("<< /Length " + QByteArray::number(content.size())
                       + " >>\nstream\n" + content + "endstream");
    }
    objects[1] = "<< /Type /Pages /Kids [" + kids + "] /Count "
        + QByteArray::number(pageLines.size()) + " >>";
    if (existingFirstLink) {
        objects.append(
            "<< /Type /Annot /Subtype /Link /Rect [38 728 350 744] "
            "/Border [0 0 0] /A << /S /URI "
            "/URI (https://existing-target.example/) >> >>");
    }
    return writePdfObjects(filePath, objects);
}

bool createRemovalWebLinkPdf(const QString &filePath, bool mixed = false, int pageCount = 1)
{
    QVector<QByteArray> objects{
        "<< /Type /Catalog /Pages 2 0 R >>",
        {},
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    };
    objects.resize(3 + 2 * pageCount);
    QByteArray kids;
    for (int page = 0; page < pageCount; ++page) {
        const int pageObject = 4 + 2 * page;
        kids += QByteArray::number(pageObject) + " 0 R ";
        const QByteArray suffix = QByteArray::number(page);
        const QByteArray content = "% web-link-source-content\n"
            "BT /F1 12 Tf 40 730 Td (https://remove-" + suffix
            + ".example/) Tj ET\n"
              "BT /F1 12 Tf 40 700 Td (http://remove-" + suffix
            + ".example/) Tj ET\n";
        QVector<QByteArray> annotations;
        auto link = [](const QByteArray &uri, int y, int flags = 0) {
            return "<< /Type /Annot /Subtype /Link /Rect [38 "
                + QByteArray::number(y) + " 350 " + QByteArray::number(y + 16)
                + "] /F " + QByteArray::number(flags)
                + " /Border [0 0 0] /A << /S /URI /URI (" + uri + ") >> >>";
        };
        annotations.append(link("https://remove-" + suffix + ".example/", 728));
        if (mixed) {
            annotations.append("<< /Type /Annot /Subtype /Text /Rect [400 700 420 720] "
                               "/NM (keep-comment) /Contents (Keep this comment) /F 4 >>");
        }
        annotations.append(link("http://remove-" + suffix + ".example/", 698));
        annotations.append(link("mailto:keep-" + suffix + "@example.org", 668));
        if (mixed) {
            annotations.append("<< /Type /Annot /Subtype /Link /Rect [38 638 350 654] "
                               "/Border [0 0 0] /Dest [4 0 R /Fit] >>");
            annotations.append(link("ftp://keep.example/file", 608));
            annotations.append(link("https://readonly.example/", 578, 64));
            annotations.append(link("https://locked.example/", 548, 128));
            annotations.append(link("https://lockedcontents.example/", 518, 512));
            annotations.append("<< /Type /Annot /Subtype /Text /Rect [440 700 460 720] "
                               "/NM (remove-comment) /Contents (Remove this comment) /F 4 >>");
        }
        QByteArray references;
        for (const QByteArray &annotation : annotations) {
            references += QByteArray::number(objects.size() + 1) + " 0 R ";
            objects.append(annotation);
        }
        objects[pageObject - 1] = "<< /Type /Page /Parent 2 0 R "
            "/MediaBox [0 0 600 800] /Resources << /Font << /F1 3 0 R >> >> "
            "/Contents " + QByteArray::number(pageObject + 1)
            + " 0 R /Annots [" + references + "] >>";
        objects[pageObject] = "<< /Length " + QByteArray::number(content.size())
            + " >>\nstream\n" + content + "endstream";
    }
    objects[1] = "<< /Type /Pages /Kids [" + kids + "] /Count "
        + QByteArray::number(pageCount) + " >>";
    return writePdfObjects(filePath, objects);
}

bool removeWebLinksAndWait(PDFEditController &controller)
{
    bool started = false;
    return QTest::qWaitFor([&]() {
        return started || (started = controller.removeWebLinks());
    }, 5000) && QTest::qWaitFor([&]() { return !controller.removingWebLinks(); }, 5000);
}

bool beginLinkEditingAndWait(PDFEditController &controller)
{
    return controller.beginLinkEditing()
        && QTest::qWaitFor([&]() { return !controller.loadingEditableLinks(); }, 5000);
}

QVariantMap manualWebProperties(const QString &url = QStringLiteral("https://manual.example/path"))
{
    return {
        {QStringLiteral("url"), url},
        {QStringLiteral("targetPage"), -1},
        {QStringLiteral("visible"), false},
        {QStringLiteral("color"), QColor(Qt::blue)},
        {QStringLiteral("width"), 1.0},
        {QStringLiteral("lineStyle"), 0},
        {QStringLiteral("highlight"), 1},
    };
}

QVariantMap editableLinkWithUrl(const PDFEditController &controller, int page, const QString &url)
{
    for (const QVariant &entry : controller.editableLinksForPage(page)) {
        if (entry.toMap().value(QStringLiteral("url")).toString() == url)
            return entry.toMap();
    }
    return {};
}

bool createManualSourcePdf(const QString &path, bool internalAction = false, bool chainedAction = false)
{
    const QByteArray content("% manual-link-source-content\nBT /F1 12 Tf 40 730 Td (Manual source link) Tj ET\n");
    return writePdfObjects(path, {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [4 0 R 6 0 R] /Count 2 >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 800] "
        "/Resources << /Font << /F1 3 0 R >> >> /Contents 5 0 R /Annots [8 0 R 9 0 R 10 0 R] >>",
        "<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" + content + "endstream",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 800] /Contents 7 0 R >>",
        "<< /Length 0 >>\nstream\nendstream",
        "<< /Type /Annot /Subtype /Link /Rect [38 728 350 744] /F 4 "
        "/Border [0 0 1] /C [0 0 1] /H /I /NM (manual-source-web) "
        "/TrePDFKeep (keep-unrelated-metadata) "
        "/A << /S /URI /URI (https://original.example/) "
        + QByteArray(chainedAction ? "/Next << /S /URI /URI (https://next.example/) >> " : "")
        + ">> >>",
        "<< /Type /Annot /Subtype /Link /Rect [38 680 350 700] /Border [0 0 0] "
        + QByteArray(internalAction ? "/A << /S /GoTo /D [6 0 R /Fit] >> >>"
                                    : "/Dest [6 0 R /Fit] >>"),
        "<< /Type /Annot /Subtype /Link /Rect [38 640 350 660] /F 128 /Border [0 0 0] "
        "/A << /S /URI /URI (https://protected.example/) >> >>",
    });
}

struct SavedWebLink {
    int page = -1;
    QString uri;
    QRectF rect;
    float borderWidth = -1.0f;
    int targetPage = -1;
    unsigned long destinationView = 0;
    QString marker;
};

struct WebLinkInspection {
    bool readable = false;
    QVector<SavedWebLink> links;
    QStringList comments;
    QStringList stampAppearances;
};

WebLinkInspection inspectWebLinks(const QString &filePath)
{
    PDFiumProcessLock lock;
    WebLinkInspection result;
    const QByteArray path = QFile::encodeName(filePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(path.constData(), nullptr);
    if (!document) {
        return result;
    }
    const auto closeDocument = qScopeGuard([&]() { FPDF_CloseDocument(document); });
    for (int pageIndex = 0; pageIndex < FPDF_GetPageCount(document); ++pageIndex) {
        FPDF_PAGE page = FPDF_LoadPage(document, pageIndex);
        if (!page) {
            return result;
        }
        const auto closePage = qScopeGuard([&]() { FPDF_ClosePage(page); });
        for (int index = 0; index < FPDFPage_GetAnnotCount(page); ++index) {
            FPDF_ANNOTATION annotation = FPDFPage_GetAnnot(page, index);
            if (!annotation) {
                return result;
            }
            const auto closeAnnotation = qScopeGuard(
                [&]() { FPDFPage_CloseAnnot(annotation); });
            if (FPDFAnnot_GetSubtype(annotation) == FPDF_ANNOT_STAMP) {
                result.stampAppearances.append(PDFAnnotationUtils::annotationNormalAppearance(annotation));
            }
            if (FPDFAnnot_GetSubtype(annotation) == FPDF_ANNOT_TEXT) {
                result.comments.append(PDFAnnotationUtils::annotationString(annotation, "Contents"));
            }
            if (FPDFAnnot_GetSubtype(annotation) != FPDF_ANNOT_LINK) {
                continue;
            }
            const FPDF_LINK link = FPDFAnnot_GetLink(annotation);
            const FPDF_ACTION action = link ? FPDFLink_GetAction(link) : nullptr;
            QByteArray uri;
            if (action && FPDFAction_GetType(action) == PDFACTION_URI) {
                const unsigned long size = FPDFAction_GetURIPath(document, action, nullptr, 0);
                if (size == 0 || size > 16384) {
                    return result;
                }
                uri.resize(size);
                if (FPDFAction_GetURIPath(document, action, uri.data(), size) != size) {
                    return result;
                }
                uri.chop(1);
            }
            FS_RECTF rect{};
            float horizontalRadius = 0.0f;
            float verticalRadius = 0.0f;
            float borderWidth = -1.0f;
            if (!FPDFAnnot_GetRect(annotation, &rect)
                || !FPDFAnnot_GetBorder(annotation,
                                        &horizontalRadius,
                                        &verticalRadius,
                                        &borderWidth)) {
                return result;
            }
            result.links.append({
                pageIndex,
                QString::fromUtf8(uri),
                QRectF(QPointF(rect.left, rect.bottom),
                       QPointF(rect.right, rect.top)),
                borderWidth,
            });
            FPDF_DEST destination = link ? FPDFLink_GetDest(document, link) : nullptr;
            if (!destination && action && FPDFAction_GetType(action) == PDFACTION_GOTO)
                destination = FPDFAction_GetDest(document, action);
            SavedWebLink &saved = result.links.last();
            if (destination) {
                saved.targetPage = FPDFDest_GetDestPageIndex(document, destination);
                unsigned long parameterCount = 0;
                FS_FLOAT parameters[4]{};
                saved.destinationView = FPDFDest_GetView(destination, &parameterCount, parameters);
            }
            saved.marker = PDFAnnotationUtils::annotationString(annotation, "TrePDFKeep");
        }
    }
    result.readable = true;
    return result;
}

QStringList draftWebLinkUrls(const PDFEditController &controller, int page)
{
    QStringList urls;
    for (const QVariant &entry : controller.webLinkDraftsForPage(page)) {
        urls.append(entry.toMap().value(QStringLiteral("url")).toString());
    }
    urls.sort();
    return urls;
}

QByteArray readFileBytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QByteArray fixtureContentStream(const QString &path)
{
    // These fixtures contain one content stream. PDFium may compress it when
    // serializing; compare decoded bytes to detect accidental regeneration.
    const QByteArray bytes = readFileBytes(path);
    const QRegularExpression streamPattern(
        QStringLiteral("<<([^<>]*)>>\\s*stream\\r?\\n"));
    const auto match = streamPattern.match(QString::fromLatin1(bytes));
    if (!match.hasMatch())
        return {};
    const auto length = QRegularExpression(QStringLiteral("/Length\\s+(\\d+)"))
                            .match(match.captured(1));
    if (!length.hasMatch())
        return {};
    QByteArray content = bytes.mid(match.capturedEnd(), length.captured(1).toInt());
    if (match.captured(1).contains(QStringLiteral("/FlateDecode"))) {
        // qUncompress expects a four-byte uncompressed-size hint before zlib.
        content.prepend(QByteArray::fromHex("00010000"));
        content = qUncompress(content);
    }
    return content;
}

QRectF firstLinePdfBounds(const QString &filePath)
{
    PDFiumProcessLock lock;
    const QByteArray path = QFile::encodeName(filePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(path.constData(), nullptr);
    if (!document) {
        return {};
    }
    const auto closeDocument = qScopeGuard([&]() { FPDF_CloseDocument(document); });
    FPDF_PAGE page = FPDF_LoadPage(document, 0);
    if (!page) {
        return {};
    }
    const auto closePage = qScopeGuard([&]() { FPDF_ClosePage(page); });
    FPDF_TEXTPAGE text = FPDFText_LoadPage(page);
    if (!text) {
        return {};
    }
    const auto closeText = qScopeGuard([&]() { FPDFText_ClosePage(text); });
    if (FPDFText_CountRects(text, 0, FPDFText_CountChars(text)) != 1) {
        return {};
    }
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    if (!FPDFText_GetRect(text, 0, &left, &top, &right, &bottom)) {
        return {};
    }
    return QRectF(QPointF(left, bottom), QPointF(right, top));
}

} // namespace

void PDFEditControllerTest::autoCreatesOnlySafeWebLinks()
{
    const QString path = m_temporaryDirectory.filePath(
        QStringLiteral("auto-web-links-detection.pdf"));
    QVERIFY(createWebLinkPdf(path, {{
        QStringLiteral("https://already-linked.example/path"),
        QStringLiteral("http://plain.example/path"),
        QStringLiteral("https://secure.example/a?x=1&y=2"),
        QStringLiteral("www.example.org/docs"),
        QStringLiteral("person@example.org"),
        QStringLiteral("file:///tmp/private.txt"),
        QStringLiteral("ftp://files.example.org/path"),
        QStringLiteral("javascript:alert(1)"),
    }}, true));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    QVERIFY(controller.autoCreateWebLinks());
    QTRY_VERIFY_WITH_TIMEOUT(!controller.creatingWebLinks(), 5000);
    const QStringList urls = draftWebLinkUrls(controller, 0);
    QCOMPARE(urls.size(), 3);
    QVERIFY(urls.contains(QStringLiteral("http://plain.example/path")));
    QVERIFY(urls.contains(QStringLiteral("https://secure.example/a?x=1&y=2")));
    QVERIFY(std::any_of(urls.cbegin(), urls.cend(), [](const QString &value) {
        const QUrl url(value);
        return url.host() == QStringLiteral("www.example.org")
            && url.path() == QStringLiteral("/docs")
            && (url.scheme() == QStringLiteral("http")
                || url.scheme() == QStringLiteral("https"));
    }));
    for (const QVariant &entry : controller.webLinkDraftsForPage(0)) {
        const QRectF rect = entry.toMap().value(QStringLiteral("rect")).toRectF();
        QVERIFY(!rect.isEmpty());
        QVERIFY(QRectF(0, 0, 1, 1).contains(rect));
    }
    QVERIFY(controller.hasDrafts());
    const WebLinkInspection source = inspectWebLinks(path);
    QVERIFY(source.readable);
    QCOMPARE(source.links.size(), 1);
    QCOMPARE(source.links.constFirst().uri,
             QStringLiteral("https://existing-target.example/"));
}

void PDFEditControllerTest::autoWebLinksUseSingleHistoryCommand()
{
    const QString path = m_temporaryDirectory.filePath(
        QStringLiteral("auto-web-links-history.pdf"));
    QVERIFY(createWebLinkPdf(path, {
        {QStringLiteral("https://first.example/a"),
         QStringLiteral("https://first.example/b")},
        {QStringLiteral("https://second.example/")},
    }));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    QVERIFY(controller.autoCreateWebLinks());
    QTRY_VERIFY_WITH_TIMEOUT(!controller.creatingWebLinks(), 5000);
    QCOMPARE(controller.webLinkDraftsForPage(0).size(), 2);
    QCOMPARE(controller.webLinkDraftsForPage(1).size(), 1);
    QVERIFY(controller.canUndo());

    // Re-running over the same saved source must not add another batch.
    QVERIFY(controller.autoCreateWebLinks());
    QTRY_VERIFY_WITH_TIMEOUT(!controller.creatingWebLinks(), 5000);
    QCOMPARE(controller.webLinkDraftsForPage(0).size(), 2);
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(controller.canRedo());
    QVERIFY(controller.webLinkDraftsForPage(0).isEmpty());
    QVERIFY(controller.webLinkDraftsForPage(1).isEmpty());

    controller.redo();
    QVERIFY(controller.hasDrafts());
    QCOMPARE(controller.webLinkDraftsForPage(0).size(), 2);
    QCOMPARE(controller.webLinkDraftsForPage(1).size(), 1);
    controller.discardDrafts();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
    QVERIFY(controller.webLinkDraftsForPage(0).isEmpty());
    QVERIFY(controller.webLinkDraftsForPage(1).isEmpty());
}

void PDFEditControllerTest::savesAutoWebLinksWithoutChangingPageContent()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("auto-web-links-save-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("auto-web-links-save-output.pdf"));
    QVERIFY(createWebLinkPdf(sourcePath, {{
        QStringLiteral("https://already-linked.example/path"),
        QStringLiteral("https://new.example/a?x=1&y=2"),
    }}, true));
    const QByteArray sourceBytes = readFileBytes(sourcePath);
    const QString sourceText = normalizedPageText(sourcePath);
    const QImage sourceImage = renderFirstPage(sourcePath, QSize(600, 800));
    QVERIFY(!sourceBytes.isEmpty());
    QVERIFY(!sourceImage.isNull());

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    QVERIFY(controller.autoCreateWebLinks());
    QTRY_VERIFY_WITH_TIMEOUT(!controller.creatingWebLinks(), 5000);
    QCOMPARE(controller.webLinkDraftsForPage(0).size(), 1);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(readFileBytes(sourcePath), sourceBytes);
    QCOMPARE(normalizedPageText(outputPath), sourceText);
    QCOMPARE(renderFirstPage(outputPath, QSize(600, 800)), sourceImage);
    // An annotation-only save must preserve the original content stream.
    const QByteArray sourceContent = fixtureContentStream(sourcePath);
    QVERIFY(sourceContent.startsWith("% web-link-source-content\n"));
    QCOMPARE(fixtureContentStream(outputPath), sourceContent);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(controller.webLinkDraftsForPage(0).isEmpty());

    const WebLinkInspection saved = inspectWebLinks(outputPath);
    QVERIFY(saved.readable);
    QCOMPARE(saved.links.size(), 2);
    QStringList urls;
    for (const SavedWebLink &link : saved.links) {
        urls.append(link.uri);
        QCOMPARE(link.page, 0);
        QCOMPARE(link.borderWidth, 0.0f);
        QVERIFY(!link.rect.isEmpty());
    }
    QVERIFY(urls.contains(QStringLiteral("https://existing-target.example/")));
    QVERIFY(urls.contains(QStringLiteral("https://new.example/a?x=1&y=2")));

    QVERIFY(controller.autoCreateWebLinks());
    QTRY_VERIFY_WITH_TIMEOUT(!controller.creatingWebLinks(), 5000);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(controller.webLinkDraftsForPage(0).isEmpty());
    QCOMPARE(inspectWebLinks(outputPath).links.size(), 2);
}

void PDFEditControllerTest::mapsAutoWebLinksAcrossPageChanges()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("auto-web-links-page-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("auto-web-links-page-output.pdf"));
    QVERIFY(createWebLinkPdf(sourcePath, {
        {QStringLiteral("https://first.example/")},
        {QStringLiteral("https://second.example/")},
        {QStringLiteral("https://third.example/")},
    }));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    QVERIFY(controller.autoCreateWebLinks());
    QTRY_VERIFY_WITH_TIMEOUT(!controller.creatingWebLinks(), 5000);

    document.setCurrentPage(0);
    QCOMPARE(controller.addBlankPage(), 1);
    QCOMPARE(document.pageCount(), 4);
    QVERIFY(controller.webLinkDraftsForPage(1).isEmpty());
    QCOMPARE(draftWebLinkUrls(controller, 2),
             QStringList{QStringLiteral("https://second.example/")});
    document.setCurrentPage(0);
    QVERIFY(controller.deleteCurrentPage() >= 0);
    QCOMPARE(document.pageCount(), 3);
    QVERIFY(controller.webLinkDraftsForPage(0).isEmpty());
    QCOMPARE(draftWebLinkUrls(controller, 1),
             QStringList{QStringLiteral("https://second.example/")});
    QCOMPARE(draftWebLinkUrls(controller, 2),
             QStringList{QStringLiteral("https://third.example/")});
    controller.undo();
    QCOMPARE(draftWebLinkUrls(controller, 0),
             QStringList{QStringLiteral("https://first.example/")});
    controller.redo();

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    const WebLinkInspection saved = inspectWebLinks(outputPath);
    QVERIFY(saved.readable);
    QCOMPARE(saved.links.size(), 2);
    QCOMPARE(saved.links.at(0).page, 1);
    QCOMPARE(saved.links.at(0).uri, QStringLiteral("https://second.example/"));
    QCOMPARE(saved.links.at(1).page, 2);
    QCOMPARE(saved.links.at(1).uri, QStringLiteral("https://third.example/"));
    QVERIFY(inspectWebLinks(sourcePath).links.isEmpty());
}

void PDFEditControllerTest::savesAutoWebLinksWithCropAndRotation_data()
{
    QTest::addColumn<int>("rotation");
    QTest::addColumn<bool>("crop");
    for (const int rotation : {0, 90, 180, 270}) {
        for (const bool crop : {false, true}) {
            const QByteArray name = QByteArray::number(rotation)
                + (crop ? "-cropped" : "-uncropped");
            QTest::newRow(name.constData()) << rotation << crop;
        }
    }
}

void PDFEditControllerTest::savesAutoWebLinksWithCropAndRotation()
{
    QFETCH(int, rotation);
    QFETCH(bool, crop);
    const QString suffix = QStringLiteral("%1-%2.pdf").arg(rotation).arg(crop);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("auto-web-links-geometry-source-") + suffix);
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("auto-web-links-geometry-output-") + suffix);
    QVERIFY(createWebLinkPdf(sourcePath,
                            {{QStringLiteral("https://geometry.example/path")}},
                            false,
                            rotation,
                            crop));
    const QRectF textBounds = firstLinePdfBounds(sourcePath);
    QVERIFY(!textBounds.isEmpty());
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    QVERIFY(controller.autoCreateWebLinks());
    QTRY_VERIFY_WITH_TIMEOUT(!controller.creatingWebLinks(), 5000);
    QCOMPARE(controller.webLinkDraftsForPage(0).size(), 1);
    const QRectF previewRect = controller.webLinkDraftsForPage(0).constFirst()
                                  .toMap().value(QStringLiteral("rect")).toRectF();
    QVERIFY(!previewRect.isEmpty());
    QVERIFY(QRectF(0, 0, 1, 1).contains(previewRect));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    const WebLinkInspection saved = inspectWebLinks(outputPath);
    QVERIFY(saved.readable);
    QCOMPARE(saved.links.size(), 1);
    const QRectF actual = saved.links.constFirst().rect;
    QVERIFY(qAbs(actual.left() - textBounds.left()) < 1.0);
    QVERIFY(qAbs(actual.right() - textBounds.right()) < 1.0);
    QVERIFY(qAbs(actual.top() - textBounds.top()) < 1.0);
    QVERIFY(qAbs(actual.bottom() - textBounds.bottom()) < 1.0);
}

void PDFEditControllerTest::rejectsAutoWebLinksWithUnsavedContent()
{
    const QString path = m_temporaryDirectory.filePath(
        QStringLiteral("auto-web-links-unsaved.pdf"));
    QVERIFY(createWebLinkPdf(path, {{QStringLiteral("https://saved.example/")}}));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(!controller.autoCreateWebLinks());
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    QCOMPARE(controller.addBlankPage(), 1);
    QVERIFY(!controller.autoCreateWebLinks());
    QVERIFY(!controller.creatingWebLinks());
    controller.discardDrafts();

    controller.setTool(PDFEditController::StickyNoteTool);
    QVERIFY(!controller.addStickyNote(0, QPointF(0.3, 0.3)).isEmpty());
    QVERIFY(!controller.autoCreateWebLinks());
    QVERIFY(controller.webLinkDraftsForPage(0).isEmpty());
    controller.discardDrafts();
    QVERIFY(controller.autoCreateWebLinks());
    QTRY_VERIFY_WITH_TIMEOUT(!controller.creatingWebLinks(), 5000);
    QCOMPARE(controller.webLinkDraftsForPage(0).size(), 1);
}

void PDFEditControllerTest::cancelsStaleAutoWebLinkScans()
{
    const QString path = m_temporaryDirectory.filePath(
        QStringLiteral("auto-web-links-cancel.pdf"));
    QVERIFY(createWebLinkPdf(path, {{QStringLiteral("https://saved.example/")}}));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);

    for (int action = 0; action < 4; ++action) {
        QVERIFY(controller.autoCreateWebLinks());
        QVERIFY(controller.creatingWebLinks());
        // Completion is queued, so invalidate the request before processing it.
        if (action == 0) {
            controller.cancelWebLinkCreation();
        } else if (action == 1) {
            controller.discardDrafts();
        } else if (action == 2) {
            controller.setDocument(nullptr);
            controller.setDocument(&document);
        } else {
            QCOMPARE(controller.addBlankPage(), 1);
            controller.undo();
        }
        QVERIFY(!controller.creatingWebLinks());
        QVERIFY(controller.webLinkDraftsForPage(0).isEmpty());
        QVERIFY(!controller.hasDrafts());

        // Cancellation is asynchronous; retry once the bounded worker is free.
        bool started = false;
        QTRY_VERIFY_WITH_TIMEOUT(started || (started = controller.autoCreateWebLinks()), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.creatingWebLinks(), 5000);
        QCOMPARE(controller.webLinkDraftsForPage(0).size(), 1);
        controller.undo();
        QVERIFY(!controller.hasDrafts());
        QVERIFY(!controller.canUndo());
        controller.discardDrafts();
    }
}

void PDFEditControllerTest::routesDraftWebLinksThroughConfirmation()
{
    const QString path = m_temporaryDirectory.filePath(
        QStringLiteral("auto-web-links-activation.pdf"));
    QVERIFY(createWebLinkPdf(path, {{QStringLiteral("https://confirm.example/path")}}));
    PDFDocument document;
    PDFFormController forms;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    forms.setDocument(&document);
    controller.setDocument(&document);
    controller.setFormController(&forms);
    QVERIFY(controller.autoCreateWebLinks());
    QTRY_VERIFY_WITH_TIMEOUT(!controller.creatingWebLinks(), 5000);
    const QVariantMap link = controller.webLinkDraftsForPage(0).constFirst().toMap();
    const int index = link.value(QStringLiteral("index")).toInt();
    const quint64 revision = link.value(QStringLiteral("revision")).toULongLong();
    QSignalSpy confirmationSpy(&forms, &PDFFormController::externalLinkRequested);
    QSignalSpy cancellationSpy(&forms, &PDFFormController::externalLinkRequestCancelled);
    controller.activateWebLink(0, index, revision);
    QCOMPARE(confirmationSpy.size(), 1);
    QCOMPARE(confirmationSpy.constFirst().at(1).toString(),
             QStringLiteral("confirm.example"));
    QCOMPARE(confirmationSpy.constFirst().at(2).toString(),
             QStringLiteral("https://confirm.example/path"));
    QCOMPARE(confirmationSpy.constFirst().at(3).toBool(), false);
    QVERIFY(!controller.activateWebLink(0, index, revision));

    controller.undo();
    QCOMPARE(cancellationSpy.size(), 1);
    QVERIFY(!forms.confirmExternalLink(confirmationSpy.constFirst().at(0).toULongLong()));
    controller.activateWebLink(0, index, revision);
    QCOMPARE(confirmationSpy.size(), 1);
    controller.redo();
    controller.activateWebLink(0, index, revision);
    QCOMPARE(confirmationSpy.size(), 1);
}

void PDFEditControllerTest::routesSavedWebLinksThroughConfirmation_data()
{
    savesAutoWebLinksWithCropAndRotation_data();
}

void PDFEditControllerTest::routesSavedWebLinksThroughConfirmation()
{
    QFETCH(int, rotation);
    QFETCH(bool, crop);
    const QString path = m_temporaryDirectory.filePath(
        QStringLiteral("auto-web-links-saved-activation-%1-%2.pdf").arg(rotation).arg(crop));
    QVERIFY(createWebLinkPdf(path, {{QStringLiteral("https://saved.example/path")}},
                            false, rotation, crop));
    PDFDocument document;
    PDFFormController forms;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    forms.setDocument(&document);
    controller.setDocument(&document);
    controller.setFormController(&forms);
    QVERIFY(controller.autoCreateWebLinks());
    QTRY_VERIFY_WITH_TIMEOUT(!controller.creatingWebLinks(), 5000);
    const QPointF center = controller.webLinkDraftsForPage(0).constFirst()
                               .toMap().value(QStringLiteral("rect")).toRectF().center();
    QVERIFY(controller.save());
    QVERIFY(controller.webLinkDraftsForPage(0).isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!forms.javascriptTransitionPending(), 5000);
    QVERIFY(forms.interactive());
    QSignalSpy confirmationSpy(&forms, &PDFFormController::externalLinkRequested);
    const QSize size(600, 800);
    const QPointF point(center.x() * size.width(), center.y() * size.height());
    // Pressing a link and releasing outside it must not navigate.
    forms.pointerEvent(0, PDFFormWorker::PointerEventType::Press, 0, point, size);
    forms.pointerEvent(0, PDFFormWorker::PointerEventType::Release, 0,
                       QPointF(10, 10), size);
    QTRY_VERIFY_WITH_TIMEOUT(!forms.hasDrafts(), 5000);
    QCOMPARE(confirmationSpy.size(), 0);
    for (const auto type : {PDFFormWorker::PointerEventType::Move,
                            PDFFormWorker::PointerEventType::Press,
                            PDFFormWorker::PointerEventType::Release}) {
        forms.pointerEvent(0, type, 0, point, size);
    }
    QTRY_COMPARE_WITH_TIMEOUT(confirmationSpy.size(), 1, 5000);
    QCOMPARE(confirmationSpy.constFirst().at(2).toString(),
             QStringLiteral("https://saved.example/path"));
    forms.cancelExternalLink(confirmationSpy.constFirst().at(0).toULongLong());
}

void PDFEditControllerTest::removesOnlyEditableWebLinks()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("remove-web-links-mixed.pdf"));
    const QString output = m_temporaryDirectory.filePath(QStringLiteral("remove-web-links-mixed-saved.pdf"));
    QVERIFY(createRemovalWebLinkPdf(path, true));
    const WebLinkInspection source = inspectWebLinks(path);
    QVERIFY(source.readable);
    QCOMPARE(source.links.size(), 8);
    QCOMPARE(source.comments.size(), 2);
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    QVERIFY(removeWebLinksAndWait(controller));
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(output)));
    const WebLinkInspection saved = inspectWebLinks(output);
    QVERIFY(saved.readable);
    QCOMPARE(saved.links.size(), 6);
    QCOMPARE(saved.comments, source.comments);
    QStringList urls;
    for (const SavedWebLink &link : saved.links)
        urls.append(link.uri);
    QVERIFY(urls.contains(QString())); // Internal /Dest link remains intact.
    QVERIFY(urls.contains(QStringLiteral("mailto:keep-0@example.org")));
    QVERIFY(urls.contains(QStringLiteral("ftp://keep.example/file")));
    QVERIFY(urls.contains(QStringLiteral("https://readonly.example/")));
    QVERIFY(urls.contains(QStringLiteral("https://locked.example/")));
    QVERIFY(urls.contains(QStringLiteral("https://lockedcontents.example/")));
    QVERIFY(removeWebLinksAndWait(controller));
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
}

void PDFEditControllerTest::removeWebLinksUsesSingleHistoryCommand()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("remove-web-links-history.pdf"));
    QVERIFY(createWebLinkPdf(path, {{QStringLiteral("https://already-linked.example/"),
                                    QStringLiteral("https://pending.example/")}}, true));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    QVERIFY(controller.autoCreateWebLinks());
    QTRY_VERIFY_WITH_TIMEOUT(!controller.creatingWebLinks(), 5000);
    QCOMPARE(controller.webLinkDraftsForPage(0).size(), 1);
    QVERIFY(removeWebLinksAndWait(controller));
    QVERIFY(controller.webLinkDraftsForPage(0).isEmpty());
    QVERIFY(controller.hasDrafts());
    // Repeating removal must not insert a no-op history command.
    QVERIFY(removeWebLinksAndWait(controller));
    controller.undo();
    QCOMPARE(controller.webLinkDraftsForPage(0).size(), 1);
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    controller.redo();
    QCOMPARE(controller.webLinkDraftsForPage(0).size(), 1);
    controller.redo();
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.webLinkDraftsForPage(0).isEmpty());
    controller.discardDrafts();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
    QCOMPARE(inspectWebLinks(path).links.size(), 1);
}

void PDFEditControllerTest::removesPendingWebLinksWithoutCreatingSourceDrafts()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("remove-web-links-pending.pdf"));
    QVERIFY(createWebLinkPdf(path, {{QStringLiteral("https://pending.example/")}}));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    QVERIFY(controller.autoCreateWebLinks());
    QTRY_VERIFY_WITH_TIMEOUT(!controller.creatingWebLinks(), 5000);
    QCOMPARE(controller.webLinkDraftsForPage(0).size(), 1);
    QVERIFY(removeWebLinksAndWait(controller));
    QVERIFY(controller.webLinkDraftsForPage(0).isEmpty());
    QVERIFY(controller.deletedSourceWebLinkAnnotationsForPage(0).isEmpty());
    QVERIFY(!controller.hasDrafts());
    QVERIFY(controller.canUndo());
    controller.undo();
    QCOMPARE(controller.webLinkDraftsForPage(0).size(), 1);
    QVERIFY(controller.hasDrafts());
    controller.redo();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(controller.webLinkDraftsForPage(0).isEmpty());
}

void PDFEditControllerTest::savesRemovedWebLinksWithoutChangingPageContent()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("remove-web-links-content.pdf"));
    const QString output = m_temporaryDirectory.filePath(QStringLiteral("remove-web-links-content-saved.pdf"));
    QVERIFY(createRemovalWebLinkPdf(path));
    const QByteArray sourceBytes = readFileBytes(path);
    const QByteArray sourceContent = fixtureContentStream(path);
    const QString sourceText = normalizedPageText(path);
    const QImage sourceImage = renderFirstPage(path, QSize(600, 800));
    QVERIFY(!sourceBytes.isEmpty());
    QVERIFY(!sourceImage.isNull());
    QVERIFY(sourceContent.startsWith("% web-link-source-content\n"));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    QVERIFY(removeWebLinksAndWait(controller));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(output)));
    QCOMPARE(readFileBytes(path), sourceBytes);
    QCOMPARE(normalizedPageText(output), sourceText);
    QCOMPARE(fixtureContentStream(output), sourceContent);
    QCOMPARE(renderFirstPage(output, QSize(600, 800)), sourceImage);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    const WebLinkInspection saved = inspectWebLinks(output);
    QVERIFY(saved.readable);
    QCOMPARE(saved.links.size(), 1);
    QCOMPARE(saved.links.constFirst().uri, QStringLiteral("mailto:keep-0@example.org"));
    // A freshly opened saved document has no removable links or hidden drafts.
    controller.setDocument(nullptr);
    QVERIFY(document.open(QUrl::fromLocalFile(output)));
    controller.setDocument(&document);
    QVERIFY(removeWebLinksAndWait(controller));
    QVERIFY(!controller.hasDrafts());
    QVERIFY(controller.autoCreateWebLinks());
    QTRY_VERIFY_WITH_TIMEOUT(!controller.creatingWebLinks(), 5000);
    QCOMPARE(controller.webLinkDraftsForPage(0).size(), 2);
    QVERIFY(controller.save());
    QCOMPARE(inspectWebLinks(output).links.size(), 3);
}

void PDFEditControllerTest::mapsRemovedWebLinksAcrossPageChanges()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("remove-web-links-pages.pdf"));
    const QString output = m_temporaryDirectory.filePath(QStringLiteral("remove-web-links-pages-saved.pdf"));
    QVERIFY(createRemovalWebLinkPdf(path, false, 3));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    QVERIFY(removeWebLinksAndWait(controller));
    document.setCurrentPage(0);
    QCOMPARE(controller.addBlankPage(), 1);
    document.setCurrentPage(0);
    QVERIFY(controller.deleteCurrentPage() >= 0);
    QCOMPARE(document.pageCount(), 3);
    controller.undo();
    QCOMPARE(document.pageCount(), 4);
    controller.redo();
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(output)));
    const WebLinkInspection saved = inspectWebLinks(output);
    QVERIFY(saved.readable);
    QCOMPARE(saved.links.size(), 2);
    QCOMPARE(saved.links.at(0).page, 1);
    QCOMPARE(saved.links.at(0).uri, QStringLiteral("mailto:keep-1@example.org"));
    QCOMPARE(saved.links.at(1).page, 2);
    QCOMPARE(saved.links.at(1).uri, QStringLiteral("mailto:keep-2@example.org"));
    QCOMPARE(inspectWebLinks(path).links.size(), 9);
}

void PDFEditControllerTest::routesRemovedWebLinksThroughHistory()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("remove-web-links-activation.pdf"));
    QVERIFY(createRemovalWebLinkPdf(path));
    PDFDocument document;
    PDFFormController forms;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    forms.setDocument(&document);
    controller.setDocument(&document);
    controller.setFormController(&forms);
    QTRY_VERIFY_WITH_TIMEOUT(!forms.javascriptTransitionPending(), 5000);
    QVERIFY(forms.interactive());
    QSignalSpy confirmationSpy(&forms, &PDFFormController::externalLinkRequested);
    QSignalSpy cancellationSpy(&forms, &PDFFormController::externalLinkRequestCancelled);
    auto clickSourceLink = [&]() {
        for (const auto type : {PDFFormWorker::PointerEventType::Move,
                               PDFFormWorker::PointerEventType::Press,
                               PDFFormWorker::PointerEventType::Release}) {
            forms.pointerEvent(0, type, 0, QPointF(150, 64), QSize(600, 800));
        }
    };
    clickSourceLink();
    QTRY_COMPARE_WITH_TIMEOUT(confirmationSpy.size(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!forms.hasDrafts(), 5000);
    const quint64 request = confirmationSpy.constFirst().at(0).toULongLong();
    QVERIFY(removeWebLinksAndWait(controller));
    QCOMPARE(cancellationSpy.size(), 1);
    QCOMPARE(controller.deletedSourceWebLinkAnnotationsForPage(0).size(), 2);
    QVERIFY(!forms.confirmExternalLink(request));
    clickSourceLink();
    QTRY_VERIFY_WITH_TIMEOUT(!forms.hasDrafts(), 5000);
    QCOMPARE(confirmationSpy.size(), 1);
    controller.undo();
    QVERIFY(controller.deletedSourceWebLinkAnnotationsForPage(0).isEmpty());
    clickSourceLink();
    QTRY_COMPARE_WITH_TIMEOUT(confirmationSpy.size(), 2, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!forms.hasDrafts(), 5000);
    forms.cancelExternalLink(confirmationSpy.constLast().at(0).toULongLong());
    controller.redo();
    QCOMPARE(controller.deletedSourceWebLinkAnnotationsForPage(0).size(), 2);
    clickSourceLink();
    QTRY_VERIFY_WITH_TIMEOUT(!forms.hasDrafts(), 5000);
    QCOMPARE(confirmationSpy.size(), 2);
    controller.discardDrafts();
    QVERIFY(controller.deletedSourceWebLinkAnnotationsForPage(0).isEmpty());
    clickSourceLink();
    QTRY_COMPARE_WITH_TIMEOUT(confirmationSpy.size(), 3, 5000);
    forms.cancelExternalLink(confirmationSpy.constLast().at(0).toULongLong());
}

void PDFEditControllerTest::previewsRemovedWebLinkAppearancesThroughHistory()
{
    const QString path = m_temporaryDirectory.filePath(
        QStringLiteral("remove-web-link-appearances.pdf"));
    auto appearance = [](const QByteArray &color) {
        const QByteArray content = color + " rg 0 0 40 40 re f\n";
        return QByteArray("<< /Type /XObject /Subtype /Form /BBox [0 0 40 40] "
                          "/Resources << >> /Length ")
            + QByteArray::number(content.size()) + " >>\nstream\n" + content + "endstream";
    };
    QVERIFY(writePdfObjects(path, {
        QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
        QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
        QByteArray("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                   "/Resources << >> /Annots [4 0 R 6 0 R] >>"),
        QByteArray("<< /Type /Annot /Subtype /Link /Rect [20 140 60 180] "
                   "/Border [0 0 0] /A << /S /URI /URI (https://remove.example/) >> "
                   "/AP << /N 5 0 R >> >>"),
        appearance("1 0 0"),
        QByteArray("<< /Type /Annot /Subtype /Link /Rect [120 140 160 180] /F 128 "
                   "/Border [0 0 0] /A << /S /URI /URI (https://locked.example/) >> "
                   "/AP << /N 7 0 R >> >>"),
        appearance("0 0 1"),
    }));

    PDFDocument document;
    PDFEditController controller;
    PDFFormController forms;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    forms.setDocument(&document);
    controller.setFormController(&forms);
    QTRY_VERIFY_WITH_TIMEOUT(!forms.javascriptTransitionPending(), 5000);
    QQuickWindow window;
    window.setColor(Qt::white);
    window.resize(400, 400);
    TestablePDFPageItem item(window.contentItem());
    item.setSize(QSizeF(400, 400));
    item.setEditController(&controller);
    item.setFormController(&forms);
    item.setDocument(&document);
    item.completeForTest();
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed() && item.draftTextureCurrent()
                                && item.draftTextureReady(), 5000);
    auto grabPage = [&]() -> QImage {
        const auto grab = item.grabToImage(QSize(400, 400));
        if (!grab || !QTest::qWaitFor([&]() { return !grab->image().isNull(); }, 5000))
            return {};
        return grab->image();
    };
    const QImage original = grabPage();
    QVERIFY(!original.isNull());
    QCOMPARE(original.pixelColor(80, 80), QColor(Qt::red));
    QCOMPARE(original.pixelColor(280, 80), QColor(Qt::blue));

    QVERIFY(removeWebLinksAndWait(controller));
    QTRY_VERIFY_WITH_TIMEOUT(item.draftTextureCurrent() && item.draftTextureReady(), 5000);
    const QImage removed = grabPage();
    QVERIFY(!removed.isNull());
    QCOMPARE(removed.pixelColor(80, 80), QColor(Qt::white));
    // Suppression must leave the protected Link's appearance visible.
    QCOMPARE(removed.pixelColor(280, 80), QColor(Qt::blue));

    controller.undo();
    QTRY_VERIFY_WITH_TIMEOUT(item.draftTextureCurrent() && item.draftTextureReady(), 5000);
    const QImage undone = grabPage();
    QVERIFY(!undone.isNull());
    QCOMPARE(changedPixelCount(original, undone, original.rect()), 0);

    controller.redo();
    QTRY_VERIFY_WITH_TIMEOUT(item.draftTextureCurrent() && item.draftTextureReady(), 5000);
    const QImage redone = grabPage();
    QVERIFY(!redone.isNull());
    QCOMPARE(changedPixelCount(removed, redone, removed.rect()), 0);

    controller.discardDrafts();
    QTRY_VERIFY_WITH_TIMEOUT(item.draftTextureCurrent() && item.draftTextureReady(), 5000);
    const QImage discarded = grabPage();
    QVERIFY(!discarded.isNull());
    QCOMPARE(changedPixelCount(original, discarded, original.rect()), 0);
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::rejectsRemovalWithUnsavedContent()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("remove-web-links-unsaved.pdf"));
    QVERIFY(createRemovalWebLinkPdf(path));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(!controller.removeWebLinks());
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    QCOMPARE(controller.addBlankPage(), 1);
    QVERIFY(!controller.removeWebLinks());
    QVERIFY(!controller.removingWebLinks());
    controller.discardDrafts();
    controller.setTool(PDFEditController::StickyNoteTool);
    QVERIFY(!controller.addStickyNote(0, QPointF(0.3, 0.3)).isEmpty());
    QVERIFY(!controller.removeWebLinks());
    controller.discardDrafts();
    QVERIFY(removeWebLinksAndWait(controller));
    QVERIFY(controller.hasDrafts());
}

void PDFEditControllerTest::cancelsStaleWebLinkRemoval()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("remove-web-links-cancel.pdf"));
    QVERIFY(createRemovalWebLinkPdf(path));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    for (int action = 0; action < 4; ++action) {
        bool started = false;
        QTRY_VERIFY_WITH_TIMEOUT(started || (started = controller.removeWebLinks()), 5000);
        QVERIFY(controller.removingWebLinks());
        if (action == 0) {
            controller.cancelWebLinkCreation();
        } else if (action == 1) {
            controller.discardDrafts();
        } else if (action == 2) {
            controller.setDocument(nullptr);
            controller.setDocument(&document);
        } else {
            QCOMPARE(controller.addBlankPage(), 1);
            controller.undo();
        }
        QVERIFY(!controller.removingWebLinks());
        QVERIFY(!controller.hasDrafts());
        QVERIFY(removeWebLinksAndWait(controller));
        QVERIFY(controller.hasDrafts());
        controller.undo();
        QVERIFY(!controller.hasDrafts());
        QVERIFY(!controller.canUndo());
        controller.discardDrafts();
    }
}

void PDFEditControllerTest::savesWebLinkRemovalWithCommentDeletion()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("remove-web-links-comment.pdf"));
    const QString output = m_temporaryDirectory.filePath(QStringLiteral("remove-web-links-comment-saved.pdf"));
    QVERIFY(createRemovalWebLinkPdf(path, true));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    QVERIFY(removeWebLinksAndWait(controller));
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentIndexState(), PDFEditController::CommentIndexReady, 5000);
    QCOMPARE(controller.commentItems().size(), 2);
    QString removedComment;
    for (const QVariant &entry : controller.commentItems()) {
        const QVariantMap item = entry.toMap();
        if (item.value(QStringLiteral("text")).toString() == QStringLiteral("Remove this comment"))
            removedComment = item.value(QStringLiteral("objectId")).toString();
    }
    QVERIFY(!removedComment.isEmpty());
    QVERIFY(controller.removeComment(0, removedComment));
    controller.setStampName(QStringLiteral("Approved"));
    QVERIFY(!controller.addStamp(0, QRectF(0.3, 0.4, 0.3, 0.1)).isEmpty());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(output)));
    const WebLinkInspection saved = inspectWebLinks(output);
    QVERIFY(saved.readable);
    QCOMPARE(saved.links.size(), 6);
    QCOMPARE(saved.comments, QStringList{QStringLiteral("Keep this comment")});
    QCOMPARE(saved.stampAppearances.size(), 1);
    QVERIFY(!saved.stampAppearances.constFirst().isEmpty());
}

void PDFEditControllerTest::addsManualWebAndPageLinks()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("manual-links-add.pdf"));
    const QString output = m_temporaryDirectory.filePath(QStringLiteral("manual-links-add-saved.pdf"));
    QVERIFY(createWebLinkPdf(source, {{QStringLiteral("Page one")}, {QStringLiteral("Page two")}}));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    controller.setDocument(&document);
    QVERIFY(beginLinkEditingAndWait(controller));
    QCOMPARE(controller.tool(), PDFEditController::LinkTool);
    // Opening and cancelling the properties dialog never creates a draft.
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    const QRectF webRect(0.1, 0.15, 0.25, 0.06);
    const QString web = controller.addLink(0, webRect, manualWebProperties());
    QVERIFY(!web.isEmpty());
    QVariantMap pageProperties = manualWebProperties();
    pageProperties[QStringLiteral("url")] = QString();
    pageProperties[QStringLiteral("targetPage")] = 1;
    const QString page = controller.addLink(0, QRectF(0.1, 0.3, 0.2, 0.05), pageProperties);
    QVERIFY(!page.isEmpty());
    QVERIFY(web != page);
    QCOMPARE(controller.editableLinksForPage(0).size(), 2);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(output)));
    QVERIFY(!controller.hasDrafts());
    const WebLinkInspection saved = inspectWebLinks(output);
    QVERIFY(saved.readable);
    QCOMPARE(saved.links.size(), 2);
    QVERIFY(std::any_of(saved.links.cbegin(), saved.links.cend(), [](const SavedWebLink &link) {
        return link.uri == QStringLiteral("https://manual.example/path") && link.borderWidth == 0;
    }));
    QVERIFY(std::any_of(saved.links.cbegin(), saved.links.cend(), [](const SavedWebLink &link) {
        return link.targetPage == 1 && link.destinationView == PDFDEST_VIEW_FIT;
    }));
    QVERIFY(inspectWebLinks(source).links.isEmpty());
    QCOMPARE(fixtureContentStream(output), fixtureContentStream(source));
    QVERIFY(beginLinkEditingAndWait(controller));
    QCOMPARE(controller.editableLinksForPage(0).size(), 2);
    const QVariantMap reopened = editableLinkWithUrl(controller, 0, QStringLiteral("https://manual.example/path"));
    QVERIFY(!reopened.isEmpty());
    QVERIFY(reopened.value(QStringLiteral("editable")).toBool());
    QVERIFY(!reopened.value(QStringLiteral("draft")).toBool());
    QVERIFY(!reopened.value(QStringLiteral("visible")).toBool());
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::reloadsManualLinksAfterSaving()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("manual-links-reload.pdf"));
    const QString output = m_temporaryDirectory.filePath(QStringLiteral("manual-links-reload-saved.pdf"));
    QVERIFY(createWebLinkPdf(source, {{QStringLiteral("Save keeps link editing active")}}));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    controller.setDocument(&document);
    QVERIFY(beginLinkEditingAndWait(controller));
    const QRectF rect(0.1, 0.2, 0.3, 0.1);
    QVERIFY(!controller.addLink(0, rect, manualWebProperties()).isEmpty());
    QVERIFY(controller.requestSaveAs(QUrl::fromLocalFile(output)));
    QTRY_VERIFY_WITH_TIMEOUT(!controller.saving(), 5000);
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(controller.tool(), PDFEditController::LinkTool);
    // The active link tool reloads source regions without reselecting its menu.
    QTRY_COMPARE_WITH_TIMEOUT(controller.editableLinksForPage(0).size(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.loadingEditableLinks(), 5000);
    const QVariantMap saved = controller.editableLinksForPage(0).constFirst().toMap();
    QVERIFY(!saved.value(QStringLiteral("draft")).toBool());
    const QString editedUrl = QStringLiteral("https://reload.example/");
    QVERIFY(controller.updateLink(0, saved.value(QStringLiteral("id")).toString(),
                                  rect, manualWebProperties(editedUrl)));
    QVERIFY(controller.save());
    QCOMPARE(controller.tool(), PDFEditController::LinkTool);
    QTRY_COMPARE_WITH_TIMEOUT(controller.editableLinksForPage(0).size(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.loadingEditableLinks(), 5000);
    const QVariantMap resaved = editableLinkWithUrl(controller, 0, editedUrl);
    QVERIFY(!resaved.isEmpty());
    QVERIFY(!resaved.value(QStringLiteral("draft")).toBool());
    QVERIFY(resaved.value(QStringLiteral("editable")).toBool());
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::reopensFractionalManualLinkBorders()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("manual-links-fractional.pdf"));
    const QString output = m_temporaryDirectory.filePath(QStringLiteral("manual-links-fractional-saved.pdf"));
    QVERIFY(createWebLinkPdf(source, {{QStringLiteral("Fractional drag geometry")}}));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    controller.setDocument(&document);
    QVERIFY(beginLinkEditingAndWait(controller));
    // A real drag at fractional zoom placed these PDF-point edges. Serializing
    // their decimals, reading them as floats, and then subtracting the edges
    // changes millipoint rounding at exactly .0005.
    const QRectF rect(47.96875 / 600.0, 124.5 / 800.0,
                      191.9375 / 600.0, 28.84375 / 800.0);
    QVariantMap properties = manualWebProperties();
    properties[QStringLiteral("visible")] = true;
    properties[QStringLiteral("color")] = QColor(Qt::black);
    QVERIFY(!controller.addLink(0, rect, properties).isEmpty());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(output)));
    QVERIFY(beginLinkEditingAndWait(controller));
    QVariantMap saved = editableLinkWithUrl(controller, 0, QStringLiteral("https://manual.example/path"));
    QVERIFY(!saved.isEmpty());
    QVERIFY(saved.value(QStringLiteral("editable")).toBool());
    // Editing and resaving also uses the rounded source frame, so its own
    // appearance must remain recognizable after another serialization.
    for (int style = 1; style <= 2; ++style) {
        properties[QStringLiteral("lineStyle")] = style;
        QVERIFY(controller.updateLink(0, saved.value(QStringLiteral("id")).toString(),
                                      saved.value(QStringLiteral("rect")).toRectF(), properties));
        QVERIFY(controller.save());
        QVERIFY(beginLinkEditingAndWait(controller));
        saved = editableLinkWithUrl(controller, 0, QStringLiteral("https://manual.example/path"));
        QVERIFY(!saved.isEmpty());
        QVERIFY(saved.value(QStringLiteral("editable")).toBool());
        QCOMPARE(saved.value(QStringLiteral("lineStyle")).toInt(), style);
    }
}

void PDFEditControllerTest::rendersSavedManualLinkBorders_data()
{
    QTest::addColumn<int>("lineStyle");
    QTest::addColumn<bool>("visible");
    QTest::addColumn<int>("rotation");
    QTest::addColumn<bool>("crop");
    const QStringList styles{QStringLiteral("solid"), QStringLiteral("dashed"),
                             QStringLiteral("underline"), QStringLiteral("invisible")};
    for (int style = 0; style < styles.size(); ++style) {
        for (const int rotation : {0, 90, 180, 270}) {
            for (const bool crop : {false, true}) {
                const QByteArray name = QStringLiteral("%1-%2-%3")
                    .arg(styles.at(style)).arg(rotation).arg(crop ? "cropped" : "uncropped").toLatin1();
                QTest::newRow(name.constData()) << (style == 3 ? 0 : style) << (style != 3)
                                              << rotation << crop;
            }
        }
    }
}

void PDFEditControllerTest::rendersSavedManualLinkBorders()
{
    QFETCH(int, lineStyle);
    QFETCH(bool, visible);
    QFETCH(int, rotation);
    QFETCH(bool, crop);
    const QString suffix = QStringLiteral("%1-%2-%3-%4.pdf").arg(lineStyle).arg(visible).arg(rotation).arg(crop);
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("manual-link-border-") + suffix);
    const QString output = m_temporaryDirectory.filePath(QStringLiteral("manual-link-border-saved-") + suffix);
    QVERIFY(createWebLinkPdf(source, {{QStringLiteral("Keep this source text unchanged")}}, false, rotation, crop));
    QSize pixelSize;
    {
        PDFDocument document;
        PDFEditController controller;
        QVERIFY(document.open(QUrl::fromLocalFile(source)));
        pixelSize = document.pageSize(0).toSize();
        controller.setDocument(&document);
        QVERIFY(beginLinkEditingAndWait(controller));
        QVariantMap properties = manualWebProperties();
        properties[QStringLiteral("visible")] = visible;
        properties[QStringLiteral("color")] = QColor(Qt::red);
        properties[QStringLiteral("width")] = 4.0;
        properties[QStringLiteral("lineStyle")] = lineStyle;
        // At one PDF point per pixel, the link occupies x=120..300, y=200..280.
        const QRectF rect(120.0 / pixelSize.width(), 200.0 / pixelSize.height(),
                          180.0 / pixelSize.width(), 80.0 / pixelSize.height());
        QVERIFY(!controller.addLink(0, rect, properties).isEmpty());
        QVERIFY(controller.saveAs(QUrl::fromLocalFile(output)));
    }
    const QImage original = renderFirstPage(source, pixelSize, FPDF_ANNOT);
    QVERIFY(!original.isNull());
    QCOMPARE(fixtureContentStream(output), fixtureContentStream(source));
    QCOMPARE(renderFirstPage(output, pixelSize), original);
    const QImage saved = renderFirstPage(output, pixelSize, FPDF_ANNOT);
    QVERIFY(!saved.isNull());
    const auto maximumColoredPixelsPerRow = [](const QImage &image, const QColor &expected, const QRect &area) {
        int maximum = 0;
        for (int y = area.top(); y <= area.bottom(); ++y) {
            int matching = 0;
            for (int x = area.left(); x <= area.right(); ++x) {
                const QColor color = image.pixelColor(x, y);
                matching += qAbs(color.red() - expected.red()) < 70
                    && qAbs(color.green() - expected.green()) < 70
                    && qAbs(color.blue() - expected.blue()) < 70;
            }
            maximum = std::max(maximum, matching);
        }
        return maximum;
    };
    const QRect topBand(135, 196, 150, 9);
    const QRect bottomBand(135, 276, 150, 9);
    const int top = maximumColoredPixelsPerRow(saved, Qt::red, topBand);
    const int bottom = maximumColoredPixelsPerRow(saved, Qt::red, bottomBand);
    if (!visible) {
        QCOMPARE(saved, original);
    } else if (lineStyle == 1) {
        // The dashed appearance has both colored segments and genuine gaps.
        QVERIFY2(top > 30 && top < 135, qPrintable(QString::number(top)));
        QVERIFY2(bottom > 30 && bottom < 135, qPrintable(QString::number(bottom)));
    } else {
        QCOMPARE(top, lineStyle == 2 ? 0 : 150);
        QCOMPARE(bottom, 150);
    }
    if (lineStyle == 2) {
        QCOMPARE(maximumColoredPixelsPerRow(saved, Qt::red, QRect(116, 210, 9, 60)), 0);
        QCOMPARE(maximumColoredPixelsPerRow(saved, Qt::red, QRect(296, 210, 9, 60)), 0);
    }
    QCOMPARE(saved.copy(QRect(130, 210, 160, 60)), original.copy(QRect(130, 210, 160, 60)));
    QCOMPARE(saved.copy(QRect(0, 0, 600, 180)), original.copy(QRect(0, 0, 600, 180)));

    PDFDocument reopenedDocument;
    PDFEditController reopenedController;
    QVERIFY(reopenedDocument.open(QUrl::fromLocalFile(output)));
    reopenedController.setDocument(&reopenedDocument);
    QVERIFY(beginLinkEditingAndWait(reopenedController));
    const QVariantMap reopened = editableLinkWithUrl(reopenedController, 0, QStringLiteral("https://manual.example/path"));
    QVERIFY(!reopened.isEmpty());
    QVERIFY(reopened.value(QStringLiteral("editable")).toBool());
    QCOMPARE(reopened.value(QStringLiteral("visible")).toBool(), visible);
    QCOMPARE(reopened.value(QStringLiteral("lineStyle")).toInt(), lineStyle);
    QCOMPARE(reopened.value(QStringLiteral("color")).value<QColor>(), QColor(Qt::red));
    QCOMPARE(reopened.value(QStringLiteral("width")).toDouble(), 4.0);
    QVariantMap recolored = reopened;
    recolored[QStringLiteral("color")] = QColor(Qt::blue);
    QVERIFY(reopenedController.updateLink(0, reopened.value(QStringLiteral("id")).toString(),
                                         reopened.value(QStringLiteral("rect")).toRectF(), recolored));
    QVERIFY(reopenedController.save());
    QCOMPARE(fixtureContentStream(output), fixtureContentStream(source));
    const QImage resaved = renderFirstPage(output, pixelSize, FPDF_ANNOT);
    QVERIFY(!resaved.isNull());
    QCOMPARE(maximumColoredPixelsPerRow(resaved, Qt::blue, topBand), top);
    QCOMPARE(maximumColoredPixelsPerRow(resaved, Qt::blue, bottomBand), bottom);
    QCOMPARE(maximumColoredPixelsPerRow(resaved, Qt::red, QRect(110, 190, 200, 100)), 0);
    QCOMPARE(resaved.copy(QRect(130, 210, 160, 60)), original.copy(QRect(130, 210, 160, 60)));
    if (!visible)
        QCOMPARE(resaved, original);
    QVERIFY(beginLinkEditingAndWait(reopenedController));
    const QVariantMap reloaded = editableLinkWithUrl(reopenedController, 0, QStringLiteral("https://manual.example/path"));
    QVERIFY(reloaded.value(QStringLiteral("editable")).toBool());
    QCOMPARE(reloaded.value(QStringLiteral("color")).value<QColor>(), QColor(Qt::blue));
}

void PDFEditControllerTest::editsManualSourceLinkAndKeepsMetadata()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("manual-links-edit.pdf"));
    const QString output = m_temporaryDirectory.filePath(QStringLiteral("manual-links-edit-saved.pdf"));
    QVERIFY(createManualSourcePdf(source));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    controller.setDocument(&document);
    QVERIFY(beginLinkEditingAndWait(controller));
    const QVariantMap original = editableLinkWithUrl(controller, 0, QStringLiteral("https://original.example/"));
    QVERIFY(!original.isEmpty());
    const QString id = original.value(QStringLiteral("id")).toString();
    QVariantMap properties = controller.linkProperties(0, id);
    QVERIFY(!properties.isEmpty());
    properties[QStringLiteral("url")] = QStringLiteral("https://edited.example/new?value=1");
    properties[QStringLiteral("visible")] = true;
    properties[QStringLiteral("color")] = QColor(Qt::red);
    properties[QStringLiteral("width")] = 3.0;
    properties[QStringLiteral("lineStyle")] = 1;
    properties[QStringLiteral("highlight")] = 2;
    const QRectF moved(0.2, 0.25, 0.3, 0.1);
    QVERIFY(controller.updateLink(0, id, moved, properties));
    QCOMPARE(controller.deletedSourceWebLinkAnnotationsForPage(0).size(), 1);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(output)));
    const WebLinkInspection saved = inspectWebLinks(output);
    QVERIFY(saved.readable);
    QCOMPARE(saved.links.size(), 3);
    const auto edited = std::find_if(saved.links.cbegin(), saved.links.cend(), [](const SavedWebLink &link) {
        return link.uri == QStringLiteral("https://edited.example/new?value=1");
    });
    QVERIFY(edited != saved.links.cend());
    QCOMPARE(edited->marker, QStringLiteral("keep-unrelated-metadata"));
    QCOMPARE(fixtureContentStream(output), fixtureContentStream(source));
    QVERIFY(beginLinkEditingAndWait(controller));
    const QVariantMap reopened = editableLinkWithUrl(controller, 0, properties.value(QStringLiteral("url")).toString());
    QVERIFY(!reopened.isEmpty());
    QCOMPARE(reopened.value(QStringLiteral("color")).value<QColor>(), QColor(Qt::red));
    QCOMPARE(reopened.value(QStringLiteral("width")).toDouble(), 3.0);
    QCOMPARE(reopened.value(QStringLiteral("lineStyle")).toInt(), 1);
    QCOMPARE(reopened.value(QStringLiteral("highlight")).toInt(), 2);
    QVERIFY(reopened.value(QStringLiteral("visible")).toBool());
    const QRectF actual = reopened.value(QStringLiteral("rect")).toRectF();
    QVERIFY(QLineF(actual.topLeft(), moved.topLeft()).length() < 0.0001);
    QVERIFY(QLineF(actual.bottomRight(), moved.bottomRight()).length() < 0.0001);
}

void PDFEditControllerTest::manualLinksUseHistoryAndDiscard()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("manual-links-history.pdf"));
    QVERIFY(createWebLinkPdf(source, {{QStringLiteral("History")}}));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    controller.setDocument(&document);
    QVERIFY(beginLinkEditingAndWait(controller));
    const QRectF initial(0.1, 0.1, 0.2, 0.1);
    const QRectF moved(0.3, 0.4, 0.4, 0.2);
    const QString id = controller.addLink(0, initial, manualWebProperties());
    QVERIFY(!id.isEmpty());
    QVERIFY(controller.updateLink(0, id, moved, manualWebProperties(QStringLiteral("https://history.example/"))));
    QVERIFY(controller.deleteLink(0, id));
    QVERIFY(controller.editableLinksForPage(0).isEmpty());
    controller.undo();
    QCOMPARE(controller.linkProperties(0, id).value(QStringLiteral("rect")).toRectF(), moved);
    controller.undo();
    QCOMPARE(controller.linkProperties(0, id).value(QStringLiteral("rect")).toRectF(), initial);
    controller.undo();
    QVERIFY(controller.editableLinksForPage(0).isEmpty());
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    controller.redo();
    controller.redo();
    QCOMPARE(controller.linkProperties(0, id).value(QStringLiteral("url")).toString(), QStringLiteral("https://history.example/"));
    controller.discardDrafts();
    QVERIFY(controller.editableLinksForPage(0).isEmpty());
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
}

void PDFEditControllerTest::changesManualSourceLinkDestinationType_data()
{
    QTest::addColumn<bool>("webToPage");
    QTest::newRow("web-to-page") << true;
    QTest::newRow("page-to-web") << false;
}

void PDFEditControllerTest::changesManualSourceLinkDestinationType()
{
    QFETCH(bool, webToPage);
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("manual-links-destination-type-%1.pdf").arg(webToPage));
    const QString output = m_temporaryDirectory.filePath(QStringLiteral("manual-links-destination-type-%1-saved.pdf").arg(webToPage));
    QVERIFY(createManualSourcePdf(source));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    controller.setDocument(&document);
    QVERIFY(beginLinkEditingAndWait(controller));
    QVariantMap selected;
    for (const QVariant &entry : controller.editableLinksForPage(0)) {
        const QVariantMap link = entry.toMap();
        if ((webToPage && link.value(QStringLiteral("url")).toString() == QStringLiteral("https://original.example/"))
            || (!webToPage && link.value(QStringLiteral("targetPage")).toInt() == 1)) {
            selected = link;
            break;
        }
    }
    QVERIFY(!selected.isEmpty());
    QVariantMap properties = manualWebProperties(QStringLiteral("https://converted.example/"));
    if (webToPage) {
        properties[QStringLiteral("url")] = QString();
        properties[QStringLiteral("targetPage")] = 0;
    }
    const QRectF rect = selected.value(QStringLiteral("rect")).toRectF();
    QVERIFY(controller.updateLink(0, selected.value(QStringLiteral("id")).toString(), rect, properties));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(output)));
    const WebLinkInspection saved = inspectWebLinks(output);
    QVERIFY(saved.readable);
    QCOMPARE(saved.links.size(), 3);
    if (webToPage) {
        const auto converted = std::find_if(saved.links.cbegin(), saved.links.cend(), [](const SavedWebLink &link) {
            return link.targetPage == 0;
        });
        QVERIFY(converted != saved.links.cend());
        QVERIFY(converted->uri.isEmpty());
        QCOMPARE(converted->destinationView, static_cast<unsigned long>(PDFDEST_VIEW_FIT));
        QCOMPARE(converted->marker, QStringLiteral("keep-unrelated-metadata"));
    } else {
        const auto converted = std::find_if(saved.links.cbegin(), saved.links.cend(), [](const SavedWebLink &link) {
            return link.uri == QStringLiteral("https://converted.example/");
        });
        QVERIFY(converted != saved.links.cend());
        QCOMPARE(converted->targetPage, -1);
    }
}

void PDFEditControllerTest::deletesManualSourceLinksThroughHistory()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("manual-links-delete.pdf"));
    const QString output = m_temporaryDirectory.filePath(QStringLiteral("manual-links-delete-saved.pdf"));
    QVERIFY(createManualSourcePdf(source));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    controller.setDocument(&document);
    QVERIFY(beginLinkEditingAndWait(controller));
    const QString id = editableLinkWithUrl(controller, 0, QStringLiteral("https://original.example/"))
                           .value(QStringLiteral("id")).toString();
    QVERIFY(!id.isEmpty());
    QVERIFY(controller.deleteLink(0, id));
    QVERIFY(controller.linkProperties(0, id).isEmpty());
    QCOMPARE(controller.deletedSourceWebLinkAnnotationsForPage(0).size(), 1);
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.linkProperties(0, id).isEmpty());
    QVERIFY(controller.deletedSourceWebLinkAnnotationsForPage(0).isEmpty());
    controller.redo();
    QVERIFY(controller.linkProperties(0, id).isEmpty());
    controller.discardDrafts();
    QVERIFY(beginLinkEditingAndWait(controller));
    const QString restored = editableLinkWithUrl(controller, 0, QStringLiteral("https://original.example/"))
                                 .value(QStringLiteral("id")).toString();
    QVERIFY(controller.deleteLink(0, restored));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(output)));
    const WebLinkInspection saved = inspectWebLinks(output);
    QVERIFY(saved.readable);
    QCOMPARE(saved.links.size(), 2);
    QVERIFY(std::any_of(saved.links.cbegin(), saved.links.cend(), [](const SavedWebLink &link) {
        return link.uri == QStringLiteral("https://protected.example/");
    }));
    QVERIFY(std::any_of(saved.links.cbegin(), saved.links.cend(), [](const SavedWebLink &link) {
        return link.targetPage == 1;
    }));
}

void PDFEditControllerTest::rejectsProtectedManualSourceLinks()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("manual-links-protected.pdf"));
    QVERIFY(createRemovalWebLinkPdf(source, true));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    controller.setDocument(&document);
    QVERIFY(beginLinkEditingAndWait(controller));
    for (const QString &url : {QStringLiteral("https://readonly.example/"),
                               QStringLiteral("https://locked.example/"),
                               QStringLiteral("https://lockedcontents.example/")}) {
        const QVariantMap link = editableLinkWithUrl(controller, 0, url);
        QVERIFY2(!link.isEmpty(), qPrintable(url));
        QVERIFY(!link.value(QStringLiteral("editable")).toBool());
        const QString id = link.value(QStringLiteral("id")).toString();
        QVERIFY(!controller.updateLink(0, id, QRectF(0.2, 0.2, 0.2, 0.1), manualWebProperties()));
        QVERIFY(!controller.deleteLink(0, id));
    }
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
}

void PDFEditControllerTest::rejectsInvalidManualLinks_data()
{
    QTest::addColumn<QString>("url");
    QTest::addColumn<QRectF>("rect");
    QTest::addColumn<int>("targetPage");
    const QRectF valid(0.1, 0.1, 0.2, 0.1);
    for (const QString &url : {QString(), QStringLiteral("javascript:alert(1)"),
                               QStringLiteral("file:///tmp/private"), QStringLiteral("ftp://example.org/a"),
                               QStringLiteral("https://name:password@example.org/"),
                               QStringLiteral("https://example.org/") + QString(4096, QLatin1Char('a'))}) {
        QTest::newRow(qPrintable(QStringLiteral("url-%1").arg(url.left(45)))) << url << valid << -1;
    }
    QTest::newRow("empty-region") << QStringLiteral("https://example.org/") << QRectF() << -1;
    QTest::newRow("negative-origin") << QStringLiteral("https://example.org/") << QRectF(-0.1, 0.1, 0.2, 0.1) << -1;
    QTest::newRow("outside-page") << QStringLiteral("https://example.org/") << QRectF(0.9, 0.1, 0.2, 0.1) << -1;
    QTest::newRow("nonfinite-region") << QStringLiteral("https://example.org/")
                                     << QRectF(std::numeric_limits<double>::quiet_NaN(), 0.1, 0.2, 0.1) << -1;
    QTest::newRow("missing-target") << QString() << valid << 10;
    QTest::newRow("invalid-negative-target") << QString() << valid << -2;
}

void PDFEditControllerTest::rejectsInvalidManualLinks()
{
    QFETCH(QString, url);
    QFETCH(QRectF, rect);
    QFETCH(int, targetPage);
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("manual-links-invalid.pdf"));
    QVERIFY(createWebLinkPdf(source, {{QStringLiteral("Invalid input")}}));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    controller.setDocument(&document);
    QVERIFY(beginLinkEditingAndWait(controller));
    QVariantMap properties = manualWebProperties(url);
    properties[QStringLiteral("targetPage")] = targetPage;
    QVERIFY(controller.addLink(0, rect, properties).isEmpty());
    QVERIFY(controller.addLink(-1, QRectF(0.1, 0.1, 0.2, 0.1), manualWebProperties()).isEmpty());
    QVERIFY(controller.addLink(1, QRectF(0.1, 0.1, 0.2, 0.1), manualWebProperties()).isEmpty());
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
}

void PDFEditControllerTest::savesManualLinksWithCropAndRotation_data()
{
    savesAutoWebLinksWithCropAndRotation_data();
}

void PDFEditControllerTest::savesManualLinksWithCropAndRotation()
{
    QFETCH(int, rotation);
    QFETCH(bool, crop);
    const QString suffix = QStringLiteral("%1-%2.pdf").arg(rotation).arg(crop);
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("manual-links-geometry-") + suffix);
    const QString output = m_temporaryDirectory.filePath(QStringLiteral("manual-links-geometry-saved-") + suffix);
    QVERIFY(createWebLinkPdf(source, {{QStringLiteral("Geometry")}}, true, rotation, crop));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    controller.setDocument(&document);
    QVERIFY(beginLinkEditingAndWait(controller));
    const QVariantMap link = editableLinkWithUrl(controller, 0, QStringLiteral("https://existing-target.example/"));
    QVERIFY(!link.isEmpty());
    const QRectF moved(0.13, 0.27, 0.38, 0.11);
    QVERIFY(controller.updateLink(0, link.value(QStringLiteral("id")).toString(), moved, manualWebProperties()));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(output)));
    QVERIFY(beginLinkEditingAndWait(controller));
    const QVariantMap reopened = editableLinkWithUrl(controller, 0, QStringLiteral("https://manual.example/path"));
    QVERIFY(!reopened.isEmpty());
    const QRectF actual = reopened.value(QStringLiteral("rect")).toRectF();
    QVERIFY(QLineF(actual.topLeft(), moved.topLeft()).length() < 0.0001);
    QVERIFY(QLineF(actual.bottomRight(), moved.bottomRight()).length() < 0.0001);
    QCOMPARE(inspectWebLinks(output).links.size(), 1);
}

void PDFEditControllerTest::mapsManualTargetsAcrossPageChanges()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("manual-links-pages.pdf"));
    const QString output = m_temporaryDirectory.filePath(QStringLiteral("manual-links-pages-saved.pdf"));
    QVERIFY(createWebLinkPdf(source, {{QStringLiteral("One")}, {QStringLiteral("Two")}, {QStringLiteral("Three")}}));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    controller.setDocument(&document);
    QVERIFY(beginLinkEditingAndWait(controller));
    QVariantMap target = manualWebProperties(QString());
    target[QStringLiteral("targetPage")] = 2;
    const QString internal = controller.addLink(0, QRectF(0.1, 0.1, 0.2, 0.1), target);
    QVERIFY(!internal.isEmpty());
    const QString web = controller.addLink(1, QRectF(0.2, 0.2, 0.2, 0.1), manualWebProperties());
    QVERIFY(!web.isEmpty());
    document.setCurrentPage(0);
    QCOMPARE(controller.addBlankPage(), 1);
    QCOMPARE(controller.linkProperties(0, internal).value(QStringLiteral("targetPage")).toInt(), 3);
    QVERIFY(!controller.linkProperties(2, web).isEmpty());
    QVERIFY(controller.addLink(1, QRectF(0.1, 0.1, 0.2, 0.1), manualWebProperties()).isEmpty());
    target[QStringLiteral("targetPage")] = 1;
    QVERIFY(controller.addLink(0, QRectF(0.1, 0.1, 0.2, 0.1), target).isEmpty());
    document.setCurrentPage(2);
    QVERIFY(controller.deleteCurrentPage() >= 0);
    QCOMPARE(controller.linkProperties(0, internal).value(QStringLiteral("targetPage")).toInt(), 2);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(output)));
    const WebLinkInspection saved = inspectWebLinks(output);
    QVERIFY(saved.readable);
    QCOMPARE(saved.links.size(), 1);
    QCOMPARE(saved.links.constFirst().page, 0);
    QCOMPARE(saved.links.constFirst().targetPage, 2);
    QCOMPARE(saved.links.constFirst().destinationView, static_cast<unsigned long>(PDFDEST_VIEW_FIT));
}

void PDFEditControllerTest::rejectsSavingDeletedManualTargetAtomically()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("manual-links-deleted-target.pdf"));
    const QString output = m_temporaryDirectory.filePath(QStringLiteral("manual-links-deleted-target-saved.pdf"));
    QVERIFY(createWebLinkPdf(source, {{QStringLiteral("One")}, {QStringLiteral("Two")}}));
    QVERIFY(createWebLinkPdf(output, {{QStringLiteral("Preserve existing destination")}}));
    const QByteArray unchanged = readFileBytes(output);
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    controller.setDocument(&document);
    QVERIFY(beginLinkEditingAndWait(controller));
    QVariantMap target = manualWebProperties(QString());
    target[QStringLiteral("targetPage")] = 1;
    QVERIFY(!controller.addLink(0, QRectF(0.1, 0.1, 0.2, 0.1), target).isEmpty());
    document.setCurrentPage(1);
    QVERIFY(controller.deleteCurrentPage() >= 0);
    QVERIFY(!controller.saveAs(QUrl::fromLocalFile(output)));
    QCOMPARE(readFileBytes(output), unchanged);
    QVERIFY(controller.hasDrafts());
    controller.undo();
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(output)));
    const WebLinkInspection saved = inspectWebLinks(output);
    QVERIFY(saved.readable);
    QCOMPARE(saved.links.size(), 1);
    QCOMPARE(saved.links.constFirst().targetPage, 1);
}

void PDFEditControllerTest::rejectsChainedManualLinkActionAtomically()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("manual-links-chained-action.pdf"));
    const QString output = m_temporaryDirectory.filePath(QStringLiteral("manual-links-chained-action-saved.pdf"));
    QVERIFY(createManualSourcePdf(source, false, true));
    QVERIFY(createWebLinkPdf(output, {{QStringLiteral("Preserve existing destination")}}));
    const QByteArray sourceBytes = readFileBytes(source);
    const QByteArray outputBytes = readFileBytes(output);
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    controller.setDocument(&document);
    QVERIFY(beginLinkEditingAndWait(controller));
    const QVariantMap original = editableLinkWithUrl(controller, 0, QStringLiteral("https://original.example/"));
    QVERIFY(!original.isEmpty());
    // PDFium exposes the primary URI but not its action chain. Save must reject
    // replacing that chain instead of silently discarding secondary actions.
    QVERIFY(controller.updateLink(0, original.value(QStringLiteral("id")).toString(),
                                  original.value(QStringLiteral("rect")).toRectF(),
                                  manualWebProperties(QStringLiteral("https://edited.example/"))));
    QVERIFY(!controller.saveAs(QUrl::fromLocalFile(output)));
    QCOMPARE(readFileBytes(output), outputBytes);
    QCOMPARE(readFileBytes(source), sourceBytes);
    QVERIFY(controller.hasDrafts());
    QVERIFY(!controller.save());
    QCOMPARE(readFileBytes(source), sourceBytes);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    controller.undo();
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::integratesManualAndAutomaticLinks()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("manual-links-automatic.pdf"));
    const QString output = m_temporaryDirectory.filePath(QStringLiteral("manual-links-automatic-saved.pdf"));
    QVERIFY(createWebLinkPdf(source, {{QStringLiteral("https://auto-one.example/"), QStringLiteral("https://auto-two.example/")}}));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    controller.setDocument(&document);
    QVERIFY(controller.autoCreateWebLinks());
    QTRY_VERIFY_WITH_TIMEOUT(!controller.creatingWebLinks(), 5000);
    QVERIFY(beginLinkEditingAndWait(controller));
    const QVariantMap first = editableLinkWithUrl(controller, 0, QStringLiteral("https://auto-one.example/"));
    QVERIFY(!first.isEmpty());
    QVERIFY(controller.updateLink(0, first.value(QStringLiteral("id")).toString(),
                                  QRectF(0.2, 0.2, 0.3, 0.1), manualWebProperties()));
    const QVariantMap second = editableLinkWithUrl(controller, 0, QStringLiteral("https://auto-two.example/"));
    QVERIFY(!second.isEmpty());
    QVERIFY(controller.deleteLink(0, second.value(QStringLiteral("id")).toString()));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(output)));
    const WebLinkInspection saved = inspectWebLinks(output);
    QVERIFY(saved.readable);
    QCOMPARE(saved.links.size(), 1);
    QCOMPARE(saved.links.constFirst().uri, QStringLiteral("https://manual.example/path"));
    QVERIFY(removeWebLinksAndWait(controller));
    QVERIFY(controller.save());
    QVERIFY(inspectWebLinks(output).links.isEmpty());
}

void PDFEditControllerTest::routesManualLinksThroughConfirmation()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("manual-links-confirmation.pdf"));
    QVERIFY(createWebLinkPdf(source, {{QStringLiteral("Confirm web destination")}}));
    PDFDocument document;
    PDFFormController forms;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    forms.setDocument(&document);
    controller.setDocument(&document);
    controller.setFormController(&forms);
    QVERIFY(beginLinkEditingAndWait(controller));
    const QString id = controller.addLink(0, QRectF(0.1, 0.1, 0.2, 0.1), manualWebProperties());
    QVERIFY(!id.isEmpty());
    const quint64 revision = controller.webLinksRevision();
    QSignalSpy confirmation(&forms, &PDFFormController::externalLinkRequested);
    QSignalSpy cancellation(&forms, &PDFFormController::externalLinkRequestCancelled);
    QVERIFY(!controller.activateManualLink(0, id, revision));
    controller.setTool(PDFEditController::BrowseTool);
    QVERIFY(controller.activateManualLink(0, id, revision));
    QCOMPARE(confirmation.size(), 1);
    QCOMPARE(confirmation.constFirst().at(2).toString(), QStringLiteral("https://manual.example/path"));
    controller.setTool(PDFEditController::LinkTool);
    QVERIFY(controller.deleteLink(0, id));
    QCOMPARE(cancellation.size(), 1);
    QVERIFY(!forms.confirmExternalLink(confirmation.constFirst().at(0).toULongLong()));
    controller.setTool(PDFEditController::BrowseTool);
    QVERIFY(!controller.activateManualLink(0, id, revision));
    controller.undo();
    QVERIFY(!controller.activateManualLink(0, id, revision));
    QVERIFY(controller.activateManualLink(0, id, controller.webLinksRevision()));
    QCOMPARE(confirmation.size(), 2);
    forms.cancelExternalLink(confirmation.constLast().at(0).toULongLong());
}

void PDFEditControllerTest::routesSavedManualPageLinksThroughNavigation_data()
{
    QTest::addColumn<bool>("internalAction");
    QTest::newRow("direct-destination") << false;
    QTest::newRow("goto-action") << true;
}

void PDFEditControllerTest::routesSavedManualPageLinksThroughNavigation()
{
    QFETCH(bool, internalAction);
    const QString source = m_temporaryDirectory.filePath(
        QStringLiteral("manual-link-navigation-%1.pdf").arg(internalAction));
    const QString output = m_temporaryDirectory.filePath(
        QStringLiteral("manual-link-navigation-%1-saved.pdf").arg(internalAction));
    QVERIFY(createManualSourcePdf(source, internalAction));
    QPointF sourceCenter;
    const QRectF addedRect(0.1, 0.3, 0.2, 0.08);
    {
        PDFDocument document;
        PDFEditController controller;
        QVERIFY(document.open(QUrl::fromLocalFile(source)));
        controller.setDocument(&document);
        QVERIFY(beginLinkEditingAndWait(controller));
        for (const QVariant &entry : controller.editableLinksForPage(0)) {
            const QVariantMap link = entry.toMap();
            if (link.value(QStringLiteral("targetPage")).toInt() == 1)
                sourceCenter = link.value(QStringLiteral("rect")).toRectF().center();
        }
        QVERIFY(!sourceCenter.isNull());
        QVariantMap properties = manualWebProperties(QString());
        properties[QStringLiteral("targetPage")] = 1;
        QVERIFY(!controller.addLink(0, addedRect, properties).isEmpty());
        QVERIFY(controller.saveAs(QUrl::fromLocalFile(output)));
    }

    // Reopen with only the native form runtime, so draft overlays cannot navigate.
    PDFDocument reopened;
    PDFFormController forms;
    QVERIFY(reopened.open(QUrl::fromLocalFile(output)));
    forms.setDocument(&reopened);
    QTRY_VERIFY_WITH_TIMEOUT(!forms.javascriptTransitionPending(), 5000);
    QSignalSpy navigation(&forms, &PDFFormController::pageNavigationRequested);
    QSignalSpy external(&forms, &PDFFormController::externalLinkRequested);
    const QSize size(600, 800);
    int navigationCount = 0;
    for (const QPointF center : {sourceCenter, addedRect.center()}) {
        reopened.setCurrentPage(0);
        const QPointF point(center.x() * size.width(), center.y() * size.height());
        forms.pointerEvent(0, PDFFormWorker::PointerEventType::Press, 0, point, size);
        forms.pointerEvent(0, PDFFormWorker::PointerEventType::Release, 0, QPointF(10, 10), size);
        QTRY_VERIFY_WITH_TIMEOUT(!forms.hasDrafts(), 5000);
        QCOMPARE(navigation.size(), navigationCount);
        QCOMPARE(reopened.currentPage(), 0);
        forms.pointerEvent(0, PDFFormWorker::PointerEventType::Press, 0, point, size);
        forms.pointerEvent(0, PDFFormWorker::PointerEventType::Release, 0, point, size);
        ++navigationCount;
        QTRY_COMPARE_WITH_TIMEOUT(navigation.size(), navigationCount, 5000);
        QCOMPARE(navigation.constLast().at(0).toInt(), 1);
        QCOMPARE(reopened.currentPage(), 1);
    }
    QCOMPARE(external.size(), 0);
}

void PDFEditControllerTest::suppressesOriginalDestinationWhileEditingSourceLink()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("manual-link-source-suppression.pdf"));
    QVERIFY(createManualSourcePdf(source));
    PDFDocument document;
    PDFFormController forms;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    forms.setDocument(&document);
    controller.setDocument(&document);
    controller.setFormController(&forms);
    QTRY_VERIFY_WITH_TIMEOUT(!forms.javascriptTransitionPending(), 5000);
    QVERIFY(beginLinkEditingAndWait(controller));
    const QVariantMap original = editableLinkWithUrl(controller, 0, QStringLiteral("https://original.example/"));
    QVERIFY(!original.isEmpty());
    const QString id = original.value(QStringLiteral("id")).toString();
    const QPointF center = original.value(QStringLiteral("rect")).toRectF().center();
    QVERIFY(controller.updateLink(0, id, QRectF(0.1, 0.3, 0.3, 0.1),
                                  manualWebProperties(QStringLiteral("https://replacement.example/"))));
    controller.setTool(PDFEditController::BrowseTool);
    QSignalSpy confirmation(&forms, &PDFFormController::externalLinkRequested);
    const QSize size(600, 800);
    const QPointF point(center.x() * size.width(), center.y() * size.height());
    const auto clickSource = [&]() {
        forms.pointerEvent(0, PDFFormWorker::PointerEventType::Press, 0, point, size);
        forms.pointerEvent(0, PDFFormWorker::PointerEventType::Release, 0, point, size);
    };
    clickSource();
    QTRY_VERIFY_WITH_TIMEOUT(!forms.hasDrafts(), 5000);
    QCOMPARE(confirmation.size(), 0);
    QVERIFY(controller.activateManualLink(0, id, controller.webLinksRevision()));
    QCOMPARE(confirmation.size(), 1);
    QCOMPARE(confirmation.constLast().at(2).toString(), QStringLiteral("https://replacement.example/"));
    forms.cancelExternalLink(confirmation.constLast().at(0).toULongLong());

    controller.undo();
    clickSource();
    QTRY_COMPARE_WITH_TIMEOUT(confirmation.size(), 2, 5000);
    QCOMPARE(confirmation.constLast().at(2).toString(), QStringLiteral("https://original.example/"));
    forms.cancelExternalLink(confirmation.constLast().at(0).toULongLong());
    controller.redo();
    clickSource();
    QTRY_VERIFY_WITH_TIMEOUT(!forms.hasDrafts(), 5000);
    QCOMPARE(confirmation.size(), 2);
    QVERIFY(controller.activateManualLink(0, id, controller.webLinksRevision()));
    QCOMPARE(confirmation.size(), 3);
    QCOMPARE(confirmation.constLast().at(2).toString(), QStringLiteral("https://replacement.example/"));
    forms.cancelExternalLink(confirmation.constLast().at(0).toULongLong());
}
