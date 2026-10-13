#include "PDFAcroFormWriter.h"
#include "PDFAtomicSaveFile.h"

#include "PDFAcroFormCanonicalizer.h"
#include "PDFFormAppearance.h"
#include "PDFiumSynchronization.h"
#include "PDFiumDocument.h"

#include <QCoreApplication>
#include <QFile>
#include <QHash>
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

constexpr int kMaximumChoiceOptions = 4096;
constexpr qsizetype kMaximumChoiceOptionCharacters = 1024 * 1024;
constexpr int kMaximumTextFieldLength = 1000000;
constexpr int kButtonNoToggleToOff = 1 << 14;
constexpr int kButtonRadio = 1 << 15;
constexpr int kTextFieldDoNotScroll = 1 << 23;
constexpr int kTextFieldComb = 1 << 24;
constexpr int kChoiceCommitOnSelectionChange = 1 << 26;

bool isTrePDFTextFieldWidgetId(const QString &widgetId)
{
    constexpr qsizetype uuidLength = 36;
    const QString prefix = QStringLiteral("text-field-");
    if (!widgetId.startsWith(prefix)) {
        return false;
    }
    const QString suffix = widgetId.sliced(prefix.size());
    if (suffix.size() == uuidLength) {
        const QUuid uuid = QUuid::fromString(suffix);
        if (!uuid.isNull()
            && uuid.toString(QUuid::WithoutBraces).compare(
                   suffix,
                   Qt::CaseInsensitive)
                == 0) {
            return true;
        }
    }

    if (suffix.isEmpty()
        || suffix.size() > 20
        || suffix.front() == QLatin1Char('0')) {
        return false;
    }
    for (const QChar character : suffix) {
        if (!character.isDigit()) {
            return false;
        }
    }
    bool converted = false;
    const qulonglong sequence = suffix.toULongLong(&converted);
    return converted && sequence > 0;
}

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

bool isTrePDFListBoxWidgetId(const QString &widgetId)
{
    constexpr qsizetype uuidLength = 36;
    const QString prefix = QStringLiteral("listbox-field-");
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

using PdfDocumentHandle = PDFiumDocument;

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

    PDFAtomicSaveFile m_file;
};

int widgetFieldFlags(const PDFAcroFormWriter::Field &field)
{
    int flags = (field.readOnly ? FPDF_FORMFLAG_READONLY : 0)
        | (field.required ? FPDF_FORMFLAG_REQUIRED : 0);
    if (field.type == PDFAcroFormWriter::FieldType::Dropdown) {
        flags |= FPDF_FORMFLAG_CHOICE_COMBO;
    } else if (field.type == PDFAcroFormWriter::FieldType::ListBox) {
        flags |= FPDF_FORMFLAG_CHOICE_MULTI_SELECT;
    } else if (field.type == PDFAcroFormWriter::FieldType::Text) {
        if (field.comb) {
            flags |= kTextFieldDoNotScroll | kTextFieldComb;
        } else if (field.multiline) {
            flags |= FPDF_FORMFLAG_TEXT_MULTILINE;
        }
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
    case PDFAcroFormWriter::FieldType::ListBox:
        return FPDF_FORMFIELD_LISTBOX;
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

bool isFixedChoiceField(PDFAcroFormWriter::FieldType type)
{
    return type == PDFAcroFormWriter::FieldType::Dropdown
        || type == PDFAcroFormWriter::FieldType::ListBox;
}

bool validChoiceOptions(const QStringList &options)
{
    if (options.isEmpty() || options.size() > kMaximumChoiceOptions) {
        return false;
    }
    QSet<QString> uniqueOptions;
    qsizetype totalCharacters = 0;
    for (const QString &option : options) {
        totalCharacters += option.size();
        if (option.isEmpty()
            || totalCharacters > kMaximumChoiceOptionCharacters
            || uniqueOptions.contains(option)) {
            return false;
        }
        uniqueOptions.insert(option);
    }
    return true;
}

bool normalizedListBoxSelection(const QStringList &options,
                                const QStringList &selectedValues,
                                QStringList &normalizedSelection)
{
    QSet<QString> requestedValues;
    for (const QString &value : selectedValues) {
        if (!options.contains(value)
            || requestedValues.contains(value)) {
            return false;
        }
        requestedValues.insert(value);
    }
    normalizedSelection.clear();
    normalizedSelection.reserve(requestedValues.size());
    for (const QString &option : options) {
        if (requestedValues.contains(option)) {
            normalizedSelection.append(option);
        }
    }
    return normalizedSelection.size() == requestedValues.size();
}

QVector<int> listBoxSelectedIndices(const QStringList &options,
                                    const QStringList &selectedValues)
{
    QVector<int> indices;
    indices.reserve(selectedValues.size());
    for (int optionIndex = 0;
         optionIndex < options.size();
         ++optionIndex) {
        if (selectedValues.contains(options.at(optionIndex))) {
            indices.append(optionIndex);
        }
    }
    return indices;
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
            && isFixedChoiceField(field.type)
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
            const bool preserveListBoxSelection
                = resolvedField.type
                      == PDFAcroFormWriter::FieldType::ListBox
                && !resolvedField.valueChanged;
            if (!resolvedField.valueChanged
                && !preserveListBoxSelection) {
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
                    == PDFAcroFormWriter::FieldType::Text
                && resolvedField.maxLength > 0
                && resolvedField.value.size()
                    > resolvedField.maxLength) {
                // The PDFium form session can hold a newer native value than
                // the designer snapshot when the limit was changed. Apply the
                // requested limit to that live value instead of rejecting the
                // otherwise valid save.
                resolvedField.value.truncate(resolvedField.maxLength);
                resolvedField.valueChanged = true;
            }
            if (isFixedChoiceField(resolvedField.type)
                && (!resolvedField.optionsChanged
                    || preserveListBoxSelection)) {
                const int optionCount = FPDFAnnot_GetOptionCount(
                    form.get(),
                    annotation.get());
                QStringList options;
                QStringList selectedValues;
                if (optionCount > 0
                    && optionCount <= kMaximumChoiceOptions) {
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
                        if (preserveListBoxSelection
                            && FPDFAnnot_IsOptionSelected(
                                form.get(),
                                annotation.get(),
                                optionIndex)) {
                            selectedValues.append(options.constLast());
                        }
                    }
                }
                if (!validChoiceOptions(options)) {
                    error = resolvedField.type
                            == PDFAcroFormWriter::FieldType::ListBox
                        ? QStringLiteral(
                              "An existing TrePDF List Box has invalid options.")
                        : QStringLiteral(
                              "An existing TrePDF Dropdown has invalid options.");
                    pageSucceeded = false;
                    break;
                }
                if (preserveListBoxSelection) {
                    resolvedField.selectedValues
                        = std::move(selectedValues);
                    resolvedField.value
                        = resolvedField.selectedValues.isEmpty()
                        ? QString()
                        : resolvedField.selectedValues.constFirst();
                }
                if (!resolvedField.optionsChanged) {
                    resolvedField.options = std::move(options);
                }
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
    QString &error,
    const QByteArray &password)
{
    assertPDFiumProcessLockHeld();
    PdfDocumentHandle document = PDFiumDocument::open(filePath, password);
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
            const bool listBox
                = field.type == PDFAcroFormWriter::FieldType::ListBox;
            const bool fixedChoice = dropdown || listBox;
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
            const QString defaultValue = fixedChoice
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
            bool choiceMatches = !fixedChoice;
            if (fixedChoice) {
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
                const QVector<int> expectedIndices = listBox
                    ? listBoxSelectedIndices(
                          field.options,
                          field.selectedValues)
                    : QVector<int>({
                          static_cast<int>(
                              field.options.indexOf(field.value))});
                const int firstExpectedIndex = expectedIndices.isEmpty()
                    ? 0
                    : expectedIndices.constFirst();
                const int expectedTopIndex
                    = PDFFormAppearance::listBoxTopIndex(
                        field.options.size(),
                        firstExpectedIndex,
                        field.pageRect.height());
                float topIndex = 0.0f;
                const bool topIndexMatches = !listBox
                    || (FPDFAnnot_GetNumberValue(
                            annotation.get(),
                            "TI",
                            &topIndex)
                        && std::isfinite(topIndex)
                        && topIndex
                            == static_cast<float>(expectedTopIndex));
                choiceMatches = options == field.options
                    && defaultValue == field.defaultValue
                    && selectedIndices == expectedIndices
                    && topIndexMatches
                    && (FPDFAnnot_GetFlags(annotation.get())
                        & FPDF_ANNOT_FLAG_PRINT)
                    && hasAppearanceVersion
                    && qFuzzyCompare(
                        appearanceVersion,
                        static_cast<float>(
                            listBox
                                ? PDFFormAppearance::kListBoxVersion
                                : PDFFormAppearance::kDropdownVersion));
            }
            bool maxLengthMatches = true;
            if (field.type == PDFAcroFormWriter::FieldType::Text) {
                float writtenMaxLength = 0.0f;
                const bool hasMaxLength = FPDFAnnot_GetNumberValue(
                    annotation.get(),
                    "MaxLen",
                    &writtenMaxLength);
                maxLengthMatches = field.maxLength > 0
                    ? hasMaxLength
                        && std::isfinite(writtenMaxLength)
                        && writtenMaxLength
                            == static_cast<float>(field.maxLength)
                    : !hasMaxLength;
            }
            constexpr qreal coordinateTolerance = 0.05;
            // PDFium can surface /DV through the scalar value accessor when a
            // multi-select List Box has no selected indices. The selected-index
            // comparison above is the authoritative validation for List Boxes.
            const bool fieldValueMatches = listBox
                || fieldValue
                    == (radioButton
                            ? expectedRadioValues.value(
                                  field.fieldName,
                                  QStringLiteral("Off"))
                            : field.value);
            const bool matches = pageIndex == field.page
                && FPDFAnnot_GetFormFieldType(
                       form.get(),
                       annotation.get())
                    == pdfiumFieldType(field.type)
                && fieldName == field.fieldName
                && fieldValueMatches
                && checkableMatches
                && choiceMatches
                && (fieldFlags
                    & (FPDF_FORMFLAG_READONLY
                       | FPDF_FORMFLAG_REQUIRED
                       | FPDF_FORMFLAG_TEXT_MULTILINE
                       | kTextFieldDoNotScroll
                       | kTextFieldComb
                       | FPDF_FORMFLAG_CHOICE_COMBO
                       | FPDF_FORMFLAG_CHOICE_EDIT
                       | FPDF_FORMFLAG_CHOICE_MULTI_SELECT
                       | kButtonNoToggleToOff
                       | kButtonRadio
                       | kChoiceCommitOnSelectionChange))
                    == expectedFlags
                && maxLengthMatches
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
    const QVector<Field> &fields,
    const ApplicationFonts::FontFaceData &fontFace,
    const QByteArray &password)
{
    assertPDFiumProcessLockHeld();
    PdfDocumentHandle document = PDFiumDocument::open(sourceFilePath, password);
    if (!document.get()) {
        return failure(QStringLiteral(
            "PDFium could not open the intermediate PDF."));
    }
    if (!fields.isEmpty() && FPDF_GetSecurityHandlerRevision(document.get()) >= 0) {
        // The byte-level canonicalizer cannot encrypt the field objects it adds.
        // Reject before writing instead of removing the source protection.
        //% "Creating fields in password-protected PDFs is not supported yet."
        return failure(qtTrId("EncryptedPdfFormCreationUnsupported"));
    }

    for (const Field &field : fields) {
        if (field.type == FieldType::ListBox
            && !isTrePDFListBoxWidgetId(field.widgetId)) {
            return failure(QStringLiteral(
                "The List Box Widget ID is invalid."));
        }
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
        if (field.type == FieldType::Text) {
            if (field.maxLength < 0
                || field.maxLength > kMaximumTextFieldLength
                || (field.comb
                    && (field.multiline || field.maxLength <= 0))
                || (field.maxLength > 0
                    && (field.value.size() > field.maxLength
                        || field.defaultValue.size()
                            > field.maxLength))) {
                return failure(QStringLiteral(
                    "The Text Field character limit is invalid."));
            }
        } else if (field.multiline
                   || field.comb
                   || field.maxLength != 0) {
            return failure(QStringLiteral(
                "Only Text Fields support multi-line input and character limits."));
        }
        if (field.type == FieldType::Checkbox
            || field.type == FieldType::RadioButton) {
            field.value = normalizedCheckboxValue(field.value);
            field.defaultValue = normalizedCheckboxValue(field.defaultValue);
        } else if (field.type == FieldType::Dropdown) {
            if (!validChoiceOptions(field.options)
                || !field.options.contains(field.value)
                || !field.options.contains(field.defaultValue)) {
                return failure(QStringLiteral(
                    "The Dropdown options or selected values are invalid."));
            }
        } else if (field.type == FieldType::ListBox) {
            QStringList normalizedSelection;
            if (!validChoiceOptions(field.options)
                || !field.options.contains(field.defaultValue)
                || !normalizedListBoxSelection(
                    field.options,
                    field.selectedValues,
                    normalizedSelection)) {
                return failure(QStringLiteral(
                    "The List Box options or selected values are invalid."));
            }
            field.selectedValues = std::move(normalizedSelection);
            field.value = field.selectedValues.isEmpty()
                ? QString()
                : field.selectedValues.constFirst();
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
            && field.type != FieldType::Text
            && field.type != FieldType::Dropdown
            && field.type != FieldType::ListBox
            && field.type != FieldType::RadioButton) {
            return failure(QStringLiteral(
                "Only TrePDF Text Field, Dropdown, List Box, and Radio Button fields can currently be removed."));
        }
        if (removing
            && ((field.type == FieldType::Text
                 && !isTrePDFTextFieldWidgetId(field.widgetId))
                || (field.type == FieldType::Dropdown
                 && !isTrePDFDropdownWidgetId(field.widgetId))
                || (field.type == FieldType::ListBox
                    && !isTrePDFListBoxWidgetId(field.widgetId))
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
        canonicalField.selectedValues = field.selectedValues;
        canonicalField.defaultValue = field.defaultValue;
        canonicalField.options = field.options;
        canonicalField.pageRect = field.pageRect;
        canonicalField.maxLength = field.maxLength;
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
        case FieldType::ListBox:
            canonicalField.type
                = PDFAcroFormCanonicalizer::FieldType::ListBox;
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
            fontFace,
            finalizeError)) {
        QFile::remove(targetFilePath);
        return failure(finalizeError);
    }
    if (!validateWrittenFields(
            targetFilePath,
            resolvedFields,
            finalizeError,
            password)) {
        QFile::remove(targetFilePath);
        return failure(finalizeError);
    }
    return {true, {}};
}
