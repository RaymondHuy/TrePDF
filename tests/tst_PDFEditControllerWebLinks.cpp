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

struct SavedWebLink {
    int page = -1;
    QString uri;
    QRectF rect;
    float borderWidth = -1.0f;
};

struct WebLinkInspection {
    bool readable = false;
    QVector<SavedWebLink> links;
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
            if (FPDFAnnot_GetSubtype(annotation) != FPDF_ANNOT_LINK) {
                continue;
            }
            const FPDF_LINK link = FPDFAnnot_GetLink(annotation);
            const FPDF_ACTION action = link ? FPDFLink_GetAction(link) : nullptr;
            const unsigned long size = action
                ? FPDFAction_GetURIPath(document, action, nullptr, 0)
                : 0;
            if (size == 0 || size > 16384) {
                return result;
            }
            QByteArray uri(size, '\0');
            if (FPDFAction_GetURIPath(document, action, uri.data(), size) != size) {
                return result;
            }
            uri.chop(1);
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
