#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

using namespace PDFEditControllerTestSupport;

void PDFEditControllerTest::savesRehydratesAndInteractsWithDropdown()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("acroform-dropdown.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFFormDesignerController formDesigner;
    PDFFormController formController;
    PDFEditController editController;
    formDesigner.setDocument(&document);
    formController.setDocument(&document);
    formDesigner.setFormController(&formController);
    editController.setDocument(&document);
    editController.setFormController(&formController);
    editController.setFormDesignerController(&formDesigner);
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));

    const QStringList options({
        QStringLiteral("Tùy chọn 1"),
        QStringLiteral("Tùy chọn 2"),
        QStringLiteral("Tùy chọn 3"),
    });
    const QRectF initialRect(0.16, 0.22, 0.40, 0.06);
    const QString fieldId = formDesigner.addField(
        PDFFormDesignerController::Dropdown,
        0,
        initialRect.x(),
        initialRect.y(),
        initialRect.width(),
        initialRect.height());
    QVERIFY(fieldId.startsWith(QStringLiteral("dropdown-field-")));
    QVERIFY(formDesigner.setFieldName(
        fieldId,
        QStringLiteral("Transport")));
    QVERIFY(formDesigner.setFieldOptions(fieldId, options));
    QVERIFY(formDesigner.setFieldValue(
        fieldId,
        QStringLiteral("Tùy chọn 2")));
    QVERIFY(formDesigner.setFieldFlags(
        fieldId,
        PDFFormDesignerController::Required));

    const QVector<PDFFormDesignerController::SaveField> addedFields
        = formDesigner.saveFields();
    QCOMPARE(addedFields.size(), 1);
    QCOMPARE(addedFields.constFirst().type,
             PDFFormDesignerController::Dropdown);
    QCOMPARE(addedFields.constFirst().options, options);
    QCOMPARE(addedFields.constFirst().value,
             QStringLiteral("Tùy chọn 2"));
    QCOMPARE(addedFields.constFirst().defaultValue,
             QStringLiteral("Tùy chọn 1"));

    QSignalSpy saveFinishedSpy(
        &editController,
        &PDFEditController::saveFinished);
    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 10000);
    QVERIFY(saveFinishedSpy.constFirst().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);

    QVariantMap hydrated = formDesigner.fieldData(fieldId);
    QCOMPARE(hydrated.value(QStringLiteral("type")).toInt(),
             PDFFormDesignerController::Dropdown);
    QCOMPARE(hydrated.value(QStringLiteral("options")).toStringList(),
             options);
    QCOMPARE(hydrated.value(QStringLiteral("value")).toString(),
             QStringLiteral("Tùy chọn 2"));
    QVERIFY(hydrated.value(QStringLiteral("sourceBacked")).toBool());
    QVERIFY(!hydrated.value(QStringLiteral("dirty")).toBool());
    QVERIFY(formDesigner.sourceFieldIdsForPage(0).contains(fieldId));
    QVERIFY(formDesigner.sourceFieldContainsPoint(
        0,
        initialRect.center()));

    AcroFormFieldInspection inspection = inspectAcroFormField(
        sourcePath,
        QStringLiteral("Transport"));
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.formType, FORMTYPE_ACRO_FORM);
    QCOMPARE(inspection.widgetCount, 1);
    QCOMPARE(inspection.fieldType, FPDF_FORMFIELD_COMBOBOX);
    QVERIFY(inspection.fieldFlags & FPDF_FORMFLAG_CHOICE_COMBO);
    QVERIFY(inspection.fieldFlags & FPDF_FORMFLAG_REQUIRED);
    QVERIFY(!(inspection.fieldFlags & FPDF_FORMFLAG_CHOICE_EDIT));
    QVERIFY(!(inspection.fieldFlags & FPDF_FORMFLAG_CHOICE_MULTI_SELECT));
    QCOMPARE(inspection.name, QStringLiteral("Transport"));
    QCOMPARE(inspection.value, QStringLiteral("Tùy chọn 2"));
    QCOMPARE(inspection.defaultValue, QStringLiteral("Tùy chọn 1"));
    QCOMPARE(inspection.options, options);
    QCOMPARE(inspection.selectedIndices, QVector<int>({1}));
    QVERIFY(inspection.hasAppearance);
    QVERIFY(inspection.appearanceStream.contains(
        QStringLiteral("/Im0 Do")));
    QVERIFY(inspection.annotationFlags & FPDF_ANNOT_FLAG_PRINT);
    QVERIFY(inspection.hasAppearanceVersion);
    QCOMPARE(inspection.appearanceVersion,
             static_cast<float>(PDFFormAppearance::kDropdownVersion));
    QVERIFY(inspection.pointHit);

    QTRY_VERIFY_WITH_TIMEOUT(formController.javascriptEnabled(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !formController.javascriptTransitionPending(),
        5000);
    QVector<PDFFormWorker::ChoiceField> choiceFields;
    bool inspectionCompleted = false;
    formController.requestChoiceFields(
        0,
        [&choiceFields, &inspectionCompleted](
            QVector<PDFFormWorker::ChoiceField> fields) {
            choiceFields = std::move(fields);
            inspectionCompleted = true;
        });
    QTRY_VERIFY_WITH_TIMEOUT(inspectionCompleted, 5000);
    QCOMPARE(choiceFields.size(), 1);
    const PDFFormWorker::ChoiceField choice = choiceFields.constFirst();
    QCOMPARE(choice.fieldType, FPDF_FORMFIELD_COMBOBOX);
    QCOMPARE(choice.options, options);
    QCOMPARE(choice.selectedIndices, QVector<int>({1}));
    QVERIFY(!choice.readOnly);
    QVERIFY(!choice.multiSelect);
    QVERIFY(!choice.identityToken.isEmpty());
    QVERIFY(formDesigner.managesSourceChoiceField(
        choice.sourcePage,
        choice.annotationIndex,
        choice.identityToken));
    QVERIFY(!formDesigner.managesSourceChoiceField(
        choice.sourcePage,
        choice.annotationIndex,
        choice.identityToken + QStringLiteral("-stale")));
    QVERIFY(formDesigner.managesSourceWidget(
        0,
        choice.annotationIndex,
        fieldId));
    QVERIFY(!formDesigner.managesSourceWidget(
        0,
        choice.annotationIndex,
        fieldId + QStringLiteral("-stale")));
    QVERIFY(!formDesigner.managesSourceWidget(
        0,
        choice.annotationIndex + 1,
        fieldId));

    editController.setFormDesignerController(nullptr);
    const QVariantMap sourceWidgetCandidate = candidateForKind(
        waitForCandidates(editController, 0),
        QStringLiteral("widget"));
    QVERIFY(!sourceWidgetCandidate.isEmpty());
    QVERIFY(editController.selectCandidate(
        0,
        sourceWidgetCandidate.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(editController.selectedKind(), QStringLiteral("widget"));
    editController.setFormDesignerController(&formDesigner);
    QCOMPARE(editController.selectedPage(), -1);
    QVERIFY(editController.selectedObjectId().isEmpty());
    QVERIFY(candidateForKind(
        editController.candidatesForPage(0),
        QStringLiteral("widget")).isEmpty());
    const QVariantMap sourcePositionCandidate = editController.candidateAt(
        0,
        initialRect.center().x(),
        initialRect.center().y());
    QVERIFY(sourcePositionCandidate.value(QStringLiteral("kind")).toString()
            != QStringLiteral("widget"));

    PDFFormView formView;
    formView.setController(&formController);
    formView.setFormDesignerController(&formDesigner);
    formView.setPage(0);
    formView.setInteractive(true);
    QTRY_COMPARE_WITH_TIMEOUT(formView.choiceFields().size(), 1, 5000);
    formView.setFormDesignMode(true);
    QCOMPARE(formView.choiceFields().size(), 0);
    formView.setFormDesignMode(false);
    QCOMPARE(formView.choiceFields().size(), 1);

    bool focusCompleted = false;
    bool focusSucceeded = false;
    formController.focusChoiceField(
        0,
        choice.annotationIndex,
        choice.sourcePage,
        choice.identityToken,
        [&focusCompleted, &focusSucceeded](bool focused) {
            focusSucceeded = focused;
            focusCompleted = true;
        });
    QTRY_VERIFY_WITH_TIMEOUT(focusCompleted, 5000);
    QVERIFY(focusSucceeded);
    QVERIFY(formDesigner.activateSourceDropdown(fieldId, 2));
    QTRY_VERIFY_WITH_TIMEOUT(formController.hasDrafts(), 5000);
    QVERIFY(!formController.canUndo());
    QVERIFY(!formController.canRedo());

    choiceFields.clear();
    inspectionCompleted = false;
    formController.requestChoiceFields(
        0,
        [&choiceFields, &inspectionCompleted](
            QVector<PDFFormWorker::ChoiceField> fields) {
            choiceFields = std::move(fields);
            inspectionCompleted = true;
        });
    QTRY_VERIFY_WITH_TIMEOUT(inspectionCompleted, 5000);
    QCOMPARE(choiceFields.size(), 1);
    QCOMPARE(choiceFields.constFirst().selectedIndices,
             QVector<int>({2}));

    QTRY_COMPARE_WITH_TIMEOUT(
        formDesigner.fieldData(fieldId)
            .value(QStringLiteral("value"))
            .toString(),
        QStringLiteral("Tùy chọn 3"),
        5000);

    // Saving only the native form draft must replace PDFium's regenerated
    // text appearance with TrePDF's Unicode-safe raster appearance.
    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 2, 10000);
    QVERIFY(saveFinishedSpy.constLast().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 10000);
    inspection = inspectAcroFormField(
        sourcePath,
        QStringLiteral("Transport"));
    QCOMPARE(inspection.value, QStringLiteral("Tùy chọn 3"));
    QVERIFY(inspection.appearanceStream.contains(
        QStringLiteral("/Im0 Do")));
    QCOMPARE(inspection.appearanceVersion,
             static_cast<float>(PDFFormAppearance::kDropdownVersion));

    formDesigner.prepareForDesignMode();
    QTRY_COMPARE_WITH_TIMEOUT(
        formDesigner.fieldData(fieldId)
            .value(QStringLiteral("value"))
            .toString(),
        QStringLiteral("Tùy chọn 3"),
        5000);

    const QRectF resizedRect(0.22, 0.32, 0.46, 0.07);
    QVERIFY(formDesigner.setFieldRect(
        fieldId,
        resizedRect.x(),
        resizedRect.y(),
        resizedRect.width(),
        resizedRect.height()));
    const QVariantMap oldPositionCandidate = editController.candidateAt(
        0,
        initialRect.center().x(),
        initialRect.center().y());
    QVERIFY(oldPositionCandidate.value(QStringLiteral("kind")).toString()
            != QStringLiteral("widget"));
    const QVector<PDFFormDesignerController::SaveField> resizedFields
        = formDesigner.saveFields(true);
    QCOMPARE(resizedFields.size(), 1);
    QCOMPARE(resizedFields.constFirst().type,
             PDFFormDesignerController::Dropdown);
    QCOMPARE(resizedFields.constFirst().operation,
             PDFFormDesignerController::SaveField::Update);
    QCOMPARE(resizedFields.constFirst().changedProperties,
             static_cast<quint32>(
                 PDFFormDesignerController::RectPropertyChanged));
    QCOMPARE(resizedFields.constFirst().value,
             QStringLiteral("Tùy chọn 3"));
    QCOMPARE(resizedFields.constFirst().defaultValue,
             QStringLiteral("Tùy chọn 1"));
    QCOMPARE(resizedFields.constFirst().options, options);

    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 3, 10000);
    QVERIFY(saveFinishedSpy.constLast().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);

    hydrated = formDesigner.fieldData(fieldId);
    QCOMPARE(hydrated.value(QStringLiteral("type")).toInt(),
             PDFFormDesignerController::Dropdown);
    QCOMPARE(hydrated.value(QStringLiteral("value")).toString(),
             QStringLiteral("Tùy chọn 3"));
    QCOMPARE(hydrated.value(QStringLiteral("options")).toStringList(),
             options);
    QVERIFY(hydrated.value(QStringLiteral("sourceBacked")).toBool());
    QVERIFY(!hydrated.value(QStringLiteral("dirty")).toBool());

    QStringList editedOptions = options;
    editedOptions.append(QStringLiteral("Tùy chọn 4"));
    QVERIFY(formDesigner.setFieldOptions(fieldId, editedOptions));
    QVERIFY(formDesigner.setFieldValue(fieldId, editedOptions.constLast()));
    editedOptions.removeAt(1);
    QVERIFY(formDesigner.setFieldOptions(fieldId, editedOptions));
    editedOptions[0] = QStringLiteral("Tùy chọn một");
    QVERIFY(formDesigner.renameFieldOption(
        fieldId,
        0,
        editedOptions.at(0)));
    hydrated = formDesigner.fieldData(fieldId);
    QCOMPARE(hydrated.value(QStringLiteral("options")).toStringList(),
             editedOptions);
    QVERIFY(hydrated.value(QStringLiteral("optionsDirty")).toBool());

    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 4, 10000);
    QVERIFY(saveFinishedSpy.constLast().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);

    hydrated = formDesigner.fieldData(fieldId);
    QCOMPARE(hydrated.value(QStringLiteral("value")).toString(),
             editedOptions.constLast());
    QCOMPARE(hydrated.value(QStringLiteral("options")).toStringList(),
             editedOptions);
    QVERIFY(!hydrated.value(QStringLiteral("optionsDirty")).toBool());
    QVERIFY(!formDesigner.removeField(
        fieldId + QStringLiteral("-missing")));

    inspection = inspectAcroFormField(
        sourcePath,
        QStringLiteral("Transport"));
    QCOMPARE(inspection.widgetCount, 1);
    QCOMPARE(inspection.fieldType, FPDF_FORMFIELD_COMBOBOX);
    QCOMPARE(inspection.value, editedOptions.constLast());
    QCOMPARE(inspection.defaultValue, editedOptions.constFirst());
    QCOMPARE(inspection.options, editedOptions);
    QCOMPARE(inspection.selectedIndices, QVector<int>({2}));
    QVERIFY(inspection.hasAppearance);
    QFile savedDropdown(sourcePath);
    QVERIFY(savedDropdown.open(QIODevice::ReadOnly));
    const QByteArray savedBytes = savedDropdown.readAll();
    const QByteArray unicodeFontMarker
        = QByteArrayLiteral("/")
        + QByteArray(PDFFormAppearance::kFontVersionKey)
        + ' '
        + QByteArray::number(PDFFormAppearance::kFontVersion);
    QCOMPARE(savedBytes.count(unicodeFontMarker), 1);
    QVERIFY(savedBytes.contains("/Type0"));
    QVERIFY(savedBytes.contains("/CIDToGIDMap"));
    QVERIFY(qAbs(inspection.rect.left - 130.9) < 1.0);
    QVERIFY(qAbs(inspection.rect.right - 404.6) < 1.0);
    QVERIFY(qAbs(inspection.rect.top - 572.56) < 1.0);
    QVERIFY(qAbs(inspection.rect.bottom - 513.62) < 1.0);
    QVERIFY(formDesigner.sourceFieldContainsPoint(
        0,
        resizedRect.center()));
    QVERIFY(!formDesigner.sourceFieldContainsPoint(
        0,
        initialRect.topLeft()));
}

void PDFEditControllerTest::savesRehydratesAndInteractsWithListBox()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("acroform-list-box.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFFormDesignerController formDesigner;
    PDFFormController formController;
    PDFEditController editController;
    formDesigner.setDocument(&document);
    formController.setDocument(&document);
    formDesigner.setFormController(&formController);
    editController.setDocument(&document);
    editController.setFormController(&formController);
    editController.setFormDesignerController(&formDesigner);
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));

    const QStringList options({
        QStringLiteral("Hà Nội"),
        QStringLiteral("Đà Nẵng"),
        QStringLiteral("TP. Hồ Chí Minh"),
        QStringLiteral("Cần Thơ"),
        QStringLiteral("Hải Phòng"),
    });
    const QStringList initialSelection({
        options.at(1),
        options.at(3),
    });
    const QRectF fieldRect(0.14, 0.18, 0.46, 0.18);
    const QString fieldId = formDesigner.addField(
        PDFFormDesignerController::ListBox,
        0,
        fieldRect.x(),
        fieldRect.y(),
        fieldRect.width(),
        fieldRect.height());
    QVERIFY(fieldId.startsWith(QStringLiteral("listbox-field-")));
    const QVariantMap initialField = formDesigner.fieldData(fieldId);
    QCOMPARE(initialField.value(QStringLiteral("options")).toStringList().size(),
             5);
    QCOMPARE(initialField.value(QStringLiteral("selectedValues"))
                 .toStringList().size(),
             1);
    QVERIFY(formDesigner.setFieldName(
        fieldId,
        QStringLiteral("OfficeLocation")));
    QVERIFY(formDesigner.setFieldOptions(fieldId, options));
    QVERIFY(formDesigner.setFieldSelectedValues(
        fieldId,
        initialSelection));
    QVERIFY(formDesigner.setFieldFlags(
        fieldId,
        PDFFormDesignerController::Required));

    const QVector<PDFFormDesignerController::SaveField> addedFields
        = formDesigner.saveFields();
    QCOMPARE(addedFields.size(), 1);
    QCOMPARE(addedFields.constFirst().type,
             PDFFormDesignerController::ListBox);
    QCOMPARE(addedFields.constFirst().options, options);
    QCOMPARE(addedFields.constFirst().value, options.at(1));
    QCOMPARE(addedFields.constFirst().selectedValues, initialSelection);
    QCOMPARE(addedFields.constFirst().defaultValue, options.constFirst());

    QSignalSpy saveFinishedSpy(
        &editController,
        &PDFEditController::saveFinished);
    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 10000);
    QVERIFY(saveFinishedSpy.constFirst().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);

    QVariantMap hydrated = formDesigner.fieldData(fieldId);
    QCOMPARE(hydrated.value(QStringLiteral("type")).toInt(),
             PDFFormDesignerController::ListBox);
    QCOMPARE(hydrated.value(QStringLiteral("options")).toStringList(),
             options);
    QCOMPARE(hydrated.value(QStringLiteral("value")).toString(),
             options.at(1));
    QCOMPARE(hydrated.value(QStringLiteral("selectedValues")).toStringList(),
             initialSelection);
    QVERIFY(hydrated.value(QStringLiteral("sourceBacked")).toBool());
    QVERIFY(!hydrated.value(QStringLiteral("dirty")).toBool());

    AcroFormFieldInspection inspection = inspectAcroFormField(
        sourcePath,
        QStringLiteral("OfficeLocation"));
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.formType, FORMTYPE_ACRO_FORM);
    QCOMPARE(inspection.widgetCount, 1);
    QCOMPARE(inspection.fieldType, FPDF_FORMFIELD_LISTBOX);
    QVERIFY(inspection.fieldFlags & FPDF_FORMFLAG_REQUIRED);
    QVERIFY(!(inspection.fieldFlags & FPDF_FORMFLAG_CHOICE_COMBO));
    QVERIFY(!(inspection.fieldFlags & FPDF_FORMFLAG_CHOICE_EDIT));
    QVERIFY(inspection.fieldFlags & FPDF_FORMFLAG_CHOICE_MULTI_SELECT);
    QCOMPARE(inspection.defaultValue, options.constFirst());
    QCOMPARE(inspection.options, options);
    QCOMPARE(inspection.selectedIndices, QVector<int>({1, 3}));
    QVERIFY(inspection.annotationFlags & FPDF_ANNOT_FLAG_PRINT);
    QVERIFY(inspection.hasAppearance);
    QVERIFY(inspection.appearanceStream.contains(
        QStringLiteral("/Im0 Do")));
    QVERIFY(inspection.hasAppearanceVersion);
    QCOMPARE(inspection.appearanceVersion,
             static_cast<float>(PDFFormAppearance::kListBoxVersion));

    QTRY_VERIFY_WITH_TIMEOUT(formController.javascriptEnabled(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !formController.javascriptTransitionPending(),
        5000);
    QVector<PDFFormWorker::ChoiceField> choiceFields;
    bool inspectionCompleted = false;
    formController.requestChoiceFields(
        0,
        [&choiceFields, &inspectionCompleted](
            QVector<PDFFormWorker::ChoiceField> fields) {
            choiceFields = std::move(fields);
            inspectionCompleted = true;
        });
    QTRY_VERIFY_WITH_TIMEOUT(inspectionCompleted, 5000);
    QCOMPARE(choiceFields.size(), 1);
    const PDFFormWorker::ChoiceField choice = choiceFields.constFirst();
    QCOMPARE(choice.fieldType, FPDF_FORMFIELD_LISTBOX);
    QCOMPARE(choice.options, options);
    QCOMPARE(choice.selectedIndices, QVector<int>({1, 3}));
    QVERIFY(!choice.readOnly);
    QVERIFY(choice.multiSelect);
    QVERIFY(!choice.identityToken.isEmpty());
    QVERIFY(formDesigner.managesSourceChoiceField(
        choice.sourcePage,
        choice.annotationIndex,
        choice.identityToken));

    QVERIFY(formDesigner.toggleSourceChoiceOption(fieldId, 0));
    QVERIFY(formDesigner.toggleSourceChoiceOption(fieldId, 1));
    QTRY_VERIFY_WITH_TIMEOUT(formController.hasDrafts(), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(
        formDesigner.fieldData(fieldId)
            .value(QStringLiteral("selectedValues"))
            .toStringList(),
        QStringList({options.at(0), options.at(3)}),
        5000);

    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 2, 10000);
    QVERIFY(saveFinishedSpy.constLast().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 10000);
    inspection = inspectAcroFormField(
        sourcePath,
        QStringLiteral("OfficeLocation"));
    QCOMPARE(inspection.fieldType, FPDF_FORMFIELD_LISTBOX);
    QVERIFY(inspection.fieldFlags & FPDF_FORMFLAG_CHOICE_MULTI_SELECT);
    QCOMPARE(inspection.selectedIndices, QVector<int>({0, 3}));
    QCOMPARE(inspection.appearanceVersion,
             static_cast<float>(PDFFormAppearance::kListBoxVersion));

    QVERIFY(formDesigner.toggleSourceChoiceOption(fieldId, 0));
    QVERIFY(formDesigner.toggleSourceChoiceOption(fieldId, 3));
    QTRY_COMPARE_WITH_TIMEOUT(
        formDesigner.fieldData(fieldId)
            .value(QStringLiteral("selectedValues"))
            .toStringList(),
        QStringList(),
        5000);
    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 3, 10000);
    QVERIFY(saveFinishedSpy.constLast().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 10000);
    inspection = inspectAcroFormField(
        sourcePath,
        QStringLiteral("OfficeLocation"));
    QCOMPARE(inspection.fieldType, FPDF_FORMFIELD_LISTBOX);
    QCOMPARE(inspection.selectedIndices, QVector<int>());

    QVERIFY(formDesigner.removeField(fieldId));
    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 4, 10000);
    QVERIFY(saveFinishedSpy.constLast().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 0, 10000);
    inspection = inspectAcroFormField(
        sourcePath,
        QStringLiteral("OfficeLocation"));
    QCOMPARE(inspection.widgetCount, 0);
    QCOMPARE(inspection.fieldType, FPDF_FORMFIELD_UNKNOWN);
}

void PDFEditControllerTest::migratesLegacyListBoxWhenAddingMultiSelectListBox()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("acroform-legacy-list-box-migration.pdf"));
    QVERIFY(createLegacyTrePDFListBoxPdf(sourcePath));

    const QString legacyFieldId = QString::fromLatin1(
        kLegacyListBoxWidgetId);
    const QString legacyFieldName = QString::fromLatin1(
        kLegacyListBoxFieldName);
    const QStringList legacyOptions({
        QStringLiteral("Alpha"),
        QStringLiteral("Beta"),
        QStringLiteral("Gamma"),
    });
    const QStringList legacySelection({legacyOptions.at(1)});
    const QStringList addedOptions({
        QStringLiteral("North"),
        QStringLiteral("South"),
        QStringLiteral("East"),
        QStringLiteral("West"),
        QStringLiteral("Central"),
        QStringLiteral("Remote"),
    });
    const QStringList addedSelection({
        addedOptions.at(1),
        addedOptions.at(4),
    });
    const QString addedFieldName = QStringLiteral("NewMultiOfficeList");
    QString addedFieldId;

    AcroFormFieldInspection legacyInspection = inspectAcroFormField(
        sourcePath,
        legacyFieldName);
    QVERIFY(legacyInspection.readable);
    QCOMPARE(legacyInspection.widgetCount, 1);
    QCOMPARE(legacyInspection.fieldType, FPDF_FORMFIELD_LISTBOX);
    QVERIFY(!(legacyInspection.fieldFlags
              & FPDF_FORMFLAG_CHOICE_COMBO));
    QVERIFY(!(legacyInspection.fieldFlags
              & FPDF_FORMFLAG_CHOICE_EDIT));
    QVERIFY(!(legacyInspection.fieldFlags
              & FPDF_FORMFLAG_CHOICE_MULTI_SELECT));
    QCOMPARE(legacyInspection.options, legacyOptions);
    QCOMPARE(legacyInspection.selectedIndices, QVector<int>({1}));
    QVERIFY(legacyInspection.hasAppearanceVersion);
    QCOMPARE(legacyInspection.appearanceVersion, 1.0f);

    {
        PDFDocument document;
        PDFFormDesignerController formDesigner;
        PDFFormController formController;
        PDFEditController editController;
        formDesigner.setDocument(&document);
        formController.setDocument(&document);
        formDesigner.setFormController(&formController);
        editController.setDocument(&document);
        editController.setFormController(&formController);
        editController.setFormDesignerController(&formDesigner);
        QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));

        QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 10000);
        const QVariantMap legacyField = formDesigner.fieldData(
            legacyFieldId);
        QCOMPARE(legacyField.value(QStringLiteral("type")).toInt(),
                 PDFFormDesignerController::ListBox);
        QCOMPARE(legacyField.value(QStringLiteral("options")).toStringList(),
                 legacyOptions);
        QCOMPARE(legacyField.value(QStringLiteral("selectedValues"))
                     .toStringList(),
                 legacySelection);
        QCOMPARE(legacyField.value(QStringLiteral("value")).toString(),
                 legacySelection.constFirst());
        QVERIFY(legacyField.value(QStringLiteral("sourceBacked")).toBool());
        QVERIFY(!legacyField.value(QStringLiteral("dirty")).toBool());

        const QRectF addedRect(0.14, 0.42, 0.46, 0.18);
        addedFieldId = formDesigner.addField(
            PDFFormDesignerController::ListBox,
            0,
            addedRect.x(),
            addedRect.y(),
            addedRect.width(),
            addedRect.height());
        QVERIFY(!addedFieldId.isEmpty());
        QVERIFY(formDesigner.setFieldName(
            addedFieldId,
            addedFieldName));
        QVERIFY(formDesigner.setFieldOptions(
            addedFieldId,
            addedOptions));
        QVERIFY(formDesigner.setFieldSelectedValues(
            addedFieldId,
            addedSelection));

        QSignalSpy saveFinishedSpy(
            &editController,
            &PDFEditController::saveFinished);
        QVERIFY(editController.requestSave());
        QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 10000);
        QVERIFY(saveFinishedSpy.constFirst().constFirst().toBool());
        QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 2, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);
    }

    {
        PDFDocument document;
        PDFFormDesignerController formDesigner;
        PDFFormController formController;
        formDesigner.setDocument(&document);
        formController.setDocument(&document);
        formDesigner.setFormController(&formController);
        QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));

        QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 2, 10000);
        const QVariantMap legacyField = formDesigner.fieldData(
            legacyFieldId);
        QCOMPARE(legacyField.value(QStringLiteral("type")).toInt(),
                 PDFFormDesignerController::ListBox);
        QCOMPARE(legacyField.value(QStringLiteral("selectedValues"))
                     .toStringList(),
                 legacySelection);
        QCOMPARE(legacyField.value(QStringLiteral("value")).toString(),
                 legacySelection.constFirst());
        QVERIFY(legacyField.value(QStringLiteral("sourceBacked")).toBool());

        const QVariantMap addedField = formDesigner.fieldData(
            addedFieldId);
        QCOMPARE(addedField.value(QStringLiteral("type")).toInt(),
                 PDFFormDesignerController::ListBox);
        QCOMPARE(addedField.value(QStringLiteral("options")).toStringList(),
                 addedOptions);
        QCOMPARE(addedField.value(QStringLiteral("selectedValues"))
                     .toStringList(),
                 addedSelection);
        QCOMPARE(addedField.value(QStringLiteral("value")).toString(),
                 addedSelection.constFirst());
        QVERIFY(addedField.value(QStringLiteral("sourceBacked")).toBool());
    }

    legacyInspection = inspectAcroFormField(
        sourcePath,
        legacyFieldName);
    QVERIFY(legacyInspection.readable);
    QCOMPARE(legacyInspection.widgetCount, 2);
    QCOMPARE(legacyInspection.widgetId, legacyFieldId);
    QCOMPARE(legacyInspection.fieldType, FPDF_FORMFIELD_LISTBOX);
    QVERIFY(legacyInspection.fieldFlags
            & FPDF_FORMFLAG_CHOICE_MULTI_SELECT);
    QVERIFY(!(legacyInspection.fieldFlags
              & FPDF_FORMFLAG_CHOICE_COMBO));
    QVERIFY(!(legacyInspection.fieldFlags
              & FPDF_FORMFLAG_CHOICE_EDIT));
    QCOMPARE(legacyInspection.options, legacyOptions);
    QCOMPARE(legacyInspection.value, legacySelection.constFirst());
    QCOMPARE(legacyInspection.selectedIndices, QVector<int>({1}));
    QVERIFY(legacyInspection.hasAppearance);
    QVERIFY(legacyInspection.hasAppearanceVersion);
    QCOMPARE(legacyInspection.appearanceVersion,
             static_cast<float>(PDFFormAppearance::kListBoxVersion));

    const AcroFormFieldInspection addedInspection = inspectAcroFormField(
        sourcePath,
        addedFieldName);
    QVERIFY(addedInspection.readable);
    QCOMPARE(addedInspection.widgetCount, 2);
    QCOMPARE(addedInspection.widgetId, addedFieldId);
    QCOMPARE(addedInspection.fieldType, FPDF_FORMFIELD_LISTBOX);
    QVERIFY(addedInspection.fieldFlags
            & FPDF_FORMFLAG_CHOICE_MULTI_SELECT);
    QVERIFY(!(addedInspection.fieldFlags
              & FPDF_FORMFLAG_CHOICE_COMBO));
    QVERIFY(!(addedInspection.fieldFlags
              & FPDF_FORMFLAG_CHOICE_EDIT));
    QCOMPARE(addedInspection.options, addedOptions);
    QCOMPARE(addedInspection.value, addedSelection.constFirst());
    QCOMPARE(addedInspection.selectedIndices, QVector<int>({1, 4}));
    QVERIFY(addedInspection.hasAppearance);
    QVERIFY(addedInspection.hasAppearanceVersion);
    QCOMPARE(addedInspection.appearanceVersion,
             static_cast<float>(PDFFormAppearance::kListBoxVersion));
}

void PDFEditControllerTest::deletesRehydratedDropdownWithoutChangingPeer()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("acroform-dropdown-delete.pdf"));
    QVERIFY(createDropdownDeleteRegressionPdf(sourcePath));

    PDFDocument document;
    PDFFormDesignerController formDesigner;
    PDFFormController formController;
    PDFEditController editController;
    formDesigner.setDocument(&document);
    formController.setDocument(&document);
    formDesigner.setFormController(&formController);
    editController.setDocument(&document);
    editController.setFormController(&formController);
    editController.setFormDesignerController(&formDesigner);
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));

    const QStringList removedOptions({
        QStringLiteral("Bus"),
        QStringLiteral("Train"),
        QStringLiteral("Plane"),
    });
    const QStringList peerOptions({
        QStringLiteral("Hanoi"),
        QStringLiteral("Da Nang"),
        QStringLiteral("Ho Chi Minh City"),
    });
    const QRectF removedRect(0.12, 0.18, 0.34, 0.06);
    const QRectF peerRect(0.54, 0.38, 0.36, 0.07);
    const QString removedFieldId = formDesigner.addField(
        PDFFormDesignerController::Dropdown,
        0,
        removedRect.x(),
        removedRect.y(),
        removedRect.width(),
        removedRect.height());
    const QString peerFieldId = formDesigner.addField(
        PDFFormDesignerController::Dropdown,
        0,
        peerRect.x(),
        peerRect.y(),
        peerRect.width(),
        peerRect.height());
    QVERIFY(removedFieldId.startsWith(
        QStringLiteral("dropdown-field-")));
    QVERIFY(peerFieldId.startsWith(QStringLiteral("dropdown-field-")));
    QVERIFY(formDesigner.setFieldName(
        removedFieldId,
        QStringLiteral("TransportToDelete")));
    QVERIFY(formDesigner.setFieldOptions(
        removedFieldId,
        removedOptions));
    QVERIFY(formDesigner.setFieldValue(
        removedFieldId,
        QStringLiteral("Train")));
    QVERIFY(formDesigner.setFieldName(
        peerFieldId,
        QStringLiteral("DestinationToKeep")));
    QVERIFY(formDesigner.setFieldOptions(peerFieldId, peerOptions));
    QVERIFY(formDesigner.setFieldValue(
        peerFieldId,
        QStringLiteral("Da Nang")));

    QSignalSpy saveFinishedSpy(
        &editController,
        &PDFEditController::saveFinished);
    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 10000);
    QVERIFY(saveFinishedSpy.constFirst().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 2, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);
    QFile savedFixture(sourcePath);
    QVERIFY(savedFixture.open(QIODevice::ReadOnly));
    QByteArray savedFixtureBytes = savedFixture.readAll();
    QVERIFY(savedFixtureBytes.contains(
        QByteArrayLiteral("/TrePDFRegression 4 0 R")));
    QVERIFY(savedFixtureBytes.contains(QByteArrayLiteral("(<<)")));

    const QVariantMap rehydratedRemoved
        = formDesigner.fieldData(removedFieldId);
    const QVariantMap rehydratedPeer
        = formDesigner.fieldData(peerFieldId);
    QVERIFY(rehydratedRemoved.value(
        QStringLiteral("sourceBacked")).toBool());
    QVERIFY(rehydratedPeer.value(
        QStringLiteral("sourceBacked")).toBool());

    QVector<PDFFormWorker::ChoiceField> choiceFields;
    bool choiceInspectionCompleted = false;
    formController.requestChoiceFields(
        0,
        [&choiceFields, &choiceInspectionCompleted](
            QVector<PDFFormWorker::ChoiceField> fields) {
            choiceFields = std::move(fields);
            choiceInspectionCompleted = true;
        });
    QTRY_VERIFY_WITH_TIMEOUT(choiceInspectionCompleted, 5000);
    QCOMPARE(choiceFields.size(), 2);
    const auto removedChoice = std::find_if(
        choiceFields.cbegin(),
        choiceFields.cend(),
        [&removedRect](const PDFFormWorker::ChoiceField &choice) {
            return qAbs(choice.normalizedRect.x() - removedRect.x())
                    < 0.01
                && qAbs(choice.normalizedRect.y() - removedRect.y())
                    < 0.01;
        });
    QVERIFY(removedChoice != choiceFields.cend());
    const int removedAnnotationIndex = removedChoice->annotationIndex;
    const int removedSourcePage = removedChoice->sourcePage;
    const QString removedIdentityToken = removedChoice->identityToken;
    QVERIFY(!removedIdentityToken.isEmpty());

    QVERIFY(formDesigner.removeField(removedFieldId));
    QCOMPARE(formDesigner.rowCount(), 1);
    QVERIFY(formDesigner.hasDrafts());
    QVERIFY(formDesigner.fieldData(removedFieldId).isEmpty());
    const QVariantList visibleFields = formDesigner.fieldsForPage(0);
    QCOMPARE(visibleFields.size(), 1);
    QCOMPARE(visibleFields.constFirst().toMap()
                 .value(QStringLiteral("id"))
                 .toString(),
             peerFieldId);

    const QStringList suppressedFieldIds
        = formDesigner.sourceFieldIdsForPage(0);
    QCOMPARE(suppressedFieldIds.size(), 2);
    QVERIFY(suppressedFieldIds.contains(removedFieldId));
    QVERIFY(suppressedFieldIds.contains(peerFieldId));
    QVERIFY(formDesigner.managesSourceChoiceField(
        removedSourcePage,
        removedAnnotationIndex,
        removedIdentityToken));
    QVERIFY(formDesigner.managesSourceWidget(
        0,
        removedAnnotationIndex,
        removedFieldId));
    QVERIFY(formDesigner.sourceFieldContainsPoint(
        0,
        removedRect.center()));
    QVERIFY(!formDesigner.sourceFieldContainsPoint(
        0,
        QPointF(0.98, 0.98)));

    const QVector<PDFFormDesignerController::SaveField> removedFields
        = formDesigner.saveFields();
    QCOMPARE(removedFields.size(), 1);
    QCOMPARE(removedFields.constFirst().id, removedFieldId);
    QCOMPARE(removedFields.constFirst().type,
             PDFFormDesignerController::Dropdown);
    QCOMPARE(removedFields.constFirst().operation,
             PDFFormDesignerController::SaveField::Remove);
    QCOMPARE(removedFields.constFirst().page, 0);

    editController.discardDrafts();
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 2, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        formDesigner.fieldData(removedFieldId)
            .value(QStringLiteral("sourceBacked"))
            .toBool(),
        5000);
    QCOMPARE(inspectAcroFormField(
                 sourcePath,
                 QStringLiteral("TransportToDelete"))
                 .fieldType,
             FPDF_FORMFIELD_COMBOBOX);

    const QStringList addedOptions({
        QStringLiteral("Standard"),
        QStringLiteral("Express"),
        QStringLiteral("Overnight"),
    });
    const QRectF addedRect(0.20, 0.64, 0.42, 0.06);
    const QString addedFieldId = formDesigner.addField(
        PDFFormDesignerController::Dropdown,
        0,
        addedRect.x(),
        addedRect.y(),
        addedRect.width(),
        addedRect.height());
    QVERIFY(addedFieldId.startsWith(QStringLiteral("dropdown-field-")));
    QVERIFY(formDesigner.setFieldName(
        addedFieldId,
        QStringLiteral("DeliveryToAdd")));
    QVERIFY(formDesigner.setFieldOptions(addedFieldId, addedOptions));
    QVERIFY(formDesigner.setFieldValue(
        addedFieldId,
        QStringLiteral("Overnight")));
    QVERIFY(formDesigner.removeField(removedFieldId));
    const QVector<PDFFormDesignerController::SaveField> mixedFields
        = formDesigner.saveFields();
    QCOMPARE(mixedFields.size(), 2);
    const auto addedSnapshot = std::find_if(
        mixedFields.cbegin(),
        mixedFields.cend(),
        [](const PDFFormDesignerController::SaveField &field) {
            return field.operation
                == PDFFormDesignerController::SaveField::Add;
        });
    const auto removedSnapshot = std::find_if(
        mixedFields.cbegin(),
        mixedFields.cend(),
        [](const PDFFormDesignerController::SaveField &field) {
            return field.operation
                == PDFFormDesignerController::SaveField::Remove;
        });
    QVERIFY(addedSnapshot != mixedFields.cend());
    QVERIFY(removedSnapshot != mixedFields.cend());
    QCOMPARE(addedSnapshot->id, addedFieldId);
    QCOMPARE(addedSnapshot->name, QStringLiteral("DeliveryToAdd"));
    QCOMPARE(addedSnapshot->options, addedOptions);
    QCOMPARE(addedSnapshot->value, QStringLiteral("Overnight"));
    QCOMPARE(removedSnapshot->id, removedFieldId);
    QCOMPARE(removedSnapshot->operation,
             PDFFormDesignerController::SaveField::Remove);
    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 2, 10000);
    QVERIFY(saveFinishedSpy.constLast().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 2, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);
    QVERIFY(formDesigner.fieldData(removedFieldId).isEmpty());
    savedFixture.close();
    QVERIFY(savedFixture.open(QIODevice::ReadOnly));
    savedFixtureBytes = savedFixture.readAll();
    QVERIFY(savedFixtureBytes.contains(
        QByteArrayLiteral("/TrePDFRegression 4 0 R")));
    QVERIFY(savedFixtureBytes.contains(QByteArrayLiteral("(<<)")));

    const QStringList remainingSourceFieldIds
        = formDesigner.sourceFieldIdsForPage(0);
    QCOMPARE(remainingSourceFieldIds.size(), 2);
    QVERIFY(!remainingSourceFieldIds.contains(removedFieldId));
    QVERIFY(remainingSourceFieldIds.contains(peerFieldId));
    QVERIFY(remainingSourceFieldIds.contains(addedFieldId));
    const QVariantMap remainingPeer
        = formDesigner.fieldData(peerFieldId);
    QVERIFY(remainingPeer.value(
        QStringLiteral("sourceBacked")).toBool());
    QCOMPARE(remainingPeer.value(QStringLiteral("value")).toString(),
             QStringLiteral("Da Nang"));
    QCOMPARE(remainingPeer.value(QStringLiteral("options")).toStringList(),
             peerOptions);
    const QVariantMap remainingAdded
        = formDesigner.fieldData(addedFieldId);
    QVERIFY(remainingAdded.value(
        QStringLiteral("sourceBacked")).toBool());
    QCOMPARE(remainingAdded.value(QStringLiteral("value")).toString(),
             QStringLiteral("Overnight"));
    QCOMPARE(remainingAdded.value(QStringLiteral("options")).toStringList(),
             addedOptions);

    const AcroFormFieldInspection deletedInspection
        = inspectAcroFormField(
            sourcePath,
            QStringLiteral("TransportToDelete"));
    QVERIFY(deletedInspection.readable);
    QCOMPARE(deletedInspection.widgetCount, 2);
    QCOMPARE(deletedInspection.fieldType, FPDF_FORMFIELD_UNKNOWN);
    QVERIFY(deletedInspection.name.isEmpty());

    const AcroFormFieldInspection peerInspection
        = inspectAcroFormField(
            sourcePath,
            QStringLiteral("DestinationToKeep"));
    QVERIFY(peerInspection.readable);
    QCOMPARE(peerInspection.formType, FORMTYPE_ACRO_FORM);
    QCOMPARE(peerInspection.widgetCount, 2);
    QCOMPARE(peerInspection.fieldType, FPDF_FORMFIELD_COMBOBOX);
    QCOMPARE(peerInspection.name,
             QStringLiteral("DestinationToKeep"));
    QCOMPARE(peerInspection.value, QStringLiteral("Da Nang"));
    QCOMPARE(peerInspection.defaultValue, peerOptions.constFirst());
    QCOMPARE(peerInspection.options, peerOptions);
    QCOMPARE(peerInspection.selectedIndices, QVector<int>({1}));
    QVERIFY(peerInspection.hasAppearance);
    QVERIFY(qAbs(peerInspection.rect.left - 321.3) < 1.0);
    QVERIFY(qAbs(peerInspection.rect.right - 535.5) < 1.0);
    QVERIFY(qAbs(peerInspection.rect.top - 521.66) < 1.0);
    QVERIFY(qAbs(peerInspection.rect.bottom - 462.72) < 1.0);

    const AcroFormFieldInspection addedInspection
        = inspectAcroFormField(
            sourcePath,
            QStringLiteral("DeliveryToAdd"));
    QVERIFY(addedInspection.readable);
    QCOMPARE(addedInspection.widgetCount, 2);
    QCOMPARE(addedInspection.fieldType, FPDF_FORMFIELD_COMBOBOX);
    QCOMPARE(addedInspection.name, QStringLiteral("DeliveryToAdd"));
    QCOMPARE(addedInspection.value, QStringLiteral("Overnight"));
    QCOMPARE(addedInspection.defaultValue, addedOptions.constFirst());
    QCOMPARE(addedInspection.options, addedOptions);
    QCOMPARE(addedInspection.selectedIndices, QVector<int>({2}));
    QVERIFY(addedInspection.hasAppearance);
    QVERIFY(qAbs(addedInspection.rect.left - 119.0) < 1.0);
    QVERIFY(qAbs(addedInspection.rect.right - 368.9) < 1.0);
    QVERIFY(qAbs(addedInspection.rect.top - 303.12) < 1.0);
    QVERIFY(qAbs(addedInspection.rect.bottom - 252.6) < 1.0);
}

void PDFEditControllerTest::deletesDropdownWhenItsSourcePageIsDeleted()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("dropdown-delete-with-source-page.pdf"));
    QVERIFY(createDifferentPageSizesPdf(sourcePath));

    PDFDocument document;
    PDFFormDesignerController formDesigner;
    PDFFormController formController;
    PDFEditController editController;
    formDesigner.setDocument(&document);
    formController.setDocument(&document);
    formDesigner.setFormController(&formController);
    editController.setDocument(&document);
    editController.setFormController(&formController);
    editController.setFormDesignerController(&formDesigner);
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    QCOMPARE(document.pageCount(), 2);

    const QString fieldName = QStringLiteral("ReusablePageZeroDropdown");
    const QStringList initialOptions({
        QStringLiteral("Alpha"),
        QStringLiteral("Beta"),
        QStringLiteral("Gamma"),
    });
    const QRectF initialRect(0.16, 0.20, 0.42, 0.07);
    const QString removedFieldId = formDesigner.addField(
        PDFFormDesignerController::Dropdown,
        0,
        initialRect.x(),
        initialRect.y(),
        initialRect.width(),
        initialRect.height());
    QVERIFY(!removedFieldId.isEmpty());
    QVERIFY(formDesigner.setFieldName(removedFieldId, fieldName));
    QVERIFY(formDesigner.setFieldOptions(
        removedFieldId,
        initialOptions));
    QVERIFY(formDesigner.setFieldValue(
        removedFieldId,
        QStringLiteral("Beta")));

    QSignalSpy saveFinishedSpy(
        &editController,
        &PDFEditController::saveFinished);
    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 10000);
    QVERIFY(saveFinishedSpy.constFirst().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);
    QVERIFY(formDesigner.fieldData(removedFieldId)
                .value(QStringLiteral("sourceBacked"))
                .toBool());

    QVERIFY(formDesigner.removeField(removedFieldId));
    QCOMPARE(formDesigner.rowCount(), 0);
    document.setCurrentPage(0);
    QCOMPARE(editController.deleteCurrentPage(), 0);
    QCOMPARE(document.pageCount(), 1);
    QCOMPARE(document.sourcePageIndex(0), 1);
    QCOMPARE(document.displayPageIndexForSourcePage(0), -1);

    const QVector<PDFFormDesignerController::SaveField> removedFields
        = formDesigner.saveFields();
    QCOMPARE(removedFields.size(), 1);
    QCOMPARE(removedFields.constFirst().id, removedFieldId);
    QCOMPARE(removedFields.constFirst().operation,
             PDFFormDesignerController::SaveField::Remove);
    QCOMPARE(removedFields.constFirst().page, -1);
    QVERIFY(editController.hasDrafts());

    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 2, 10000);
    QVERIFY(saveFinishedSpy.constLast().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(document.pageCount(), 1, 10000);
    QCOMPARE(document.sourcePageCount(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 0, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!editController.hasDrafts(), 10000);
    QVERIFY(formDesigner.fieldData(removedFieldId).isEmpty());
    QVERIFY(formDesigner.sourceFieldIdsForPage(0).isEmpty());

    AcroFormFieldInspection inspection = inspectAcroFormField(
        sourcePath,
        fieldName);
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.widgetCount, 0);
    QCOMPARE(inspection.fieldType, FPDF_FORMFIELD_UNKNOWN);

    const QStringList replacementOptions({
        QStringLiteral("One"),
        QStringLiteral("Two"),
        QStringLiteral("Three"),
    });
    const QRectF replacementRect(0.22, 0.34, 0.52, 0.09);
    const QString replacementFieldId = formDesigner.addField(
        PDFFormDesignerController::Dropdown,
        0,
        replacementRect.x(),
        replacementRect.y(),
        replacementRect.width(),
        replacementRect.height());
    QVERIFY(!replacementFieldId.isEmpty());
    QVERIFY(formDesigner.setFieldName(replacementFieldId, fieldName));
    QVERIFY(formDesigner.setFieldOptions(
        replacementFieldId,
        replacementOptions));
    QVERIFY(formDesigner.setFieldValue(
        replacementFieldId,
        QStringLiteral("Three")));

    const QVector<PDFFormDesignerController::SaveField> replacementFields
        = formDesigner.saveFields();
    QCOMPARE(replacementFields.size(), 1);
    QCOMPARE(replacementFields.constFirst().id, replacementFieldId);
    QCOMPARE(replacementFields.constFirst().operation,
             PDFFormDesignerController::SaveField::Add);
    QCOMPARE(replacementFields.constFirst().name, fieldName);

    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 3, 10000);
    QVERIFY(saveFinishedSpy.constLast().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!editController.hasDrafts(), 10000);

    const QVariantMap replacement
        = formDesigner.fieldData(replacementFieldId);
    QVERIFY(replacement.value(
        QStringLiteral("sourceBacked")).toBool());
    QCOMPARE(replacement.value(QStringLiteral("name")).toString(),
             fieldName);
    QCOMPARE(replacement.value(QStringLiteral("options")).toStringList(),
             replacementOptions);
    QCOMPARE(replacement.value(QStringLiteral("value")).toString(),
             QStringLiteral("Three"));

    inspection = inspectAcroFormField(sourcePath, fieldName);
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.widgetCount, 1);
    QCOMPARE(inspection.fieldType, FPDF_FORMFIELD_COMBOBOX);
    QCOMPARE(inspection.name, fieldName);
    QCOMPARE(inspection.options, replacementOptions);
    QCOMPARE(inspection.value, QStringLiteral("Three"));
    QCOMPARE(inspection.selectedIndices, QVector<int>({2}));
}

void PDFEditControllerTest::rejectsDropdownExportDisplayPairs()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("dropdown-export-display-pairs.pdf"));
    const QString savedPath = m_temporaryDirectory.filePath(
        QStringLiteral("dropdown-export-display-pairs-saved.pdf"));
    QVERIFY(createTrePDFDropdownWithExportValuesPdf(sourcePath));

    PDFDocument document;
    PDFFormDesignerController formDesigner;
    PDFFormController formController;
    PDFEditController editController;
    formDesigner.setDocument(&document);
    formController.setDocument(&document);
    formDesigner.setFormController(&formController);
    editController.setDocument(&document);
    editController.setFormController(&formController);
    editController.setFormDesignerController(&formDesigner);
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 5000);

    const QVariantMap field = formDesigner.fieldData(
        QStringLiteral(
            "dropdown-field-11111111-1111-1111-1111-111111111111"));
    QCOMPARE(field.value(QStringLiteral("type")).toInt(),
             PDFFormDesignerController::Dropdown);
    QCOMPARE(field.value(QStringLiteral("options")).toStringList(),
             QStringList({
                 QStringLiteral("Airplane"),
                 QStringLiteral("Bicycle"),
                 QStringLiteral("Car"),
             }));
    QCOMPARE(field.value(QStringLiteral("value")).toString(),
             QStringLiteral("Airplane"));
    QVERIFY(formDesigner.setFieldRect(
        field.value(QStringLiteral("id")).toString(),
        0.10,
        0.20,
        0.50,
        0.08));

    const PDFSaveWriter::Result result = PDFSaveWriter::save(
        editController,
        savedPath);
    QCOMPARE(result.error, PDFSaveWriter::Error::AcroFormCreationFailed);
    QVERIFY(!result.detail.isEmpty());
    QVERIFY(!QFileInfo::exists(savedPath));
    QVERIFY(QFileInfo::exists(sourcePath));
}
