#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

using namespace PDFEditControllerTestSupport;

void PDFEditControllerTest::showsGroupedStampPresetMenuUi()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("stamp-preset-menu-ui.pdf"));
    QVERIFY(createDifferentPageSizesPdf(sourcePath));
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(
        QFINDTESTDATA("../src/Main.qml")));
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window);
    window->resize(1200, 720);
    window->show();
    auto *tabs = root->findChild<QObject *>(QStringLiteral("ribbonTabs"));
    auto *model = root->findChild<PDFTabModel *>();
    QVERIFY(tabs);
    QVERIFY(model);
    auto *session = model->open(QUrl::fromLocalFile(sourcePath));
    QVERIFY(session);
    PDFEditController &controller = *session->editController();
    QVERIFY(tabs->setProperty("currentIndex", 2));
    QTRY_COMPARE(controller.tool(), PDFEditController::CommentSelectTool);
    QObject *stampButton = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((stampButton = root->findChild<QObject *>(
        QStringLiteral("stampRibbonButton"))), 1000);
    QObject *popup = root->findChild<QObject *>(QStringLiteral("stampPresetPopup"));
    QObject *loader = root->findChild<QObject *>(QStringLiteral("stampMenuLoader"));
    QVERIFY(stampButton);
    QVERIFY(popup);
    QVERIFY(loader);
    QVERIFY(stampButton->property("enabled").toBool());
    QVERIFY(!popup->property("visible").toBool());
    QVERIFY(!loader->property("active").toBool());
    QVERIFY(!loader->property("item").value<QObject *>());

    // Popup content is reparented into the window overlay. Follow visual items
    // so the check also sees delegates created by the section Repeaters.
    const auto findItem = [](auto &&self, QQuickItem *parent,
                             const QString &name) -> QQuickItem * {
        if (parent->objectName() == name) {
            return parent;
        }
        for (QQuickItem *child : parent->childItems()) {
            if (QQuickItem *match = self(self, child, name)) {
                return match;
            }
        }
        return nullptr;
    };
    const auto itemNamed = [&window, &findItem](const QString &name) {
        return findItem(findItem, window->contentItem(), name);
    };
    QVERIFY(!itemNamed(QStringLiteral("stampPresetMenu")));
    const QVariantList presets = controller.stampPresets();
    QCOMPARE(presets.size(), 24);
    const QMap<QString, int> expectedGroups{
        {QStringLiteral("dynamic"), 5},
        {QStringLiteral("standard"), 12},
        {QStringLiteral("signHere"), 5},
        {QStringLiteral("additional"), 2}};

    for (const QString &selectedName : {
             QStringLiteral("DynamicReceived"), QStringLiteral("SignHere")}) {
        QVERIFY(QMetaObject::invokeMethod(stampButton, "menuRequested",
                                          Qt::DirectConnection));
        QTRY_VERIFY_WITH_TIMEOUT(popup->property("opened").toBool(), 1000);
        QVERIFY(loader->property("active").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(itemNamed(QStringLiteral("stampPresetMenu")), 1000);
        for (auto group = expectedGroups.cbegin(); group != expectedGroups.cend(); ++group) {
            QQuickItem *section = itemNamed(QStringLiteral("stampSection_") + group.key());
            QVERIFY2(section, qPrintable(group.key()));
            QCOMPARE(section->property("optionCount").toInt(), group.value());
            QVERIFY(!section->property("title").toString().isEmpty());
        }
        int checkedCount = 0;
        for (const QVariant &entry : presets) {
            const QString name = entry.toMap().value(QStringLiteral("name")).toString();
            QQuickItem *option = itemNamed(QStringLiteral("stampOption_") + name);
            QVERIFY2(option, qPrintable(name));
            QCOMPARE(option->property("optionStampName").toString(), name);
            QVERIFY(option->property("enabled").toBool());
            QTRY_VERIFY_WITH_TIMEOUT(option->width() > 0, 1000);
            QTRY_VERIFY_WITH_TIMEOUT(option->height() > 0, 1000);
            if (option->property("checked").toBool()) {
                ++checkedCount;
                QCOMPARE(name, controller.stampName());
            }
        }
        QCOMPARE(checkedCount, 1);
        QQuickItem *selectedOption = itemNamed(QStringLiteral("stampOption_") + selectedName);
        QVERIFY(selectedOption);
        QVERIFY(QMetaObject::invokeMethod(selectedOption, "clicked", Qt::DirectConnection));
        QCOMPARE(controller.stampName(), selectedName);
        QCOMPARE(controller.tool(), PDFEditController::StampTool);
        QTRY_VERIFY_WITH_TIMEOUT(!popup->property("visible").toBool(), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(!loader->property("active").toBool(), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(!loader->property("item").value<QObject *>(), 1000);
        QVERIFY(!itemNamed(QStringLiteral("stampPresetMenu")));
        QVERIFY(!controller.hasDrafts());
        controller.setTool(PDFEditController::CommentSelectTool);
    }
    QVERIFY(root->setProperty("allowWindowClose", true));
}

void PDFEditControllerTest::savesStampPresetWithUndoRedo_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<QString>("text");
    QTest::addColumn<QString>("category");

    const auto addPreset = [](const char *name,
                              const char *text,
                              const char *category) {
        QTest::newRow(name) << QString::fromLatin1(name)
                            << QString::fromLatin1(text)
                            << QString::fromLatin1(category);
    };
    addPreset("DynamicApproved", "APPROVED", "dynamic");
    addPreset("DynamicRevised", "REVISED", "dynamic");
    addPreset("DynamicReviewed", "REVIEWED", "dynamic");
    addPreset("DynamicReceived", "RECEIVED", "dynamic");
    addPreset("DynamicConfidential", "CONFIDENTIAL", "dynamic");
    addPreset("Approved", "APPROVED", "standard");
    addPreset("Void", "VOID", "standard");
    addPreset("Verified", "VERIFIED", "standard");
    addPreset("Revised", "REVISED", "standard");
    addPreset("Reviewed", "REVIEWED", "standard");
    addPreset("Received", "RECEIVED", "standard");
    addPreset("Final", "FINAL", "standard");
    addPreset("Expired", "EXPIRED", "standard");
    addPreset("Emergency", "EMERGENCY", "standard");
    addPreset("Draft", "DRAFT", "standard");
    addPreset("Confidential", "CONFIDENTIAL", "standard");
    addPreset("Completed", "COMPLETED", "standard");
    addPreset("Accepted", "ACCEPTED", "signHere");
    addPreset("Witness", "WITNESS", "signHere");
    addPreset("SignHere", "SIGN HERE", "signHere");
    addPreset("Rejected", "REJECTED", "signHere");
    addPreset("Initial", "INITIAL", "signHere");
    addPreset("ForComment", "FOR COMMENT", "additional");
    addPreset("NotApproved", "NOT APPROVED", "additional");
}

void PDFEditControllerTest::savesStampPresetWithUndoRedo()
{
    QFETCH(QString, name);
    QFETCH(QString, text);
    QFETCH(QString, category);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-stamp-%1-source.pdf").arg(name));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-stamp-%1-output.pdf").arg(name));
    QVERIFY(createThreePageTextPdf(sourcePath));
    const int sourceObjectCount = topLevelPageObjectCount(sourcePath);
    QVERIFY(sourceObjectCount > 0);
    const QString originalPageText = pageText(sourcePath);
    QVERIFY(!originalPageText.isEmpty());

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantList presets = controller.stampPresets();
    QCOMPARE(presets.size(), 24);
    QSet<QString> presetNames;
    QVariantMap matchingPreset;
    for (const QVariant &entry : presets) {
        const QVariantMap preset = entry.toMap();
        const QString presetName = preset.value(QStringLiteral("name")).toString();
        QVERIFY(!presetName.isEmpty());
        QVERIFY(!presetNames.contains(presetName));
        presetNames.insert(presetName);
        if (presetName == name) {
            matchingPreset = preset;
        }
    }
    QVERIFY(!matchingPreset.isEmpty());
    QCOMPARE(matchingPreset.value(QStringLiteral("text")).toString(), text);
    QCOMPARE(matchingPreset.value(QStringLiteral("category")).toString(), category);
    controller.setStampName(name);
    QCOMPARE(controller.stampName(), name);
    QCOMPARE(controller.stampText(), text);
    QCOMPARE(controller.stampColor(),
             matchingPreset.value(QStringLiteral("color")).value<QColor>());
    QVERIFY(controller.stampColor().isValid());
    QVERIFY(!controller.stampArtwork().isEmpty());
    QCOMPARE(controller.stampArtwork(),
             matchingPreset.value(QStringLiteral("artwork")).toString());
    controller.setStampName(QStringLiteral("UnknownStamp"));
    QCOMPARE(controller.stampName(), name);
    controller.setTool(PDFEditController::StampTool);

    const QRectF placement(0.16, 0.18, 0.34, 0.12);
    const QVariantMap addedStamp = controller.addStamp(0, placement);
    QVERIFY(!addedStamp.isEmpty());
    QCOMPARE(
        addedStamp.value(QStringLiteral("kind")).toString(),
        QStringLiteral("stamp"));
    QCOMPARE(
        addedStamp.value(QStringLiteral("stampName")).toString(),
        name);
    QCOMPARE(
        addedStamp.value(QStringLiteral("stampText")).toString(),
        text);
    QCOMPARE(
        addedStamp.value(QStringLiteral("text")).toString(),
        text);
    const QColor stampColor = addedStamp
        .value(QStringLiteral("stampColor"))
        .value<QColor>();
    QVERIFY(stampColor.isValid());
    const QString stampDetail = addedStamp
        .value(QStringLiteral("stampDetail"))
        .toString();
    QCOMPARE(stampDetail.isEmpty(), category != QLatin1String("dynamic"));
    QVERIFY(!addedStamp.value(QStringLiteral("stampArtwork")).toString().isEmpty());
    QVERIFY(addedStamp.value(QStringLiteral("editable")).toBool());
    QCOMPARE(controller.tool(), PDFEditController::CommentSelectTool);
    QCOMPARE(controller.selectedPage(), 0);
    const QString objectId = addedStamp
        .value(QStringLiteral("objectId"))
        .toString();
    QVERIFY(!objectId.isEmpty());
    QCOMPARE(controller.selectedObjectId(), objectId);
    QVERIFY(controller.selectedEditable());
    QVERIFY(controller.selectedDeletable());
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QCOMPARE(controller.commentItems().size(), 1);

    QCOMPARE(controller.undo(), -1);
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(controller.commentItems().isEmpty());
    QCOMPARE(controller.redo(), -1);
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QCOMPARE(controller.commentItems().size(), 1);
    const QVariantMap redoneStamp = controller.commentItems()
        .constFirst()
        .toMap();
    QCOMPARE(
        redoneStamp.value(QStringLiteral("objectId")).toString(),
        objectId);
    QCOMPARE(
        redoneStamp.value(QStringLiteral("stampName")).toString(),
        name);
    QCOMPARE(redoneStamp.value(QStringLiteral("stampDetail")).toString(),
             stampDetail);

    const QRectF resizedPlacement(0.20, 0.22, 0.38, 0.15);
    QVERIFY(controller.selectCandidate(0, objectId));
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(resizedPlacement);
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), resizedPlacement);
    QCOMPARE(
        controller.draftsForPage(0)
            .constFirst()
            .toMap()
            .value(QStringLiteral("frameRect"))
            .toRectF(),
        resizedPlacement);
    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.selectedFrameRect(), placement);
    QCOMPARE(controller.redo(), -1);
    QCOMPARE(controller.selectedFrameRect(), resizedPlacement);

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(document.source().toLocalFile(), outputPath);
    QCOMPARE(topLevelPageObjectCount(outputPath), sourceObjectCount);
    QCOMPARE(pageText(outputPath), originalPageText);

    const CommentAnnotationInspection inspection
        = inspectCommentAnnotation(outputPath, objectId);
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.annotationCount, 1);
    QCOMPARE(inspection.subtype, FPDF_ANNOT_STAMP);
    QCOMPARE(inspection.appearanceName, name);
    QCOMPARE(inspection.contents, text);
    QVERIFY(inspection.flags & FPDF_ANNOT_FLAG_PRINT);
    QVERIFY(inspection.hasAppearance);
    QVERIFY(!inspection.appearance.isEmpty());
    const QByteArray dictionary = annotationDictionaryBytes(
        outputPath,
        objectId);
    QVERIFY(!dictionary.isEmpty());
    QVERIFY(QRegularExpression(QStringLiteral(R"(/Name\s*/%1\b)").arg(name))
        .match(QString::fromLatin1(dictionary))
        .hasMatch());

    const QVariantMap rehydratedStamp = candidateForKind(
        waitForCandidates(controller, 0),
        QStringLiteral("stamp"));
    QVERIFY(!rehydratedStamp.isEmpty());
    QCOMPARE(
        rehydratedStamp.value(QStringLiteral("stampName")).toString(),
        name);
    QCOMPARE(
        rehydratedStamp.value(QStringLiteral("stampText")).toString(),
        text);
    QCOMPARE(
        rehydratedStamp.value(QStringLiteral("stampColor")).value<QColor>(),
        stampColor);
    QCOMPARE(rehydratedStamp.value(QStringLiteral("stampDetail")).toString(),
             stampDetail);
    QCOMPARE(rehydratedStamp.value(QStringLiteral("stampArtwork")).toString(),
             addedStamp.value(QStringLiteral("stampArtwork")).toString());
    QCOMPARE(
        rehydratedStamp.value(QStringLiteral("text")).toString(),
        text);
    const QRectF rehydratedFrame = rehydratedStamp
        .value(QStringLiteral("frameRect"))
        .toRectF();
    QVERIFY(qAbs(rehydratedFrame.x() - resizedPlacement.x()) < 0.001);
    QVERIFY(qAbs(rehydratedFrame.y() - resizedPlacement.y()) < 0.001);
    QVERIFY(qAbs(rehydratedFrame.width() - resizedPlacement.width()) < 0.001);
    QVERIFY(qAbs(rehydratedFrame.height() - resizedPlacement.height()) < 0.001);
    QVERIFY(rehydratedStamp.value(QStringLiteral("editable")).toBool());
    QVERIFY(controller.selectComment(
        0,
        rehydratedStamp.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.selectedEditable());

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);
    const QVariantMap indexedStamp = controller.commentItems()
        .constFirst()
        .toMap();
    QCOMPARE(
        indexedStamp.value(QStringLiteral("kind")).toString(),
        QStringLiteral("stamp"));
    QCOMPARE(
        indexedStamp.value(QStringLiteral("stampName")).toString(),
        name);
    QCOMPARE(
        indexedStamp.value(QStringLiteral("stampText")).toString(),
        text);
    QCOMPARE(
        indexedStamp.value(QStringLiteral("stampColor")).value<QColor>(),
        stampColor);
    QCOMPARE(indexedStamp.value(QStringLiteral("stampDetail")).toString(),
             stampDetail);
    QVERIFY(indexedStamp
        .value(QStringLiteral("commentTextEditable"))
        .toBool());
}

void PDFEditControllerTest::preservesDynamicStampSnapshotAcrossEditsAndSave()
{
    const bool userWasSet = qEnvironmentVariableIsSet("USER");
    const QByteArray originalUser = qgetenv("USER");
    const auto restoreUser = qScopeGuard([userWasSet, originalUser]() {
        if (userWasSet) {
            qputenv("USER", originalUser);
        } else {
            qunsetenv("USER");
        }
    });
    const QString author = QStringLiteral("Nguyễn Văn Huy");
    QVERIFY(qputenv("USER", author.toUtf8()));

    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("dynamic-stamp-source.pdf"));
    const QString firstSavedPath = m_temporaryDirectory.filePath(
        QStringLiteral("dynamic-stamp-first-save.pdf"));
    const QString secondSavedPath = m_temporaryDirectory.filePath(
        QStringLiteral("dynamic-stamp-second-save.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));
    const int sourceObjectCount = topLevelPageObjectCount(sourcePath);
    const QString originalPageText = pageText(sourcePath);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setStampName(QStringLiteral("DynamicApproved"));
    controller.setTool(PDFEditController::StampTool);
    const QVariantMap added = controller.addStamp(0, QRectF(0.12, 0.24, 0.7, 0.16));
    QVERIFY(!added.isEmpty());
    const QString objectId = added.value(QStringLiteral("objectId")).toString();
    const QString detail = added.value(QStringLiteral("stampDetail")).toString();
    const QString artwork = added.value(QStringLiteral("stampArtwork")).toString();
    const QDateTime creationDate = added.value(QStringLiteral("commentCreationDate"))
        .toDateTime();
    QVERIFY(!objectId.isEmpty());
    QVERIFY(!detail.isEmpty());
    QVERIFY(detail.contains(author, Qt::CaseInsensitive));
    QVERIFY(!artwork.isEmpty());
    QVERIFY(creationDate.isValid());
    QVERIFY(detail.contains(creationDate.toString(QStringLiteral("dd/MM/yy"))));
    QVERIFY(detail.contains(creationDate.toString(QStringLiteral("HH:mm:ss"))));
    QCOMPARE(added.value(QStringLiteral("commentAuthor")).toString(), author);

    // The reviewer and selected preset may change after placement. Neither may
    // replace the author/date or title already captured by this annotation.
    QVERIFY(qputenv("USER", "different-reviewer"));
    controller.setStampName(QStringLiteral("DynamicReceived"));
    const QString editedContents = QStringLiteral("Đã duyệt bản hợp đồng mới.");
    QVERIFY(controller.setCommentText(0, objectId, editedContents));
    const auto currentDraft = [&controller]() {
        return controller.draftsForPage(0).constFirst().toMap();
    };
    QCOMPARE(currentDraft().value(QStringLiteral("text")).toString(), editedContents);
    QCOMPARE(currentDraft().value(QStringLiteral("stampText")).toString(),
             QStringLiteral("APPROVED"));
    QCOMPARE(currentDraft().value(QStringLiteral("stampDetail")).toString(), detail);
    QCOMPARE(currentDraft().value(QStringLiteral("stampArtwork")).toString(), artwork);
    QCOMPARE(controller.undo(), -1);
    QCOMPARE(currentDraft().value(QStringLiteral("text")).toString(),
             QStringLiteral("APPROVED"));
    QCOMPARE(currentDraft().value(QStringLiteral("stampDetail")).toString(), detail);
    QCOMPARE(controller.redo(), -1);
    QCOMPARE(currentDraft().value(QStringLiteral("text")).toString(), editedContents);
    QCOMPARE(currentDraft().value(QStringLiteral("stampDetail")).toString(), detail);
    QCOMPARE(currentDraft().value(QStringLiteral("stampArtwork")).toString(), artwork);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(firstSavedPath)));
    QVERIFY(!controller.hasDrafts());

    const CommentAnnotationInspection firstInspection
        = inspectCommentAnnotation(firstSavedPath, objectId);
    QVERIFY(firstInspection.readable);
    QCOMPARE(firstInspection.appearanceName, QStringLiteral("DynamicApproved"));
    QCOMPARE(firstInspection.author, author);
    QCOMPARE(firstInspection.contents, editedContents);
    QVERIFY(!firstInspection.creationDate.isEmpty());
    QVERIFY(!firstInspection.appearance.isEmpty());
    const AnnotationAppearanceStructureInspection firstAppearance
        = inspectAnnotationAppearanceStructure(firstSavedPath, objectId);
    QVERIFY(!firstAppearance.boundingBox.isEmpty());

    const QVariantMap reopened = candidateForKind(
        waitForCandidates(controller, 0), QStringLiteral("stamp"));
    QVERIFY(!reopened.isEmpty());
    QCOMPARE(reopened.value(QStringLiteral("stampName")).toString(),
             QStringLiteral("DynamicApproved"));
    QCOMPARE(reopened.value(QStringLiteral("stampText")).toString(),
             QStringLiteral("APPROVED"));
    QCOMPARE(reopened.value(QStringLiteral("stampDetail")).toString(), detail);
    QCOMPARE(reopened.value(QStringLiteral("stampArtwork")).toString(), artwork);
    QCOMPARE(reopened.value(QStringLiteral("commentAuthor")).toString(), author);
    QCOMPARE(reopened.value(QStringLiteral("commentCreationDate")).toDateTime()
                 .toSecsSinceEpoch(),
             creationDate.toSecsSinceEpoch());
    const QString sourceObjectId = reopened.value(QStringLiteral("objectId")).toString();
    QVERIFY(controller.selectComment(0, sourceObjectId));
    const QRectF originalFrame = controller.selectedFrameRect();
    const QRectF resizedFrame(0.2, 0.4, 0.6, 0.2);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(resizedFrame);
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), resizedFrame);
    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.selectedFrameRect(), originalFrame);
    QCOMPARE(controller.redo(), -1);
    QCOMPARE(controller.selectedFrameRect(), resizedFrame);
    const QString secondContents = QStringLiteral("Ghi chú bổ sung sau khi mở lại.");
    QVERIFY(controller.setCommentText(0, sourceObjectId, secondContents));
    QCOMPARE(currentDraft().value(QStringLiteral("stampDetail")).toString(), detail);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(secondSavedPath)));
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(topLevelPageObjectCount(secondSavedPath), sourceObjectCount);
    QCOMPARE(pageText(secondSavedPath), originalPageText);

    const CommentAnnotationInspection secondInspection
        = inspectCommentAnnotation(secondSavedPath, objectId);
    QVERIFY(secondInspection.readable);
    QCOMPARE(secondInspection.contents, secondContents);
    QCOMPARE(secondInspection.author, author);
    QCOMPARE(secondInspection.creationDate, firstInspection.creationDate);
    QCOMPARE(secondInspection.appearance, firstInspection.appearance);
    const AnnotationAppearanceStructureInspection secondAppearance
        = inspectAnnotationAppearanceStructure(secondSavedPath, objectId);
    QCOMPARE(secondAppearance.boundingBox, firstAppearance.boundingBox);
    const QSizeF pageSize = document.pageSize(0);
    QVERIFY(qAbs(secondInspection.rect.left
                 - resizedFrame.left() * pageSize.width()) < 0.05);
    QVERIFY(qAbs(secondInspection.rect.right
                 - resizedFrame.right() * pageSize.width()) < 0.05);
    QVERIFY(qAbs(secondInspection.rect.top
                 - (1.0 - resizedFrame.top()) * pageSize.height()) < 0.05);
    QVERIFY(qAbs(secondInspection.rect.bottom
                 - (1.0 - resizedFrame.bottom()) * pageSize.height()) < 0.05);
    const QVariantMap savedAgain = candidateForKind(
        waitForCandidates(controller, 0), QStringLiteral("stamp"));
    QCOMPARE(savedAgain.value(QStringLiteral("stampDetail")).toString(), detail);
    QCOMPARE(savedAgain.value(QStringLiteral("stampArtwork")).toString(), artwork);
    QCOMPARE(savedAgain.value(QStringLiteral("text")).toString(), secondContents);
}

void PDFEditControllerTest::editsAndDeletesSourceStampThreadRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("source-stamp-thread.pdf"));
    const QString editedPath = m_temporaryDirectory.filePath(
        QStringLiteral("source-stamp-thread-edited.pdf"));
    const QString deletedPath = m_temporaryDirectory.filePath(
        QStringLiteral("source-stamp-thread-deleted.pdf"));
    QVERIFY(createUnsupportedMarkupCommentsPdf(sourcePath));
    const int sourceObjectCount = topLevelPageObjectCount(sourcePath);
    QVERIFY(sourceObjectCount >= 0);

    const QString sourceName = QStringLiteral("unsupported-5");
    const CommentAnnotationInspection originalInspection
        = inspectCommentAnnotation(sourcePath, sourceName);
    QVERIFY(originalInspection.readable);
    QCOMPARE(originalInspection.subtype, FPDF_ANNOT_STAMP);
    QCOMPARE(originalInspection.appearanceName, QStringLiteral("Draft"));
    QCOMPARE(
        originalInspection.contents,
        QStringLiteral("unsupported-stamp"));
    QVERIFY(originalInspection.hasAppearance);
    QVERIFY(!originalInspection.appearance.isEmpty());

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 12, 5000);

    QVariantMap sourceStamp;
    for (const QVariant &commentValue : controller.commentItems()) {
        const QVariantMap comment = commentValue.toMap();
        if (comment.value(QStringLiteral("kind")).toString()
                == QLatin1String("stamp")) {
            sourceStamp = comment;
            break;
        }
    }
    QVERIFY(!sourceStamp.isEmpty());
    QVERIFY(sourceStamp.value(QStringLiteral("stampArtwork")).toString().isEmpty());
    QCOMPARE(
        sourceStamp.value(QStringLiteral("text")).toString(),
        QStringLiteral("unsupported-stamp"));
    QCOMPARE(
        sourceStamp.value(QStringLiteral("stampName")).toString(),
        QStringLiteral("Draft"));
    QCOMPARE(
        sourceStamp.value(QStringLiteral("stampText")).toString(),
        QStringLiteral("DRAFT"));
    QVERIFY(sourceStamp
        .value(QStringLiteral("stampColor"))
        .value<QColor>()
        .isValid());
    QVERIFY(sourceStamp.value(QStringLiteral("editable")).toBool());
    QVERIFY(sourceStamp.value(QStringLiteral("deletable")).toBool());
    QVERIFY(sourceStamp
        .value(QStringLiteral("commentTextEditable"))
        .toBool());
    const QString objectId = sourceStamp
        .value(QStringLiteral("objectId"))
        .toString();
    const QRectF sourceFrame = sourceStamp
        .value(QStringLiteral("frameRect"))
        .toRectF();
    const QRectF savedFrame(0.25, 0.25, 0.45, 0.12);
    QVERIFY(!objectId.isEmpty());
    QVERIFY(controller.selectComment(0, objectId));
    QVERIFY(controller.selectedEditable());
    QVERIFY(controller.selectedDeletable());

    {
        const QString themePath = QFINDTESTDATA(
            "../src/Components/AppTheme.qml");
        const QString overlayPath = QFINDTESTDATA(
            "../src/Components/EditOverlay.qml");
        QVERIFY(!themePath.isEmpty());
        QVERIFY(!overlayPath.isEmpty());
        QVERIFY(registerQmlTestTypes(themePath));

        QQmlEngine engine;
        QQmlComponent themeComponent(&engine);
        themeComponent.setData(
            QByteArrayLiteral("import TrePDF\nAppTheme {}"),
            QUrl());
        std::unique_ptr<QObject> theme(themeComponent.create());
        QVERIFY2(theme, qPrintable(themeComponent.errorString()));

        QQmlComponent overlayComponent(
            &engine,
            QUrl::fromLocalFile(overlayPath));
        const QVariantMap overlayProperties{
            {QStringLiteral("page"), 0},
            {QStringLiteral("pooled"), false},
            {QStringLiteral("draftTextureCurrent"), true},
            {QStringLiteral("draftTextureReady"), true},
            {QStringLiteral("displayedSuppressedCommentObjectIds"),
             QStringList()},
            {QStringLiteral("theme"), QVariant::fromValue(theme.get())},
            {QStringLiteral("controller"), QVariant::fromValue(&controller)},
            {QStringLiteral("pagePointScale"), 1.0},
            {QStringLiteral("width"), 200.0},
            {QStringLiteral("height"), 200.0}};
        std::unique_ptr<QObject> overlay(
            overlayComponent.createWithInitialProperties(overlayProperties));
        QVERIFY2(overlay, qPrintable(overlayComponent.errorString()));

        const auto invokeOneArgument = [&overlay](
                                           const char *method,
                                           const QVariant &argument) {
            QVariant result;
            const bool invoked = QMetaObject::invokeMethod(
                overlay.get(),
                method,
                Qt::DirectConnection,
                Q_RETURN_ARG(QVariant, result),
                Q_ARG(QVariant, argument));
            return qMakePair(invoked, result);
        };
        for (const QString &geometryKind : {
                 QStringLiteral("stickyNote"),
                 QStringLiteral("freeText"),
                 QStringLiteral("stamp"),
                 QStringLiteral("highlight")}) {
            const auto movable = invokeOneArgument(
                "isMovableKind",
                geometryKind);
            QVERIFY(movable.first);
            QVERIFY(movable.second.toBool());
        }
        const auto compactHitExtent = invokeOneArgument(
            "resizeHandleHitExtent",
            18.0);
        QVERIFY(compactHitExtent.first);
        QCOMPARE(compactHitExtent.second.toReal(), 5.0);

        const auto resizeCursor = [&overlay](int horizontal, int vertical) {
            QVariant result;
            const QVariant horizontalValue = horizontal;
            const QVariant verticalValue = vertical;
            const bool invoked = QMetaObject::invokeMethod(
                overlay.get(),
                "resizeCursorForHandle",
                Qt::DirectConnection,
                Q_RETURN_ARG(QVariant, result),
                Q_ARG(QVariant, horizontalValue),
                Q_ARG(QVariant, verticalValue));
            return qMakePair(invoked, result);
        };
        auto cursor = resizeCursor(1, 0);
        QVERIFY(cursor.first);
        QCOMPARE(cursor.second.toInt(), static_cast<int>(Qt::SizeHorCursor));
        QVERIFY(overlay->setProperty("viewRotation", 90));
        cursor = resizeCursor(1, 0);
        QVERIFY(cursor.first);
        QCOMPARE(cursor.second.toInt(), static_cast<int>(Qt::SizeVerCursor));

        QVERIFY(QMetaObject::invokeMethod(
            overlay.get(),
            "startFrameResize",
            Qt::DirectConnection));
        QVERIFY(overlay->property("resizingFrame").toBool());
        QCOMPARE(
            overlay->property("frameResizePreview").toMap()
                .value(QStringLiteral("kind")).toString(),
            QStringLiteral("stamp"));
        QVERIFY(QMetaObject::invokeMethod(
            overlay.get(),
            "cancelFrameResize",
            Qt::DirectConnection));
        QVERIFY(!overlay->property("resizingFrame").toBool());
        QVERIFY(!controller.hasDrafts());

        QSignalSpy deleteCommentRequestSpy(
            overlay.get(),
            SIGNAL(deleteCommentRequested(int,QString)));
        QVERIFY(deleteCommentRequestSpy.isValid());

        QVariant deletedByKeyboardPath;
        QVERIFY(QMetaObject::invokeMethod(
            overlay.get(),
            "deleteSelectedNonText",
            Qt::DirectConnection,
            Q_RETURN_ARG(QVariant, deletedByKeyboardPath)));
        QVERIFY(deletedByKeyboardPath.toBool());
        QCOMPARE(deleteCommentRequestSpy.count(), 1);
        const QList<QVariant> deleteArguments
            = deleteCommentRequestSpy.takeFirst();
        QCOMPARE(deleteArguments.at(0).toInt(), 0);
        QCOMPARE(deleteArguments.at(1).toString(), objectId);
        QCOMPARE(controller.commentItems().size(), 12);
        QVERIFY(controller.removeComment(
            deleteArguments.at(0).toInt(),
            deleteArguments.at(1).toString()));
        QCOMPARE(controller.commentItems().size(), 11);
        QVERIFY(controller.selectedObjectId().isEmpty());
        QCOMPARE(controller.undo(), -1);
        QCOMPARE(controller.commentItems().size(), 12);
        QVERIFY(controller.selectComment(0, objectId));
    }

    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(savedFrame);
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), savedFrame);
    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.selectedFrameRect(), sourceFrame);
    QCOMPARE(controller.redo(), -1);
    QCOMPARE(controller.selectedFrameRect(), savedFrame);

    const QString editedContents = QStringLiteral(
        "Nội dung Stamp đã được chỉnh sửa.");
    const QString replyContents = QStringLiteral(
        "Phản hồi cho Stamp nguồn.");
    QVERIFY(controller.setCommentText(0, objectId, editedContents));
    const QVariantMap reply = controller.addCommentReply(
        0,
        objectId,
        replyContents);
    QVERIFY(!reply.isEmpty());
    const QString replyId = reply
        .value(QStringLiteral("objectId"))
        .toString();
    QVERIFY(!replyId.isEmpty());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(editedPath)));
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(topLevelPageObjectCount(editedPath), sourceObjectCount);

    const CommentAnnotationInspection sourceStillOriginal
        = inspectCommentAnnotation(sourcePath, sourceName);
    QCOMPARE(
        sourceStillOriginal.contents,
        QStringLiteral("unsupported-stamp"));
    QCOMPARE(
        sourceStillOriginal.appearance,
        originalInspection.appearance);
    const CommentAnnotationInspection editedInspection
        = inspectCommentAnnotation(editedPath, sourceName);
    QVERIFY(editedInspection.readable);
    QCOMPARE(editedInspection.subtype, FPDF_ANNOT_STAMP);
    QCOMPARE(editedInspection.appearanceName, QStringLiteral("Draft"));
    QCOMPARE(editedInspection.contents, editedContents);
    QCOMPARE(editedInspection.appearance, originalInspection.appearance);
    QVERIFY(qAbs(editedInspection.rect.left
                 - savedFrame.left() * 300.0) < 0.05);
    QVERIFY(qAbs(editedInspection.rect.right
                 - savedFrame.right() * 300.0) < 0.05);
    QVERIFY(qAbs(editedInspection.rect.top
                 - (1.0 - savedFrame.top()) * 600.0) < 0.05);
    QVERIFY(qAbs(editedInspection.rect.bottom
                 - (1.0 - savedFrame.bottom()) * 600.0) < 0.05);
    const QByteArray editedDictionary = annotationDictionaryBytes(
        editedPath,
        sourceName);
    QVERIFY(!editedDictionary.contains(
        "TrePDFSourceCommentGeometryId"));
    const AnnotationAppearanceStructureInspection originalAppearanceStructure
        = inspectAnnotationAppearanceStructure(sourcePath, sourceName);
    const AnnotationAppearanceStructureInspection editedAppearanceStructure
        = inspectAnnotationAppearanceStructure(editedPath, sourceName);
    QCOMPARE(
        editedAppearanceStructure.boundingBox,
        originalAppearanceStructure.boundingBox);
    const CommentAnnotationInspection replyInspection
        = inspectCommentAnnotation(editedPath, replyId);
    QVERIFY(replyInspection.readable);
    QCOMPARE(replyInspection.subtype, FPDF_ANNOT_TEXT);
    QCOMPARE(replyInspection.contents, replyContents);
    QCOMPARE(replyInspection.parentName, sourceName);
    QCOMPARE(replyInspection.relation, QStringLiteral("R"));

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 13, 5000);
    QVariantMap rehydratedStamp;
    for (const QVariant &commentValue : controller.commentItems()) {
        const QVariantMap comment = commentValue.toMap();
        if (!comment.value(QStringLiteral("isReply")).toBool()
            && comment.value(QStringLiteral("kind")).toString()
                == QLatin1String("stamp")) {
            rehydratedStamp = comment;
            break;
        }
    }
    QVERIFY(!rehydratedStamp.isEmpty());
    QCOMPARE(
        rehydratedStamp.value(QStringLiteral("stampName")).toString(),
        QStringLiteral("Draft"));
    QCOMPARE(
        rehydratedStamp.value(QStringLiteral("stampText")).toString(),
        QStringLiteral("DRAFT"));
    QCOMPARE(
        rehydratedStamp.value(QStringLiteral("text")).toString(),
        editedContents);
    QCOMPARE(
        rehydratedStamp.value(QStringLiteral("replyCount")).toInt(),
        1);
    const QRectF rehydratedFrame = rehydratedStamp
        .value(QStringLiteral("frameRect"))
        .toRectF();
    QVERIFY(qAbs(rehydratedFrame.x() - savedFrame.x()) < 0.001);
    QVERIFY(qAbs(rehydratedFrame.y() - savedFrame.y()) < 0.001);
    QVERIFY(qAbs(rehydratedFrame.width() - savedFrame.width()) < 0.001);
    QVERIFY(qAbs(rehydratedFrame.height() - savedFrame.height()) < 0.001);
    QVERIFY(rehydratedStamp.value(QStringLiteral("deletable")).toBool());

    QVERIFY(controller.selectComment(
        0,
        rehydratedStamp.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.selectedEditable());
    QVERIFY(controller.selectedDeletable());
    controller.deleteSelectedObject();
    QCOMPARE(controller.commentItems().size(), 11);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(deletedPath)));
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(topLevelPageObjectCount(deletedPath), sourceObjectCount);
    QCOMPARE(annotationSubtypeCount(sourcePath, FPDF_ANNOT_STAMP), 1);
    QCOMPARE(annotationSubtypeCount(editedPath, FPDF_ANNOT_STAMP), 1);
    QCOMPARE(annotationSubtypeCount(deletedPath, FPDF_ANNOT_STAMP), 0);
    QCOMPARE(
        inspectCommentAnnotation(deletedPath, sourceName).annotationIndex,
        -1);
    QCOMPARE(
        inspectCommentAnnotation(deletedPath, replyId).annotationIndex,
        -1);
}

void PDFEditControllerTest::savesStickyNoteRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-sticky-note-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-sticky-note-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::StickyNoteTool);
    const QSizeF pageSize = document.pageSize(0);
    const QPointF position(0.2, 0.3);
    const QString commentText = QStringLiteral(
        "Cần kiểm tra lại số liệu.");
    const QVariantMap addedNote = controller.addStickyNote(0, position);
    QVERIFY(!addedNote.isEmpty());
    const QString commentAuthor = addedNote
        .value(QStringLiteral("commentAuthor"))
        .toString();
    const QDateTime creationDate = addedNote
        .value(QStringLiteral("commentCreationDate"))
        .toDateTime();
    QVERIFY(!commentAuthor.isEmpty());
    QVERIFY(creationDate.isValid());
    QVERIFY(!addedNote
        .value(QStringLiteral("commentModifiedDate"))
        .toDateTime()
        .isValid());
    QCOMPARE(
        addedNote.value(QStringLiteral("commentDate")).toDateTime(),
        creationDate);
    controller.setSelectedCommentText(commentText);
    const QVariantMap editedNote = controller.commentItems()
        .constFirst()
        .toMap();
    const QDateTime modificationDate = editedNote
        .value(QStringLiteral("commentModifiedDate"))
        .toDateTime();
    QVERIFY(modificationDate.isValid());
    QVERIFY(modificationDate >= creationDate);
    QCOMPARE(
        editedNote.value(QStringLiteral("commentDate")).toDateTime(),
        modificationDate);

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(document.source().toLocalFile(), outputPath);

    const StickyNoteSaveInspection sourceInspection
        = inspectStickyNotes(sourcePath);
    QVERIFY(sourceInspection.readable);
    QCOMPARE(sourceInspection.count, 0);

    const StickyNoteSaveInspection inspection
        = inspectStickyNotes(outputPath);
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.count, 1);
    QVERIFY(inspection.flags & FPDF_ANNOT_FLAG_PRINT);
    QCOMPARE(inspection.contents, commentText);
    QCOMPARE(inspection.author, commentAuthor);
    QVERIFY(!inspection.creationDate.isEmpty());
    QVERIFY(!inspection.modificationDate.isEmpty());
    const CommentAnnotationInspection appearanceInspection
        = inspectCommentAnnotation(
            outputPath,
            addedNote.value(QStringLiteral("objectId")).toString());
    QVERIFY(appearanceInspection.readable);
    QVERIFY(appearanceInspection.hasAppearance);
    QVERIFY(!appearanceInspection.appearance.isEmpty());

    const qreal expectedLeft = position.x() * pageSize.width();
    const qreal expectedRight = expectedLeft + 24.0;
    const qreal expectedTop = (1.0 - position.y()) * pageSize.height();
    const qreal expectedBottom = expectedTop - 24.0;
    const auto closeEnough = [](qreal actual, qreal expected) {
        return qAbs(actual - expected) < 0.05;
    };
    QVERIFY(closeEnough(inspection.rect.left, expectedLeft));
    QVERIFY(closeEnough(inspection.rect.right, expectedRight));
    QVERIFY(closeEnough(inspection.rect.top, expectedTop));
    QVERIFY(closeEnough(inspection.rect.bottom, expectedBottom));

    const QSize renderSize(
        qRound(pageSize.width()),
        qRound(pageSize.height()));
    const QImage withoutAnnotations = renderFirstPage(
        outputPath,
        renderSize);
    const QImage withAnnotations = renderFirstPage(
        outputPath,
        renderSize,
        FPDF_ANNOT);
    const QRect notePixels(
        qRound(position.x() * renderSize.width()),
        qRound(position.y() * renderSize.height()),
        26,
        26);
    QVERIFY(changedPixelCount(
        withoutAnnotations,
        withAnnotations,
        notePixels) > 0);

    const QVariantMap rehydratedNote = candidateForKind(
        waitForCandidates(controller, 0),
        QStringLiteral("stickyNote"));
    QVERIFY(!rehydratedNote.isEmpty());
    QCOMPARE(
        rehydratedNote.value(QStringLiteral("highlightColor")).value<QColor>(),
        QColor(255, 235, 59));
    QCOMPARE(
        rehydratedNote.value(QStringLiteral("text")).toString(),
        commentText);
    QCOMPARE(
        rehydratedNote.value(QStringLiteral("commentAuthor")).toString(),
        commentAuthor);
    const QDateTime rehydratedCreationDate = rehydratedNote
        .value(QStringLiteral("commentCreationDate"))
        .toDateTime();
    const QDateTime rehydratedModificationDate = rehydratedNote
        .value(QStringLiteral("commentModifiedDate"))
        .toDateTime();
    QCOMPARE(
        rehydratedCreationDate.toSecsSinceEpoch(),
        creationDate.toSecsSinceEpoch());
    QCOMPARE(
        rehydratedModificationDate.toSecsSinceEpoch(),
        modificationDate.toSecsSinceEpoch());
    QVERIFY(rehydratedNote.value(QStringLiteral("editable")).toBool());
    QVERIFY(controller.selectComment(
        0,
        rehydratedNote.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.selectedEditable());
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);
    QVERIFY(controller.commentItems().constFirst().toMap()
        .value(QStringLiteral("commentTextEditable")).toBool());
    QCOMPARE(controller.commentItems().size(), 1);
}

void PDFEditControllerTest::savesAndEditsFreeTextRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-free-text-source.pdf"));
    const QString firstOutputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-free-text-first-output.pdf"));
    const QString secondOutputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-free-text-second-output.pdf"));
    QVERIFY(createDifferentPageSizesPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QSizeF pageSize = document.pageSize(0);
    const QRectF placement(0.12, 0.14, 0.28, 0.15);
    const QString firstText = QStringLiteral(
        "Tiếng Việt có dấu phải hiển thị đầy đủ và tự động xuống dòng "
        "bên trong hộp Free Text hẹp.");

    controller.setTool(PDFEditController::FreeTextTool);
    const QVariantMap added = controller.addFreeText(0, placement);
    QVERIFY(!added.isEmpty());
    const QString objectId = added
        .value(QStringLiteral("objectId"))
        .toString();
    const QString author = added
        .value(QStringLiteral("commentAuthor"))
        .toString();
    QVERIFY(!objectId.isEmpty());
    QVERIFY(!author.isEmpty());
    QVERIFY(controller.setCommentText(0, objectId, firstText));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(firstOutputPath)));
    QVERIFY(!controller.hasDrafts());

    const CommentAnnotationInspection firstInspection
        = inspectCommentAnnotation(firstOutputPath, objectId);
    QVERIFY(firstInspection.readable);
    QCOMPARE(firstInspection.annotationCount, 1);
    QCOMPARE(firstInspection.subtype, FPDF_ANNOT_FREETEXT);
    QCOMPARE(firstInspection.contents, firstText);
    QCOMPARE(firstInspection.author, author);
    QVERIFY(!firstInspection.creationDate.isEmpty());
    QVERIFY(!firstInspection.modificationDate.isEmpty());
    QVERIFY(firstInspection.flags & FPDF_ANNOT_FLAG_PRINT);
    QVERIFY(!firstInspection.defaultAppearance.isEmpty());
    QVERIFY(firstInspection.hasAppearance);

    const auto closeEnough = [](qreal actual, qreal expected) {
        return qAbs(actual - expected) < 0.05;
    };
    QVERIFY(closeEnough(
        firstInspection.rect.left,
        placement.left() * pageSize.width()));
    QVERIFY(closeEnough(
        firstInspection.rect.right,
        placement.right() * pageSize.width()));
    QVERIFY(closeEnough(
        firstInspection.rect.top,
        (1.0 - placement.top()) * pageSize.height()));
    QVERIFY(closeEnough(
        firstInspection.rect.bottom,
        (1.0 - placement.bottom()) * pageSize.height()));

    const QSize renderSize(
        qRound(pageSize.width()),
        qRound(pageSize.height()));
    const QImage sourceWithoutAnnotations = renderFirstPage(
        sourcePath,
        renderSize);
    const QImage firstWithoutAnnotations = renderFirstPage(
        firstOutputPath,
        renderSize);
    const QImage firstWithAnnotations = renderFirstPage(
        firstOutputPath,
        renderSize,
        FPDF_ANNOT);
    QCOMPARE(
        changedPixelCount(
            sourceWithoutAnnotations,
            firstWithoutAnnotations,
            sourceWithoutAnnotations.rect()),
        0);
    const QRect placementPixels(
        qRound(placement.x() * renderSize.width()),
        qRound(placement.y() * renderSize.height()),
        qRound(placement.width() * renderSize.width()),
        qRound(placement.height() * renderSize.height()));
    const QRect textInterior = placementPixels.adjusted(6, 6, -6, -6);
    const QRect firstTextBounds = darkChangedPixelBounds(
        firstWithoutAnnotations,
        firstWithAnnotations,
        textInterior);
    QVERIFY2(
        !firstTextBounds.isEmpty(),
        "The Free Text appearance did not render Vietnamese glyphs.");
    QVERIFY2(
        firstTextBounds.height() > 20,
        "The narrow Free Text appearance did not wrap onto multiple lines.");

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);
    const QVariantMap rehydrated = controller.commentItems()
        .constFirst()
        .toMap();
    QCOMPARE(
        rehydrated.value(QStringLiteral("kind")).toString(),
        QStringLiteral("freeText"));
    QCOMPARE(
        rehydrated.value(QStringLiteral("text")).toString(),
        firstText);
    QVERIFY(rehydrated
        .value(QStringLiteral("commentTextEditable"))
        .toBool());
    QVERIFY(rehydrated.value(QStringLiteral("editable")).toBool());
    const QString rehydratedObjectId = rehydrated
        .value(QStringLiteral("objectId"))
        .toString();
    QVERIFY(!rehydratedObjectId.isEmpty());
    const QRectF rehydratedSourceFrame = rehydrated
        .value(QStringLiteral("frameRect"))
        .toRectF();

    const QRectF secondPlacement(0.48, 0.18, 0.34, 0.20);
    QVERIFY(controller.selectComment(0, rehydratedObjectId));
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(secondPlacement);
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), secondPlacement);
    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.selectedFrameRect(), rehydratedSourceFrame);
    QCOMPARE(controller.redo(), -1);
    QCOMPARE(controller.selectedFrameRect(), secondPlacement);

    const QString secondText = QStringLiteral(
        "Nội dung Free Text đã được chỉnh sửa sau khi mở lại.");
    QVERIFY(controller.setCommentText(
        0,
        rehydratedObjectId,
        secondText));
    const QString replyText = QStringLiteral(
        "Phản hồi cho bình luận Free Text.");
    const QVariantMap reply = controller.addCommentReply(
        0,
        rehydratedObjectId,
        replyText);
    QVERIFY(!reply.isEmpty());
    const QString replyId = reply
        .value(QStringLiteral("objectId"))
        .toString();
    QVERIFY(!replyId.isEmpty());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(secondOutputPath)));
    QVERIFY(!controller.hasDrafts());

    const CommentAnnotationInspection preservedFirst
        = inspectCommentAnnotation(firstOutputPath, objectId);
    QCOMPARE(preservedFirst.contents, firstText);
    const CommentAnnotationInspection secondInspection
        = inspectCommentAnnotation(secondOutputPath, objectId);
    QVERIFY(secondInspection.readable);
    QCOMPARE(secondInspection.subtype, FPDF_ANNOT_FREETEXT);
    QCOMPARE(secondInspection.contents, secondText);
    QVERIFY(secondInspection.hasAppearance);
    QVERIFY(!secondInspection.modificationDate.isEmpty());
    QVERIFY(closeEnough(
        secondInspection.rect.left,
        secondPlacement.left() * pageSize.width()));
    QVERIFY(closeEnough(
        secondInspection.rect.right,
        secondPlacement.right() * pageSize.width()));
    QVERIFY(closeEnough(
        secondInspection.rect.top,
        (1.0 - secondPlacement.top()) * pageSize.height()));
    QVERIFY(closeEnough(
        secondInspection.rect.bottom,
        (1.0 - secondPlacement.bottom()) * pageSize.height()));
    const CommentAnnotationInspection replyInspection
        = inspectCommentAnnotation(secondOutputPath, replyId);
    QVERIFY(replyInspection.readable);
    QCOMPARE(replyInspection.subtype, FPDF_ANNOT_TEXT);
    QCOMPARE(replyInspection.contents, replyText);
    QCOMPARE(replyInspection.parentName, objectId);
    QCOMPARE(replyInspection.relation, QStringLiteral("R"));

    const QImage secondWithAnnotations = renderFirstPage(
        secondOutputPath,
        renderSize,
        FPDF_ANNOT);
    QVERIFY(
        changedPixelCount(
            firstWithAnnotations,
            secondWithAnnotations,
            placementPixels) > 0);
    const QRect secondPlacementPixels(
        qRound(secondPlacement.x() * renderSize.width()),
        qRound(secondPlacement.y() * renderSize.height()),
        qRound(secondPlacement.width() * renderSize.width()),
        qRound(secondPlacement.height() * renderSize.height()));
    QVERIFY(changedPixelCount(
        sourceWithoutAnnotations,
        secondWithAnnotations,
        secondPlacementPixels) > 0);

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 2, 5000);
    QVariantMap rehydratedRoot;
    for (const QVariant &commentValue : controller.commentItems()) {
        const QVariantMap comment = commentValue.toMap();
        if (comment.value(QStringLiteral("kind")).toString()
                == QLatin1String("freeText")
            && !comment.value(QStringLiteral("isReply")).toBool()) {
            rehydratedRoot = comment;
            break;
        }
    }
    QVERIFY(!rehydratedRoot.isEmpty());
    QCOMPARE(
        rehydratedRoot.value(QStringLiteral("text")).toString(),
        secondText);
    QVERIFY(rehydratedRoot.value(QStringLiteral("editable")).toBool());
    const QRectF rehydratedFrame = rehydratedRoot
        .value(QStringLiteral("frameRect"))
        .toRectF();
    QVERIFY(qAbs(rehydratedFrame.x() - secondPlacement.x()) < 0.001);
    QVERIFY(qAbs(rehydratedFrame.y() - secondPlacement.y()) < 0.001);
    QVERIFY(qAbs(rehydratedFrame.width() - secondPlacement.width()) < 0.001);
    QVERIFY(qAbs(rehydratedFrame.height() - secondPlacement.height()) < 0.001);
}

void PDFEditControllerTest::savesFreeTextWithPageLocalDuplicateNames()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("duplicate-free-text-names-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("duplicate-free-text-names-output.pdf"));
    QVERIFY(createPageLocalDuplicateFreeTextNamesPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 2, 5000);

    QVariantMap secondPageComment;
    for (const QVariant &commentValue : controller.commentItems()) {
        const QVariantMap comment = commentValue.toMap();
        QCOMPARE(
            comment.value(QStringLiteral("kind")).toString(),
            QStringLiteral("freeText"));
        if (comment.value(QStringLiteral("page")).toInt() == 1) {
            secondPageComment = comment;
        }
    }
    QVERIFY(!secondPageComment.isEmpty());
    const QString replacement = QStringLiteral(
        "Nội dung chỉ được thay trên trang thứ hai.");
    QVERIFY(controller.setCommentText(
        1,
        secondPageComment.value(QStringLiteral("objectId")).toString(),
        replacement));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));

    const CommentAnnotationInspection firstPage
        = inspectCommentAnnotation(
            outputPath,
            QStringLiteral("page-local-name"),
            0);
    const CommentAnnotationInspection secondPage
        = inspectCommentAnnotation(
            outputPath,
            QStringLiteral("page-local-name"),
            1);
    QVERIFY(firstPage.readable);
    QVERIFY(secondPage.readable);
    QCOMPARE(firstPage.contents, QStringLiteral("Page zero text"));
    QCOMPARE(secondPage.contents, replacement);
    QVERIFY(!firstPage.hasAppearance);
    QVERIFY(secondPage.hasAppearance);
}
