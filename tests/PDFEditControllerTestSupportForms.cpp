#include "PDFEditControllerTestSupport.h"

namespace PDFEditControllerTestSupport {


bool createLegacyTrePDFRadioButtonPdf(const QString &filePath)
{
    const QByteArray offContent(
        "q 1 1 1 rg 0 0 18 18 re f 0 0 0 RG 1 w "
        "2 2 14 14 re S Q");
    const QByteArray onContent(
        "q 1 1 1 rg 0 0 18 18 re f 0 0 0 RG 1 w "
        "2 2 14 14 re S 0 0 0 rg 6 6 6 6 re f Q");
    const auto appearanceObject = [](const QByteArray &content) {
        return QByteArray(
                   "<< /Type /XObject /Subtype /Form /FormType 1 "
                   "/BBox [0 0 18 18] /Resources << >> /Length ")
            + QByteArray::number(content.size())
            + QByteArray(" >>\nstream\n")
            + content
            + QByteArray("\nendstream");
    };
    return writePdfObjects(
        filePath,
        {
            QByteArray(
                "<< /Type /Catalog /Pages 2 0 R /AcroForm 5 0 R >>"),
            QByteArray(
                "<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R "
                "/MediaBox [0 0 595 842] /Resources << >> "
                "/Annots [4 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Widget /FT /Btn /Ff 49152 "
                "/T (PreferredContact) /TU (Preferred contact) "
                "/NM (radio-field-11111111-1111-1111-1111-111111111111) "
                "/P 3 0 R /F 4 /Rect [100 600 118 618] "
                "/V /Yes /DV /Yes /AS /Yes "
                "/AP << /N << /Off 6 0 R /Yes 7 0 R >> >> "
                "/TrePDFApVersion 2 >>"),
            QByteArray(
                "<< /Fields [4 0 R] /NeedAppearances false >>"),
            appearanceObject(offContent),
            appearanceObject(onContent),
        });
}

bool createLegacyTrePDFListBoxPdf(const QString &filePath)
{
    const QByteArray appearanceContent(
        "q\n"
        "1 1 1 rg\n"
        "0 0 220 72 re f\n"
        "0.55 0.55 0.55 RG\n"
        "1 w\n"
        "0.5 0.5 219 71 re S\n"
        "Q\n");
    const QByteArray appearanceStream
        = QByteArray(
              "<< /Type /XObject /Subtype /Form /FormType 1 "
              "/BBox [0 0 220 72] /Resources << >> /Length ")
        + QByteArray::number(appearanceContent.size())
        + QByteArray(" >>\nstream\n")
        + appearanceContent
        + QByteArray("endstream");

    return writePdfObjects(
        filePath,
        {
            QByteArray(
                "<< /Type /Catalog /Pages 2 0 R /AcroForm 5 0 R >>"),
            QByteArray(
                "<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R "
                "/MediaBox [0 0 595 842] "
                "/Resources << /Font << /Helv 7 0 R >> >> "
                "/Annots [4 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Widget /FT /Ch /Ff 0 "
                "/T (")
                + QByteArray(kLegacyListBoxFieldName)
                + QByteArray(") /TU (Legacy office list) /NM (")
                + QByteArray(kLegacyListBoxWidgetId)
                + QByteArray(") /P 3 0 R /F 4 "
                    "/Rect [80 620 300 692] "
                    "/Opt [(Alpha) (Beta) (Gamma)] "
                    "/V (Beta) /DV (Alpha) /I [1] /TI 0 "
                    "/DA (/Helv 12 Tf 0 g) "
                    "/AP << /N 6 0 R >> /TrePDFApVersion 1 >>"),
            QByteArray(
                "<< /Fields [4 0 R] /NeedAppearances false "
                "/DR << /Font << /Helv 7 0 R >> >> "
                "/DA (/Helv 12 Tf 0 g) >>"),
            appearanceStream,
            QByteArray(
                "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"),
        });
}

bool createDropdownDeleteRegressionPdf(const QString &filePath)
{
    return writePdfObjects(
        filePath,
        {
            QByteArray(
                "<< /Type /Catalog /Pages 2 0 R "
                "/TrePDFRegression 4 0 R >>"),
            QByteArray(
                "<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R "
                "/MediaBox [0 0 595 842] /Resources << >> >>"),
            QByteArray("(<<)"),
        });
}

bool createPushButtonActionPdf(
    const QString &filePath,
    const QByteArray &uri)
{
    const QByteArray appearanceContent(
        "q\n"
        "0.96 0.94 0.91 rg\n"
        "0 0 80 40 re f\n"
        "0.75 0.35 0.10 RG\n"
        "1 w\n"
        "0.5 0.5 79 39 re S\n"
        "Q\n");
    const QByteArray appearanceStream
        = QByteArray(
              "<< /Type /XObject /Subtype /Form /FormType 1 "
              "/BBox [0 0 80 40] /Resources << >> /Length ")
        + QByteArray::number(appearanceContent.size())
        + QByteArray(" >>\nstream\n")
        + appearanceContent
        + QByteArray("endstream");

    return writePdfObjects(
        filePath,
        {
            QByteArray(
                "<< /Type /Catalog /Pages 2 0 R "
                "/AcroForm 9 0 R >>"),
            QByteArray(
                "<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] "
                "/Count 3 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R "
                "/MediaBox [0 0 360 480] /Resources << >> >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R "
                "/MediaBox [0 0 360 480] /Resources << >> "
                "/Annots [6 0 R 7 0 R 8 0 R] >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R "
                "/MediaBox [0 0 360 480] /Resources << >> >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Widget /FT /Btn "
                "/Ff 65536 /T (PrevButton) /P 4 0 R /F 4 "
                "/Rect [20 400 100 440] /H /P "
                "/AP << /N 10 0 R >> "
                "/A << /S /GoTo /D [3 0 R /Fit] >> >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Widget /FT /Btn "
                "/Ff 65536 /T (UriButton) /P 4 0 R /F 4 "
                "/Rect [140 400 220 440] /H /P "
                "/AP << /N 10 0 R >> /A << /S /URI /URI (")
                + uri
                + QByteArray(") >> >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Widget /FT /Btn "
                "/Ff 65536 /T (NextButton) /P 4 0 R /F 4 "
                "/Rect [260 400 340 440] /H /P "
                "/AP << /N 10 0 R >> "
                "/A << /S /GoTo /D [5 0 R /Fit] >> >>"),
            QByteArray(
                "<< /Fields [6 0 R 7 0 R 8 0 R] "
                "/NeedAppearances false >>"),
            appearanceStream,
        });
}

bool createChoiceFieldsPdf(const QString &filePath)
{
    return writePdfObjects(
        filePath,
        {
            QByteArray(
                "<< /Type /Catalog /Pages 2 0 R "
                "/AcroForm 7 0 R >>"),
            QByteArray(
                "<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R "
                "/MediaBox [0 0 360 480] /Resources << >> "
                "/Annots [4 0 R 5 0 R 6 0 R 9 0 R 10 0 R 11 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Widget /FT /Ch "
                "/Ff 131072 /T (Transport) /TU (Transport choice) "
                "/P 3 0 R /F 4 /Rect [20 400 180 430] "
                "/Opt [(Airplane) (Bicycle) (Car)] "
                "/V (Airplane) /I [0] /DA (/Helv 12 Tf 0 g) >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Widget /FT /Ch "
                "/Ff 0 /T (Month) /TU (Month list) "
                "/P 3 0 R /F 4 /Rect [20 260 180 380] "
                "/Opt [(March) (April) (May) (June)] "
                "/V (April) /I [1] /DA (/Helv 12 Tf 0 g) >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Widget /FT /Ch "
                "/Ff 2097152 /T (Colors) /TU (Color list) "
                "/P 3 0 R /F 4 /Rect [200 260 340 380] "
                "/Opt [(Red) (Yellow) (Blue)] "
                "/V [(Red) (Blue)] /I [0 2] "
                "/DA (/Helv 12 Tf 0 g) >>"),
            QByteArray(
                "<< /Fields [4 0 R 5 0 R 6 0 R 9 0 R 10 0 R 11 0 R] "
                "/NeedAppearances true "
                "/DA (/Helv 12 Tf 0 g) "
                "/DR << /Font << /Helv 8 0 R >> >> >>"),
            QByteArray(
                "<< /Type /Font /Subtype /Type1 "
                "/BaseFont /Helvetica >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Widget /FT /Ch "
                "/Ff 393216 /T (EditableTransport) "
                "/P 3 0 R /F 4 /Rect [20 210 180 240] "
                "/Opt [(Airplane) (Bicycle)] "
                "/V (Airplane) /I [0] /DA (/Helv 12 Tf 0 g) >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Widget /FT /Ch "
                "/Ff 67239936 /T (ImmediateTransport) "
                "/P 3 0 R /F 4 /Rect [200 210 340 240] "
                "/Opt [(Airplane) (Bicycle)] "
                "/V (Airplane) /I [0] /DA (/Helv 12 Tf 0 g) >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Widget /FT /Ch "
                "/Ff 131072 /T (ScriptedTransport) "
                "/P 3 0 R /F 4 /Rect [20 160 180 190] "
                "/Opt [(Airplane) (Bicycle)] "
                "/V (Airplane) /I [0] /DA (/Helv 12 Tf 0 g) "
                "/AA << /D << /S /JavaScript /JS () >> >> >>"),
        });
}

bool createTrePDFDropdownWithExportValuesPdf(const QString &filePath)
{
    return writePdfObjects(
        filePath,
        {
            QByteArray(
                "<< /Type /Catalog /Pages 2 0 R /AcroForm 5 0 R >>"),
            QByteArray(
                "<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R "
                "/MediaBox [0 0 360 480] /Resources << >> "
                "/Annots [4 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Widget /FT /Ch "
                "/Ff 131072 /T (Transport) /TU (Transport choice) "
                "/NM (dropdown-field-11111111-1111-1111-1111-111111111111) "
                "/P 3 0 R /F 4 /Rect [20 400 180 430] "
                "/Opt [[(Airplane) (Airplane)] [(bike) (Bicycle)] "
                "[(car) (Car)]] /V (Airplane) /DV (Airplane) /I [0] "
                "/DA (/Helv 12 Tf 0 g) >>"),
            QByteArray(
                "<< /Fields [4 0 R] /NeedAppearances true "
                "/DA (/Helv 12 Tf 0 g) "
                "/DR << /Font << /Helv 6 0 R >> >> >>"),
            QByteArray(
                "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"),
        });
}

QVector<int> selectedChoiceOptions(
    const QString &filePath,
    int annotationIndex)
{
    PDFiumProcessLock pdfiumLock;
    QVector<int> selectedIndices;
    const QByteArray encodedPath = QFile::encodeName(filePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(
        encodedPath.constData(),
        nullptr);
    if (!document) {
        return selectedIndices;
    }

    FPDF_FORMFILLINFO formFillInfo = {};
    formFillInfo.version = 2;
    formFillInfo.xfa_disabled = true;
    FPDF_FORMHANDLE formHandle = FPDFDOC_InitFormFillEnvironment(
        document,
        &formFillInfo);
    FPDF_PAGE page = FPDF_LoadPage(document, 0);
    if (formHandle && page) {
        FORM_OnAfterLoadPage(page, formHandle);
        FPDF_ANNOTATION annotation = FPDFPage_GetAnnot(
            page,
            annotationIndex);
        if (annotation) {
            const int optionCount = FPDFAnnot_GetOptionCount(
                formHandle,
                annotation);
            for (int optionIndex = 0;
                 optionIndex < optionCount;
                 ++optionIndex) {
                if (FPDFAnnot_IsOptionSelected(
                        formHandle,
                        annotation,
                        optionIndex)) {
                    selectedIndices.append(optionIndex);
                }
            }
            FPDFPage_CloseAnnot(annotation);
        }
        FORM_OnBeforeClosePage(page, formHandle);
    }
    if (page) {
        FPDF_ClosePage(page);
    }
    if (formHandle) {
        FPDFDOC_ExitFormFillEnvironment(formHandle);
    }
    FPDF_CloseDocument(document);
    return selectedIndices;
}

AcroFormFieldInspection inspectAcroFormField(
    const QString &filePath,
    const QString &targetFieldName,
    int pageIndex,
    const QString &targetWidgetId)
{
    PDFiumProcessLock pdfiumLock;
    AcroFormFieldInspection inspection;
    const QByteArray encodedPath = QFile::encodeName(filePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(
        encodedPath.constData(),
        nullptr);
    if (!document) {
        return inspection;
    }

    inspection.formType = FPDF_GetFormType(document);
    FPDF_FORMFILLINFO formFillInfo = {};
    formFillInfo.version = 2;
    formFillInfo.xfa_disabled = true;
    FPDF_FORMHANDLE formHandle = FPDFDOC_InitFormFillEnvironment(
        document,
        &formFillInfo);
    FPDF_PAGE page = FPDF_LoadPage(document, pageIndex);
    inspection.readable = formHandle && page;
    if (formHandle && page) {
        FORM_OnAfterLoadPage(page, formHandle);
        const auto readWideString = [](auto getter) {
            const unsigned long byteCount = getter(nullptr, 0);
            if (byteCount < sizeof(FPDF_WCHAR)) {
                return QString();
            }
            QVector<FPDF_WCHAR> buffer(
                static_cast<qsizetype>(
                    (byteCount + sizeof(FPDF_WCHAR) - 1)
                    / sizeof(FPDF_WCHAR)));
            const unsigned long actualByteCount = getter(
                buffer.data(),
                byteCount);
            qsizetype characterCount = static_cast<qsizetype>(
                actualByteCount / sizeof(FPDF_WCHAR));
            while (characterCount > 0
                   && buffer.at(characterCount - 1) == 0) {
                --characterCount;
            }
            return QString::fromUtf16(
                reinterpret_cast<const char16_t *>(
                    buffer.constData()),
                characterCount);
        };
        const int annotationCount = FPDFPage_GetAnnotCount(page);
        for (int index = 0; index < annotationCount; ++index) {
            FPDF_ANNOTATION annotation = FPDFPage_GetAnnot(page, index);
            if (!annotation) {
                continue;
            }
            if (FPDFAnnot_GetSubtype(annotation) != FPDF_ANNOT_WIDGET) {
                FPDFPage_CloseAnnot(annotation);
                continue;
            }

            ++inspection.widgetCount;
            const QString fieldName = readWideString(
                [formHandle, annotation](FPDF_WCHAR *buffer,
                                         unsigned long byteCount) {
                    return FPDFAnnot_GetFormFieldName(
                        formHandle,
                        annotation,
                        buffer,
                        byteCount);
                });
            const QString widgetId = readWideString(
                [annotation](FPDF_WCHAR *buffer,
                             unsigned long byteCount) {
                    return FPDFAnnot_GetStringValue(
                        annotation,
                        "NM",
                        buffer,
                        byteCount);
                });
            if (inspection.fieldType == FPDF_FORMFIELD_UNKNOWN
                && (targetFieldName.isEmpty()
                    || fieldName == targetFieldName)
                && (targetWidgetId.isEmpty()
                    || widgetId == targetWidgetId)) {
                inspection.fieldType = FPDFAnnot_GetFormFieldType(
                    formHandle,
                    annotation);
                inspection.fieldFlags = FPDFAnnot_GetFormFieldFlags(
                    formHandle,
                    annotation);
                inspection.widgetId = widgetId;
                inspection.name = fieldName;
                inspection.value = readWideString(
                    [formHandle, annotation](FPDF_WCHAR *buffer,
                                             unsigned long byteCount) {
                        return FPDFAnnot_GetFormFieldValue(
                            formHandle,
                            annotation,
                            buffer,
                            byteCount);
                    });
                inspection.defaultValue = readWideString(
                    [annotation](FPDF_WCHAR *buffer,
                                 unsigned long byteCount) {
                        return FPDFAnnot_GetStringValue(
                            annotation,
                            "DV",
                            buffer,
                            byteCount);
                    });
                if (inspection.defaultValue.isEmpty()) {
                    FPDF_ANNOTATION parent = FPDFAnnot_GetLinkedAnnot(
                        annotation,
                        "Parent");
                    if (parent) {
                        inspection.defaultValue = readWideString(
                            [parent](FPDF_WCHAR *buffer,
                                     unsigned long byteCount) {
                                return FPDFAnnot_GetStringValue(
                                    parent,
                                    "DV",
                                    buffer,
                                    byteCount);
                            });
                        FPDFPage_CloseAnnot(parent);
                    }
                }
                if (inspection.fieldType == FPDF_FORMFIELD_RADIOBUTTON) {
                    float defaultChecked = 0.0f;
                    if (FPDFAnnot_GetNumberValue(
                            annotation,
                            PDFFormAppearance::kRadioButtonDefaultCheckedKey,
                            &defaultChecked)) {
                        inspection.defaultValue = defaultChecked != 0.0f
                            ? QStringLiteral("Yes")
                            : QStringLiteral("Off");
                    } else if (inspection.defaultValue.isEmpty()) {
                        inspection.defaultValue = QStringLiteral("Off");
                    }
                }
                const int optionCount = FPDFAnnot_GetOptionCount(
                    formHandle,
                    annotation);
                if (optionCount > 0) {
                    inspection.options.reserve(optionCount);
                    for (int optionIndex = 0;
                         optionIndex < optionCount;
                         ++optionIndex) {
                        inspection.options.append(readWideString(
                            [formHandle,
                             annotation,
                             optionIndex](FPDF_WCHAR *buffer,
                                          unsigned long byteCount) {
                                return FPDFAnnot_GetOptionLabel(
                                    formHandle,
                                    annotation,
                                    optionIndex,
                                    buffer,
                                    byteCount);
                            }));
                        if (FPDFAnnot_IsOptionSelected(
                                formHandle,
                                annotation,
                                optionIndex)) {
                            inspection.selectedIndices.append(optionIndex);
                        }
                    }
                }
                FPDFAnnot_GetRect(annotation, &inspection.rect);
                inspection.hasAppearance = FPDFAnnot_HasKey(
                    annotation,
                    "AP");
                inspection.exportValue = readWideString(
                    [formHandle, annotation](FPDF_WCHAR *buffer,
                                             unsigned long byteCount) {
                        return FPDFAnnot_GetFormFieldExportValue(
                            formHandle,
                            annotation,
                            buffer,
                            byteCount);
                    });
                inspection.checked = FPDFAnnot_IsChecked(
                    formHandle,
                    annotation);
                inspection.annotationFlags = FPDFAnnot_GetFlags(annotation);
                inspection.controlCount = FPDFAnnot_GetFormControlCount(
                    formHandle,
                    annotation);
                inspection.controlIndex = FPDFAnnot_GetFormControlIndex(
                    formHandle,
                    annotation);
                inspection.hasParent = FPDFAnnot_HasKey(
                    annotation,
                    "Parent");
                inspection.hasLocalFieldType = FPDFAnnot_HasKey(
                    annotation,
                    "FT");
                inspection.hasLocalFieldFlags = FPDFAnnot_HasKey(
                    annotation,
                    "Ff");
                inspection.hasLocalValue = FPDFAnnot_HasKey(
                    annotation,
                    "V");
                inspection.hasAppearanceVersion
                    = FPDFAnnot_GetNumberValue(
                        annotation,
                        PDFFormAppearance::kVersionKey,
                        &inspection.appearanceVersion);
                inspection.appearanceStream = readWideString(
                    [annotation](FPDF_WCHAR *buffer,
                                 unsigned long byteCount) {
                        return FPDFAnnot_GetAP(
                            annotation,
                            FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                            buffer,
                            byteCount);
                    });
                inspection.hasMaxLength = FPDFAnnot_GetNumberValue(
                    annotation,
                    "MaxLen",
                    &inspection.maxLength);
            }
            FPDFPage_CloseAnnot(annotation);
        }
        if (inspection.fieldType != FPDF_FORMFIELD_UNKNOWN) {
            const FS_POINTF center{
                (inspection.rect.left + inspection.rect.right) / 2.0f,
                (inspection.rect.bottom + inspection.rect.top) / 2.0f};
            FPDF_ANNOTATION hit = FPDFAnnot_GetFormFieldAtPoint(
                formHandle,
                page,
                &center);
            inspection.pointHit = hit != nullptr;
            if (hit) {
                FPDFPage_CloseAnnot(hit);
            }
            inspection.focusHandled = FORM_OnFocus(
                formHandle,
                page,
                0,
                center.x,
                center.y);
        }
        FORM_OnBeforeClosePage(page, formHandle);
    }
    if (page) {
        FPDF_ClosePage(page);
    }
    if (formHandle) {
        FPDFDOC_ExitFormFillEnvironment(formHandle);
    }
    FPDF_CloseDocument(document);
    return inspection;
}

QVector<bool> clickAcroFormFieldTwice(
    const QString &filePath,
    const QString &targetFieldName,
    int pageIndex)
{
    PDFiumProcessLock pdfiumLock;
    QVector<bool> states;
    const QByteArray encodedPath = QFile::encodeName(filePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(
        encodedPath.constData(),
        nullptr);
    FPDF_FORMFILLINFO formFillInfo = {};
    formFillInfo.version = 2;
    formFillInfo.xfa_disabled = true;
    FPDF_FORMHANDLE formHandle = document
        ? FPDFDOC_InitFormFillEnvironment(document, &formFillInfo)
        : nullptr;
    FPDF_PAGE page = document
        ? FPDF_LoadPage(document, pageIndex)
        : nullptr;
    if (formHandle && page) {
        FORM_OnAfterLoadPage(page, formHandle);
        const auto readWideString = [](auto getter) {
            const unsigned long byteCount = getter(nullptr, 0);
            if (byteCount < sizeof(FPDF_WCHAR)) {
                return QString();
            }
            QVector<FPDF_WCHAR> buffer(
                static_cast<qsizetype>(
                    byteCount / sizeof(FPDF_WCHAR)));
            const unsigned long actualByteCount = getter(
                buffer.data(),
                byteCount);
            qsizetype characterCount = static_cast<qsizetype>(
                actualByteCount / sizeof(FPDF_WCHAR));
            while (characterCount > 0
                   && buffer.at(characterCount - 1) == 0) {
                --characterCount;
            }
            return QString::fromUtf16(
                reinterpret_cast<const char16_t *>(buffer.constData()),
                characterCount);
        };
        const int annotationCount = FPDFPage_GetAnnotCount(page);
        for (int index = 0; index < annotationCount; ++index) {
            FPDF_ANNOTATION annotation = FPDFPage_GetAnnot(page, index);
            if (!annotation) {
                continue;
            }
            const QString fieldName = readWideString(
                [formHandle, annotation](FPDF_WCHAR *buffer,
                                         unsigned long byteCount) {
                    return FPDFAnnot_GetFormFieldName(
                        formHandle,
                        annotation,
                        buffer,
                        byteCount);
                });
            FS_RECTF rect = {};
            if (FPDFAnnot_GetSubtype(annotation) != FPDF_ANNOT_WIDGET
                || fieldName != targetFieldName
                || !FPDFAnnot_GetRect(annotation, &rect)) {
                FPDFPage_CloseAnnot(annotation);
                continue;
            }

            const double centerX = (rect.left + rect.right) / 2.0;
            const double centerY = (rect.bottom + rect.top) / 2.0;
            states.append(FPDFAnnot_IsChecked(formHandle, annotation));
            for (int click = 0; click < 2; ++click) {
                FORM_OnFocus(
                    formHandle,
                    page,
                    0,
                    centerX,
                    centerY);
                FORM_OnLButtonDown(
                    formHandle,
                    page,
                    0,
                    centerX,
                    centerY);
                FORM_OnLButtonUp(
                    formHandle,
                    page,
                    0,
                    centerX,
                    centerY);
                states.append(FPDFAnnot_IsChecked(
                    formHandle,
                    annotation));
            }
            FPDFPage_CloseAnnot(annotation);
            break;
        }
        FORM_OnBeforeClosePage(page, formHandle);
    }
    if (page) {
        FPDF_ClosePage(page);
    }
    if (formHandle) {
        FPDFDOC_ExitFormFillEnvironment(formHandle);
    }
    if (document) {
        FPDF_CloseDocument(document);
    }
    return states;
}

} // namespace PDFEditControllerTestSupport
