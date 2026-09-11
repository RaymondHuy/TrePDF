#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

using namespace PDFEditControllerTestSupport;

void PDFEditControllerTest::savesAndRehydratesCheckbox()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("acroform-checkbox.pdf"));
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

    const QRectF initialRect(0.12, 0.24, 0.06, 0.06);
    const QString fieldId = formDesigner.addField(
        PDFFormDesignerController::Checkbox,
        0,
        initialRect.x(),
        initialRect.y(),
        initialRect.width(),
        initialRect.height());
    QVERIFY(fieldId.startsWith(QStringLiteral("checkbox-field-")));
    QVERIFY(formDesigner.setFieldName(
        fieldId,
        QStringLiteral("AcceptTerms")));
    QVERIFY(formDesigner.setFieldValue(
        fieldId,
        QStringLiteral("Yes")));
    QVERIFY(formDesigner.setFieldFlags(
        fieldId,
        PDFFormDesignerController::Required));
    const QRectF otherInitialRect(0.65, 0.30, 0.08, 0.08);
    const QString otherCheckboxId = formDesigner.addField(
        PDFFormDesignerController::Checkbox,
        0,
        otherInitialRect.x(),
        otherInitialRect.y(),
        otherInitialRect.width(),
        otherInitialRect.height());
    QVERIFY(otherCheckboxId.startsWith(
        QStringLiteral("checkbox-field-")));
    QVERIFY(formDesigner.setFieldName(
        otherCheckboxId,
        QStringLiteral("OtherChoice")));

    const QVector<PDFFormDesignerController::SaveField> addedFields
        = formDesigner.saveFields();
    QCOMPARE(addedFields.size(), 2);
    QCOMPARE(addedFields.constFirst().type,
             PDFFormDesignerController::Checkbox);
    QCOMPARE(addedFields.constFirst().value, QStringLiteral("Yes"));
    QCOMPARE(addedFields.constFirst().defaultValue, QStringLiteral("Off"));
    QCOMPARE(addedFields.constLast().type,
             PDFFormDesignerController::Checkbox);
    QCOMPARE(addedFields.constLast().value, QStringLiteral("Off"));

    QSignalSpy saveFinishedSpy(
        &editController,
        &PDFEditController::saveFinished);
    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 10000);
    QVERIFY(saveFinishedSpy.constFirst().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 2, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);

    QVariantMap hydrated = formDesigner.fieldData(fieldId);
    QCOMPARE(hydrated.value(QStringLiteral("type")).toInt(),
             PDFFormDesignerController::Checkbox);
    QCOMPARE(hydrated.value(QStringLiteral("value")).toString(),
             QStringLiteral("Yes"));
    QVERIFY(hydrated.value(QStringLiteral("sourceBacked")).toBool());
    QVERIFY(!hydrated.value(QStringLiteral("dirty")).toBool());
    const QStringList sourceFieldIds
        = formDesigner.sourceFieldIdsForPage(0);
    QCOMPARE(sourceFieldIds.size(), 2);
    QVERIFY(sourceFieldIds.contains(fieldId));
    QVERIFY(sourceFieldIds.contains(otherCheckboxId));
    QVERIFY(formDesigner.sourceTextFieldIdsForPage(0).isEmpty());
    QVERIFY(formDesigner.sourceFieldContainsPoint(
        0,
        initialRect.center()));

    AcroFormFieldInspection inspection = inspectAcroFormField(
        sourcePath,
        QStringLiteral("AcceptTerms"));
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.formType, FORMTYPE_ACRO_FORM);
    QCOMPARE(inspection.widgetCount, 2);
    QCOMPARE(inspection.fieldType, FPDF_FORMFIELD_CHECKBOX);
    QCOMPARE(inspection.value, QStringLiteral("Yes"));
    QCOMPARE(inspection.exportValue, QStringLiteral("Yes"));
    QVERIFY(inspection.checked);
    QVERIFY(inspection.hasAppearance);
    QVERIFY(inspection.annotationFlags & FPDF_ANNOT_FLAG_PRINT);
    QCOMPARE(inspection.controlCount, 1);
    QCOMPARE(inspection.controlIndex, 0);
    QVERIFY(inspection.hasAppearanceVersion);
    QCOMPARE(inspection.appearanceVersion,
             static_cast<float>(
                 PDFFormAppearance::kCheckboxVersion));
    QVERIFY(inspection.pointHit);
    QVERIFY(qAbs(inspection.rect.left - 71.4) < 1.0);
    QVERIFY(qAbs(inspection.rect.right - 107.1) < 1.0);
    QVERIFY(qAbs(inspection.rect.top - 639.92) < 1.0);
    QVERIFY(qAbs(inspection.rect.bottom - 589.4) < 1.0);

    QTRY_VERIFY_WITH_TIMEOUT(formController.javascriptEnabled(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !formController.javascriptTransitionPending(),
        5000);
    const QSize deviceSize(595, 842);
    const QPointF fieldCenter(
        initialRect.center().x() * deviceSize.width(),
        initialRect.center().y() * deviceSize.height());
    formController.pointerEvent(
        0,
        PDFFormWorker::PointerEventType::Press,
        0,
        fieldCenter,
        deviceSize);
    formController.pointerEvent(
        0,
        PDFFormWorker::PointerEventType::Release,
        0,
        fieldCenter,
        deviceSize);
    QTRY_VERIFY_WITH_TIMEOUT(formController.hasDrafts(), 5000);
    QVERIFY(!formController.acceptsTextInput());

    formDesigner.prepareForDesignMode();
    QTRY_COMPARE_WITH_TIMEOUT(
        formDesigner.fieldData(fieldId)
            .value(QStringLiteral("value"))
            .toString(),
        QStringLiteral("Off"),
        5000);

    const QRectF resizedRect(0.20, 0.30, 0.08, 0.08);
    formDesigner.selectTextField(fieldId);
    QVERIFY(formDesigner.setFieldRect(
        fieldId,
        resizedRect.x(),
        resizedRect.y(),
        resizedRect.width(),
        resizedRect.height()));
    formDesigner.selectTextField(otherCheckboxId);
    QVERIFY(formDesigner.activateSourceCheckbox(
        otherCheckboxId,
        deviceSize.width(),
        deviceSize.height()));
    QTRY_COMPARE_WITH_TIMEOUT(
        formDesigner.fieldData(otherCheckboxId)
            .value(QStringLiteral("value"))
            .toString(),
        QStringLiteral("Yes"),
        5000);
    QTRY_VERIFY_WITH_TIMEOUT(!formController.hasFocus(), 5000);
    formDesigner.selectTextField(fieldId);
    QCOMPARE(formDesigner.selectedTextFieldId(), fieldId);

    QVERIFY(formDesigner.activateSourceCheckbox(
        fieldId,
        deviceSize.width(),
        deviceSize.height()));
    QTRY_COMPARE_WITH_TIMEOUT(
        formDesigner.fieldData(fieldId)
            .value(QStringLiteral("value"))
            .toString(),
        QStringLiteral("Yes"),
        5000);
    QTRY_VERIFY_WITH_TIMEOUT(!formController.hasFocus(), 5000);
    QVERIFY(formDesigner.activateSourceCheckbox(
        fieldId,
        deviceSize.width(),
        deviceSize.height()));
    QTRY_COMPARE_WITH_TIMEOUT(
        formDesigner.fieldData(fieldId)
            .value(QStringLiteral("value"))
            .toString(),
        QStringLiteral("Off"),
        5000);
    QTRY_VERIFY_WITH_TIMEOUT(!formController.hasFocus(), 5000);
    QCOMPARE(
        formDesigner.fieldData(fieldId)
            .value(QStringLiteral("rect"))
            .toRectF(),
        resizedRect);
    QCOMPARE(formDesigner.fieldsForPage(0).size(), 2);
    QCOMPARE(formDesigner.selectedTextFieldId(), fieldId);

    const QVector<PDFFormDesignerController::SaveField> resizedFields
        = formDesigner.saveFields(true);
    QCOMPARE(resizedFields.size(), 1);
    QCOMPARE(resizedFields.constFirst().type,
             PDFFormDesignerController::Checkbox);
    QCOMPARE(resizedFields.constFirst().operation,
             PDFFormDesignerController::SaveField::Update);
    QCOMPARE(resizedFields.constFirst().changedProperties,
             static_cast<quint32>(
                 PDFFormDesignerController::RectPropertyChanged));
    QCOMPARE(resizedFields.constFirst().value, QStringLiteral("Off"));
    QCOMPARE(resizedFields.constFirst().defaultValue, QStringLiteral("Off"));

    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 2, 10000);
    QVERIFY(saveFinishedSpy.constLast().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 2, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);

    hydrated = formDesigner.fieldData(fieldId);
    QCOMPARE(hydrated.value(QStringLiteral("type")).toInt(),
             PDFFormDesignerController::Checkbox);
    QCOMPARE(hydrated.value(QStringLiteral("value")).toString(),
             QStringLiteral("Off"));
    QVERIFY(hydrated.value(QStringLiteral("sourceBacked")).toBool());
    QVERIFY(!hydrated.value(QStringLiteral("dirty")).toBool());

    const QSize focusedRenderSize(1190, 1684);
    const QImage pageWithoutFocusedCheckbox = renderFirstPage(
        sourcePath,
        focusedRenderSize);
    const QImage pageWithFocusedCheckboxSuppressed
        = renderFirstPageWithForms(
            sourcePath,
            focusedRenderSize,
            {fieldId},
            resizedRect.center());
    QVERIFY(!pageWithoutFocusedCheckbox.isNull());
    QVERIFY(!pageWithFocusedCheckboxSuppressed.isNull());
    const QRect focusedCheckboxArea(
        qRound(resizedRect.x() * focusedRenderSize.width()),
        qRound(resizedRect.y() * focusedRenderSize.height()),
        qRound(resizedRect.width() * focusedRenderSize.width()),
        qRound(resizedRect.height() * focusedRenderSize.height()));
    QCOMPARE(
        changedPixelCount(
            pageWithoutFocusedCheckbox,
            pageWithFocusedCheckboxSuppressed,
            focusedCheckboxArea.adjusted(-8, -8, 8, 8)),
        0);
    QVERIFY(formDesigner.sourceFieldContainsPoint(
        0,
        resizedRect.center()));
    QVERIFY(!formDesigner.sourceFieldContainsPoint(
        0,
        initialRect.topLeft()));

    inspection = inspectAcroFormField(
        sourcePath,
        QStringLiteral("AcceptTerms"));
    QCOMPARE(inspection.widgetCount, 2);
    QCOMPARE(inspection.fieldType, FPDF_FORMFIELD_CHECKBOX);
    QCOMPARE(inspection.value, QStringLiteral("Off"));
    QCOMPARE(inspection.exportValue, QStringLiteral("Yes"));
    QVERIFY(!inspection.checked);
    QVERIFY(inspection.hasAppearance);
    QVERIFY(inspection.pointHit);
    QVERIFY(qAbs(inspection.rect.left - 119.0) < 1.0);
    QVERIFY(qAbs(inspection.rect.right - 166.6) < 1.0);
    QVERIFY(qAbs(inspection.rect.top - 589.4) < 1.0);
    QVERIFY(qAbs(inspection.rect.bottom - 522.04) < 1.0);
}

void PDFEditControllerTest::savesAndRehydratesRadioButton()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("acroform-radio-source.pdf"));
    const QString savedPath = m_temporaryDirectory.filePath(
        QStringLiteral("acroform-radio-saved.pdf"));
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

    const QRectF initialRect(0.18, 0.26, 0.06, 0.06);
    const auto normalizedRectsMatch = [](const QRectF &actual,
                                         const QRectF &expected) {
        constexpr qreal tolerance = 0.0001;
        return qAbs(actual.x() - expected.x()) < tolerance
            && qAbs(actual.y() - expected.y()) < tolerance
            && qAbs(actual.width() - expected.width()) < tolerance
            && qAbs(actual.height() - expected.height()) < tolerance;
    };
    const QString fieldId = formDesigner.addField(
        PDFFormDesignerController::RadioButton,
        0,
        initialRect.x(),
        initialRect.y(),
        initialRect.width(),
        initialRect.height());
    QVERIFY(fieldId.startsWith(QStringLiteral("radio-field-")));
    QCOMPARE(
        formDesigner.fieldData(fieldId)
            .value(QStringLiteral("value"))
            .toString(),
        QStringLiteral("Off"));
    QVERIFY(formDesigner.setFieldName(
        fieldId,
        QStringLiteral("PreferredContact")));
    QVERIFY(formDesigner.setFieldValue(fieldId, QStringLiteral("Yes")));
    QVERIFY(formDesigner.setFieldFlags(
        fieldId,
        PDFFormDesignerController::Required));

    QVector<PDFFormDesignerController::SaveField> savedFields
        = formDesigner.saveFields();
    QCOMPARE(savedFields.size(), 1);
    QCOMPARE(savedFields.constFirst().type,
             PDFFormDesignerController::RadioButton);
    QCOMPARE(savedFields.constFirst().operation,
             PDFFormDesignerController::SaveField::Add);
    QCOMPARE(savedFields.constFirst().value, QStringLiteral("Yes"));
    QCOMPARE(savedFields.constFirst().defaultValue, QStringLiteral("Off"));
    QCOMPARE(savedFields.constFirst().normalizedRect, initialRect);

    QSignalSpy saveFinishedSpy(
        &editController,
        &PDFEditController::saveFinished);
    QVERIFY(editController.requestSaveAs(QUrl::fromLocalFile(savedPath)));
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 10000);
    QVERIFY(saveFinishedSpy.constFirst().constFirst().toBool());
    QCOMPARE(document.source().toLocalFile(), savedPath);
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);

    const AcroFormFieldInspection untouchedSource
        = inspectAcroFormField(sourcePath);
    QVERIFY(untouchedSource.readable);
    QCOMPARE(untouchedSource.widgetCount, 0);

    QVariantMap hydrated = formDesigner.fieldData(fieldId);
    QCOMPARE(hydrated.value(QStringLiteral("type")).toInt(),
             PDFFormDesignerController::RadioButton);
    QCOMPARE(hydrated.value(QStringLiteral("value")).toString(),
             QStringLiteral("Yes"));
    QVERIFY(normalizedRectsMatch(
        hydrated.value(QStringLiteral("rect")).toRectF(),
        initialRect));
    QVERIFY(hydrated.value(QStringLiteral("sourceBacked")).toBool());
    QVERIFY(!hydrated.value(QStringLiteral("dirty")).toBool());
    QVERIFY(formDesigner.sourceFieldIdsForPage(0).contains(fieldId));
    QVERIFY(formDesigner.sourceTextFieldIdsForPage(0).isEmpty());
    QVERIFY(formDesigner.sourceFieldContainsPoint(0, initialRect.center()));

    AcroFormFieldInspection inspection = inspectAcroFormField(
        savedPath,
        QStringLiteral("PreferredContact"));
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.formType, FORMTYPE_ACRO_FORM);
    QCOMPARE(inspection.widgetCount, 1);
    QCOMPARE(inspection.fieldType, FPDF_FORMFIELD_RADIOBUTTON);
    QCOMPARE(inspection.name, QStringLiteral("PreferredContact"));
    QCOMPARE(inspection.value, fieldId);
    QCOMPARE(inspection.defaultValue, QStringLiteral("Off"));
    QCOMPARE(inspection.exportValue, fieldId);
    QVERIFY(inspection.checked);
    QVERIFY(inspection.fieldFlags & FPDF_FORMFLAG_REQUIRED);
    QCOMPARE(
        inspection.fieldFlags
            & (kPdfButtonNoToggleToOff | kPdfButtonRadio),
        kPdfButtonRadio);
    QVERIFY(inspection.annotationFlags & FPDF_ANNOT_FLAG_PRINT);
    QVERIFY(inspection.hasAppearance);
    QVERIFY(!inspection.appearanceStream.isEmpty());
    QVERIFY(inspection.appearanceStream.contains("0 0 0 RG"));
    QVERIFY(inspection.appearanceStream.contains("0 0 0 rg"));
    QVERIFY(inspection.hasAppearanceVersion);
    QCOMPARE(inspection.appearanceVersion,
             static_cast<float>(
                 PDFFormAppearance::kRadioButtonVersion));
    QCOMPARE(inspection.controlCount, 1);
    QCOMPARE(inspection.controlIndex, 0);
    QVERIFY(inspection.hasParent);
    QVERIFY(!inspection.hasLocalFieldType);
    QVERIFY(!inspection.hasLocalFieldFlags);
    QVERIFY(!inspection.hasLocalValue);
    QVERIFY(inspection.pointHit);
    QCOMPARE(
        clickAcroFormFieldTwice(
            savedPath,
            QStringLiteral("PreferredContact")),
        QVector<bool>({true, true, true}));
    QVERIFY(qAbs(inspection.rect.left
                 - initialRect.left() * 595.0)
            < 1.0);
    QVERIFY(qAbs(inspection.rect.right
                 - initialRect.right() * 595.0)
            < 1.0);
    QVERIFY(qAbs(inspection.rect.top
                 - (1.0 - initialRect.top()) * 842.0)
            < 1.0);
    QVERIFY(qAbs(inspection.rect.bottom
                 - (1.0 - initialRect.bottom()) * 842.0)
            < 1.0);

    formDesigner.prepareForDesignMode();
    const QString renamedGroupId
        = QStringLiteral("PreferredContactRenamed");
    QVERIFY(formDesigner.setFieldName(fieldId, renamedGroupId));
    QVERIFY(formDesigner.setFieldValue(fieldId, QStringLiteral("Off")));
    const QRectF movedRect(0.38,
                           0.34,
                           initialRect.width(),
                           initialRect.height());
    QVERIFY(formDesigner.moveField(
        fieldId,
        movedRect.x(),
        movedRect.y()));
    QVERIFY(normalizedRectsMatch(
        formDesigner.fieldData(fieldId)
            .value(QStringLiteral("rect"))
            .toRectF(),
        movedRect));
    const QRectF finalRect(0.42, 0.38, 0.08, 0.07);
    QVERIFY(formDesigner.setFieldRect(
        fieldId,
        finalRect.x(),
        finalRect.y(),
        finalRect.width(),
        finalRect.height()));

    savedFields = formDesigner.saveFields(true);
    QCOMPARE(savedFields.size(), 1);
    QCOMPARE(savedFields.constFirst().type,
             PDFFormDesignerController::RadioButton);
    QCOMPARE(savedFields.constFirst().operation,
             PDFFormDesignerController::SaveField::Update);
    QCOMPARE(savedFields.constFirst().changedProperties,
             static_cast<quint32>(
                 PDFFormDesignerController::NamePropertyChanged
                 | PDFFormDesignerController::ValuePropertyChanged
                 | PDFFormDesignerController::RectPropertyChanged));
    QCOMPARE(savedFields.constFirst().name, renamedGroupId);
    QCOMPARE(savedFields.constFirst().value, QStringLiteral("Off"));
    QCOMPARE(savedFields.constFirst().defaultValue, QStringLiteral("Off"));
    QCOMPARE(savedFields.constFirst().normalizedRect, finalRect);

    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 2, 10000);
    QVERIFY(saveFinishedSpy.constLast().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);

    hydrated = formDesigner.fieldData(fieldId);
    QCOMPARE(hydrated.value(QStringLiteral("type")).toInt(),
             PDFFormDesignerController::RadioButton);
    QCOMPARE(hydrated.value(QStringLiteral("value")).toString(),
             QStringLiteral("Off"));
    QCOMPARE(hydrated.value(QStringLiteral("name")).toString(),
             renamedGroupId);
    QVERIFY(normalizedRectsMatch(
        hydrated.value(QStringLiteral("rect")).toRectF(),
        finalRect));
    QVERIFY(hydrated.value(QStringLiteral("sourceBacked")).toBool());
    QVERIFY(!hydrated.value(QStringLiteral("dirty")).toBool());
    QVERIFY(formDesigner.sourceFieldContainsPoint(0, finalRect.center()));
    QVERIFY(!formDesigner.sourceFieldContainsPoint(0, initialRect.center()));

    inspection = inspectAcroFormField(
        savedPath,
        renamedGroupId);
    QCOMPARE(inspection.widgetCount, 1);
    QCOMPARE(inspection.fieldType, FPDF_FORMFIELD_RADIOBUTTON);
    QCOMPARE(inspection.name, renamedGroupId);
    QCOMPARE(inspection.value, QStringLiteral("Off"));
    QCOMPARE(inspection.defaultValue, QStringLiteral("Off"));
    QCOMPARE(inspection.exportValue, fieldId);
    QVERIFY(!inspection.checked);
    QCOMPARE(
        inspection.fieldFlags
            & (kPdfButtonNoToggleToOff | kPdfButtonRadio),
        kPdfButtonRadio);
    QVERIFY(inspection.annotationFlags & FPDF_ANNOT_FLAG_PRINT);
    QVERIFY(inspection.hasAppearance);
    QVERIFY(!inspection.appearanceStream.isEmpty());
    QVERIFY(inspection.hasParent);
    QVERIFY(!inspection.hasLocalFieldType);
    QVERIFY(!inspection.hasLocalFieldFlags);
    QVERIFY(!inspection.hasLocalValue);
    QCOMPARE(
        clickAcroFormFieldTwice(
            savedPath,
            renamedGroupId),
        QVector<bool>({false, true, true}));
    QVERIFY(inspection.hasAppearanceVersion);
    QCOMPARE(inspection.appearanceVersion,
             static_cast<float>(
                 PDFFormAppearance::kRadioButtonVersion));
    QVERIFY(qAbs(inspection.rect.left
                 - finalRect.left() * 595.0)
            < 1.0);
    QVERIFY(qAbs(inspection.rect.right
                 - finalRect.right() * 595.0)
            < 1.0);
    QVERIFY(qAbs(inspection.rect.top
                 - (1.0 - finalRect.top()) * 842.0)
            < 1.0);
    QVERIFY(qAbs(inspection.rect.bottom
                 - (1.0 - finalRect.bottom()) * 842.0)
            < 1.0);
}

void PDFEditControllerTest::savesRehydratesAndMutatesRadioButtonGroup()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("acroform-radio-group-source.pdf"));
    const QString savedPath = m_temporaryDirectory.filePath(
        QStringLiteral("acroform-radio-group-saved.pdf"));
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

    const QString groupId = QStringLiteral("PreferredChannel");
    const QRectF firstRect(0.14, 0.24, 0.06, 0.06);
    const QRectF secondRect(0.32, 0.24, 0.06, 0.06);
    const QString firstId = formDesigner.addField(
        PDFFormDesignerController::RadioButton,
        0,
        firstRect.x(),
        firstRect.y(),
        firstRect.width(),
        firstRect.height());
    const QString secondId = formDesigner.addField(
        PDFFormDesignerController::RadioButton,
        0,
        secondRect.x(),
        secondRect.y(),
        secondRect.width(),
        secondRect.height());
    QVERIFY(firstId.startsWith(QStringLiteral("radio-field-")));
    QVERIFY(secondId.startsWith(QStringLiteral("radio-field-")));
    QVERIFY(firstId != secondId);
    QVERIFY(formDesigner.setFieldName(firstId, groupId));
    QVERIFY(formDesigner.setFieldName(secondId, groupId));
    QVERIFY(formDesigner.setFieldFlags(
        firstId,
        PDFFormDesignerController::Required));

    QVERIFY(formDesigner.setFieldValue(firstId, QStringLiteral("Yes")));
    QCOMPARE(
        formDesigner.fieldData(firstId)
            .value(QStringLiteral("value"))
            .toString(),
        QStringLiteral("Yes"));
    QCOMPARE(
        formDesigner.fieldData(secondId)
            .value(QStringLiteral("value"))
            .toString(),
        QStringLiteral("Off"));
    QVERIFY(formDesigner.setFieldValue(secondId, QStringLiteral("Yes")));
    QCOMPARE(
        formDesigner.fieldData(firstId)
            .value(QStringLiteral("value"))
            .toString(),
        QStringLiteral("Off"));
    QCOMPARE(
        formDesigner.fieldData(secondId)
            .value(QStringLiteral("value"))
            .toString(),
        QStringLiteral("Yes"));

    const auto savedFieldForId = [](
        const QVector<PDFFormDesignerController::SaveField> &fields,
        const QString &fieldId)
        -> const PDFFormDesignerController::SaveField * {
        const auto field = std::find_if(
            fields.cbegin(),
            fields.cend(),
            [&fieldId](const PDFFormDesignerController::SaveField &candidate) {
                return candidate.id == fieldId;
            });
        return field != fields.cend() ? &*field : nullptr;
    };

    QVector<PDFFormDesignerController::SaveField> savedFields
        = formDesigner.saveFields();
    QCOMPARE(savedFields.size(), 2);
    const PDFFormDesignerController::SaveField *firstSaved
        = savedFieldForId(savedFields, firstId);
    const PDFFormDesignerController::SaveField *secondSaved
        = savedFieldForId(savedFields, secondId);
    QVERIFY(firstSaved);
    QVERIFY(secondSaved);
    QCOMPARE(firstSaved->type, PDFFormDesignerController::RadioButton);
    QCOMPARE(secondSaved->type, PDFFormDesignerController::RadioButton);
    QCOMPARE(firstSaved->operation,
             PDFFormDesignerController::SaveField::Add);
    QCOMPARE(secondSaved->operation,
             PDFFormDesignerController::SaveField::Add);
    QCOMPARE(firstSaved->name, groupId);
    QCOMPARE(secondSaved->name, groupId);
    QCOMPARE(firstSaved->value, QStringLiteral("Off"));
    QCOMPARE(secondSaved->value, QStringLiteral("Yes"));
    QCOMPARE(firstSaved->defaultValue, QStringLiteral("Off"));
    QCOMPARE(secondSaved->defaultValue, QStringLiteral("Off"));
    QCOMPARE(firstSaved->flags,
             static_cast<int>(PDFFormDesignerController::Required));
    QCOMPARE(secondSaved->flags,
             static_cast<int>(PDFFormDesignerController::Required));

    QSignalSpy saveFinishedSpy(
        &editController,
        &PDFEditController::saveFinished);
    QVERIFY(editController.requestSaveAs(QUrl::fromLocalFile(savedPath)));
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 10000);
    QVERIFY(saveFinishedSpy.constFirst().constFirst().toBool());
    QCOMPARE(document.source().toLocalFile(), savedPath);
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 2, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);

    const AcroFormFieldInspection untouchedSource
        = inspectAcroFormField(sourcePath);
    QVERIFY(untouchedSource.readable);
    QCOMPARE(untouchedSource.widgetCount, 0);

    QVariantMap firstHydrated = formDesigner.fieldData(firstId);
    QVariantMap secondHydrated = formDesigner.fieldData(secondId);
    QCOMPARE(firstHydrated.value(QStringLiteral("type")).toInt(),
             PDFFormDesignerController::RadioButton);
    QCOMPARE(secondHydrated.value(QStringLiteral("type")).toInt(),
             PDFFormDesignerController::RadioButton);
    QCOMPARE(firstHydrated.value(QStringLiteral("name")).toString(), groupId);
    QCOMPARE(secondHydrated.value(QStringLiteral("name")).toString(), groupId);
    QCOMPARE(firstHydrated.value(QStringLiteral("value")).toString(),
             QStringLiteral("Off"));
    QCOMPARE(secondHydrated.value(QStringLiteral("value")).toString(),
             QStringLiteral("Yes"));
    QVERIFY(firstHydrated.value(QStringLiteral("sourceBacked")).toBool());
    QVERIFY(secondHydrated.value(QStringLiteral("sourceBacked")).toBool());
    QVERIFY(!firstHydrated.value(QStringLiteral("dirty")).toBool());
    QVERIFY(!secondHydrated.value(QStringLiteral("dirty")).toBool());

    AcroFormFieldInspection firstInspection = inspectAcroFormField(
        savedPath,
        groupId,
        0,
        firstId);
    AcroFormFieldInspection secondInspection = inspectAcroFormField(
        savedPath,
        groupId,
        0,
        secondId);
    QVERIFY(firstInspection.readable);
    QVERIFY(secondInspection.readable);
    QCOMPARE(firstInspection.formType, FORMTYPE_ACRO_FORM);
    QCOMPARE(firstInspection.widgetCount, 2);
    QCOMPARE(secondInspection.widgetCount, 2);
    QCOMPARE(firstInspection.fieldType, FPDF_FORMFIELD_RADIOBUTTON);
    QCOMPARE(secondInspection.fieldType, FPDF_FORMFIELD_RADIOBUTTON);
    QCOMPARE(firstInspection.widgetId, firstId);
    QCOMPARE(secondInspection.widgetId, secondId);
    QCOMPARE(firstInspection.name, groupId);
    QCOMPARE(secondInspection.name, groupId);
    QCOMPARE(firstInspection.value, secondId);
    QCOMPARE(secondInspection.value, secondId);
    QCOMPARE(firstInspection.defaultValue, QStringLiteral("Off"));
    QCOMPARE(secondInspection.defaultValue, QStringLiteral("Off"));
    QCOMPARE(firstInspection.exportValue, firstId);
    QCOMPARE(secondInspection.exportValue, secondId);
    QVERIFY(!firstInspection.checked);
    QVERIFY(secondInspection.checked);
    QCOMPARE(
        firstInspection.fieldFlags
            & (kPdfButtonNoToggleToOff | kPdfButtonRadio),
        kPdfButtonRadio);
    QCOMPARE(
        secondInspection.fieldFlags
            & (kPdfButtonNoToggleToOff | kPdfButtonRadio),
        kPdfButtonRadio);
    QVERIFY(firstInspection.fieldFlags & FPDF_FORMFLAG_REQUIRED);
    QVERIFY(secondInspection.fieldFlags & FPDF_FORMFLAG_REQUIRED);
    QVERIFY(firstInspection.annotationFlags & FPDF_ANNOT_FLAG_PRINT);
    QVERIFY(secondInspection.annotationFlags & FPDF_ANNOT_FLAG_PRINT);
    QVERIFY(firstInspection.hasAppearance);
    QVERIFY(secondInspection.hasAppearance);
    QVERIFY(!firstInspection.appearanceStream.isEmpty());
    QVERIFY(!secondInspection.appearanceStream.isEmpty());
    QVERIFY(firstInspection.hasAppearanceVersion);
    QVERIFY(secondInspection.hasAppearanceVersion);
    QCOMPARE(firstInspection.appearanceVersion,
             static_cast<float>(
                 PDFFormAppearance::kRadioButtonVersion));
    QCOMPARE(secondInspection.appearanceVersion,
             static_cast<float>(
                 PDFFormAppearance::kRadioButtonVersion));
    QVERIFY(firstInspection.hasParent);
    QVERIFY(secondInspection.hasParent);
    QVERIFY(!firstInspection.hasLocalFieldType);
    QVERIFY(!secondInspection.hasLocalFieldType);
    QVERIFY(!firstInspection.hasLocalFieldFlags);
    QVERIFY(!secondInspection.hasLocalFieldFlags);
    QVERIFY(!firstInspection.hasLocalValue);
    QVERIFY(!secondInspection.hasLocalValue);
    QCOMPARE(firstInspection.controlCount, 2);
    QCOMPARE(secondInspection.controlCount, 2);
    QVERIFY(firstInspection.controlIndex >= 0);
    QVERIFY(secondInspection.controlIndex >= 0);
    QVERIFY(firstInspection.controlIndex != secondInspection.controlIndex);

    formDesigner.prepareForDesignMode();
    QVERIFY(formDesigner.setFieldValue(firstId, QStringLiteral("Yes")));
    QCOMPARE(
        formDesigner.fieldData(firstId)
            .value(QStringLiteral("value"))
            .toString(),
        QStringLiteral("Yes"));
    QCOMPARE(
        formDesigner.fieldData(secondId)
            .value(QStringLiteral("value"))
            .toString(),
        QStringLiteral("Off"));
    const QRectF movedFirstRect(0.20, 0.34, 0.08, 0.07);
    QVERIFY(formDesigner.moveField(
        firstId,
        movedFirstRect.x(),
        movedFirstRect.y()));
    QVERIFY(formDesigner.setFieldRect(
        firstId,
        movedFirstRect.x(),
        movedFirstRect.y(),
        movedFirstRect.width(),
        movedFirstRect.height()));

    savedFields = formDesigner.saveFields(true);
    QCOMPARE(savedFields.size(), 2);
    firstSaved = savedFieldForId(savedFields, firstId);
    secondSaved = savedFieldForId(savedFields, secondId);
    QVERIFY(firstSaved);
    QVERIFY(secondSaved);
    QCOMPARE(firstSaved->operation,
             PDFFormDesignerController::SaveField::Update);
    QCOMPARE(secondSaved->operation,
             PDFFormDesignerController::SaveField::Update);
    QCOMPARE(firstSaved->value, QStringLiteral("Yes"));
    QCOMPARE(secondSaved->value, QStringLiteral("Off"));
    QVERIFY(firstSaved->changedProperties
            & PDFFormDesignerController::ValuePropertyChanged);
    QVERIFY(firstSaved->changedProperties
            & PDFFormDesignerController::RectPropertyChanged);
    QCOMPARE(secondSaved->changedProperties,
             static_cast<quint32>(
                 PDFFormDesignerController::ValuePropertyChanged));

    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 2, 10000);
    QVERIFY(saveFinishedSpy.constLast().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 2, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);

    firstInspection = inspectAcroFormField(
        savedPath,
        groupId,
        0,
        firstId);
    secondInspection = inspectAcroFormField(
        savedPath,
        groupId,
        0,
        secondId);
    QCOMPARE(firstInspection.value, firstId);
    QCOMPARE(secondInspection.value, firstId);
    QCOMPARE(firstInspection.exportValue, firstId);
    QCOMPARE(secondInspection.exportValue, secondId);
    QVERIFY(firstInspection.checked);
    QVERIFY(!secondInspection.checked);
    QCOMPARE(firstInspection.controlCount, 2);
    QCOMPARE(secondInspection.controlCount, 2);
    QVERIFY(firstInspection.controlIndex != secondInspection.controlIndex);
    QVERIFY(qAbs(firstInspection.rect.left
                 - movedFirstRect.left() * 595.0)
            < 1.0);
    QVERIFY(qAbs(firstInspection.rect.right
                 - movedFirstRect.right() * 595.0)
            < 1.0);
    QVERIFY(qAbs(firstInspection.rect.top
                 - (1.0 - movedFirstRect.top()) * 842.0)
            < 1.0);
    QVERIFY(qAbs(firstInspection.rect.bottom
                 - (1.0 - movedFirstRect.bottom()) * 842.0)
            < 1.0);

    QVERIFY(formDesigner.removeField(secondId));
    QCOMPARE(formDesigner.rowCount(), 1);
    QVERIFY(formDesigner.hasDrafts());
    savedFields = formDesigner.saveFields();
    QCOMPARE(savedFields.size(), 2);
    firstSaved = savedFieldForId(savedFields, firstId);
    secondSaved = savedFieldForId(savedFields, secondId);
    QVERIFY(firstSaved);
    QVERIFY(secondSaved);
    QCOMPARE(firstSaved->operation,
             PDFFormDesignerController::SaveField::Update);
    QCOMPARE(firstSaved->changedProperties,
             static_cast<quint32>(
                 PDFFormDesignerController::NoPropertyChanges));
    QCOMPARE(secondSaved->operation,
             PDFFormDesignerController::SaveField::Remove);

    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 3, 10000);
    QVERIFY(saveFinishedSpy.constLast().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);
    QVERIFY(formDesigner.fieldData(secondId).isEmpty());
    firstHydrated = formDesigner.fieldData(firstId);
    QCOMPARE(firstHydrated.value(QStringLiteral("type")).toInt(),
             PDFFormDesignerController::RadioButton);
    QCOMPARE(firstHydrated.value(QStringLiteral("name")).toString(), groupId);
    QCOMPARE(firstHydrated.value(QStringLiteral("value")).toString(),
             QStringLiteral("Yes"));
    QVERIFY(firstHydrated.value(QStringLiteral("sourceBacked")).toBool());
    const QStringList sourceFieldIds
        = formDesigner.sourceFieldIdsForPage(0);
    QCOMPARE(sourceFieldIds.size(), 1);
    QCOMPARE(sourceFieldIds.constFirst(), firstId);

    firstInspection = inspectAcroFormField(
        savedPath,
        groupId,
        0,
        firstId);
    QCOMPARE(firstInspection.widgetCount, 1);
    QCOMPARE(firstInspection.fieldType, FPDF_FORMFIELD_RADIOBUTTON);
    QCOMPARE(firstInspection.value, firstId);
    QCOMPARE(firstInspection.defaultValue, QStringLiteral("Off"));
    QCOMPARE(firstInspection.exportValue, firstId);
    QVERIFY(firstInspection.checked);
    QCOMPARE(firstInspection.controlCount, 1);
    QCOMPARE(firstInspection.controlIndex, 0);
    QVERIFY(firstInspection.hasAppearance);
    QVERIFY(firstInspection.hasParent);
    const AcroFormFieldInspection removedInspection = inspectAcroFormField(
        savedPath,
        groupId,
        0,
        secondId);
    QCOMPARE(removedInspection.widgetCount, 1);
    QCOMPARE(removedInspection.fieldType, FPDF_FORMFIELD_UNKNOWN);
}

void PDFEditControllerTest::scopesRadioSnapshotsToAffectedGroups()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("acroform-radio-scope-source.pdf"));
    const QString savedPath = m_temporaryDirectory.filePath(
        QStringLiteral("acroform-radio-scope-saved.pdf"));
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

    const QString firstGroup = QStringLiteral("FirstRadioGroup");
    const QString secondGroup = QStringLiteral("SecondRadioGroup");
    const auto addRadio = [&formDesigner](const QRectF &rect,
                                          const QString &groupName) {
        const QString id = formDesigner.addField(
            PDFFormDesignerController::RadioButton,
            0,
            rect.x(),
            rect.y(),
            rect.width(),
            rect.height());
        return !id.isEmpty() && formDesigner.setFieldName(id, groupName)
            ? id
            : QString();
    };
    const QString firstGroupFirst = addRadio(
        QRectF(0.12, 0.20, 0.05, 0.05),
        firstGroup);
    const QString firstGroupSecond = addRadio(
        QRectF(0.22, 0.20, 0.05, 0.05),
        firstGroup);
    const QString secondGroupFirst = addRadio(
        QRectF(0.12, 0.34, 0.05, 0.05),
        secondGroup);
    const QString secondGroupSecond = addRadio(
        QRectF(0.22, 0.34, 0.05, 0.05),
        secondGroup);
    QVERIFY(!firstGroupFirst.isEmpty());
    QVERIFY(!firstGroupSecond.isEmpty());
    QVERIFY(!secondGroupFirst.isEmpty());
    QVERIFY(!secondGroupSecond.isEmpty());

    QSignalSpy saveFinishedSpy(
        &editController,
        &PDFEditController::saveFinished);
    QVERIFY(editController.requestSaveAs(QUrl::fromLocalFile(savedPath)));
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 10000);
    QVERIFY(saveFinishedSpy.constFirst().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 4, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);

    const auto sortedFieldIds = [](
        const QVector<PDFFormDesignerController::SaveField> &fields) {
        QStringList ids;
        ids.reserve(fields.size());
        for (const PDFFormDesignerController::SaveField &field : fields) {
            ids.append(field.id);
        }
        ids.sort();
        return ids;
    };
    const auto sortedIds = [](QStringList ids) {
        ids.sort();
        return ids;
    };

    formDesigner.prepareForDesignMode();
    QVERIFY(formDesigner.moveField(firstGroupFirst, 0.16, 0.24));
    QVector<PDFFormDesignerController::SaveField> savedFields
        = formDesigner.saveFields();
    QCOMPARE(
        sortedFieldIds(savedFields),
        sortedIds({firstGroupFirst, firstGroupSecond}));

    formDesigner.resetFromDocument();
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 4, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);

    formDesigner.prepareForDesignMode();
    QVERIFY(formDesigner.setFieldName(firstGroupFirst, secondGroup));
    savedFields = formDesigner.saveFields();
    QCOMPARE(
        sortedFieldIds(savedFields),
        sortedIds({
            firstGroupFirst,
            firstGroupSecond,
            secondGroupFirst,
            secondGroupSecond}));

    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 2, 10000);
    QVERIFY(saveFinishedSpy.constLast().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 4, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);

    const AcroFormFieldInspection firstGroupInspection
        = inspectAcroFormField(
            savedPath,
            firstGroup,
            0,
            firstGroupSecond);
    QVERIFY(firstGroupInspection.readable);
    QCOMPARE(firstGroupInspection.fieldType, FPDF_FORMFIELD_RADIOBUTTON);
    QCOMPARE(firstGroupInspection.controlCount, 1);

    const AcroFormFieldInspection secondGroupInspection
        = inspectAcroFormField(
            savedPath,
            secondGroup,
            0,
            firstGroupFirst);
    QVERIFY(secondGroupInspection.readable);
    QCOMPARE(secondGroupInspection.fieldType, FPDF_FORMFIELD_RADIOBUTTON);
    QCOMPARE(secondGroupInspection.controlCount, 3);
}

void PDFEditControllerTest::migratesLegacyMergedRadioButton()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("legacy-merged-radio.pdf"));
    const QString savedPath = m_temporaryDirectory.filePath(
        QStringLiteral("migrated-radio.pdf"));
    QVERIFY(createLegacyTrePDFRadioButtonPdf(sourcePath));

    AcroFormFieldInspection inspection = inspectAcroFormField(
        sourcePath,
        QStringLiteral("PreferredContact"));
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.fieldType, FPDF_FORMFIELD_RADIOBUTTON);
    QVERIFY(inspection.checked);
    QCOMPARE(inspection.defaultValue, QStringLiteral("Yes"));
    QVERIFY(!inspection.hasParent);
    QCOMPARE(inspection.appearanceVersion, 2.0f);

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

    const QString fieldId
        = QStringLiteral(
            "radio-field-11111111-1111-1111-1111-111111111111");
    QCOMPARE(
        formDesigner.fieldData(fieldId)
            .value(QStringLiteral("type"))
            .toInt(),
        PDFFormDesignerController::RadioButton);
    formDesigner.prepareForDesignMode();
    QVERIFY(formDesigner.setFieldValue(fieldId, QStringLiteral("Off")));
    QVector<PDFFormDesignerController::SaveField> radioFields
        = formDesigner.saveFields(true);
    QCOMPARE(radioFields.size(), 1);
    QCOMPARE(radioFields.constFirst().defaultValue,
             QStringLiteral("Yes"));

    QSignalSpy saveFinishedSpy(
        &editController,
        &PDFEditController::saveFinished);
    QVERIFY(editController.requestSaveAs(QUrl::fromLocalFile(savedPath)));
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 10000);
    QVERIFY(saveFinishedSpy.constFirst().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);

    const AcroFormFieldInspection untouchedSource = inspectAcroFormField(
        sourcePath,
        QStringLiteral("PreferredContact"));
    QVERIFY(!untouchedSource.hasParent);
    QCOMPARE(untouchedSource.appearanceVersion, 2.0f);

    inspection = inspectAcroFormField(
        savedPath,
        QStringLiteral("PreferredContact"));
    QCOMPARE(inspection.widgetCount, 1);
    QCOMPARE(inspection.fieldType, FPDF_FORMFIELD_RADIOBUTTON);
    QCOMPARE(inspection.value, QStringLiteral("Off"));
    QCOMPARE(inspection.defaultValue, QStringLiteral("Yes"));
    QVERIFY(!inspection.checked);
    QCOMPARE(
        inspection.fieldFlags
            & (kPdfButtonNoToggleToOff | kPdfButtonRadio),
        kPdfButtonRadio);
    QVERIFY(inspection.hasParent);
    QVERIFY(!inspection.hasLocalFieldType);
    QVERIFY(!inspection.hasLocalFieldFlags);
    QVERIFY(!inspection.hasLocalValue);
    QCOMPARE(inspection.appearanceVersion,
             static_cast<float>(
                 PDFFormAppearance::kRadioButtonVersion));

    formDesigner.prepareForDesignMode();
    const QRectF hydratedRect = formDesigner.fieldData(fieldId)
                                    .value(QStringLiteral("rect"))
                                    .toRectF();
    QVERIFY(formDesigner.setFieldRect(
        fieldId,
        hydratedRect.x() + 0.01,
        hydratedRect.y(),
        hydratedRect.width(),
        hydratedRect.height()));
    radioFields = formDesigner.saveFields();
    QCOMPARE(radioFields.size(), 1);
    QCOMPARE(radioFields.constFirst().defaultValue,
             QStringLiteral("Yes"));
    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 2, 10000);
    QVERIFY(saveFinishedSpy.constLast().constFirst().toBool());
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);

    inspection = inspectAcroFormField(
        savedPath,
        QStringLiteral("PreferredContact"));
    QCOMPARE(inspection.value, QStringLiteral("Off"));
    QCOMPARE(inspection.defaultValue, QStringLiteral("Yes"));
    QVERIFY(!inspection.checked);
    QCOMPARE(
        clickAcroFormFieldTwice(
            savedPath,
            QStringLiteral("PreferredContact")),
        QVector<bool>({false, true, true}));
}
