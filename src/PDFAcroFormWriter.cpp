#include "PDFAcroFormWriter.h"

#include "PDFAcroFormCanonicalizer.h"
#include "PDFiumSynchronization.h"

#include <QFile>
#include <QHash>
#include <QSaveFile>

#include <fpdf_annot.h>
#include <fpdf_formfill.h>
#include <fpdf_save.h>
#include <fpdfview.h>

#include <utility>

namespace {

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
    return (field.readOnly ? FPDF_FORMFLAG_READONLY : 0)
        | (field.required ? FPDF_FORMFLAG_REQUIRED : 0);
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
        if (field.operation == PDFAcroFormWriter::Operation::Add
            || field.valueChanged) {
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
                != FPDF_FORMFIELD_TEXTFIELD) {
                error = QStringLiteral(
                    "An existing TrePDF Widget is not a Text Field.");
                pageSucceeded = false;
                break;
            }
            fields[*unresolved].value = formWideString(
                [&form, &annotation](FPDF_WCHAR *buffer,
                                     unsigned long byteCount) {
                    return FPDFAnnot_GetFormFieldValue(
                        form.get(),
                        annotation.get(),
                        buffer,
                        byteCount);
                });
            unresolvedFields.erase(unresolved);
        }
        FORM_OnBeforeClosePage(page.get(), form.get());
        if (!pageSucceeded) {
            return false;
        }
    }
    if (!unresolvedFields.isEmpty()) {
        error = QStringLiteral(
            "An existing TrePDF Text Field value could not be resolved.");
        return false;
    }
    return true;
}

bool validateWrittenTextFields(
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
    if (!form.get()
        || FPDF_GetFormType(document.get()) != FORMTYPE_ACRO_FORM) {
        error = QStringLiteral(
            "The written PDF does not expose a valid AcroForm session.");
        return false;
    }

    QHash<QString, const PDFAcroFormWriter::Field *> expectedFields;
    for (const PDFAcroFormWriter::Field &field : fields) {
        if (expectedFields.contains(field.widgetId)) {
            error = QStringLiteral(
                "The AcroForm validation request contains duplicate Widget IDs.");
            return false;
        }
        expectedFields.insert(field.widgetId, &field);
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
            const auto expected = expectedFields.constFind(widgetId);
            if (expected == expectedFields.cend()) {
                continue;
            }

            const PDFAcroFormWriter::Field &field = **expected;
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
            constexpr qreal coordinateTolerance = 0.05;
            const bool matches = pageIndex == field.page
                && FPDFAnnot_GetFormFieldType(
                       form.get(),
                       annotation.get())
                    == FPDF_FORMFIELD_TEXTFIELD
                && fieldName == field.fieldName
                && fieldValue == field.value
                && (fieldFlags
                    & (FPDF_FORMFLAG_READONLY
                       | FPDF_FORMFLAG_REQUIRED))
                    == expectedFlags
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
                    "A written TrePDF Text Field failed PDFium validation.");
                pageSucceeded = false;
                break;
            }
        }
        FORM_OnBeforeClosePage(page.get(), form.get());
        if (!pageSucceeded) {
            return false;
        }
    }

    for (auto expected = expectedFields.cbegin();
         expected != expectedFields.cend();
         ++expected) {
        if (matchedFields.value(expected.key()) != 1) {
            error = QStringLiteral(
                "A written TrePDF Text Field is missing after PDFium reopen.");
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

    const int pageCount = FPDF_GetPageCount(document.get());
    QVector<PDFAcroFormCanonicalizer::TextField> textFields;
    textFields.reserve(resolvedFields.size());
    for (const Field &field : std::as_const(resolvedFields)) {
        if (field.type != FieldType::Text) {
            return failure(QStringLiteral(
                "This AcroForm field type is not implemented yet."));
        }
        if (field.page < 0
            || field.page >= pageCount
            || field.pageRect.isEmpty()
            || field.fieldName.isEmpty()
            || field.widgetId.isEmpty()) {
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

        textFields.append({
            field.widgetId,
            field.fieldName,
            field.tooltip,
            field.value,
            field.defaultValue,
            field.pageRect,
            widgetFieldFlags(field),
            field.operation == Operation::Add
                ? PDFAcroFormCanonicalizer::Operation::Add
                : field.operation
                          == Operation::NormalizeAppearance
                    ? PDFAcroFormCanonicalizer::Operation::
                          NormalizeAppearance
                    : PDFAcroFormCanonicalizer::Operation::Update});
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
    if (!PDFAcroFormCanonicalizer::finalizeTextFields(
            targetFilePath,
            textFields,
            finalizeError)) {
        QFile::remove(targetFilePath);
        return failure(finalizeError);
    }
    if (!validateWrittenTextFields(
            targetFilePath,
            resolvedFields,
            finalizeError)) {
        QFile::remove(targetFilePath);
        return failure(finalizeError);
    }
    return {true, {}};
}
