#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

using namespace PDFEditControllerTestSupport;

void PDFEditControllerTest::savesTextFieldAsAcroForm()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("acroform-source.pdf"));
    const QString savedPath = m_temporaryDirectory.filePath(
        QStringLiteral("acroform-saved.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    PDFFormDesignerController formDesigner;
    formDesigner.setDocument(&document);
    PDFEditController editController;
    editController.setDocument(&document);
    editController.setFormDesignerController(&formDesigner);

    const QString fieldId = formDesigner.addTextField(
        0,
        0.1,
        0.2,
        0.4,
        0.08);
    QVERIFY(!fieldId.isEmpty());
    QVERIFY(formDesigner.setFieldName(
        fieldId,
        QStringLiteral("TênKháchHàng")));
    QVERIFY(formDesigner.setFieldValue(
        fieldId,
        QStringLiteral("Nguyễn Văn A")));
    QVERIFY(formDesigner.setFieldFlags(
        fieldId,
        PDFFormDesignerController::Required
            | PDFFormDesignerController::Multiline));
    QVERIFY(formDesigner.setFieldMaxLength(fieldId, 32));
    const QString secondFieldId = formDesigner.addTextField(
        0,
        0.55,
        0.2,
        0.3,
        0.08);
    QVERIFY(!secondFieldId.isEmpty());
    QVERIFY(formDesigner.setFieldName(
        secondFieldId,
        QStringLiteral("SecondText")));
    QVERIFY(formDesigner.setFieldValue(
        secondFieldId,
        QStringLiteral("Second value")));
    QVERIFY(formDesigner.hasDrafts());
    QVERIFY(editController.hasDrafts());

    const PDFSaveWriter::Result result = PDFSaveWriter::save(
        editController,
        savedPath);
    QVERIFY2(result.succeeded(), qPrintable(result.detail));
    QVERIFY(QFileInfo::exists(savedPath));

    const AcroFormFieldInspection inspection
        = inspectAcroFormField(
            savedPath,
            QStringLiteral("TênKháchHàng"));
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.formType, FORMTYPE_ACRO_FORM);
    QCOMPARE(inspection.widgetCount, 2);
    QCOMPARE(inspection.fieldType, FPDF_FORMFIELD_TEXTFIELD);
    QCOMPARE(inspection.name, QStringLiteral("TênKháchHàng"));
    QCOMPARE(inspection.value, QStringLiteral("Nguyễn Văn A"));
    QVERIFY(inspection.fieldFlags & FPDF_FORMFLAG_REQUIRED);
    QVERIFY(inspection.fieldFlags & FPDF_FORMFLAG_TEXT_MULTILINE);
    QVERIFY(inspection.hasMaxLength);
    QCOMPARE(inspection.maxLength, 32.0f);
    QVERIFY(inspection.hasAppearance);
    QVERIFY(inspection.pointHit);
    QVERIFY(inspection.focusHandled);
    QVERIFY(qAbs(inspection.rect.left - 59.5) < 1.0);
    QVERIFY(qAbs(inspection.rect.right - 297.5) < 1.0);
    QVERIFY(qAbs(inspection.rect.top - 673.6) < 1.0);
    QVERIFY(qAbs(inspection.rect.bottom - 606.24) < 1.0);
    const AcroFormFieldInspection secondInspection
        = inspectAcroFormField(
            savedPath,
            QStringLiteral("SecondText"));
    QCOMPARE(secondInspection.widgetCount, 2);
    QCOMPARE(secondInspection.fieldType, FPDF_FORMFIELD_TEXTFIELD);
    QCOMPARE(secondInspection.value, QStringLiteral("Second value"));
    QVERIFY(secondInspection.hasAppearance);
    QVERIFY(!secondInspection.hasMaxLength);

    PDFFormController formController;
    PDFDocument reopened;
    formController.setDocument(&reopened);
    QVERIFY(reopened.open(QUrl::fromLocalFile(savedPath)));
    QCOMPARE(reopened.pageCount(), document.pageCount());
    QVERIFY(formController.interactive());
    QTRY_VERIFY_WITH_TIMEOUT(formController.javascriptEnabled(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !formController.javascriptTransitionPending(),
        5000);
    const QSize deviceSize(595, 842);
    const QPointF fieldCenter(178.5, 202.08);
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
    QTRY_VERIFY_WITH_TIMEOUT(formController.acceptsTextInput(), 5000);
    formController.selectAll();
    formController.replaceSelection(QStringLiteral("Đã chỉnh sửa"));
    QTRY_VERIFY_WITH_TIMEOUT(formController.hasDrafts(), 5000);

    QString snapshotPath;
    QString snapshotError;
    QVERIFY2(
        formController.exportSnapshotBlocking(
            snapshotPath,
            snapshotError),
        qPrintable(snapshotError));
    const AcroFormFieldInspection editedInspection
        = inspectAcroFormField(
            snapshotPath,
            QStringLiteral("TênKháchHàng"));
    QCOMPARE(editedInspection.value, QStringLiteral("Đã chỉnh sửa"));
    QVERIFY(QFile::remove(snapshotPath));
}

void PDFEditControllerTest::savesAndRehydratesCombField()
{
    constexpr int defaultCombLength = 5;
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("acroform-comb-field.pdf"));
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

    const QString fieldId = formDesigner.addCombField(
        0,
        0.16,
        0.24,
        0.50,
        0.07);
    QVERIFY(!fieldId.isEmpty());
    QVERIFY(formDesigner.setFieldName(
        fieldId,
        QStringLiteral("VerificationCode")));
    QVERIFY(formDesigner.setFieldValue(
        fieldId,
        QStringLiteral("ABCDE")));

    QVariantMap field = formDesigner.fieldData(fieldId);
    QCOMPARE(
        field.value(QStringLiteral("type")).toInt(),
        PDFFormDesignerController::Text);
    QVERIFY(
        field.value(QStringLiteral("flags")).toInt()
        & PDFFormDesignerController::Comb);
    QVERIFY(
        !(field.value(QStringLiteral("flags")).toInt()
          & PDFFormDesignerController::Multiline));
    QCOMPARE(
        field.value(QStringLiteral("maxLength")).toInt(),
        defaultCombLength);

    QVERIFY(!formDesigner.setFieldFlags(
        fieldId,
        PDFFormDesignerController::Comb
            | PDFFormDesignerController::Multiline));
    field = formDesigner.fieldData(fieldId);
    QVERIFY(
        field.value(QStringLiteral("flags")).toInt()
        & PDFFormDesignerController::Comb);
    QVERIFY(
        !(field.value(QStringLiteral("flags")).toInt()
          & PDFFormDesignerController::Multiline));

    QVERIFY(formDesigner.setFieldMaxLength(fieldId, 0));
    field = formDesigner.fieldData(fieldId);
    QCOMPARE(field.value(QStringLiteral("maxLength")).toInt(), 0);
    QVERIFY(
        !(field.value(QStringLiteral("flags")).toInt()
          & PDFFormDesignerController::Comb));
    QVERIFY(formDesigner.setFieldMaxLength(
        fieldId,
        defaultCombLength));
    QVERIFY(formDesigner.setFieldFlags(
        fieldId,
        PDFFormDesignerController::Comb));

    QSignalSpy saveFinishedSpy(
        &editController,
        &PDFEditController::saveFinished);
    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 10000);
    QVERIFY(saveFinishedSpy.constFirst().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);

    field = formDesigner.fieldData(fieldId);
    QCOMPARE(
        field.value(QStringLiteral("type")).toInt(),
        PDFFormDesignerController::Text);
    QVERIFY(field.value(QStringLiteral("sourceBacked")).toBool());
    QVERIFY(!field.value(QStringLiteral("dirty")).toBool());
    QVERIFY(
        field.value(QStringLiteral("flags")).toInt()
        & PDFFormDesignerController::Comb);
    QVERIFY(
        !(field.value(QStringLiteral("flags")).toInt()
          & PDFFormDesignerController::Multiline));
    QCOMPARE(
        field.value(QStringLiteral("maxLength")).toInt(),
        defaultCombLength);

    const AcroFormFieldInspection inspection = inspectAcroFormField(
        sourcePath,
        QStringLiteral("VerificationCode"));
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.formType, FORMTYPE_ACRO_FORM);
    QCOMPARE(inspection.widgetCount, 1);
    QCOMPARE(inspection.fieldType, FPDF_FORMFIELD_TEXTFIELD);
    QVERIFY(!inspection.hasParent);
    QVERIFY(inspection.hasLocalFieldType);
    QVERIFY(inspection.hasLocalFieldFlags);
    QVERIFY(inspection.fieldFlags & kPdfTextComb);
    QVERIFY(inspection.fieldFlags & kPdfTextDoNotScroll);
    QVERIFY(!(inspection.fieldFlags & FPDF_FORMFLAG_TEXT_MULTILINE));
    QVERIFY(inspection.hasMaxLength);
    QCOMPARE(inspection.maxLength, float(defaultCombLength));
    QCOMPARE(inspection.name, QStringLiteral("VerificationCode"));
    QCOMPARE(inspection.value, QStringLiteral("ABCDE"));
    QVERIFY(inspection.hasAppearance);
    QVERIFY(inspection.pointHit);
}

void PDFEditControllerTest::rehydratesTextFieldAfterSamePathSave()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("acroform-same-path.pdf"));
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

    const QRectF initialFieldRect(0.18, 0.28, 0.42, 0.07);
    const QRectF blankFieldRect(0.18, 0.40, 0.42, 0.07);
    const QString fieldId = formDesigner.addTextField(
        0,
        initialFieldRect.x(),
        initialFieldRect.y(),
        initialFieldRect.width(),
        initialFieldRect.height());
    QVERIFY(!fieldId.isEmpty());
    QVERIFY(formDesigner.setFieldName(
        fieldId,
        QStringLiteral("PersistentText")));
    QVERIFY(formDesigner.setFieldValue(
        fieldId,
        QStringLiteral("First saved value")));
    QVERIFY(formDesigner.setFieldFlags(
        fieldId,
        PDFFormDesignerController::Required
            | PDFFormDesignerController::Multiline));
    QVERIFY(formDesigner.setFieldMaxLength(fieldId, 64));
    QVERIFY(!formDesigner.sourceTextFieldContainsPoint(
        0,
        initialFieldRect.center()));
    const QString blankFieldId = formDesigner.addTextField(
        0,
        blankFieldRect.x(),
        blankFieldRect.y(),
        blankFieldRect.width(),
        blankFieldRect.height());
    QVERIFY(!blankFieldId.isEmpty());
    QVERIFY(formDesigner.setFieldName(
        blankFieldId,
        QStringLiteral("PersistentBlank")));

    QSignalSpy saveFinishedSpy(
        &editController,
        &PDFEditController::saveFinished);
    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 10000);
    QCOMPARE(
        saveFinishedSpy.constFirst().constFirst().toBool(),
        true);
    QVERIFY(document.isLoaded());
    QCOMPARE(
        QFileInfo(document.source().toLocalFile()).absoluteFilePath(),
        QFileInfo(sourcePath).absoluteFilePath());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 2, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);
    QStringList sourceFieldIds
        = formDesigner.sourceTextFieldIdsForPage(0);
    sourceFieldIds.sort();
    QStringList expectedSourceFieldIds{fieldId, blankFieldId};
    expectedSourceFieldIds.sort();
    QCOMPARE(sourceFieldIds, expectedSourceFieldIds);
    QVERIFY(formDesigner.sourceTextFieldContainsPoint(
        0,
        initialFieldRect.center()));
    QVERIFY(formDesigner.sourceTextFieldContainsPoint(
        0,
        blankFieldRect.center()));
    QVERIFY(!formDesigner.sourceTextFieldContainsPoint(
        0,
        QPointF(0.9, 0.9)));

    QVariantMap hydrated = formDesigner.fieldData(fieldId);
    QCOMPARE(hydrated.value(QStringLiteral("id")).toString(), fieldId);
    QCOMPARE(
        hydrated.value(QStringLiteral("name")).toString(),
        QStringLiteral("PersistentText"));
    QCOMPARE(
        hydrated.value(QStringLiteral("value")).toString(),
        QStringLiteral("First saved value"));
    QVERIFY(hydrated.value(QStringLiteral("sourceBacked")).toBool());
    QVERIFY(!hydrated.value(QStringLiteral("dirty")).toBool());
    QVERIFY(
        hydrated.value(QStringLiteral("flags")).toInt()
        & PDFFormDesignerController::Required);
    QVERIFY(
        hydrated.value(QStringLiteral("flags")).toInt()
        & PDFFormDesignerController::Multiline);
    QCOMPARE(
        hydrated.value(QStringLiteral("maxLength")).toInt(),
        64);

    const AcroFormFieldInspection firstSave
        = inspectAcroFormField(
            sourcePath,
            QStringLiteral("PersistentText"));
    QVERIFY(firstSave.readable);
    QCOMPARE(firstSave.widgetCount, 2);
    QCOMPARE(firstSave.value, QStringLiteral("First saved value"));
    QVERIFY(firstSave.fieldFlags & FPDF_FORMFLAG_TEXT_MULTILINE);
    QVERIFY(firstSave.hasMaxLength);
    QCOMPARE(firstSave.maxLength, 64.0f);
    QVERIFY(firstSave.hasAppearance);
    QVERIFY(firstSave.pointHit);
    QVERIFY(firstSave.focusHandled);
    const AcroFormFieldInspection blankSave
        = inspectAcroFormField(
            sourcePath,
            QStringLiteral("PersistentBlank"));
    QCOMPARE(blankSave.widgetCount, 2);
    QCOMPARE(blankSave.value, QString());
    QVERIFY(blankSave.hasAppearance);
    QVERIFY(blankSave.pointHit);
    QVERIFY(blankSave.focusHandled);

    const QRectF resizedFieldRect(0.24, 0.20, 0.50, 0.10);
    const auto normalizedRectsMatch = [](const QRectF &actual,
                                         const QRectF &expected) {
        constexpr qreal tolerance = 0.00005;
        return qAbs(actual.x() - expected.x()) <= tolerance
            && qAbs(actual.y() - expected.y()) <= tolerance
            && qAbs(actual.width() - expected.width()) <= tolerance
            && qAbs(actual.height() - expected.height()) <= tolerance;
    };
    QVERIFY(formDesigner.setFieldValue(
        fieldId,
        QStringLiteral("Updated after reopen")));
    QVERIFY(formDesigner.setFieldMaxLength(fieldId, 0));
    QVERIFY(formDesigner.setTextFieldRect(
        fieldId,
        resizedFieldRect.x(),
        resizedFieldRect.y(),
        resizedFieldRect.width(),
        resizedFieldRect.height()));
    const QPointF exposedSourcePoint(0.20, 0.33);
    const QPointF resizedOnlyPoint(0.70, 0.25);
    QVERIFY(initialFieldRect.contains(exposedSourcePoint));
    QVERIFY(!resizedFieldRect.contains(exposedSourcePoint));
    QVERIFY(!initialFieldRect.contains(resizedOnlyPoint));
    QVERIFY(resizedFieldRect.contains(resizedOnlyPoint));
    QVERIFY(formDesigner.sourceTextFieldContainsPoint(
        0,
        exposedSourcePoint));
    QVERIFY(!formDesigner.sourceTextFieldContainsPoint(
        0,
        resizedOnlyPoint));
    const QVector<PDFFormDesignerController::SaveField> geometryChange
        = formDesigner.saveFields();
    QCOMPARE(geometryChange.size(), 1);
    QCOMPARE(
        geometryChange.constFirst().operation,
        PDFFormDesignerController::SaveField::Update);
    QCOMPARE(
        geometryChange.constFirst().changedProperties,
        static_cast<quint32>(
            PDFFormDesignerController::ValuePropertyChanged
            | PDFFormDesignerController::MaxLengthPropertyChanged
            | PDFFormDesignerController::RectPropertyChanged));
    QCOMPARE(
        geometryChange.constFirst().normalizedRect,
        resizedFieldRect);
    const QVector<PDFFormDesignerController::SaveField>
        fieldsWithAppearanceNormalization
        = formDesigner.saveFields(true);
    QCOMPARE(fieldsWithAppearanceNormalization.size(), 1);
    QCOMPARE(
        fieldsWithAppearanceNormalization.constFirst().id,
        fieldId);
    QVERIFY(formDesigner.hasDrafts());
    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 2, 10000);
    QCOMPARE(
        saveFinishedSpy.constLast().constFirst().toBool(),
        true);
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 2, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);
    QVERIFY(!formDesigner.sourceTextFieldContainsPoint(
        0,
        exposedSourcePoint));
    QVERIFY(formDesigner.sourceTextFieldContainsPoint(
        0,
        resizedOnlyPoint));

    hydrated = formDesigner.fieldData(fieldId);
    QCOMPARE(
        hydrated.value(QStringLiteral("value")).toString(),
        QStringLiteral("Updated after reopen"));
    QVERIFY(normalizedRectsMatch(
        hydrated.value(QStringLiteral("rect")).toRectF(),
        resizedFieldRect));
    QVERIFY(hydrated.value(QStringLiteral("sourceBacked")).toBool());
    QCOMPARE(
        hydrated.value(QStringLiteral("maxLength")).toInt(),
        0);
    const AcroFormFieldInspection secondSave
        = inspectAcroFormField(
            sourcePath,
            QStringLiteral("PersistentText"));
    QCOMPARE(secondSave.widgetCount, 2);
    QCOMPARE(secondSave.value, QStringLiteral("Updated after reopen"));
    QVERIFY(secondSave.fieldFlags & FPDF_FORMFLAG_TEXT_MULTILINE);
    QVERIFY(!secondSave.hasMaxLength);
    QVERIFY(secondSave.hasAppearance);
    QVERIFY(secondSave.pointHit);
    QVERIFY(secondSave.focusHandled);
    constexpr qreal rectTolerance = 0.05;
    QVERIFY(qAbs(
        secondSave.rect.left
        - resizedFieldRect.left() * 595.0) <= rectTolerance);
    QVERIFY(qAbs(
        secondSave.rect.right
        - resizedFieldRect.right() * 595.0) <= rectTolerance);
    QVERIFY(qAbs(
        secondSave.rect.bottom
        - (1.0 - resizedFieldRect.bottom()) * 842.0) <= rectTolerance);
    QVERIFY(qAbs(
        secondSave.rect.top
        - (1.0 - resizedFieldRect.top()) * 842.0) <= rectTolerance);

    QTRY_VERIFY_WITH_TIMEOUT(formController.javascriptEnabled(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !formController.javascriptTransitionPending(),
        5000);
    const QSize nativeEditSize(595, 842);
    const QPointF nativeEditCenter(
        resizedFieldRect.center().x() * nativeEditSize.width(),
        resizedFieldRect.center().y() * nativeEditSize.height());
    formController.pointerEvent(
        0,
        PDFFormWorker::PointerEventType::Press,
        0,
        nativeEditCenter,
        nativeEditSize);
    formController.pointerEvent(
        0,
        PDFFormWorker::PointerEventType::Release,
        0,
        nativeEditCenter,
        nativeEditSize);
    QTRY_VERIFY_WITH_TIMEOUT(formController.acceptsTextInput(), 5000);
    formController.selectAll();
    formController.replaceSelection(
        QStringLiteral("Native value must survive"));
    formController.keyEvent(FWL_VKEY_Back, 0, {});
    const QString nativeValueAfterBackspace
        = QStringLiteral("Native value must surviv");
    QTRY_VERIFY_WITH_TIMEOUT(formController.hasDrafts(), 5000);
    QCOMPARE(
        formDesigner.fieldData(fieldId)
            .value(QStringLiteral("value"))
            .toString(),
        QStringLiteral("Updated after reopen"));
    formDesigner.prepareForDesignMode();
    constexpr int tightenedMaxLength = 22;
    const QString limitedNativeValue
        = nativeValueAfterBackspace.first(tightenedMaxLength);
    QVERIFY(formDesigner.setFieldMaxLength(
        fieldId,
        tightenedMaxLength));
    QTRY_COMPARE_WITH_TIMEOUT(
        formDesigner.fieldData(fieldId)
            .value(QStringLiteral("value"))
            .toString(),
        limitedNativeValue,
        5000);
    QVERIFY(formDesigner.setFieldName(
        fieldId,
        QStringLiteral("PersistentTextRenamed")));
    const QVector<PDFFormDesignerController::SaveField> nameChange
        = formDesigner.saveFields();
    QCOMPARE(nameChange.size(), 1);
    QCOMPARE(nameChange.constFirst().operation,
             PDFFormDesignerController::SaveField::Update);
    QCOMPARE(
        nameChange.constFirst().changedProperties,
        static_cast<quint32>(
            PDFFormDesignerController::ValuePropertyChanged
            | PDFFormDesignerController::NamePropertyChanged
            | PDFFormDesignerController::MaxLengthPropertyChanged));
    QCOMPARE(nameChange.constFirst().maxLength, tightenedMaxLength);
    QCOMPARE(nameChange.constFirst().value, limitedNativeValue);

    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 3, 10000);
    QCOMPARE(
        saveFinishedSpy.constLast().constFirst().toBool(),
        true);
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 2, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);
    hydrated = formDesigner.fieldData(fieldId);
    QCOMPARE(
        hydrated.value(QStringLiteral("name")).toString(),
        QStringLiteral("PersistentTextRenamed"));
    QCOMPARE(
        hydrated.value(QStringLiteral("value")).toString(),
        limitedNativeValue);
    QCOMPARE(
        hydrated.value(QStringLiteral("maxLength")).toInt(),
        tightenedMaxLength);
    const AcroFormFieldInspection nativeValueSave
        = inspectAcroFormField(
            sourcePath,
            QStringLiteral("PersistentTextRenamed"));
    QCOMPARE(nativeValueSave.widgetCount, 2);
    QCOMPARE(
        nativeValueSave.value,
        limitedNativeValue);
    QVERIFY(nativeValueSave.hasMaxLength);
    QCOMPARE(
        nativeValueSave.maxLength,
        static_cast<float>(tightenedMaxLength));
    QVERIFY(nativeValueSave.hasAppearance);
    QVERIFY(nativeValueSave.pointHit);
    QVERIFY(nativeValueSave.focusHandled);

    const QSize renderSize(595, 842);
    const QImage pageWithoutForms = renderFirstPage(
        sourcePath,
        renderSize);
    const QImage pageWithForms = renderFirstPageWithForms(
        sourcePath,
        renderSize);
    const QImage pageWithDesignerWidgetsSuppressed
        = renderFirstPageWithForms(
            sourcePath,
            renderSize,
            {fieldId, blankFieldId});
    const QImage pageWithFocusedDesignerWidgetsSuppressed
        = renderFirstPageWithForms(
            sourcePath,
            renderSize,
            {fieldId, blankFieldId},
            resizedFieldRect.center());
    const QImage pageWithOneDesignerWidgetSuppressed
        = renderFirstPageWithForms(
            sourcePath,
            renderSize,
            {fieldId});
    QVERIFY(!pageWithoutForms.isNull());
    QVERIFY(!pageWithForms.isNull());
    QVERIFY(!pageWithDesignerWidgetsSuppressed.isNull());
    QVERIFY(!pageWithFocusedDesignerWidgetsSuppressed.isNull());
    QVERIFY(!pageWithOneDesignerWidgetSuppressed.isNull());
    const auto fieldArea = [&renderSize](const QRectF &normalizedRect) {
        return QRect(
            qRound(normalizedRect.x() * renderSize.width()),
            qRound(normalizedRect.y() * renderSize.height()),
            qRound(normalizedRect.width() * renderSize.width()),
            qRound(normalizedRect.height() * renderSize.height()));
    };
    QVERIFY(
        changedPixelCount(
            pageWithoutForms,
            pageWithForms,
            fieldArea(resizedFieldRect))
        > 100);
    QVERIFY(
        changedPixelCount(
            pageWithoutForms,
            pageWithForms,
            fieldArea(blankFieldRect))
        > 100);
    QCOMPARE(
        changedPixelCount(
            pageWithoutForms,
            pageWithDesignerWidgetsSuppressed,
            fieldArea(resizedFieldRect)),
        0);
    QCOMPARE(
        changedPixelCount(
            pageWithoutForms,
            pageWithDesignerWidgetsSuppressed,
            fieldArea(blankFieldRect)),
        0);
    QCOMPARE(
        changedPixelCount(
            pageWithoutForms,
            pageWithFocusedDesignerWidgetsSuppressed,
            fieldArea(resizedFieldRect).adjusted(-6, -6, 6, 6)),
        0);
    QCOMPARE(
        changedPixelCount(
            pageWithoutForms,
            pageWithOneDesignerWidgetSuppressed,
            fieldArea(resizedFieldRect)),
        0);
    QVERIFY(
        changedPixelCount(
            pageWithoutForms,
            pageWithOneDesignerWidgetSuppressed,
            fieldArea(blankFieldRect))
        > 100);
    const QRect appearanceBounds = changedPixelBounds(
        pageWithOneDesignerWidgetSuppressed,
        pageWithForms,
        pageWithForms.rect());
    QVERIFY(!appearanceBounds.isEmpty());
    const QRect expectedAppearanceBounds = fieldArea(resizedFieldRect);
    constexpr int appearanceEdgeTolerance = 2;
    QVERIFY(
        qAbs(
            appearanceBounds.left()
            - expectedAppearanceBounds.left())
        <= appearanceEdgeTolerance);
    QVERIFY(
        qAbs(
            appearanceBounds.top()
            - expectedAppearanceBounds.top())
        <= appearanceEdgeTolerance);
    QVERIFY(
        qAbs(
            appearanceBounds.right()
            - expectedAppearanceBounds.right())
        <= appearanceEdgeTolerance);
    QVERIFY(
        qAbs(
            appearanceBounds.bottom()
            - expectedAppearanceBounds.bottom())
        <= appearanceEdgeTolerance);
    constexpr int cornerExtent = 4;
    const QVector<QRect> cornerArms{
        QRect(appearanceBounds.left(),
              appearanceBounds.top(),
              cornerExtent,
              1),
        QRect(appearanceBounds.left(),
              appearanceBounds.top(),
              1,
              cornerExtent),
        QRect(appearanceBounds.right() - cornerExtent + 1,
              appearanceBounds.top(),
              cornerExtent,
              1),
        QRect(appearanceBounds.right(),
              appearanceBounds.top(),
              1,
              cornerExtent),
        QRect(appearanceBounds.left(),
              appearanceBounds.bottom(),
              cornerExtent,
              1),
        QRect(appearanceBounds.left(),
              appearanceBounds.bottom() - cornerExtent + 1,
              1,
              cornerExtent),
        QRect(appearanceBounds.right() - cornerExtent + 1,
              appearanceBounds.bottom(),
              cornerExtent,
              1),
        QRect(appearanceBounds.right(),
              appearanceBounds.bottom() - cornerExtent + 1,
              1,
              cornerExtent)};
    for (qsizetype arm = 0; arm < cornerArms.size(); ++arm) {
        const qsizetype changedArmPixels = changedPixelCount(
            pageWithoutForms,
            pageWithForms,
            cornerArms.at(arm));
        QVERIFY2(
            changedArmPixels >= cornerExtent - 1,
            qPrintable(QStringLiteral(
                "AcroForm text-field corner arm %1 contains only %2 "
                "changed pixels; its border must remain square.")
                           .arg(arm)
                           .arg(changedArmPixels)));
    }
    const QRect textBounds = darkChangedPixelBounds(
        pageWithoutForms,
        pageWithForms,
        fieldArea(resizedFieldRect).adjusted(4, 4, -4, -4));
    QVERIFY(!textBounds.isEmpty());
    QVERIFY2(
        textBounds.height() >= 7 && textBounds.height() <= 18,
        qPrintable(QStringLiteral(
            "The unfocused AcroForm appearance rendered text %1 px high; "
            "it must stay consistent with the 12 pt native editor.")
                       .arg(textBounds.height())));

    PDFDocumentSession freshSession;
    QVERIFY(freshSession.open(QUrl::fromLocalFile(sourcePath)));
    QTRY_COMPARE_WITH_TIMEOUT(
        freshSession.formDesignerController()->rowCount(),
        2,
        10000);
    const QVariantMap freshField
        = freshSession.formDesignerController()->fieldData(fieldId);
    QCOMPARE(
        freshField.value(QStringLiteral("value")).toString(),
        limitedNativeValue);
    QCOMPARE(
        freshField.value(QStringLiteral("name")).toString(),
        QStringLiteral("PersistentTextRenamed"));
    QVERIFY(normalizedRectsMatch(
        freshField.value(QStringLiteral("rect")).toRectF(),
        resizedFieldRect));
    QVERIFY(freshField.value(QStringLiteral("sourceBacked")).toBool());
    QVERIFY(!freshSession.formDesignerController()->hasDrafts());
    QVERIFY(freshSession.formDesignerController()->setFieldValue(
        fieldId,
        QStringLiteral("Discard this value")));
    QVERIFY(freshSession.formDesignerController()->hasDrafts());
    freshSession.editController()->discardDrafts();
    QTRY_COMPARE_WITH_TIMEOUT(
        freshSession.formDesignerController()->rowCount(),
        2,
        10000);
    QTRY_COMPARE_WITH_TIMEOUT(
        freshSession.formDesignerController()
            ->fieldData(fieldId)
            .value(QStringLiteral("value"))
            .toString(),
        limitedNativeValue,
        10000);
    QVERIFY(!freshSession.formDesignerController()->hasDrafts());

    PDFFormDesignerController *freshDesigner
        = freshSession.formDesignerController();
    PDFEditController *freshEditor = freshSession.editController();
    freshDesigner->selectTextField(fieldId);
    freshDesigner->setTextFieldFocused(fieldId, true);
    QVERIFY(freshDesigner->removeField(fieldId));
    QCOMPARE(freshDesigner->rowCount(), 1);
    QVERIFY(freshDesigner->fieldData(fieldId).isEmpty());
    QVERIFY(freshDesigner->selectedTextFieldId().isEmpty());
    QVERIFY(!freshDesigner->textFieldFocused());
    QVERIFY(freshDesigner->hasDrafts());
    const QStringList suppressedAfterRemoval
        = freshDesigner->sourceFieldIdsForPage(0);
    QCOMPARE(suppressedAfterRemoval.size(), 2);
    QVERIFY(suppressedAfterRemoval.contains(fieldId));
    QVERIFY(suppressedAfterRemoval.contains(blankFieldId));
    const QVector<PDFFormDesignerController::SaveField> removalSnapshot
        = freshDesigner->saveFields();
    QCOMPARE(removalSnapshot.size(), 1);
    QCOMPARE(removalSnapshot.constFirst().id, fieldId);
    QCOMPARE(removalSnapshot.constFirst().type,
             PDFFormDesignerController::Text);
    QCOMPARE(removalSnapshot.constFirst().operation,
             PDFFormDesignerController::SaveField::Remove);
    QCOMPARE(removalSnapshot.constFirst().page, 0);

    freshEditor->discardDrafts();
    QTRY_COMPARE_WITH_TIMEOUT(freshDesigner->rowCount(), 2, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(
        freshDesigner->fieldData(fieldId)
            .value(QStringLiteral("sourceBacked"))
            .toBool(),
        10000);
    QVERIFY(!freshDesigner->hasDrafts());

    QVERIFY(freshDesigner->removeField(fieldId));
    QSignalSpy freshSaveFinishedSpy(
        freshEditor,
        &PDFEditController::saveFinished);
    QVERIFY(freshEditor->requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(freshSaveFinishedSpy.count(), 1, 10000);
    QVERIFY(freshSaveFinishedSpy.constFirst().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(freshDesigner->rowCount(), 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!freshDesigner->hasDrafts(), 10000);
    QVERIFY(freshDesigner->fieldData(fieldId).isEmpty());
    const QVariantMap remainingBlankField
        = freshDesigner->fieldData(blankFieldId);
    QCOMPARE(
        remainingBlankField.value(QStringLiteral("name")).toString(),
        QStringLiteral("PersistentBlank"));
    QVERIFY(
        remainingBlankField.value(QStringLiteral("sourceBacked")).toBool());

    const AcroFormFieldInspection removedTextField
        = inspectAcroFormField(
            sourcePath,
            QStringLiteral("PersistentTextRenamed"));
    QVERIFY(removedTextField.readable);
    QCOMPARE(removedTextField.widgetCount, 1);
    QCOMPARE(
        removedTextField.fieldType,
        FPDF_FORMFIELD_UNKNOWN);
    const AcroFormFieldInspection remainingTextField
        = inspectAcroFormField(
            sourcePath,
            QStringLiteral("PersistentBlank"));
    QVERIFY(remainingTextField.readable);
    QCOMPARE(remainingTextField.widgetCount, 1);
    QCOMPARE(
        remainingTextField.fieldType,
        FPDF_FORMFIELD_TEXTFIELD);
    QVERIFY(remainingTextField.hasAppearance);
    QVERIFY(remainingTextField.pointHit);
    QVERIFY(remainingTextField.focusHandled);
}

void PDFEditControllerTest::normalizesLegacyTextFieldAppearances()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("legacy-appearance-source.pdf"));
    const QString legacyPath = m_temporaryDirectory.filePath(
        QStringLiteral("legacy-appearance.pdf"));
    const QString normalizedPath = m_temporaryDirectory.filePath(
        QStringLiteral("normalized-appearance.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument sourceDocument;
    QVERIFY(sourceDocument.open(QUrl::fromLocalFile(sourcePath)));
    PDFFormDesignerController sourceDesigner;
    sourceDesigner.setDocument(&sourceDocument);
    PDFEditController sourceEditor;
    sourceEditor.setDocument(&sourceDocument);
    sourceEditor.setFormDesignerController(&sourceDesigner);

    const QString editedFieldId = sourceDesigner.addTextField(
        0,
        0.1,
        0.2,
        0.35,
        0.07);
    const QString legacyFieldId = sourceDesigner.addTextField(
        0,
        0.55,
        0.2,
        0.35,
        0.07);
    QVERIFY(!editedFieldId.isEmpty());
    QVERIFY(!legacyFieldId.isEmpty());
    QVERIFY(sourceDesigner.setFieldName(
        editedFieldId,
        QStringLiteral("EditedPeer")));
    QVERIFY(sourceDesigner.setFieldName(
        legacyFieldId,
        QStringLiteral("LegacyPeer")));
    QVERIFY(sourceDesigner.setFieldValue(
        editedFieldId,
        QStringLiteral("Edited value")));
    QVERIFY(sourceDesigner.setFieldValue(
        legacyFieldId,
        QStringLiteral("Legacy value")));
    const PDFSaveWriter::Result initialSave = PDFSaveWriter::save(
        sourceEditor,
        legacyPath);
    QVERIFY2(initialSave.succeeded(), qPrintable(initialSave.detail));

    QFile legacyFile(legacyPath);
    QVERIFY(legacyFile.open(QIODevice::ReadWrite));
    QByteArray legacyBytes = legacyFile.readAll();
    const QByteArray appearanceMarker("/TrePDFApVersion 1");
    const qsizetype firstMarker = legacyBytes.indexOf(appearanceMarker);
    const qsizetype secondMarker = legacyBytes.indexOf(
        appearanceMarker,
        firstMarker + appearanceMarker.size());
    QVERIFY(firstMarker >= 0);
    QVERIFY(secondMarker > firstMarker);
    const QByteArray legacyMaxLength
        = QByteArrayLiteral("/MaxLen 12")
              .leftJustified(appearanceMarker.size(), ' ');
    QCOMPARE(legacyMaxLength.size(), appearanceMarker.size());
    legacyBytes.replace(
        secondMarker,
        appearanceMarker.size(),
        legacyMaxLength);
    QVERIFY(legacyFile.seek(0));
    QCOMPARE(legacyFile.write(legacyBytes), legacyBytes.size());
    QVERIFY(legacyFile.resize(legacyBytes.size()));
    legacyFile.close();

    PDFDocument legacyDocument;
    PDFFormController legacyFormController;
    PDFFormDesignerController legacyDesigner;
    PDFEditController legacyEditor;
    legacyFormController.setDocument(&legacyDocument);
    legacyDesigner.setFormController(&legacyFormController);
    legacyDesigner.setDocument(&legacyDocument);
    legacyEditor.setDocument(&legacyDocument);
    legacyEditor.setFormController(&legacyFormController);
    legacyEditor.setFormDesignerController(&legacyDesigner);
    QVERIFY(legacyDocument.open(QUrl::fromLocalFile(legacyPath)));
    QTRY_COMPARE_WITH_TIMEOUT(legacyDesigner.rowCount(), 2, 10000);

    QVERIFY(legacyDesigner.setFieldValue(
        editedFieldId,
        QStringLiteral("Trigger normalization")));
    const QVector<PDFFormDesignerController::SaveField> saveFields
        = legacyDesigner.saveFields(true);
    QCOMPARE(saveFields.size(), 2);
    const auto legacyField = std::find_if(
        saveFields.cbegin(),
        saveFields.cend(),
        [&legacyFieldId](
            const PDFFormDesignerController::SaveField &field) {
            return field.id == legacyFieldId;
        });
    QVERIFY(legacyField != saveFields.cend());
    QCOMPARE(
        legacyField->changedProperties,
        static_cast<quint32>(
            PDFFormDesignerController::NoPropertyChanges));

    const PDFSaveWriter::Result normalizedSave = PDFSaveWriter::save(
        legacyEditor,
        normalizedPath);
    QVERIFY2(
        normalizedSave.succeeded(),
        qPrintable(normalizedSave.detail));
    const AcroFormFieldInspection normalizedLegacyField
        = inspectAcroFormField(
            normalizedPath,
            QStringLiteral("LegacyPeer"));
    QVERIFY(normalizedLegacyField.readable);
    QVERIFY(normalizedLegacyField.hasAppearance);
    QVERIFY(normalizedLegacyField.hasMaxLength);
    QCOMPARE(normalizedLegacyField.maxLength, 12.0f);

    PDFDocument reopenedDocument;
    PDFFormController reopenedFormController;
    PDFFormDesignerController reopenedDesigner;
    reopenedFormController.setDocument(&reopenedDocument);
    reopenedDesigner.setFormController(&reopenedFormController);
    reopenedDesigner.setDocument(&reopenedDocument);
    QVERIFY(reopenedDocument.open(
        QUrl::fromLocalFile(normalizedPath)));
    QTRY_COMPARE_WITH_TIMEOUT(reopenedDesigner.rowCount(), 2, 10000);
    QVERIFY(reopenedDesigner.setFieldValue(
        editedFieldId,
        QStringLiteral("Current appearance")));
    const QVector<PDFFormDesignerController::SaveField>
        currentAppearanceFields = reopenedDesigner.saveFields(true);
    QCOMPARE(currentAppearanceFields.size(), 1);
    QCOMPARE(currentAppearanceFields.constFirst().id, editedFieldId);
}

void PDFEditControllerTest::addsTextFieldToExistingAcroForm()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("existing-acroform-source.pdf"));
    const QString savedPath = m_temporaryDirectory.filePath(
        QStringLiteral("existing-acroform-saved.pdf"));
    const QString resavedPath = m_temporaryDirectory.filePath(
        QStringLiteral("existing-acroform-resaved.pdf"));
    QVERIFY(createChoiceFieldsPdf(sourcePath));

    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    PDFFormDesignerController formDesigner;
    formDesigner.setDocument(&document);
    PDFEditController editController;
    editController.setDocument(&document);
    editController.setFormDesignerController(&formDesigner);

    const QString fieldId = formDesigner.addTextField(
        0,
        0.55,
        0.75,
        0.35,
        0.08);
    QVERIFY(!fieldId.isEmpty());
    QVERIFY(formDesigner.setFieldName(
        fieldId,
        QStringLiteral("AdditionalText")));
    QVERIFY(formDesigner.setFieldValue(
        fieldId,
        QStringLiteral("Preserved choices")));

    const PDFSaveWriter::Result result = PDFSaveWriter::save(
        editController,
        savedPath);
    QVERIFY2(result.succeeded(), qPrintable(result.detail));
    QVERIFY(QFileInfo::exists(savedPath));

    const AcroFormFieldInspection addedField
        = inspectAcroFormField(
            savedPath,
            QStringLiteral("AdditionalText"));
    QVERIFY(addedField.readable);
    QCOMPARE(addedField.formType, FORMTYPE_ACRO_FORM);
    QCOMPARE(addedField.widgetCount, 7);
    QCOMPARE(addedField.fieldType, FPDF_FORMFIELD_TEXTFIELD);
    QCOMPARE(addedField.name, QStringLiteral("AdditionalText"));
    QCOMPARE(addedField.value, QStringLiteral("Preserved choices"));
    QVERIFY(addedField.hasAppearance);
    QVERIFY(addedField.pointHit);
    QVERIFY(addedField.focusHandled);

    PDFDocument reopened;
    PDFFormController formController;
    formController.setDocument(&reopened);
    QVERIFY(reopened.open(QUrl::fromLocalFile(savedPath)));
    QVERIFY(formController.interactive());
    QTRY_VERIFY_WITH_TIMEOUT(formController.javascriptEnabled(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !formController.javascriptTransitionPending(),
        5000);
    QVector<PDFFormWorker::ChoiceField> fields;
    bool inspectionCompleted = false;
    formController.requestChoiceFields(
        0,
        [&fields, &inspectionCompleted](
            QVector<PDFFormWorker::ChoiceField> inspectedFields) {
            fields = std::move(inspectedFields);
            inspectionCompleted = true;
        });
    QTRY_VERIFY_WITH_TIMEOUT(inspectionCompleted, 5000);
    QCOMPARE(fields.size(), 3);
    QCOMPARE(
        fields.at(0).options,
        QStringList({
            QStringLiteral("Airplane"),
            QStringLiteral("Bicycle"),
            QStringLiteral("Car"),
        }));
    QCOMPARE(fields.at(0).selectedIndices, QVector<int>({0}));
    QCOMPARE(
        fields.at(1).options,
        QStringList({
            QStringLiteral("March"),
            QStringLiteral("April"),
            QStringLiteral("May"),
            QStringLiteral("June"),
        }));
    QCOMPARE(fields.at(1).selectedIndices, QVector<int>({1}));
    QCOMPARE(
        fields.at(2).options,
        QStringList({
            QStringLiteral("Red"),
            QStringLiteral("Yellow"),
            QStringLiteral("Blue"),
        }));
    QCOMPARE(fields.at(2).selectedIndices, QVector<int>({0, 2}));

    const QSize deviceSize(360, 480);
    const QPointF fieldCenter(261.0, 379.2);
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
    QTRY_VERIFY_WITH_TIMEOUT(formController.acceptsTextInput(), 5000);
    formController.selectAll();
    formController.replaceSelection(
        QStringLiteral("Edited after reopen"));
    QTRY_VERIFY_WITH_TIMEOUT(formController.hasDrafts(), 5000);

    PDFFormDesignerController reopenedFormDesigner;
    reopenedFormDesigner.setDocument(&reopened);
    const QString secondFieldId = reopenedFormDesigner.addTextField(
        0,
        0.55,
        0.67,
        0.35,
        0.06);
    QVERIFY(!secondFieldId.isEmpty());
    QVERIFY(reopenedFormDesigner.setFieldName(
        secondFieldId,
        QStringLiteral("SecondAdditionalText")));
    QVERIFY(reopenedFormDesigner.setFieldValue(
        secondFieldId,
        QStringLiteral("Second pass")));

    PDFEditController reopenedEditController;
    reopenedEditController.setDocument(&reopened);
    reopenedEditController.setFormController(&formController);
    reopenedEditController.setFormDesignerController(
        &reopenedFormDesigner);
    QVERIFY(reopenedEditController.hasDrafts());
    QSignalSpy saveFinishedSpy(
        &reopenedEditController,
        &PDFEditController::saveFinished);
    QVERIFY(reopenedEditController.requestSaveAs(
        QUrl::fromLocalFile(resavedPath)));
    QVERIFY(reopenedEditController.saving());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 10000);
    QCOMPARE(
        saveFinishedSpy.constFirst().constFirst().toBool(),
        true);
    QTRY_VERIFY_WITH_TIMEOUT(!reopenedEditController.saving(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        QFileInfo::exists(resavedPath)
            && QFileInfo(resavedPath).size() > 0,
        5000);

    const AcroFormFieldInspection editedField
        = inspectAcroFormField(
            resavedPath,
            QStringLiteral("AdditionalText"));
    QVERIFY(editedField.readable);
    QCOMPARE(editedField.widgetCount, 8);
    QCOMPARE(editedField.fieldType, FPDF_FORMFIELD_TEXTFIELD);
    QCOMPARE(editedField.value, QStringLiteral("Edited after reopen"));
    const AcroFormFieldInspection secondAddedField
        = inspectAcroFormField(
            resavedPath,
            QStringLiteral("SecondAdditionalText"));
    QCOMPARE(secondAddedField.widgetCount, 8);
    QCOMPARE(secondAddedField.fieldType, FPDF_FORMFIELD_TEXTFIELD);
    QCOMPARE(secondAddedField.value, QStringLiteral("Second pass"));
    QVERIFY(secondAddedField.hasAppearance);
    QVERIFY(secondAddedField.pointHit);
    QVERIFY(secondAddedField.focusHandled);

    PDFDocument twiceReopened;
    PDFFormController twiceReopenedFormController;
    twiceReopenedFormController.setDocument(&twiceReopened);
    QVERIFY(twiceReopened.open(QUrl::fromLocalFile(resavedPath)));
    fields.clear();
    inspectionCompleted = false;
    twiceReopenedFormController.requestChoiceFields(
        0,
        [&fields, &inspectionCompleted](
            QVector<PDFFormWorker::ChoiceField> inspectedFields) {
            fields = std::move(inspectedFields);
            inspectionCompleted = true;
        });
    QTRY_VERIFY_WITH_TIMEOUT(inspectionCompleted, 5000);
    QCOMPARE(fields.size(), 3);
    QCOMPARE(
        fields.at(0).options,
        QStringList({
            QStringLiteral("Airplane"),
            QStringLiteral("Bicycle"),
            QStringLiteral("Car"),
        }));
    QCOMPARE(fields.at(0).selectedIndices, QVector<int>({0}));
    QCOMPARE(
        fields.at(1).options,
        QStringList({
            QStringLiteral("March"),
            QStringLiteral("April"),
            QStringLiteral("May"),
            QStringLiteral("June"),
        }));
    QCOMPARE(fields.at(1).selectedIndices, QVector<int>({1}));
    QCOMPARE(
        fields.at(2).options,
        QStringList({
            QStringLiteral("Red"),
            QStringLiteral("Yellow"),
            QStringLiteral("Blue"),
        }));
    QCOMPARE(fields.at(2).selectedIndices, QVector<int>({0, 2}));
}

void PDFEditControllerTest::rejectsDuplicateAcroFormFieldNames()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("duplicate-acroform-name-source.pdf"));
    const QString savedPath = m_temporaryDirectory.filePath(
        QStringLiteral("duplicate-acroform-name-saved.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    PDFFormDesignerController formDesigner;
    formDesigner.setDocument(&document);
    PDFEditController editController;
    editController.setDocument(&document);
    editController.setFormDesignerController(&formDesigner);

    const QString firstFieldId = formDesigner.addTextField(
        0,
        0.1,
        0.2,
        0.3,
        0.08);
    const QString secondFieldId = formDesigner.addTextField(
        0,
        0.55,
        0.2,
        0.3,
        0.08);
    QVERIFY(!firstFieldId.isEmpty());
    QVERIFY(!secondFieldId.isEmpty());
    QVERIFY(formDesigner.setFieldName(
        firstFieldId,
        QStringLiteral("DuplicateName")));
    QVERIFY(formDesigner.setFieldName(
        secondFieldId,
        QStringLiteral("DuplicateName")));

    const PDFSaveWriter::Result result = PDFSaveWriter::save(
        editController,
        savedPath);
    QCOMPARE(result.error, PDFSaveWriter::Error::AcroFormCreationFailed);
    QVERIFY(!result.detail.isEmpty());
    QVERIFY(!QFileInfo::exists(savedPath));
}
