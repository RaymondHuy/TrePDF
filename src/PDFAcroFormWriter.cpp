#include "PDFAcroFormWriter.h"

#include "PDFAcroFormCanonicalizer.h"
#include "PDFFormAppearance.h"
#include "PDFiumSynchronization.h"

#include <QFile>
#include <QHash>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

#include <fpdf_annot.h>
#include <fpdf_formfill.h>
#include <fpdf_save.h>
#include <fpdfview.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

constexpr int kMaximumDropdownOptions = 4096;
constexpr qsizetype kMaximumDropdownOptionCharacters = 1024 * 1024;
constexpr int kButtonNoToggleToOff = 1 << 14;
constexpr int kButtonRadio = 1 << 15;
constexpr int kChoiceCommitOnSelectionChange = 1 << 26;

bool isTrePDFDropdownWidgetId(const QString &widgetId)
{
    constexpr qsizetype uuidLength = 36;
    const QString prefix = QStringLiteral("dropdown-field-");
    if (!widgetId.startsWith(prefix)
        || widgetId.size() != prefix.size() + uuidLength) {
        return false;
    }
    const QString suffix = widgetId.sliced(prefix.size());
    const QUuid uuid = QUuid::fromString(suffix);
    return !uuid.isNull()
        && uuid.toString(QUuid::WithoutBraces).compare(
               suffix,
               Qt::CaseInsensitive)
            == 0;
}

bool isTrePDFRadioButtonWidgetId(const QString &widgetId)
{
    constexpr qsizetype uuidLength = 36;
    const QString prefix = QStringLiteral("radio-field-");
    if (!widgetId.startsWith(prefix)
        || widgetId.size() != prefix.size() + uuidLength) {
        return false;
    }
    const QString suffix = widgetId.sliced(prefix.size());
    const QUuid uuid = QUuid::fromString(suffix);
    return !uuid.isNull()
        && uuid.toString(QUuid::WithoutBraces).compare(
               suffix,
               Qt::CaseInsensitive)
            == 0;
}

class PdfDocumentHandle
{
public:
    explicit PdfDocumentHandle(FPDF_DOCUMENT document = nullptr)
        : m_document(document)
    {
    }

    ~PdfDocumentHandle()
    {
        if (m_document) {
            assertPDFiumProcessLockHeld();
            FPDF_CloseDocument(m_document);
        }
    }

    FPDF_DOCUMENT get() const
    {
        return m_document;
    }

    Q_DISABLE_COPY_MOVE(PdfDocumentHandle)

private:
    FPDF_DOCUMENT m_document = nullptr;
};

class PdfPageHandle
{
public:
    explicit PdfPageHandle(FPDF_PAGE page = nullptr)
        : m_page(page)
    {
    }

    ~PdfPageHandle()
    {
        if (m_page) {
            assertPDFiumProcessLockHeld();
            FPDF_ClosePage(m_page);
        }
    }

    FPDF_PAGE get() const
    {
        return m_page;
    }

    Q_DISABLE_COPY_MOVE(PdfPageHandle)

private:
    FPDF_PAGE m_page = nullptr;
};

class PdfAnnotationHandle
{
public:
    explicit PdfAnnotationHandle(FPDF_ANNOTATION annotation = nullptr)
        : m_annotation(annotation)
    {
    }

    ~PdfAnnotationHandle()
    {
        if (m_annotation) {
            assertPDFiumProcessLockHeld();
            FPDFPage_CloseAnnot(m_annotation);
        }
    }

    FPDF_ANNOTATION get() const
    {
        return m_annotation;
    }

    Q_DISABLE_COPY_MOVE(PdfAnnotationHandle)

private:
    FPDF_ANNOTATION m_annotation = nullptr;
};

class PdfFormHandle
{
public:
    explicit PdfFormHandle(FPDF_FORMHANDLE formHandle)
        : m_formHandle(formHandle)
    {
    }

    ~PdfFormHandle()
    {
        if (m_formHandle) {
            assertPDFiumProcessLockHeld();
            FPDFDOC_ExitFormFillEnvironment(m_formHandle);
        }
    }

    FPDF_FORMHANDLE get() const
    {
        return m_formHandle;
    }

    Q_DISABLE_COPY_MOVE(PdfFormHandle)

private:
    FPDF_FORMHANDLE m_formHandle = nullptr;
};

class SaveFileWriter final : public FPDF_FILEWRITE
{
public:
    explicit SaveFileWriter(const QString &filePath)
        : m_file(filePath)
    {
        version = 1;
        WriteBlock = &SaveFileWriter::writeBlock;
    }

    bool open()
    {
        return m_file.open(QIODevice::WriteOnly);
    }

    bool commit()
    {
        return m_file.commit();
    }

    QString errorString() const
    {
        return m_file.errorString();
    }

private:
    static int writeBlock(FPDF_FILEWRITE *fileWriter,
                          const void *data,
                          unsigned long size)
    {
        auto *writer = static_cast<SaveFileWriter *>(fileWriter);
        const qint64 byteCount = static_cast<qint64>(size);
        return writer->m_file.write(
                   static_cast<const char *>(data),
                   byteCount)
                == byteCount
            ? 1
            : 0;
    }

    QSaveFile m_file;
};

int widgetFieldFlags(const PDFAcroFormWriter::Field &field)
{
    int flags = (field.readOnly ? FPDF_FORMFLAG_READONLY : 0)
        | (field.required ? FPDF_FORMFLAG_REQUIRED : 0);
    if (field.type == PDFAcroFormWriter::FieldType::Dropdown) {
        flags |= FPDF_FORMFLAG_CHOICE_COMBO;
    } else if (field.type
               == PDFAcroFormWriter::FieldType::RadioButton) {
        flags |= kButtonRadio;
    }
    return flags;
}

int pdfiumFieldType(PDFAcroFormWriter::FieldType type)
{
    switch (type) {
    case PDFAcroFormWriter::FieldType::Text:
        return FPDF_FORMFIELD_TEXTFIELD;
    case PDFAcroFormWriter::FieldType::Checkbox:
        return FPDF_FORMFIELD_CHECKBOX;
    case PDFAcroFormWriter::FieldType::RadioButton:
        return FPDF_FORMFIELD_RADIOBUTTON;
    case PDFAcroFormWriter::FieldType::Dropdown:
        return FPDF_FORMFIELD_COMBOBOX;
    case PDFAcroFormWriter::FieldType::Date:
    case PDFAcroFormWriter::FieldType::Signature:
        return FPDF_FORMFIELD_UNKNOWN;
    }
    return FPDF_FORMFIELD_UNKNOWN;
}

bool checkboxChecked(const QString &value)
{
    return value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0
        || value.compare(QStringLiteral("yes"), Qt::CaseInsensitive) == 0
        || value.compare(QStringLiteral("on"), Qt::CaseInsensitive) == 0
        || value == QStringLiteral("1");
}

QString normalizedCheckboxValue(const QString &value)
{
    return checkboxChecked(value)
        ? QStringLiteral("Yes")
        : QStringLiteral("Off");
}

bool validDropdownOptions(const QStringList &options)
{
    if (options.isEmpty() || options.size() > kMaximumDropdownOptions) {
        return false;
    }
    QSet<QString> uniqueOptions;
    qsizetype totalCharacters = 0;
    for (const QString &option : options) {
        totalCharacters += option.size();
        if (option.isEmpty()
            || totalCharacters > kMaximumDropdownOptionCharacters
            || uniqueOptions.contains(option)) {
            return false;
        }
        uniqueOptions.insert(option);
    }
    return true;
}

PDFAcroFormWriter::Result failure(const QString &message)
{
    return {false, message};
}

template<typename Getter>
QString formWideString(const Getter &getter)
{
    const unsigned long byteCount = getter(nullptr, 0);
    if (byteCount < sizeof(FPDF_WCHAR)
        || byteCount > 1024UL * 1024UL
        || byteCount % sizeof(FPDF_WCHAR) != 0) {
        return {};
    }
    QVector<FPDF_WCHAR> buffer(
        static_cast<qsizetype>(byteCount / sizeof(FPDF_WCHAR)));
    if (getter(buffer.data(), byteCount) != byteCount) {
        return {};
    }
    qsizetype characterCount = buffer.size();
    while (characterCount > 0
           && buffer.at(characterCount - 1) == 0) {
        --characterCount;
    }
    return QString::fromUtf16(
        reinterpret_cast<const char16_t *>(buffer.constData()),
        characterCount);
}

bool resolveUnchangedUpdateValues(
    FPDF_DOCUMENT document,
    QVector<PDFAcroFormWriter::Field> &fields,
    QString &error)
{
    assertPDFiumProcessLockHeld();
    QHash<QString, int> unresolvedFields;
    for (int index = 0; index < fields.size(); ++index) {
        const PDFAcroFormWriter::Field &field = fields.at(index);
        if (field.operation == PDFAcroFormWriter::Operation::Remove) {
            continue;
        }
        const bool preserveValue
            = field.operation != PDFAcroFormWriter::Operation::Add
            && !field.valueChanged;
        const bool preserveOptions
            = field.operation != PDFAcroFormWriter::Operation::Add
            && field.type == PDFAcroFormWriter::FieldType::Dropdown
            && !field.optionsChanged;
        if (!preserveValue && !preserveOptions) {
            continue;
        }
        if (field.widgetId.isEmpty()
            || unresolvedFields.contains(field.widgetId)) {
            error = QStringLiteral(
                "The unchanged AcroForm value request contains duplicate Widget IDs.");
            return false;
        }
        unresolvedFields.insert(field.widgetId, index);
    }
    if (unresolvedFields.isEmpty()) {
        return true;
    }

    FPDF_FORMFILLINFO formFillInfo = {};
    formFillInfo.version = 2;
    formFillInfo.xfa_disabled = true;
    PdfFormHandle form(
        FPDFDOC_InitFormFillEnvironment(
            document,
            &formFillInfo));
    if (!form.get()
        || FPDF_GetFormType(document) != FORMTYPE_ACRO_FORM) {
        error = QStringLiteral(
            "The source PDF does not expose the existing AcroForm values.");
        return false;
    }

    const int pageCount = FPDF_GetPageCount(document);
    for (int pageIndex = 0;
         pageIndex < pageCount && !unresolvedFields.isEmpty();
         ++pageIndex) {
        PdfPageHandle page(FPDF_LoadPage(document, pageIndex));
        if (!page.get()) {
            error = QStringLiteral(
                "PDFium could not load a page while preserving an AcroForm value.");
            return false;
        }
        FORM_OnAfterLoadPage(page.get(), form.get());
        bool pageSucceeded = true;
        const int annotationCount = FPDFPage_GetAnnotCount(page.get());
        for (int annotationIndex = 0;
             annotationIndex < annotationCount;
             ++annotationIndex) {
            PdfAnnotationHandle annotation(FPDFPage_GetAnnot(
                page.get(),
                annotationIndex));
            if (!annotation.get()
                || FPDFAnnot_GetSubtype(annotation.get())
                    != FPDF_ANNOT_WIDGET) {
                continue;
            }
            const QString widgetId = formWideString(
                [&annotation](FPDF_WCHAR *buffer,
                              unsigned long byteCount) {
                    return FPDFAnnot_GetStringValue(
                        annotation.get(),
                        "NM",
                        buffer,
                        byteCount);
                });
            const auto unresolved = unresolvedFields.constFind(widgetId);
            if (unresolved == unresolvedFields.cend()) {
                continue;
            }
            if (FPDFAnnot_GetFormFieldType(
                    form.get(),
                    annotation.get())
                != pdfiumFieldType(fields[*unresolved].type)) {
                error = QStringLiteral(
                    "An existing TrePDF Widget has an unexpected field type.");
                pageSucceeded = false;
                break;
            }
            PDFAcroFormWriter::Field &resolvedField
                = fields[*unresolved];
            if (!resolvedField.valueChanged) {
                const bool checkable
                    = resolvedField.type
                          == PDFAcroFormWriter::FieldType::Checkbox
                    || resolvedField.type
                          == PDFAcroFormWriter::FieldType::RadioButton;
                resolvedField.value
                    = checkable
                    ? FPDFAnnot_IsChecked(form.get(), annotation.get())
                        ? QStringLiteral("Yes")
                        : QStringLiteral("Off")
                    : formWideString(
                          [&form, &annotation](FPDF_WCHAR *buffer,
                                               unsigned long byteCount) {
                              return FPDFAnnot_GetFormFieldValue(
                                  form.get(),
                                  annotation.get(),
                                  buffer,
                                  byteCount);
                          });
            }
            if (resolvedField.type
                    == PDFAcroFormWriter::FieldType::Dropdown
                && !resolvedField.optionsChanged) {
                const int optionCount = FPDFAnnot_GetOptionCount(
                    form.get(),
                    annotation.get());
                QStringList options;
                if (optionCount > 0
                    && optionCount <= kMaximumDropdownOptions) {
                    options.reserve(optionCount);
                    for (int optionIndex = 0;
                         optionIndex < optionCount;
                         ++optionIndex) {
                        options.append(formWideString(
                            [&form,
                             &annotation,
                             optionIndex](FPDF_WCHAR *buffer,
                                          unsigned long byteCount) {
                                return FPDFAnnot_GetOptionLabel(
                                    form.get(),
                                    annotation.get(),
                                    optionIndex,
                                    buffer,
                                    byteCount);
                            }));
                    }
                }
                if (!validDropdownOptions(options)) {
                    error = QStringLiteral(
                        "An existing TrePDF Dropdown has invalid options.");
                    pageSucceeded = false;
                    break;
                }
                resolvedField.options = std::move(options);
            }
            unresolvedFields.erase(unresolved);
        }
        FORM_OnBeforeClosePage(page.get(), form.get());
        if (!pageSucceeded) {
            return false;
        }
    }
    if (!unresolvedFields.isEmpty()) {
        error = QStringLiteral(
            "An existing TrePDF form field value could not be resolved.");
        return false;
    }
    return true;
}

bool validateWrittenFields(
    const QString &filePath,
    const QVector<PDFAcroFormWriter::Field> &fields,
    QString &error)
{
    assertPDFiumProcessLockHeld();
    const QByteArray encodedPath = QFile::encodeName(filePath);
    PdfDocumentHandle document(FPDF_LoadDocument(
        encodedPath.constData(),
        nullptr));
    if (!document.get()) {
        error = QStringLiteral(
            "PDFium could not reopen the written AcroForm PDF.");
        return false;
    }

    FPDF_FORMFILLINFO formFillInfo = {};
    formFillInfo.version = 2;
    formFillInfo.xfa_disabled = true;
    PdfFormHandle form(
        FPDFDOC_InitFormFillEnvironment(
            document.get(),
            &formFillInfo));
    const bool requiresFormSession = std::any_of(
        fields.cbegin(),
        fields.cend(),
        [](const PDFAcroFormWriter::Field &field) {
            return field.operation
                != PDFAcroFormWriter::Operation::Remove;
        });
    if (requiresFormSession
        && (!form.get()
            || FPDF_GetFormType(document.get()) != FORMTYPE_ACRO_FORM)) {
        error = QStringLiteral(
            "The written PDF does not expose a valid AcroForm session.");
        return false;
    }

    QHash<QString, const PDFAcroFormWriter::Field *> expectedFields;
    QHash<QString, int> expectedRadioControlCounts;
    QHash<QString, QString> expectedRadioValues;
    for (const PDFAcroFormWriter::Field &field : fields) {
        if (expectedFields.contains(field.widgetId)) {
            error = QStringLiteral(
                "The AcroForm validation request contains duplicate Widget IDs.");
            return false;
        }
        expectedFields.insert(field.widgetId, &field);
        if (field.operation != PDFAcroFormWriter::Operation::Remove
            && field.type == PDFAcroFormWriter::FieldType::RadioButton) {
            ++expectedRadioControlCounts[field.fieldName];
            if (checkboxChecked(field.value)) {
                expectedRadioValues.insert(field.fieldName, field.widgetId);
            }
        }
    }

    QHash<QString, int> matchedFields;
    const int pageCount = FPDF_GetPageCount(document.get());
    for (int pageIndex = 0; pageIndex < pageCount; ++pageIndex) {
        PdfPageHandle page(FPDF_LoadPage(document.get(), pageIndex));
        if (!page.get()) {
            error = QStringLiteral(
                "PDFium could not load a page from the written AcroForm PDF.");
            return false;
        }
        if (form.get()) {
            FORM_OnAfterLoadPage(page.get(), form.get());
        }
        bool pageSucceeded = true;
        const int annotationCount = FPDFPage_GetAnnotCount(page.get());
        for (int annotationIndex = 0;
             annotationIndex < annotationCount;
             ++annotationIndex) {
            PdfAnnotationHandle annotation(FPDFPage_GetAnnot(
                page.get(),
                annotationIndex));
            if (!annotation.get()
                || FPDFAnnot_GetSubtype(annotation.get())
                    != FPDF_ANNOT_WIDGET) {
                continue;
            }
            const QString widgetId = formWideString(
                [&annotation](FPDF_WCHAR *buffer,
                              unsigned long byteCount) {
                    return FPDFAnnot_GetStringValue(
                        annotation.get(),
                        "NM",
                        buffer,
                        byteCount);
                });
            const auto expected = expectedFields.constFind(widgetId);
            if (expected == expectedFields.cend()) {
                continue;
            }

            const PDFAcroFormWriter::Field &field = **expected;
            if (field.operation == PDFAcroFormWriter::Operation::Remove) {
                error = QStringLiteral(
                    "A removed TrePDF form field remains after PDFium reopen.");
                pageSucceeded = false;
                break;
            }
            FS_RECTF rect = {};
            const QString fieldName = formWideString(
                [&form, &annotation](FPDF_WCHAR *buffer,
                                     unsigned long byteCount) {
                    return FPDFAnnot_GetFormFieldName(
                        form.get(),
                        annotation.get(),
                        buffer,
                        byteCount);
                });
            const QString fieldValue = formWideString(
                [&form, &annotation](FPDF_WCHAR *buffer,
                                     unsigned long byteCount) {
                    return FPDFAnnot_GetFormFieldValue(
                        form.get(),
                        annotation.get(),
                        buffer,
                        byteCount);
                });
            const int fieldFlags = FPDFAnnot_GetFormFieldFlags(
                form.get(),
                annotation.get());
            const int expectedFlags = widgetFieldFlags(field);
            const bool checkable
                = field.type == PDFAcroFormWriter::FieldType::Checkbox
                || field.type
                    == PDFAcroFormWriter::FieldType::RadioButton;
            const bool radioButton
                = field.type
                == PDFAcroFormWriter::FieldType::RadioButton;
            const bool dropdown
                = field.type == PDFAcroFormWriter::FieldType::Dropdown;
            const QString exportValue = checkable
                ? formWideString(
                      [&form, &annotation](FPDF_WCHAR *buffer,
                                           unsigned long byteCount) {
                          return FPDFAnnot_GetFormFieldExportValue(
                              form.get(),
                              annotation.get(),
                              buffer,
                              byteCount);
                      })
                : QString();
            float appearanceVersion = 0.0f;
            const bool hasAppearanceVersion
                = FPDFAnnot_GetNumberValue(
                    annotation.get(),
                    PDFFormAppearance::kVersionKey,
                    &appearanceVersion);
            float radioDefaultChecked = 0.0f;
            const bool radioDefaultMatches = !radioButton
                || (FPDFAnnot_GetNumberValue(
                        annotation.get(),
                        PDFFormAppearance::kRadioButtonDefaultCheckedKey,
                        &radioDefaultChecked)
                    && std::isfinite(radioDefaultChecked)
                    && radioDefaultChecked
                        == (checkboxChecked(field.defaultValue)
                                ? 1.0f
                                : 0.0f));
            const bool checkableMatches = !checkable
                || (FPDFAnnot_IsChecked(form.get(), annotation.get())
                        == checkboxChecked(field.value)
                    && exportValue
                        == (radioButton
                                ? field.widgetId
                                : QStringLiteral("Yes"))
                    && FPDFAnnot_GetFormControlCount(
                           form.get(),
                           annotation.get())
                        == (radioButton
                                ? expectedRadioControlCounts.value(
                                      field.fieldName)
                                : 1)
                    && FPDFAnnot_GetFormControlIndex(
                           form.get(),
                           annotation.get())
                        >= 0
                    && FPDFAnnot_GetFormControlIndex(
                           form.get(),
                           annotation.get())
                        < (radioButton
                                ? expectedRadioControlCounts.value(
                                      field.fieldName)
                                : 1)
                    && (FPDFAnnot_GetFlags(annotation.get())
                        & FPDF_ANNOT_FLAG_PRINT)
                    && radioDefaultMatches
                    && hasAppearanceVersion
                    && qFuzzyCompare(
                        appearanceVersion,
                        static_cast<float>(
                            radioButton
                                ? PDFFormAppearance::kRadioButtonVersion
                                : PDFFormAppearance::kCheckboxVersion)));
            const QString defaultValue = dropdown
                ? formWideString(
                      [&annotation](FPDF_WCHAR *buffer,
                                    unsigned long byteCount) {
                          return FPDFAnnot_GetStringValue(
                              annotation.get(),
                              "DV",
                              buffer,
                              byteCount);
                      })
                : QString();
            bool dropdownMatches = !dropdown;
            if (dropdown) {
                const int optionCount = FPDFAnnot_GetOptionCount(
                    form.get(),
                    annotation.get());
                QStringList options;
                QVector<int> selectedIndices;
                if (optionCount >= 0) {
                    options.reserve(optionCount);
                    for (int optionIndex = 0;
                         optionIndex < optionCount;
                         ++optionIndex) {
                        options.append(formWideString(
                            [&form,
                             &annotation,
                             optionIndex](FPDF_WCHAR *buffer,
                                          unsigned long byteCount) {
                                return FPDFAnnot_GetOptionLabel(
                                    form.get(),
                                    annotation.get(),
                                    optionIndex,
                                    buffer,
                                    byteCount);
                            }));
                        if (FPDFAnnot_IsOptionSelected(
                                form.get(),
                                annotation.get(),
                                optionIndex)) {
                            selectedIndices.append(optionIndex);
                        }
                    }
                }
                const int expectedIndex = field.options.indexOf(field.value);
                dropdownMatches = options == field.options
                    && defaultValue == field.defaultValue
                    && selectedIndices == QVector<int>({expectedIndex})
                    && (FPDFAnnot_GetFlags(annotation.get())
                        & FPDF_ANNOT_FLAG_PRINT)
                    && hasAppearanceVersion
                    && qFuzzyCompare(
                        appearanceVersion,
                        static_cast<float>(
                            PDFFormAppearance::kDropdownVersion));
            }
            constexpr qreal coordinateTolerance = 0.05;
            const bool matches = pageIndex == field.page
                && FPDFAnnot_GetFormFieldType(
                       form.get(),
                       annotation.get())
                    == pdfiumFieldType(field.type)
                && fieldName == field.fieldName
                && fieldValue
                    == (radioButton
                            ? expectedRadioValues.value(
                                  field.fieldName,
                                  QStringLiteral("Off"))
                            : field.value)
                && checkableMatches
                && dropdownMatches
                && (fieldFlags
                    & (FPDF_FORMFLAG_READONLY
                       | FPDF_FORMFLAG_REQUIRED
                       | FPDF_FORMFLAG_CHOICE_COMBO
                       | FPDF_FORMFLAG_CHOICE_EDIT
                       | FPDF_FORMFLAG_CHOICE_MULTI_SELECT
                       | kButtonNoToggleToOff
                       | kButtonRadio
                       | kChoiceCommitOnSelectionChange))
                    == expectedFlags
                && (FPDFAnnot_GetFlags(annotation.get())
                    & FPDF_ANNOT_FLAG_PRINT)
                && FPDFAnnot_HasKey(annotation.get(), "AP")
                && FPDFAnnot_GetRect(annotation.get(), &rect)
                && qAbs(rect.left - field.pageRect.left())
                    <= coordinateTolerance
                && qAbs(rect.right - field.pageRect.right())
                    <= coordinateTolerance
                && qAbs(rect.bottom - field.pageRect.top())
                    <= coordinateTolerance
                && qAbs(rect.top - field.pageRect.bottom())
                    <= coordinateTolerance;
            if (!matches || ++matchedFields[widgetId] != 1) {
                error = QStringLiteral(
                    "A written TrePDF form field failed PDFium validation.");
                pageSucceeded = false;
                break;
            }
        }
        if (form.get()) {
            FORM_OnBeforeClosePage(page.get(), form.get());
        }
        if (!pageSucceeded) {
            return false;
        }
    }

    for (auto expected = expectedFields.cbegin();
         expected != expectedFields.cend();
         ++expected) {
        if (expected.value()->operation
            == PDFAcroFormWriter::Operation::Remove) {
            continue;
        }
        if (matchedFields.value(expected.key()) != 1) {
            error = QStringLiteral(
                "A written TrePDF form field is missing after PDFium reopen.");
            return false;
        }
    }
    return true;
}

} // namespace

PDFAcroFormWriter::Result PDFAcroFormWriter::write(
    const QString &sourceFilePath,
    const QString &targetFilePath,
    const QVector<Field> &fields)
{
    assertPDFiumProcessLockHeld();
    const QByteArray encodedSourcePath = QFile::encodeName(sourceFilePath);
    PdfDocumentHandle document(FPDF_LoadDocument(
        encodedSourcePath.constData(),
        nullptr));
    if (!document.get()) {
        return failure(QStringLiteral(
            "PDFium could not open the intermediate PDF."));
    }

    QVector<Field> resolvedFields = fields;
    QString resolveError;
    if (!resolveUnchangedUpdateValues(
            document.get(),
            resolvedFields,
            resolveError)) {
        return failure(resolveError);
    }
    for (Field &field : resolvedFields) {
        if (field.operation == Operation::Remove) {
            continue;
        }
        if (field.type == FieldType::Checkbox
            || field.type == FieldType::RadioButton) {
            field.value = normalizedCheckboxValue(field.value);
            field.defaultValue = normalizedCheckboxValue(field.defaultValue);
        } else if (field.type == FieldType::Dropdown) {
            if (!validDropdownOptions(field.options)
                || !field.options.contains(field.value)
                || !field.options.contains(field.defaultValue)) {
                return failure(QStringLiteral(
                    "The Dropdown options or selected values are invalid."));
            }
        }
    }
    QHash<QString, QString> selectedRadioWidgets;
    QHash<QString, QString> defaultRadioWidgets;
    QHash<QString, int> radioGroupFlags;
    QHash<QString, QString> radioGroupTooltips;
    for (const Field &field : std::as_const(resolvedFields)) {
        if (field.operation == Operation::Remove
            || field.type != FieldType::RadioButton) {
            continue;
        }
        if (!isTrePDFRadioButtonWidgetId(field.widgetId)) {
            return failure(QStringLiteral(
                "The Radio Button Widget ID is invalid."));
        }
        const int flags = widgetFieldFlags(field);
        const auto existingFlags = radioGroupFlags.constFind(field.fieldName);
        const auto existingTooltip = radioGroupTooltips.constFind(
            field.fieldName);
        if ((existingFlags != radioGroupFlags.cend()
             && *existingFlags != flags)
            || (existingTooltip != radioGroupTooltips.cend()
                && *existingTooltip != field.tooltip)) {
            return failure(QStringLiteral(
                "Radio Buttons in one group must share flags and tooltip."));
        }
        radioGroupFlags.insert(field.fieldName, flags);
        radioGroupTooltips.insert(field.fieldName, field.tooltip);
        if (checkboxChecked(field.value)) {
            if (selectedRadioWidgets.contains(field.fieldName)) {
                return failure(QStringLiteral(
                    "A Radio Button group has more than one selected Widget."));
            }
            selectedRadioWidgets.insert(field.fieldName, field.widgetId);
        }
        if (checkboxChecked(field.defaultValue)) {
            if (defaultRadioWidgets.contains(field.fieldName)) {
                return failure(QStringLiteral(
                    "A Radio Button group has more than one default Widget."));
            }
            defaultRadioWidgets.insert(field.fieldName, field.widgetId);
        }
    }

    const int pageCount = FPDF_GetPageCount(document.get());
    QVector<PDFAcroFormCanonicalizer::Field> formFields;
    formFields.reserve(resolvedFields.size());
    for (const Field &field : std::as_const(resolvedFields)) {
        if (pdfiumFieldType(field.type) == FPDF_FORMFIELD_UNKNOWN) {
            return failure(QStringLiteral(
                "This AcroForm field type is not implemented yet."));
        }
        const bool removing = field.operation == Operation::Remove;
        if (removing
            && field.type != FieldType::Dropdown
            && field.type != FieldType::RadioButton) {
            return failure(QStringLiteral(
                "Only TrePDF Dropdown and Radio Button fields can currently be removed."));
        }
        if (removing
            && ((field.type == FieldType::Dropdown
                 && !isTrePDFDropdownWidgetId(field.widgetId))
                || (field.type == FieldType::RadioButton
                    && !isTrePDFRadioButtonWidgetId(field.widgetId)))) {
            return failure(QStringLiteral(
                "The removed Widget ID is not a supported TrePDF form field ID."));
        }
        if (field.widgetId.isEmpty()
            || (!removing
                && (field.page < 0
                    || field.page >= pageCount
                    || field.pageRect.isEmpty()
                    || field.fieldName.isEmpty()))) {
            return failure(QStringLiteral(
                "The AcroForm Widget placement is invalid."));
        }

        if (field.operation == Operation::Add) {
            PdfPageHandle page(FPDF_LoadPage(document.get(), field.page));
            if (!page.get()) {
                return failure(QStringLiteral(
                    "The AcroForm Widget page could not be loaded."));
            }
            PdfAnnotationHandle placeholder(FPDFPage_CreateAnnot(
                page.get(),
                FPDF_ANNOT_SQUARE));
            if (!placeholder.get()) {
                return failure(QStringLiteral(
                    "The AcroForm Widget placeholder could not be created."));
            }
            const FS_RECTF rect{
                static_cast<float>(field.pageRect.left()),
                static_cast<float>(field.pageRect.bottom()),
                static_cast<float>(field.pageRect.right()),
                static_cast<float>(field.pageRect.top())};
            const auto *widgetId = reinterpret_cast<const FPDF_WCHAR *>(
                field.widgetId.utf16());
            if (!FPDFAnnot_SetRect(placeholder.get(), &rect)
                || !FPDFAnnot_SetStringValue(
                    placeholder.get(),
                    "NM",
                    widgetId)
                || !FPDFAnnot_SetFlags(
                    placeholder.get(),
                    FPDF_ANNOT_FLAG_PRINT)) {
                return failure(QStringLiteral(
                    "The AcroForm Widget placeholder could not be initialized."));
            }
        }

        PDFAcroFormCanonicalizer::Field canonicalField;
        canonicalField.widgetId = field.widgetId;
        canonicalField.fieldName = field.fieldName;
        canonicalField.tooltip = field.tooltip;
        canonicalField.value = field.value;
        canonicalField.defaultValue = field.defaultValue;
        canonicalField.options = field.options;
        canonicalField.pageRect = field.pageRect;
        switch (field.type) {
        case FieldType::Text:
            canonicalField.type
                = PDFAcroFormCanonicalizer::FieldType::Text;
            break;
        case FieldType::Checkbox:
            canonicalField.type
                = PDFAcroFormCanonicalizer::FieldType::Checkbox;
            break;
        case FieldType::RadioButton:
            canonicalField.type
                = PDFAcroFormCanonicalizer::FieldType::RadioButton;
            break;
        case FieldType::Dropdown:
            canonicalField.type
                = PDFAcroFormCanonicalizer::FieldType::Dropdown;
            break;
        case FieldType::Date:
        case FieldType::Signature:
            Q_UNREACHABLE();
        }
        canonicalField.fieldFlags = widgetFieldFlags(field);
        switch (field.operation) {
        case Operation::Add:
            canonicalField.operation
                = PDFAcroFormCanonicalizer::Operation::Add;
            break;
        case Operation::Update:
            canonicalField.operation
                = PDFAcroFormCanonicalizer::Operation::Update;
            break;
        case Operation::NormalizeAppearance:
            canonicalField.operation
                = PDFAcroFormCanonicalizer::Operation::NormalizeAppearance;
            break;
        case Operation::Remove:
            canonicalField.operation
                = PDFAcroFormCanonicalizer::Operation::Remove;
            break;
        }
        formFields.append(std::move(canonicalField));
    }

    SaveFileWriter writer(targetFilePath);
    if (!writer.open()) {
        return failure(writer.errorString());
    }
    if (!FPDF_SaveAsCopy(
            document.get(),
            &writer,
            FPDF_NO_INCREMENTAL | FPDF_SUBSET_NEW_FONTS)) {
        return failure(QStringLiteral(
            "PDFium could not write the AcroForm intermediate PDF."));
    }
    if (!writer.commit()) {
        return failure(writer.errorString());
    }

    QString finalizeError;
    if (!PDFAcroFormCanonicalizer::finalizeFields(
            targetFilePath,
            formFields,
            finalizeError)) {
        QFile::remove(targetFilePath);
        return failure(finalizeError);
    }
    if (!validateWrittenFields(
            targetFilePath,
            resolvedFields,
            finalizeError)) {
        QFile::remove(targetFilePath);
        return failure(finalizeError);
    }
    return {true, {}};
}
