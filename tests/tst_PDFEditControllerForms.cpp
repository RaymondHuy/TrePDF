#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"
#include "PDFAcroFormCanonicalizer.h"

#include <QCryptographicHash>

using namespace PDFEditControllerTestSupport;

void PDFEditControllerTest::keepsAcroFormFontResourcesConsistentAcrossFontChanges()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("form-font-fingerprint.pdf"));
    QVERIFY(writePdfObjects(path, {
        QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
        QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
        QByteArray("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] "
                   "/Resources << >> /Annots [4 0 R 5 0 R] >>"),
        QByteArray("<< /Type /Annot /Subtype /Square /Rect [40 700 240 730] /NM (text-field-1) >>"),
        QByteArray("<< /Type /Annot /Subtype /Square /Rect [40 650 240 680] /NM (text-field-2) >>"),
    }));
    const auto regular = ApplicationFonts::downloadedFontFace(testFontFamily(), false, false);
    const auto bold = ApplicationFonts::downloadedFontFace(testFontFamily(), true, false);
    QVERIFY(regular.isValid());
    QVERIFY(bold.isValid());
    QVERIFY(regular.data != bold.data);

    PDFAcroFormCanonicalizer::Field first;
    first.widgetId = QStringLiteral("text-field-1");
    first.fieldName = QStringLiteral("First");
    first.value = QStringLiteral("Font example");
    first.defaultValue = first.value;
    first.pageRect = QRectF(40, 700, 200, 30);
    auto second = first;
    second.widgetId = QStringLiteral("text-field-2");
    second.fieldName = QStringLiteral("Second");
    second.pageRect.moveTop(650);
    QString error;
    QVERIFY2(PDFAcroFormCanonicalizer::finalizeFields(path, {first, second}, regular, error),
             qPrintable(error));

    const auto readFile = [](const QString &filePath) {
        QFile file(filePath);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    const auto defaultAppearance = [](const QByteArray &pdf, int objectNumber) {
        const QByteArray objectToken = '\n' + QByteArray::number(objectNumber) + " 0 obj";
        const qsizetype start = pdf.lastIndexOf(objectToken);
        if (start < 0)
            return QByteArray();
        const QByteArray object = pdf.mid(start, pdf.indexOf("endobj", start) - start);
        const qsizetype appearanceStart = object.indexOf("/DA (");
        if (appearanceStart < 0)
            return QByteArray();
        const qsizetype valueStart = appearanceStart + 5;
        return object.mid(valueStart, object.indexOf(')', valueStart) - valueStart);
    };
    const QByteArray original = readFile(path);
    const QByteArray originalAppearance = defaultAppearance(original, 4);
    QVERIFY(originalAppearance.startsWith("/TrePDFUF"));
    QCOMPARE(defaultAppearance(original, 5), originalAppearance);
    QCOMPARE(original.count("/Subtype /Type0"), 1);

    first.operation = PDFAcroFormCanonicalizer::Operation::NormalizeAppearance;
    QVERIFY2(PDFAcroFormCanonicalizer::finalizeFields(path, {first}, bold, error), qPrintable(error));
    const QByteArray changed = readFile(path);
    const QByteArray changedAppearance = defaultAppearance(changed, 4);
    QVERIFY(changedAppearance.startsWith("/TrePDFUF"));
    QVERIFY(changedAppearance != originalAppearance);
    QCOMPARE(defaultAppearance(changed, 5), originalAppearance);
    QCOMPARE(changed.count("/Subtype /Type0"), 2);
    const QByteArray boldFingerprint = QCryptographicHash::hash(bold.data, QCryptographicHash::Sha256);
    QVERIFY(changed.contains("/TrePDFFontSHA256 <" + boldFingerprint.toHex() + '>'));

    // The identical face reuses its resource instead of growing the PDF on each save.
    QVERIFY2(PDFAcroFormCanonicalizer::finalizeFields(path, {first}, bold, error), qPrintable(error));
    const QByteArray repeated = readFile(path);
    QCOMPARE(defaultAppearance(repeated, 4), changedAppearance);
    QCOMPARE(repeated.count("/Subtype /Type0"), 2);

    // Old TrePDF fonts remain referenced by untouched fields even without a digest.
    const QString legacyPath = m_temporaryDirectory.filePath(QStringLiteral("form-font-legacy.pdf"));
    QByteArray legacy = original;
    const QByteArray regularFingerprint = QCryptographicHash::hash(regular.data, QCryptographicHash::Sha256);
    const QByteArray marker = "/TrePDFFontSHA256 <" + regularFingerprint.toHex() + '>';
    QVERIFY(legacy.contains(marker));
    legacy.replace(marker, QByteArray(marker.size(), ' '));
    QFile legacyFile(legacyPath);
    QVERIFY(legacyFile.open(QIODevice::WriteOnly));
    QCOMPARE(legacyFile.write(legacy), legacy.size());
    legacyFile.close();
    QVERIFY2(PDFAcroFormCanonicalizer::finalizeFields(legacyPath, {first}, regular, error), qPrintable(error));
    const QByteArray migrated = readFile(legacyPath);
    QCOMPARE(migrated.count("/Subtype /Type0"), 2);
    QVERIFY(defaultAppearance(migrated, 4) != originalAppearance);
    QCOMPARE(defaultAppearance(migrated, 5), originalAppearance);
    const AcroFormFieldInspection inspection = inspectAcroFormField(legacyPath, QStringLiteral("First"));
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.value, first.value);
}

void PDFEditControllerTest::storesTransientTextFieldsByPage()
{
    PDFFormDesignerController controller;
    QSignalSpy fieldsChangedSpy(&controller,
                                &PDFFormDesignerController::textFieldsChanged);
    QSignalSpy focusChangedSpy(
        &controller,
        &PDFFormDesignerController::textFieldFocusedChanged);
    QSignalSpy selectionChangedSpy(
        &controller,
        &PDFFormDesignerController::textFieldSelectionChanged);
    QSignalSpy rowsInsertedSpy(&controller,
                               &QAbstractItemModel::rowsInserted);
    QSignalSpy rowsRemovedSpy(&controller,
                              &QAbstractItemModel::rowsRemoved);
    QSignalSpy dataChangedSpy(&controller,
                              &QAbstractItemModel::dataChanged);

    const QString fieldId = controller.addTextField(2, 0.1, 0.2, 0.4, 0.08);
    QVERIFY(!fieldId.isEmpty());
    QCOMPARE(controller.rowCount(), 1);
    QCOMPARE(rowsInsertedSpy.count(), 1);
    QCOMPARE(controller.roleNames().value(PDFFormDesignerController::IdRole),
             QByteArray("id"));
    QCOMPARE(controller.roleNames().value(PDFFormDesignerController::TypeRole),
             QByteArray("type"));
    const QModelIndex fieldIndex = controller.index(0);
    QCOMPARE(controller.data(fieldIndex,
                             PDFFormDesignerController::IdRole).toString(),
             fieldId);
    QCOMPARE(controller.data(fieldIndex,
                             PDFFormDesignerController::TypeRole).toInt(),
             PDFFormDesignerController::Text);
    QCOMPARE(controller.data(fieldIndex,
                             PDFFormDesignerController::PageRole).toInt(),
             2);
    QCOMPARE(fieldsChangedSpy.count(), 1);
    QCOMPARE(fieldsChangedSpy.takeFirst().at(0).toInt(), 2);
    QCOMPARE(controller.textFieldsForPage(1).size(), 0);

    QVariantList fields = controller.textFieldsForPage(2);
    QCOMPARE(fields.size(), 1);
    const QVariantMap field = fields.constFirst().toMap();
    QCOMPARE(field.value(QStringLiteral("id")).toString(), fieldId);
    QCOMPARE(field.value(QStringLiteral("x")).toReal(), 0.1);
    QCOMPARE(field.value(QStringLiteral("y")).toReal(), 0.2);
    QCOMPARE(field.value(QStringLiteral("width")).toReal(), 0.4);
    QCOMPARE(field.value(QStringLiteral("height")).toReal(), 0.08);

    controller.setTextFieldFocused(fieldId, true);
    controller.selectTextField(fieldId);
    QVERIFY(controller.textFieldFocused());
    QCOMPARE(controller.focusedTextFieldId(), fieldId);
    QCOMPARE(focusChangedSpy.count(), 1);
    QCOMPARE(controller.selectedTextFieldId(), fieldId);
    QCOMPARE(selectionChangedSpy.count(), 1);

    const QString secondFieldId
        = controller.addTextField(2, 0.2, 0.4, 0.25, 0.06);
    QVERIFY(!secondFieldId.isEmpty());
    QCOMPARE(rowsInsertedSpy.count(), 2);
    controller.setTextFieldFocused(secondFieldId, true);
    QCOMPARE(controller.focusedTextFieldId(), secondFieldId);
    QCOMPARE(focusChangedSpy.count(), 2);
    controller.setTextFieldFocused(fieldId, true);
    QCOMPARE(controller.focusedTextFieldId(), fieldId);
    QCOMPARE(focusChangedSpy.count(), 3);
    QVERIFY(controller.removeTextField(secondFieldId));

    controller.setTextFieldText(fieldId, QStringLiteral("Sample value"));
    QCOMPARE(dataChangedSpy.count(), 1);
    fields = controller.textFieldsForPage(2);
    QCOMPARE(fields.constFirst().toMap().value(QStringLiteral("text")).toString(),
             QStringLiteral("Sample value"));

    QVERIFY(controller.setTextFieldRect(fieldId, 0.15, 0.25, 0.5, 0.1));
    QCOMPARE(dataChangedSpy.count(), 2);
    fields = controller.textFieldsForPage(2);
    QCOMPARE(fields.constFirst().toMap().value(QStringLiteral("x")).toReal(),
             0.15);
    QCOMPARE(fields.constFirst().toMap().value(QStringLiteral("y")).toReal(),
             0.25);
    QCOMPARE(fields.constFirst().toMap().value(QStringLiteral("width")).toReal(),
             0.5);
    QCOMPARE(fields.constFirst().toMap().value(QStringLiteral("height")).toReal(),
             0.1);

    QVERIFY(controller.setFieldFlags(
        fieldId,
        PDFFormDesignerController::Required
            | PDFFormDesignerController::Multiline));
    QVERIFY(controller.setFieldMaxLength(fieldId, 6));
    controller.setTextFieldText(fieldId, QStringLiteral("ab\ncdef"));
    QVariantMap configuredTextField = controller.fieldData(fieldId);
    QCOMPARE(
        configuredTextField.value(QStringLiteral("flags")).toInt(),
        PDFFormDesignerController::Required
            | PDFFormDesignerController::Multiline);
    QCOMPARE(
        configuredTextField.value(QStringLiteral("maxLength")).toInt(),
        6);
    QCOMPARE(
        configuredTextField.value(QStringLiteral("value")).toString(),
        QStringLiteral("ab\ncde"));
    QVERIFY(controller.setFieldFlags(
        fieldId,
        PDFFormDesignerController::Required));
    QCOMPARE(
        controller.fieldData(fieldId)
            .value(QStringLiteral("value")).toString(),
        QStringLiteral("ab cde"));
    QVERIFY(controller.setFieldMaxLength(fieldId, 0));
    QCOMPARE(
        controller.fieldData(fieldId)
            .value(QStringLiteral("maxLength")).toInt(),
        0);

    QVERIFY(controller.removeTextField(fieldId));
    QCOMPARE(rowsRemovedSpy.count(), 2);
    QVERIFY(!controller.textFieldFocused());
    QCOMPARE(focusChangedSpy.count(), 4);
    QVERIFY(controller.selectedTextFieldId().isEmpty());
    QCOMPARE(selectionChangedSpy.count(), 2);
    QCOMPARE(controller.textFieldsForPage(2).size(), 0);

    controller.clear();
    QVERIFY(!controller.textFieldFocused());
    QCOMPARE(focusChangedSpy.count(), 4);
    QCOMPARE(controller.textFieldsForPage(2).size(), 0);

    const QString checkboxId = controller.addField(
        PDFFormDesignerController::Checkbox,
        0,
        0.1,
        0.1,
        0.04,
        0.04);
    QVERIFY(!checkboxId.isEmpty());
    QVERIFY(controller.setFieldName(checkboxId,
                                    QStringLiteral("AcceptTerms")));
    QVERIFY(controller.setFieldOptions(
        checkboxId,
        {QStringLiteral("Yes"), QStringLiteral("Off")}));
    QVERIFY(controller.setFieldFlags(
        checkboxId,
        PDFFormDesignerController::Required
            | PDFFormDesignerController::ReadOnly));
    QVERIFY(controller.setFieldValue(checkboxId, QStringLiteral("Yes")));
    const QVariantMap checkbox = controller.fieldData(checkboxId);
    QCOMPARE(checkbox.value(QStringLiteral("type")).toInt(),
             PDFFormDesignerController::Checkbox);
    QCOMPARE(checkbox.value(QStringLiteral("name")).toString(),
             QStringLiteral("AcceptTerms"));
    QCOMPARE(checkbox.value(QStringLiteral("options")).toStringList(),
             QStringList({QStringLiteral("Yes"), QStringLiteral("Off")}));
    QCOMPARE(checkbox.value(QStringLiteral("flags")).toInt(),
             PDFFormDesignerController::Required
                 | PDFFormDesignerController::ReadOnly);
    QVERIFY(checkbox.value(QStringLiteral("dirty")).toBool());
    QCOMPARE(controller.fieldsForPage(0).size(), 1);
    QCOMPARE(controller.textFieldsForPage(0).size(), 0);
    QVERIFY(controller.removeTextField(checkboxId));

    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("form-designer-pages.pdf"));
    QVERIFY(createPushButtonActionPdf(sourcePath));

    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    PDFEditController editController;
    editController.setDocument(&document);
    controller.setDocument(&document);

    const QString sourceFieldId
        = controller.addTextField(1, 0.1, 0.2, 0.4, 0.08);
    QVERIFY(!sourceFieldId.isEmpty());
    document.setCurrentPage(0);
    QCOMPARE(editController.addBlankPage(), 1);
    QCOMPARE(controller.textFieldsForPage(1).size(), 0);
    fields = controller.textFieldsForPage(2);
    QCOMPARE(fields.size(), 1);
    QCOMPARE(fields.constFirst().toMap().value(QStringLiteral("id")).toString(),
             sourceFieldId);

    const QString transientFieldId
        = controller.addTextField(1, 0.15, 0.25, 0.35, 0.07);
    QVERIFY(!transientFieldId.isEmpty());
    editController.undo();
    fields = controller.textFieldsForPage(1);
    QCOMPARE(fields.size(), 1);
    QCOMPARE(fields.constFirst().toMap().value(QStringLiteral("id")).toString(),
             sourceFieldId);
}

void PDFEditControllerTest::limitsTransientTextFieldInputLength()
{
    constexpr int maximumInputLength = 300000;
    PDFFormDesignerController controller;
    QCOMPARE(
        controller.maximumTextFieldInputLength(),
        maximumInputLength);

    const QString fieldId = controller.addTextField(
        0,
        0.1,
        0.1,
        0.4,
        0.2);
    QVERIFY(!fieldId.isEmpty());
    QVERIFY(controller.setFieldFlags(
        fieldId,
        PDFFormDesignerController::Multiline));

    QString oversizedValue(
        maximumInputLength + 1,
        QLatin1Char('a'));
    oversizedValue[100] = QLatin1Char('\n');
    controller.setTextFieldText(fieldId, oversizedValue);

    const QString acceptedValue = controller.fieldData(fieldId)
                                      .value(QStringLiteral("value"))
                                      .toString();
    QCOMPARE(acceptedValue.size(), maximumInputLength);
    QCOMPARE(acceptedValue.at(100), QLatin1Char('\n'));
    QCOMPARE(
        controller.fieldData(fieldId)
            .value(QStringLiteral("maxLength")).toInt(),
        0);

    QString surrogateBoundaryValue(
        maximumInputLength - 1,
        QLatin1Char('b'));
    surrogateBoundaryValue.append(
        QString::fromUtf8("\xF0\x9F\x98\x80"));
    QCOMPARE(
        surrogateBoundaryValue.size(),
        maximumInputLength + 1);
    controller.setTextFieldText(fieldId, surrogateBoundaryValue);
    const QString surrogateSafeValue = controller.fieldData(fieldId)
                                           .value(QStringLiteral("value"))
                                           .toString();
    QCOMPARE(
        surrogateSafeValue.size(),
        maximumInputLength - 1);
    QVERIFY(!surrogateSafeValue.at(
                 surrogateSafeValue.size() - 1).isHighSurrogate());

    QVERIFY(controller.setFieldMaxLength(
        fieldId,
        maximumInputLength));
    QVERIFY(!controller.setFieldMaxLength(
        fieldId,
        maximumInputLength + 1));
}

void PDFEditControllerTest::storesTransientRadioButtons()
{
    PDFFormDesignerController controller;
    const QRectF initialRect(0.16, 0.22, 0.05, 0.05);
    const QString fieldId = controller.addField(
        PDFFormDesignerController::RadioButton,
        0,
        initialRect.x(),
        initialRect.y(),
        initialRect.width(),
        initialRect.height());

    QVERIFY(fieldId.startsWith(QStringLiteral("radio-field-")));
    QCOMPARE(controller.rowCount(), 1);
    QVERIFY(controller.hasDrafts());
    QVariantMap field = controller.fieldData(fieldId);
    QCOMPARE(field.value(QStringLiteral("type")).toInt(),
             PDFFormDesignerController::RadioButton);
    QCOMPARE(field.value(QStringLiteral("value")).toString(),
             QStringLiteral("Off"));
    QCOMPARE(field.value(QStringLiteral("options")).toStringList(),
             QStringList({QStringLiteral("Yes"), QStringLiteral("Off")}));
    QCOMPARE(field.value(QStringLiteral("rect")).toRectF(), initialRect);

    QVector<PDFFormDesignerController::SaveField> savedFields
        = controller.saveFields();
    QCOMPARE(savedFields.size(), 1);
    QCOMPARE(savedFields.constFirst().type,
             PDFFormDesignerController::RadioButton);
    QCOMPARE(savedFields.constFirst().operation,
             PDFFormDesignerController::SaveField::Add);
    QCOMPARE(savedFields.constFirst().value, QStringLiteral("Off"));
    QCOMPARE(savedFields.constFirst().defaultValue, QStringLiteral("Off"));
    QCOMPARE(savedFields.constFirst().normalizedRect, initialRect);

    QVERIFY(!controller.setFieldValue(fieldId, QStringLiteral("On")));
    QVERIFY(controller.setFieldValue(fieldId, QStringLiteral("Yes")));
    field = controller.fieldData(fieldId);
    QCOMPARE(field.value(QStringLiteral("value")).toString(),
             QStringLiteral("Yes"));
    savedFields = controller.saveFields();
    QCOMPARE(savedFields.size(), 1);
    QCOMPARE(savedFields.constFirst().type,
             PDFFormDesignerController::RadioButton);
    QCOMPARE(savedFields.constFirst().value, QStringLiteral("Yes"));
    QCOMPARE(savedFields.constFirst().defaultValue, QStringLiteral("Off"));

    QVERIFY(controller.removeField(fieldId));
    QCOMPARE(controller.rowCount(), 0);
    QVERIFY(controller.fieldData(fieldId).isEmpty());
    QVERIFY(controller.fieldsForPage(0).isEmpty());
    QVERIFY(controller.saveFields().isEmpty());
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::renamesDropdownOptionsWithoutLosingSelection()
{
    PDFFormDesignerController controller;
    const QString fieldId = controller.addField(
        PDFFormDesignerController::Dropdown,
        0,
        0.1,
        0.2,
        0.4,
        0.08);
    QVERIFY(!fieldId.isEmpty());

    const QStringList options({
        QStringLiteral("Alpha"),
        QStringLiteral("Beta"),
        QStringLiteral("Gamma"),
    });
    QVERIFY(controller.setFieldOptions(fieldId, options));
    QVERIFY(controller.setFieldValue(fieldId, QStringLiteral("Beta")));

    QVERIFY(controller.renameFieldOption(
        fieldId,
        1,
        QStringLiteral("Beta renamed")));
    QVariantMap field = controller.fieldData(fieldId);
    QCOMPARE(field.value(QStringLiteral("options")).toStringList(),
             QStringList({QStringLiteral("Alpha"),
                          QStringLiteral("Beta renamed"),
                          QStringLiteral("Gamma")}));
    QCOMPARE(field.value(QStringLiteral("value")).toString(),
             QStringLiteral("Beta renamed"));

    QVERIFY(controller.renameFieldOption(
        fieldId,
        0,
        QStringLiteral("Alpha renamed")));
    const QVector<PDFFormDesignerController::SaveField> savedFields
        = controller.saveFields();
    QCOMPARE(savedFields.size(), 1);
    QCOMPARE(savedFields.constFirst().defaultValue,
             QStringLiteral("Alpha renamed"));
    QCOMPARE(savedFields.constFirst().value,
             QStringLiteral("Beta renamed"));

    QVERIFY(!controller.renameFieldOption(
        fieldId,
        1,
        QStringLiteral("Gamma")));
    QVERIFY(!controller.renameFieldOption(fieldId, 1, QString{}));
    QVERIFY(!controller.renameFieldOption(
        fieldId,
        3,
        QStringLiteral("Out of range")));
    field = controller.fieldData(fieldId);
    QCOMPARE(field.value(QStringLiteral("options")).toStringList(),
             QStringList({QStringLiteral("Alpha renamed"),
                          QStringLiteral("Beta renamed"),
                          QStringLiteral("Gamma")}));
}

void PDFEditControllerTest::limitsFormFieldPointerHitArea()
{
    PDFFormDesignerController controller;
    const QString fieldId = controller.addTextField(
        0,
        0.1,
        0.2,
        0.4,
        0.08);
    QVERIFY(!fieldId.isEmpty());

    TestableFormFieldLayer fieldLayer;
    fieldLayer.setWidth(1000);
    fieldLayer.setHeight(1000);
    fieldLayer.setController(&controller);
    fieldLayer.setPage(0);

    const QPointF fieldCenter(300, 240);
    const QPointF emptyPagePoint(800, 500);
    QVERIFY(!fieldLayer.contains(fieldCenter));

    fieldLayer.setFieldsVisible(true);
    QVERIFY(!fieldLayer.contains(fieldCenter));

    fieldLayer.setFieldsInteractive(true);
    fieldLayer.setMoveEnabled(true);
    QVERIFY(fieldLayer.acceptHoverEvents());
    QVERIFY(fieldLayer.contains(fieldCenter));
    QVERIFY(!fieldLayer.contains(emptyPagePoint));
    QVERIFY(!fieldLayer.contains(QPointF(1001, 500)));

    controller.selectTextField(fieldId);
    QCOMPARE(fieldLayer.activeFieldId(), fieldId);
    QCOMPARE(fieldLayer.activeFieldMaxLength(), 0);
    QVERIFY(controller.setFieldMaxLength(fieldId, 12));
    QCOMPARE(fieldLayer.activeFieldMaxLength(), 12);
    controller.clearTextFieldSelection();
    QCOMPARE(fieldLayer.activeFieldId(), QString());
    QCOMPARE(fieldLayer.activeFieldMaxLength(), 0);
    controller.selectTextField(fieldId);
    QCOMPARE(fieldLayer.activeFieldMaxLength(), 12);

    fieldLayer.setMoveEnabled(false);
    QVERIFY(!fieldLayer.acceptHoverEvents());
    QVERIFY(fieldLayer.contains(fieldCenter));

    fieldLayer.setPage(1);
    QVERIFY(!fieldLayer.contains(fieldCenter));
    fieldLayer.setPage(0);
    QVERIFY(fieldLayer.contains(fieldCenter));

    fieldLayer.setPooled(true);
    QVERIFY(!fieldLayer.contains(fieldCenter));
    fieldLayer.setPooled(false);
    QVERIFY(fieldLayer.contains(fieldCenter));

    fieldLayer.setFieldsInteractive(false);
    QVERIFY(!fieldLayer.contains(fieldCenter));
    fieldLayer.setFieldsInteractive(true);
    QVERIFY(fieldLayer.contains(fieldCenter));

    QVERIFY(controller.moveField(fieldId, 0.5, 0.6));
    QVERIFY(!fieldLayer.contains(fieldCenter));
    QVERIFY(fieldLayer.contains(QPointF(700, 640)));
}

void PDFEditControllerTest::rendersMultilineFormFieldPreview()
{
    if (QGuiApplication::screens().isEmpty()) {
        QSKIP("The form preview needs a Qt Quick render screen.");
    }

    PDFFormDesignerController controller;
    const QString fieldId = controller.addTextField(
        0,
        0.05,
        0.08,
        0.9,
        0.84);
    QVERIFY(!fieldId.isEmpty());
    QVERIFY(controller.setFieldFlags(
        fieldId,
        PDFFormDesignerController::Multiline));
    QVERIFY(controller.setFieldValue(
        fieldId,
        QStringLiteral("Alpha\nBeta")));

    QQuickWindow window;
    window.setColor(Qt::white);
    window.resize(320, 120);

    TestableFormFieldLayer fieldLayer;
    fieldLayer.setParentItem(window.contentItem());
    fieldLayer.setWidth(window.width());
    fieldLayer.setHeight(window.height());
    fieldLayer.setController(&controller);
    fieldLayer.setPage(0);
    fieldLayer.setFieldsVisible(true);

    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
    const QSharedPointer<QQuickItemGrabResult> grabResult
        = fieldLayer.grabToImage(window.size());
    QVERIFY(grabResult);
    QSignalSpy readySpy(
        grabResult.data(),
        &QQuickItemGrabResult::ready);
    if (grabResult->image().isNull()) {
        QTRY_VERIFY_WITH_TIMEOUT(readySpy.count() > 0, 5000);
    }
    const QImage preview = grabResult->image();
    QVERIFY(!preview.isNull());

    int firstTextRow = preview.height();
    int lastTextRow = -1;
    for (int y = 8; y < preview.height() - 8; ++y) {
        bool rowContainsText = false;
        for (int x = 24; x < preview.width() - 24; ++x) {
            const QColor pixel = preview.pixelColor(x, y);
            if (pixel.red() < 100
                && pixel.green() < 100
                && pixel.blue() < 100) {
                rowContainsText = true;
                break;
            }
        }
        if (rowContainsText) {
            firstTextRow = qMin(firstTextRow, y);
            lastTextRow = qMax(lastTextRow, y);
        }
    }
    QVERIFY(firstTextRow < preview.height() / 3);
    QVERIFY(lastTextRow - firstTextRow >= 16);
}

void PDFEditControllerTest::movesFormFieldsWithoutResizing()
{
    PDFFormDesignerController controller;
    const QString textFieldId = controller.addTextField(
        0,
        0.1,
        0.2,
        0.4,
        0.08);
    const QString checkboxId = controller.addField(
        PDFFormDesignerController::Checkbox,
        0,
        0.8,
        0.8,
        0.06,
        0.06);
    QVERIFY(!textFieldId.isEmpty());
    QVERIFY(!checkboxId.isEmpty());

    TestableFormFieldLayer fieldLayer;
    fieldLayer.setWidth(1000);
    fieldLayer.setHeight(1000);
    fieldLayer.setController(&controller);
    fieldLayer.setPage(0);
    fieldLayer.setFieldsVisible(true);
    fieldLayer.setFieldsInteractive(true);
    fieldLayer.setMoveEnabled(true);
    controller.selectTextField(textFieldId);
    QSignalSpy dataChangedSpy(
        &controller,
        &QAbstractItemModel::dataChanged);

    QVERIFY(fieldLayer.beginFieldMove(textFieldId));
    QVERIFY(fieldLayer.moving());
    QVERIFY(fieldLayer.updateFieldMove(1.0, 1.0));
    QRectF previewRect = fieldLayer.activeFieldRect();
    QVERIFY(qAbs(previewRect.x() - 0.6) < 0.000001);
    QVERIFY(qAbs(previewRect.y() - 0.92) < 0.000001);
    QVERIFY(qAbs(previewRect.width() - 0.4) < 0.000001);
    QVERIFY(qAbs(previewRect.height() - 0.08) < 0.000001);
    QVariantMap field = controller.fieldData(textFieldId);
    QCOMPARE(field.value(QStringLiteral("x")).toReal(), 0.1);
    QCOMPARE(field.value(QStringLiteral("y")).toReal(), 0.2);
    QCOMPARE(dataChangedSpy.count(), 0);

    fieldLayer.cancelFieldMove();
    QVERIFY(!fieldLayer.moving());
    previewRect = fieldLayer.activeFieldRect();
    QCOMPARE(previewRect, QRectF(0.1, 0.2, 0.4, 0.08));
    QCOMPARE(dataChangedSpy.count(), 0);

    QVERIFY(fieldLayer.beginFieldMove(textFieldId));
    QVERIFY(fieldLayer.updateFieldMove(0.2, 0.3));
    QVERIFY(fieldLayer.finishFieldMove());
    QVERIFY(!fieldLayer.moving());
    field = controller.fieldData(textFieldId);
    QCOMPARE(field.value(QStringLiteral("x")).toReal(), 0.3);
    QCOMPARE(field.value(QStringLiteral("y")).toReal(), 0.5);
    QCOMPARE(field.value(QStringLiteral("width")).toReal(), 0.4);
    QCOMPARE(field.value(QStringLiteral("height")).toReal(), 0.08);
    QCOMPARE(dataChangedSpy.count(), 1);

    QVERIFY(controller.moveField(checkboxId, 0.98, -0.4));
    field = controller.fieldData(checkboxId);
    QVERIFY(qAbs(field.value(QStringLiteral("x")).toReal() - 0.94)
            < 0.000001);
    QCOMPARE(field.value(QStringLiteral("y")).toReal(), 0.0);
    QCOMPARE(field.value(QStringLiteral("width")).toReal(), 0.06);
    QCOMPARE(field.value(QStringLiteral("height")).toReal(), 0.06);
    QCOMPARE(dataChangedSpy.count(), 2);

    QVERIFY(controller.moveField(checkboxId, 0.94, 0.0));
    QCOMPARE(dataChangedSpy.count(), 2);
    QVERIFY(!controller.moveField(
        QStringLiteral("missing-field"),
        0.2,
        0.3));
    QVERIFY(!controller.moveField(
        textFieldId,
        std::numeric_limits<qreal>::quiet_NaN(),
        0.3));

    QSignalSpy fieldActivatedSpy(
        &fieldLayer,
        &FormFieldLayer::fieldActivated);
    const QPointF checkboxCenter(970, 30);
    QMouseEvent clickPress(
        QEvent::MouseButtonPress,
        checkboxCenter,
        checkboxCenter,
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier);
    fieldLayer.mousePressEvent(&clickPress);
    QCOMPARE(fieldActivatedSpy.count(), 0);
    QMouseEvent clickRelease(
        QEvent::MouseButtonRelease,
        checkboxCenter,
        checkboxCenter,
        Qt::LeftButton,
        Qt::NoButton,
        Qt::NoModifier);
    fieldLayer.mouseReleaseEvent(&clickRelease);
    QCOMPARE(fieldActivatedSpy.count(), 1);

    QMouseEvent dragPress(
        QEvent::MouseButtonPress,
        checkboxCenter,
        checkboxCenter,
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier);
    fieldLayer.mousePressEvent(&dragPress);
    const QPointF movedPointer = checkboxCenter + QPointF(-50, 50);
    QMouseEvent dragMove(
        QEvent::MouseMove,
        movedPointer,
        movedPointer,
        Qt::NoButton,
        Qt::LeftButton,
        Qt::NoModifier);
    fieldLayer.mouseMoveEvent(&dragMove);
    QMouseEvent dragRelease(
        QEvent::MouseButtonRelease,
        movedPointer,
        movedPointer,
        Qt::LeftButton,
        Qt::NoButton,
        Qt::NoModifier);
    fieldLayer.mouseReleaseEvent(&dragRelease);
    QCOMPARE(fieldActivatedSpy.count(), 1);
    field = controller.fieldData(checkboxId);
    QVERIFY(qAbs(field.value(QStringLiteral("x")).toReal() - 0.89)
            < 0.000001);
    QVERIFY(qAbs(field.value(QStringLiteral("y")).toReal() - 0.05)
            < 0.000001);
    QCOMPARE(field.value(QStringLiteral("width")).toReal(), 0.06);
    QCOMPARE(field.value(QStringLiteral("height")).toReal(), 0.06);

    const QPointF movedCheckboxCenter(920, 80);
    QMouseEvent canceledPress(
        QEvent::MouseButtonPress,
        movedCheckboxCenter,
        movedCheckboxCenter,
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier);
    fieldLayer.mousePressEvent(&canceledPress);
    const QPointF canceledPointer
        = movedCheckboxCenter + QPointF(50, 50);
    QMouseEvent canceledMove(
        QEvent::MouseMove,
        canceledPointer,
        canceledPointer,
        Qt::NoButton,
        Qt::LeftButton,
        Qt::NoModifier);
    fieldLayer.mouseMoveEvent(&canceledMove);
    QVERIFY(fieldLayer.moving());
    fieldLayer.cancelFieldMove();
    QVERIFY(!fieldLayer.moving());
    QMouseEvent canceledRelease(
        QEvent::MouseButtonRelease,
        canceledPointer,
        canceledPointer,
        Qt::LeftButton,
        Qt::NoButton,
        Qt::NoModifier);
    fieldLayer.mouseReleaseEvent(&canceledRelease);
    QCOMPARE(fieldActivatedSpy.count(), 1);
    field = controller.fieldData(checkboxId);
    QVERIFY(qAbs(field.value(QStringLiteral("x")).toReal() - 0.89)
            < 0.000001);
    QVERIFY(qAbs(field.value(QStringLiteral("y")).toReal() - 0.05)
            < 0.000001);
}

void PDFEditControllerTest::enablesFormScriptingAutomatically()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("automatic-form-scripting.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFFormController controller;
    controller.setDocument(&document);
    QSignalSpy enableFailedSpy(
        &controller,
        &PDFFormController::javascriptEnableFailed);

    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    QTRY_VERIFY_WITH_TIMEOUT(controller.javascriptEnabled(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !controller.javascriptTransitionPending(),
        5000);
    QCOMPARE(enableFailedSpy.count(), 0);
}

void PDFEditControllerTest::customizesChoiceFieldsAndPersistsSelection()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("choice-fields.pdf"));
    QVERIFY(createChoiceFieldsPdf(sourcePath));

    PDFDocument document;
    PDFFormController controller;
    controller.setDocument(&document);
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    QVERIFY(controller.interactive());

    QVector<PDFFormWorker::ChoiceField> fields;
    bool inspectionCompleted = false;
    controller.requestChoiceFields(
        0,
        [&fields, &inspectionCompleted](
            QVector<PDFFormWorker::ChoiceField> inspectedFields) {
            fields = std::move(inspectedFields);
            inspectionCompleted = true;
        });
    QTRY_VERIFY_WITH_TIMEOUT(inspectionCompleted, 5000);
    QCOMPARE(fields.size(), 3);

    const PDFFormWorker::ChoiceField combo = fields.at(0);
    QCOMPARE(combo.annotationIndex, 0);
    QCOMPARE(combo.sourcePage, 0);
    QCOMPARE(combo.fieldType, FPDF_FORMFIELD_COMBOBOX);
    QCOMPARE(
        combo.accessibleName,
        QStringLiteral("Transport choice"));
    QVERIFY(!combo.identityToken.isEmpty());
    QCOMPARE(
        combo.options,
        QStringList({
            QStringLiteral("Airplane"),
            QStringLiteral("Bicycle"),
            QStringLiteral("Car"),
        }));
    QCOMPARE(combo.selectedIndices, QVector<int>({0}));
    QVERIFY(!combo.readOnly);
    QVERIFY(!combo.multiSelect);
    QVERIFY(qAbs(combo.normalizedRect.x() - 20.0 / 360.0) < 0.00001);
    QVERIFY(qAbs(combo.normalizedRect.y() - 50.0 / 480.0) < 0.00001);
    QVERIFY(qAbs(combo.normalizedRect.width() - 160.0 / 360.0) < 0.00001);
    QVERIFY(qAbs(combo.normalizedRect.height() - 30.0 / 480.0) < 0.00001);

    const PDFFormWorker::ChoiceField singleList = fields.at(1);
    QCOMPARE(singleList.fieldType, FPDF_FORMFIELD_LISTBOX);
    QCOMPARE(singleList.selectedIndices, QVector<int>({1}));
    QVERIFY(!singleList.multiSelect);

    const PDFFormWorker::ChoiceField multiList = fields.at(2);
    QCOMPARE(multiList.fieldType, FPDF_FORMFIELD_LISTBOX);
    QCOMPARE(multiList.selectedIndices, QVector<int>({0, 2}));
    QVERIFY(multiList.multiSelect);

    bool focusCompleted = false;
    bool focusSucceeded = false;
    controller.focusChoiceField(
        0,
        combo.annotationIndex,
        combo.sourcePage,
        combo.identityToken,
        [&focusCompleted, &focusSucceeded](bool focused) {
            focusSucceeded = focused;
            focusCompleted = true;
        });
    QTRY_VERIFY_WITH_TIMEOUT(focusCompleted, 5000);
    QVERIFY(focusSucceeded);
    QCOMPARE(controller.focusedPage(), 0);
    QCOMPARE(
        controller.focusedAnnotationIndex(),
        combo.annotationIndex);

    controller.selectChoiceOption(
        0,
        combo.annotationIndex,
        2,
        true,
        combo.sourcePage,
        false,
        combo.identityToken);
    controller.selectChoiceOption(
        0,
        singleList.annotationIndex,
        3,
        true,
        singleList.sourcePage,
        false,
        singleList.identityToken);
    controller.selectChoiceOption(
        0,
        multiList.annotationIndex,
        0,
        false,
        multiList.sourcePage,
        true,
        multiList.identityToken);
    controller.selectChoiceOption(
        0,
        multiList.annotationIndex,
        1,
        true,
        multiList.sourcePage,
        true,
        multiList.identityToken);
    // Toggle twice before either async refresh completes. The worker must
    // resolve each toggle against live PDFium state, not a stale QML snapshot.
    controller.selectChoiceOption(
        0,
        multiList.annotationIndex,
        2,
        false,
        multiList.sourcePage,
        true,
        multiList.identityToken);
    controller.selectChoiceOption(
        0,
        multiList.annotationIndex,
        2,
        false,
        multiList.sourcePage,
        true,
        multiList.identityToken);
    QTRY_VERIFY_WITH_TIMEOUT(controller.hasDrafts(), 5000);

    fields.clear();
    inspectionCompleted = false;
    controller.requestChoiceFields(
        0,
        [&fields, &inspectionCompleted](
            QVector<PDFFormWorker::ChoiceField> inspectedFields) {
            fields = std::move(inspectedFields);
            inspectionCompleted = true;
        });
    QTRY_VERIFY_WITH_TIMEOUT(inspectionCompleted, 5000);
    QCOMPARE(fields.size(), 3);
    QCOMPARE(fields.at(0).selectedIndices, QVector<int>({2}));
    QCOMPARE(fields.at(1).selectedIndices, QVector<int>({3}));
    QCOMPARE(fields.at(2).selectedIndices, QVector<int>({1, 2}));
    // PDFium does not expose undo history for non-editable choice fields,
    // including selections made through its native key and pointer paths.
    // Keep the custom controls aligned with that focus-local form behavior.
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());

    QString snapshotPath;
    QString error;
    QVERIFY2(
        controller.exportSnapshotBlocking(snapshotPath, error),
        qPrintable(error));
    QCOMPARE(selectedChoiceOptions(snapshotPath, 0), QVector<int>({2}));
    QCOMPARE(selectedChoiceOptions(snapshotPath, 1), QVector<int>({3}));
    QCOMPARE(
        selectedChoiceOptions(snapshotPath, 2),
        QVector<int>({1, 2}));
    QVERIFY(QFile::remove(snapshotPath));
}

void PDFEditControllerTest::routesPushButtonActions_data()
{
    QTest::addColumn<QPointF>("clickPosition");
    QTest::addColumn<int>("initialCurrentPage");
    QTest::addColumn<int>("expectedPage");
    QTest::addColumn<bool>("expectExternalLink");

    QTest::newRow("goto-prev")
        << QPointF(60.0, 60.0)
        << 1
        << 0
        << false;
    QTest::newRow("goto-prev-while-model-already-targets-page")
        << QPointF(60.0, 60.0)
        << 0
        << 0
        << false;
    QTest::newRow("uri-https")
        << QPointF(180.0, 60.0)
        << 1
        << -1
        << true;
    QTest::newRow("goto-next")
        << QPointF(300.0, 60.0)
        << 1
        << 2
        << false;
}

void PDFEditControllerTest::routesPushButtonActions()
{
    QFETCH(QPointF, clickPosition);
    QFETCH(int, initialCurrentPage);
    QFETCH(int, expectedPage);
    QFETCH(bool, expectExternalLink);

    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("form-actions-%1.pdf").arg(
            QString::fromLatin1(QTest::currentDataTag())));
    QVERIFY(createPushButtonActionPdf(sourcePath));

    PDFDocument document;
    PDFFormController controller;
    controller.setDocument(&document);
    QSignalSpy navigationSpy(
        &controller,
        &PDFFormController::pageNavigationRequested);
    QSignalSpy externalLinkSpy(
        &controller,
        &PDFFormController::externalLinkRequested);
    QSignalSpy cancelledLinkSpy(
        &controller,
        &PDFFormController::externalLinkRequestCancelled);
    QSignalSpy enableFailedSpy(
        &controller,
        &PDFFormController::javascriptEnableFailed);

    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    QVERIFY(controller.interactive());
    QTRY_VERIFY_WITH_TIMEOUT(controller.javascriptEnabled(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !controller.javascriptTransitionPending(),
        5000);
    QCOMPARE(enableFailedSpy.count(), 0);

    document.setCurrentPage(initialCurrentPage);
    QCOMPARE(document.currentPage(), initialCurrentPage);
    const QSize deviceSize(360, 480);
    controller.pointerEvent(
        1,
        PDFFormWorker::PointerEventType::Press,
        0,
        clickPosition,
        deviceSize);
    controller.pointerEvent(
        1,
        PDFFormWorker::PointerEventType::Release,
        0,
        clickPosition,
        deviceSize);

    if (expectExternalLink) {
        QTRY_COMPARE_WITH_TIMEOUT(externalLinkSpy.count(), 1, 5000);
        QCOMPARE(navigationSpy.count(), 0);
        QCOMPARE(document.currentPage(), initialCurrentPage);
        const QList<QVariant> arguments
            = externalLinkSpy.takeFirst();
        const quint64 requestId = arguments.at(0).toULongLong();
        QVERIFY(requestId != 0);
        QCOMPARE(
            arguments.at(1).toString(),
            QStringLiteral("example.com"));
        QCOMPARE(
            arguments.at(2).toString(),
            QStringLiteral("https://example.com/"));
        QCOMPARE(arguments.at(3).toBool(), false);
        controller.cancelExternalLink(requestId);
        QCOMPARE(cancelledLinkSpy.count(), 1);
    } else {
        QTRY_COMPARE_WITH_TIMEOUT(navigationSpy.count(), 1, 5000);
        QCOMPARE(
            navigationSpy.constFirst().constFirst().toInt(),
            expectedPage);
        QTRY_COMPARE_WITH_TIMEOUT(
            document.currentPage(),
            expectedPage,
            5000);
        QCOMPARE(externalLinkSpy.count(), 0);
    }
}

void PDFEditControllerTest::blocksUnsupportedFormUriSchemes()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("form-actions-blocked-uri.pdf"));
    QVERIFY(createPushButtonActionPdf(
        sourcePath,
        QByteArray("file:///tmp/blocked")));

    PDFDocument document;
    PDFFormController controller;
    controller.setDocument(&document);
    QSignalSpy navigationSpy(
        &controller,
        &PDFFormController::pageNavigationRequested);
    QSignalSpy externalLinkSpy(
        &controller,
        &PDFFormController::externalLinkRequested);

    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    QTRY_VERIFY_WITH_TIMEOUT(controller.javascriptEnabled(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !controller.javascriptTransitionPending(),
        5000);
    document.setCurrentPage(1);

    controller.pointerEvent(
        1,
        PDFFormWorker::PointerEventType::Press,
        0,
        QPointF(180.0, 60.0),
        QSize(360, 480));
    controller.pointerEvent(
        1,
        PDFFormWorker::PointerEventType::Release,
        0,
        QPointF(180.0, 60.0),
        QSize(360, 480));

    QVERIFY(controller.hasDrafts());
    QTRY_VERIFY_WITH_TIMEOUT(!controller.hasDrafts(), 5000);
    QCOMPARE(externalLinkSpy.count(), 0);
    QCOMPARE(navigationSpy.count(), 0);
    QCOMPARE(document.currentPage(), 1);
}

void PDFEditControllerTest::forwardsExternalLinkRequestsThroughTabModel()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("form-actions-tab-model.pdf"));
    QVERIFY(createPushButtonActionPdf(sourcePath));

    PDFTabModel tabs;
    QSignalSpy externalLinkSpy(
        &tabs,
        &PDFTabModel::externalLinkRequested);
    PDFDocumentSession *session = tabs.open(
        QUrl::fromLocalFile(sourcePath));
    QVERIFY(session);
    QTRY_VERIFY_WITH_TIMEOUT(
        session->formController()->javascriptEnabled(),
        5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !session->formController()->javascriptTransitionPending(),
        5000);
    session->document()->setCurrentPage(1);

    session->formController()->pointerEvent(
        1,
        PDFFormWorker::PointerEventType::Press,
        0,
        QPointF(180.0, 60.0),
        QSize(360, 480));
    session->formController()->pointerEvent(
        1,
        PDFFormWorker::PointerEventType::Release,
        0,
        QPointF(180.0, 60.0),
        QSize(360, 480));

    QTRY_COMPARE_WITH_TIMEOUT(externalLinkSpy.count(), 1, 5000);
    const QList<QVariant> arguments = externalLinkSpy.takeFirst();
    QCOMPARE(
        arguments.at(0).value<PDFDocumentSession *>(),
        session);
    const quint64 requestId = arguments.at(1).toULongLong();
    QVERIFY(requestId != 0);
    QCOMPARE(
        arguments.at(2).toString(),
        QStringLiteral("example.com"));
    QCOMPARE(
        arguments.at(3).toString(),
        QStringLiteral("https://example.com/"));
    QCOMPARE(arguments.at(4).toBool(), false);
    session->formController()->cancelExternalLink(requestId);
}
