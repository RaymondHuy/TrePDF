#include "PDFDocument.h"
#include "PDFSplitController.h"
#include "PDFiumSynchronization.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <fpdf_annot.h>
#include <fpdf_edit.h>
#include <fpdf_formfill.h>
#include <fpdf_save.h>
#include <fpdf_text.h>
#include <fpdfview.h>
#include <qpdf/qpdf-c.h>

namespace {
struct PdfHandle {
    qpdf_data value = qpdf_init();
    ~PdfHandle() { qpdf_cleanup(&value); }
};

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
QByteArray stream(const QByteArray &data, const QByteArray &dictionary = {})
{
    return "<< /Length " + QByteArray::number(data.size()) + " " + dictionary
        + " >>\nstream\n" + data + "\nendstream";
}
bool writeObjects(const QString &path, const QList<QByteArray> &objects)
{
    QByteArray pdf("%PDF-1.7\n");
    QList<qsizetype> offsets;
    for (qsizetype i = 0; i < objects.size(); ++i) {
        offsets.append(pdf.size());
        pdf += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const auto xref = pdf.size();
    pdf += "xref\n0 " + QByteArray::number(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (const auto offset : offsets)
        pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    pdf += "trailer\n<< /Size " + QByteArray::number(objects.size() + 1)
        + " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    return writeFile(path, pdf);
}

// Five native vector/text pages with distinct geometry and rotation. Page 1
// contains a note, direct and named internal links, and an absolute URI.
bool createPdf(const QString &path, const QByteArray &label = "Page",
               const QByteArray &catalogExtra = {}, const QByteArray &annotationExtra = {})
{
    QList<QByteArray> objects;
    objects << "<< /Type /Catalog /Pages 2 0 R /Names << /Dests << /Names [(third) [7 0 R /XYZ 12 34 1.5]] >> >> "
                   + catalogExtra + " >>"
            << "<< /Type /Pages /Count 5 /Kids [3 0 R 5 0 R 7 0 R 9 0 R 11 0 R] >>";
    for (int page = 0; page < 5; ++page) {
        const int id = 3 + 2 * page;
        const QByteArray contents = "BT /F1 12 Tf 20 200 Td (" + label + " "
            + QByteArray::number(page + 1) + ") Tj ET\n0.1 0.4 0.7 RG 20 20 80 40 re S\n"
            + (page == 0 ? QByteArray("q 20 0 0 20 20 100 cm /Im1 Do Q\n") : QByteArray());
        objects << "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 "
            + QByteArray::number(300 + page * 10) + " 400] /CropBox [10 15 280 380] /Rotate "
            + QByteArray::number((page % 4) * 90) + " /Resources << /Font << /F1 13 0 R >> "
            + "/XObject << /Im1 18 0 R >> >> /Contents " + QByteArray::number(id + 1) + " 0 R "
            + (page == 0 ? QByteArray("/Annots [14 0 R 15 0 R 16 0 R 17 0 R] ") : QByteArray()) + ">>"
            << stream(contents);
    }
    objects << "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"
            << "<< /Type /Annot /Subtype /Text /P 3 0 R /Rect [20 50 40 70] /Contents (Keep note) /F 4 >>"
            << "<< /Type /Annot /Subtype /Link /P 3 0 R /Rect [20 80 100 95] /Dest [5 0 R /Fit] >>"
            << "<< /Type /Annot /Subtype /Link /P 3 0 R /Rect [20 110 100 125] /A << /S /GoTo /D (third) >> >>"
            << "<< /Type /Annot /Subtype /Link /P 3 0 R /Rect [20 140 100 155] /A << /S /URI /URI (https://example.com/) >> "
                + annotationExtra + " >>"
            << stream(QByteArray::fromHex("137bc9"),
                "/Type /XObject /Subtype /Image /Width 1 /Height 1 /ColorSpace /DeviceRGB /BitsPerComponent 8");
    return writeObjects(path, objects);
}

struct PageSnapshot {
    QSizeF size;
    int rotation = 0;
    QString text;
    int images = 0;
    int paths = 0;
};
QList<PageSnapshot> inspectPages(const QString &path)
{
    PDFiumProcessLock lock;
    auto document = FPDF_LoadDocument(QFile::encodeName(path).constData(), nullptr);
    if (!document)
        return {};
    QList<PageSnapshot> result;
    for (int index = 0; index < FPDF_GetPageCount(document); ++index) {
        auto page = FPDF_LoadPage(document, index);
        if (!page)
            break;
        PageSnapshot snapshot;
        snapshot.size = {FPDF_GetPageWidthF(page), FPDF_GetPageHeightF(page)};
        snapshot.rotation = FPDFPage_GetRotation(page);
        auto textPage = FPDFText_LoadPage(page);
        if (textPage) {
            QVector<unsigned short> text(FPDFText_CountChars(textPage) + 1);
            const int copied = FPDFText_GetText(textPage, 0, int(text.size()) - 1, text.data());
            if (copied > 0)
                snapshot.text = QString::fromUtf16(reinterpret_cast<const char16_t *>(text.constData()), copied - 1);
            FPDFText_ClosePage(textPage);
        }
        for (int object = 0; object < FPDFPage_CountObjects(page); ++object) {
            const int type = FPDFPageObj_GetType(FPDFPage_GetObject(page, object));
            snapshot.images += type == FPDF_PAGEOBJ_IMAGE;
            snapshot.paths += type == FPDF_PAGEOBJ_PATH;
        }
        result.append(snapshot);
        FPDF_ClosePage(page);
    }
    FPDF_CloseDocument(document);
    return result;
}
QStringList directoryEntries(const QString &path)
{
    return QDir(path).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
}
bool createManyPages(const QString &path, int pages)
{
    QByteArray kids;
    QList<QByteArray> objects{QByteArray("<< /Type /Catalog /Pages 2 0 R >>"), {}};
    for (int index = 0; index < pages; ++index) {
        kids += QByteArray::number(index + 3) + " 0 R ";
        objects.append("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Resources << >> >>");
    }
    objects[1] = "<< /Type /Pages /Count " + QByteArray::number(pages) + " /Kids [" + kids + "] >>";
    return writeObjects(path, objects);
}

// Includes both merged field/widget dictionaries and separate Widgets with a
// field Parent, inherited DA/DR, a hierarchical name, and shared text/radio
// fields spanning two pages. Page 3 deliberately has no fields.
QList<QByteArray> ordinaryFormObjects()
{
    const QByteArray appearanceDictionary =
        "/Type /XObject /Subtype /Form /BBox [0 0 160 24] "
        "/Resources << /Font << /Helv 7 0 R >> >>";
    return {
        "<< /Type /Catalog /Pages 2 0 R /AcroForm 6 0 R >>",
        "<< /Type /Pages /Count 3 /Kids [3 0 R 4 0 R 5 0 R] >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Resources << >> /Annots [10 0 R 12 0 R 14 0 R 17 0 R] >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Resources << >> /Annots [11 0 R 15 0 R 16 0 R 18 0 R] >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Resources << >> >>",
        "<< /Fields [8 0 R 12 0 R 13 0 R 17 0 R 18 0 R] /DA (/Helv 12 Tf 0 g) /DR << /Font << /Helv 7 0 R >> >> /NeedAppearances false >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
        "<< /T (person) /Kids [9 0 R 16 0 R] >>",
        "<< /Parent 8 0 R /T (name) /FT /Tx /Ff 4096 /V (Alice) /DV (Default) /Kids [10 0 R 11 0 R] >>",
        "<< /Type /Annot /Subtype /Widget /Parent 9 0 R /P 3 0 R /Rect [20 330 180 354] /F 4 /AP << /N 19 0 R >> >>",
        "<< /Type /Annot /Subtype /Widget /Parent 9 0 R /P 4 0 R /Rect [20 330 180 354] /F 4 /AP << /N 19 0 R >> >>",
        "<< /Type /Annot /Subtype /Widget /FT /Btn /T (accept) /V /Yes /DV /Off /AS /Yes /P 3 0 R /Rect [20 280 44 304] /F 4 /AP << /N << /Off 20 0 R /Yes 21 0 R >> >> >>",
        "<< /T (consent) /FT /Btn /Ff 32768 /V /YesB /DV /YesA /Kids [14 0 R 15 0 R] >>",
        "<< /Type /Annot /Subtype /Widget /Parent 13 0 R /P 3 0 R /Rect [70 280 94 304] /F 4 /AS /Off /AP << /N << /Off 20 0 R /YesA 21 0 R >> >> >>",
        "<< /Type /Annot /Subtype /Widget /Parent 13 0 R /P 4 0 R /Rect [70 280 94 304] /F 4 /AS /YesB /AP << /N << /Off 20 0 R /YesB 21 0 R >> >> >>",
        "<< /Type /Annot /Subtype /Widget /Parent 8 0 R /FT /Tx /T (code) /Ff 16777216 /MaxLen 6 /V (ABC123) /DV (000000) /P 4 0 R /Rect [20 220 180 244] /F 4 /AP << /N 22 0 R >> >>",
        "<< /Type /Annot /Subtype /Widget /FT /Ch /T (category) /Ff 131072 /Opt [[(a) (Alpha)] [(b) (Beta)]] /V (b) /DV (a) /I [1] /P 3 0 R /Rect [20 220 180 244] /F 4 /AP << /N 23 0 R >> >>",
        "<< /Type /Annot /Subtype /Widget /FT /Ch /T (regions) /Ff 2097152 /Opt [(North) (South) (West)] /V [(North) (West)] /I [0 2] /P 4 0 R /Rect [20 150 180 198] /F 4 /AP << /N 24 0 R >> >>",
        stream("BT /Helv 12 Tf 2 6 Td (Alice) Tj ET", appearanceDictionary),
        stream("0 G 0 0 24 24 re S", "/Type /XObject /Subtype /Form /BBox [0 0 24 24] /Resources << >>"),
        stream("0 G 0 0 24 24 re S 3 3 m 21 21 l 3 21 m 21 3 l S", "/Type /XObject /Subtype /Form /BBox [0 0 24 24] /Resources << >>"),
        stream("BT /Helv 12 Tf 2 6 Td (ABC123) Tj ET", appearanceDictionary),
        stream("BT /Helv 12 Tf 2 6 Td (Beta) Tj ET", appearanceDictionary),
        stream("BT /Helv 12 Tf 2 16 Td (North) Tj 0 -12 Td (West) Tj ET", appearanceDictionary)
    };
}

bool encryptPdf(const QString &source, const QString &destination,
                const QByteArray &userPassword = "secret",
                bool extract = true, bool assemble = true)
{
    const QByteArray sourcePath = source.toUtf8();
    const QByteArray destinationPath = destination.toUtf8();
    PdfHandle pdf;
    if (qpdf_read(pdf.value, sourcePath.constData(), "") != QPDF_SUCCESS
        || qpdf_init_write(pdf.value, destinationPath.constData()) != QPDF_SUCCESS)
        return false;
    // Leave printing, annotations and other modifications restricted. A split
    // must not silently replace these settings with unrestricted encryption.
    qpdf_set_r6_encryption_parameters2(pdf.value, userPassword.constData(), "owner",
        true, extract, assemble, false, true, false, qpdf_r3p_none, false);
    return qpdf_write(pdf.value) == QPDF_SUCCESS;
}

QMap<QByteArray, QByteArray> encryptionParameters(const QString &path, const QByteArray &password)
{
    const QByteArray encoded = path.toUtf8();
    PdfHandle pdf;
    if (qpdf_read(pdf.value, encoded.constData(), password.constData()) != QPDF_SUCCESS
        || !qpdf_is_encrypted(pdf.value))
        return {};
    const auto encrypt = qpdf_oh_get_key(pdf.value, qpdf_get_trailer(pdf.value), "/Encrypt");
    QMap<QByteArray, QByteArray> result;
    for (const char *key : {"/V", "/R", "/Length", "/P", "/EncryptMetadata",
                           "/CF", "/StmF", "/StrF", "/U", "/UE", "/O", "/OE", "/Perms"}) {
        const auto value = qpdf_oh_get_key(pdf.value, encrypt, key);
        // /P is a 32-bit permission mask; its signed/unsigned spelling is not
        // an encryption change (the legacy fixture uses unsigned spelling).
        result.insert(key, QByteArray(key) == "/P"
            ? QByteArray::number(quint32(qpdf_oh_get_int_value(pdf.value, value)))
            : QByteArray(qpdf_oh_unparse_resolved(pdf.value, value)));
    }
    return result;
}

QString pdfWideString(const auto &getter)
{
    const unsigned long bytes = getter(nullptr, 0);
    if (bytes < sizeof(FPDF_WCHAR) || bytes > 65536)
        return {};
    QVector<FPDF_WCHAR> buffer((bytes + sizeof(FPDF_WCHAR) - 1) / sizeof(FPDF_WCHAR));
    const auto actual = getter(buffer.data(), bytes) / sizeof(FPDF_WCHAR);
    return actual > 0 ? QString::fromUtf16(
        reinterpret_cast<const char16_t *>(buffer.constData()), actual - 1) : QString();
}

struct FormWidget {
    int page = -1;
    QString name;
    QString value;
    QString exportValue;
    int type = 0;
    int flags = 0;
    QStringList options;
    QList<int> selected;
    bool checked = false;
    int controls = 0;
};

QList<FormWidget> inspectFormWidgets(const QString &path, const QByteArray &password = {})
{
    PDFiumProcessLock lock;
    const QByteArray encoded = QFile::encodeName(path);
    const auto document = FPDF_LoadDocument(encoded.constData(), password.constData());
    if (!document)
        return {};
    FPDF_FORMFILLINFO info{};
    info.version = 2;
    info.xfa_disabled = true;
    const auto form = FPDFDOC_InitFormFillEnvironment(document, &info);
    QList<FormWidget> widgets;
    if (form) {
        for (int pageIndex = 0; pageIndex < FPDF_GetPageCount(document); ++pageIndex) {
            const auto page = FPDF_LoadPage(document, pageIndex);
            if (!page)
                break;
            FORM_OnAfterLoadPage(page, form);
            for (int index = 0; index < FPDFPage_GetAnnotCount(page); ++index) {
                const auto annotation = FPDFPage_GetAnnot(page, index);
                if (FPDFAnnot_GetSubtype(annotation) == FPDF_ANNOT_WIDGET) {
                    FormWidget widget;
                    widget.page = pageIndex;
                    widget.name = pdfWideString([&](FPDF_WCHAR *buffer, unsigned long bytes) {
                        return FPDFAnnot_GetFormFieldName(form, annotation, buffer, bytes);
                    });
                    widget.value = pdfWideString([&](FPDF_WCHAR *buffer, unsigned long bytes) {
                        return FPDFAnnot_GetFormFieldValue(form, annotation, buffer, bytes);
                    });
                    widget.exportValue = pdfWideString([&](FPDF_WCHAR *buffer, unsigned long bytes) {
                        return FPDFAnnot_GetFormFieldExportValue(form, annotation, buffer, bytes);
                    });
                    widget.type = FPDFAnnot_GetFormFieldType(form, annotation);
                    widget.flags = FPDFAnnot_GetFormFieldFlags(form, annotation);
                    widget.checked = FPDFAnnot_IsChecked(form, annotation);
                    widget.controls = FPDFAnnot_GetFormControlCount(form, annotation);
                    for (int option = 0; option < FPDFAnnot_GetOptionCount(form, annotation); ++option) {
                        widget.options.append(pdfWideString([&](FPDF_WCHAR *buffer, unsigned long bytes) {
                            return FPDFAnnot_GetOptionLabel(form, annotation, option, buffer, bytes);
                        }));
                        if (FPDFAnnot_IsOptionSelected(form, annotation, option))
                            widget.selected.append(option);
                    }
                    widgets.append(widget);
                }
                FPDFPage_CloseAnnot(annotation);
            }
            FORM_OnBeforeClosePage(page, form);
            FPDF_ClosePage(page);
        }
        FPDFDOC_ExitFormFillEnvironment(form);
    }
    FPDF_CloseDocument(document);
    return widgets;
}

// Every widget reachable from /AcroForm must occur exactly once on a retained
// page, and its /Parent and /P must point to that same output's field/page tree.
// This catches invisible orphan fields that a rendering-only check misses.
bool hasConsistentFormTree(const QString &path, const QByteArray &password = {})
{
    const QByteArray encoded = path.toUtf8();
    PdfHandle document;
    auto pdf = document.value;
    if (qpdf_read(pdf, encoded.constData(), password.constData()) != QPDF_SUCCESS)
        return false;
    QSet<int> pageWidgets;
    for (int pageIndex = 0; pageIndex < qpdf_get_num_pages(pdf); ++pageIndex) {
        const auto page = qpdf_get_page_n(pdf, pageIndex);
        const auto annotations = qpdf_oh_get_key(pdf, page, "/Annots");
        const int annotationCount = qpdf_oh_is_array(pdf, annotations)
            ? qpdf_oh_get_array_n_items(pdf, annotations) : 0;
        for (int index = 0; index < annotationCount; ++index) {
            const auto annotation = qpdf_oh_get_array_item(pdf, annotations, index);
            if (!qpdf_oh_is_name_and_equals(pdf, qpdf_oh_get_key(pdf, annotation, "/Subtype"), "/Widget"))
                continue;
            const int id = qpdf_oh_get_object_id(pdf, annotation);
            if (pageWidgets.contains(id) || id == 0
                || qpdf_oh_get_object_id(pdf, qpdf_oh_get_key(pdf, annotation, "/P"))
                    != qpdf_oh_get_object_id(pdf, page))
                return false;
            pageWidgets.insert(id);
        }
    }
    QSet<int> treeWidgets;
    QSet<int> visited;
    const auto visit = [&](const auto &self, qpdf_oh node, int parent, int depth) -> bool {
        const int id = qpdf_oh_get_object_id(pdf, node);
        if (!qpdf_oh_is_dictionary(pdf, node) || id == 0 || depth > 32 || visited.contains(id))
            return false;
        visited.insert(id);
        if (qpdf_oh_get_object_id(pdf, qpdf_oh_get_key(pdf, node, "/Parent")) != parent)
            return false;
        if (qpdf_oh_is_name_and_equals(pdf, qpdf_oh_get_key(pdf, node, "/Subtype"), "/Widget"))
            treeWidgets.insert(id);
        const auto kids = qpdf_oh_get_key(pdf, node, "/Kids");
        const int childCount = qpdf_oh_is_array(pdf, kids) ? qpdf_oh_get_array_n_items(pdf, kids) : 0;
        for (int index = 0; index < childCount; ++index) {
            if (!self(self, qpdf_oh_get_array_item(pdf, kids, index), id, depth + 1))
                return false;
        }
        return true;
    };
    const auto acroForm = qpdf_oh_get_key(pdf, qpdf_get_root(pdf), "/AcroForm");
    if (qpdf_oh_is_null(pdf, acroForm))
        return pageWidgets.isEmpty();
    const auto fields = qpdf_oh_get_key(pdf, acroForm, "/Fields");
    if (!qpdf_oh_is_array(pdf, fields))
        return false;
    for (int index = 0; index < qpdf_oh_get_array_n_items(pdf, fields); ++index) {
        if (!visit(visit, qpdf_oh_get_array_item(pdf, fields, index), 0, 0))
            return false;
    }
    return treeWidgets == pageWidgets;
}

QImage renderFormPage(const QString &path, int pageIndex, const QByteArray &password = {})
{
    PDFiumProcessLock lock;
    const QByteArray encoded = QFile::encodeName(path);
    const auto document = FPDF_LoadDocument(encoded.constData(), password.constData());
    if (!document)
        return {};
    FPDF_FORMFILLINFO info{};
    info.version = 2;
    info.xfa_disabled = true;
    const auto form = FPDFDOC_InitFormFillEnvironment(document, &info);
    const auto page = FPDF_LoadPage(document, pageIndex);
    QImage result(600, 800, QImage::Format_ARGB32);
    result.fill(Qt::white);
    if (page && form) {
        FORM_OnAfterLoadPage(page, form);
        const auto bitmap = FPDFBitmap_CreateEx(result.width(), result.height(),
            FPDFBitmap_BGRA, result.bits(), result.bytesPerLine());
        FPDF_RenderPageBitmap(bitmap, page, 0, 0, result.width(), result.height(), 0, FPDF_ANNOT);
        FPDF_FFLDraw(form, bitmap, page, 0, 0, result.width(), result.height(), 0, FPDF_ANNOT);
        FPDFBitmap_Destroy(bitmap);
        FORM_OnBeforeClosePage(page, form);
    } else {
        result = {};
    }
    if (page)
        FPDF_ClosePage(page);
    if (form)
        FPDFDOC_ExitFormFillEnvironment(form);
    FPDF_CloseDocument(document);
    return result;
}

struct MemoryWriter : FPDF_FILEWRITE {
    QByteArray bytes;
    MemoryWriter()
    {
        version = 1;
        WriteBlock = [](FPDF_FILEWRITE *self, const void *data, unsigned long size) -> int {
            static_cast<MemoryWriter *>(self)->bytes.append(static_cast<const char *>(data), size);
            return 1;
        };
    }
};

bool editFormAndSave(const QString &path, const QString &destination,
                     const QString &fieldName, const auto &edit)
{
    PDFiumProcessLock lock;
    const QByteArray encoded = QFile::encodeName(path);
    const auto document = FPDF_LoadDocument(encoded.constData(), nullptr);
    if (!document)
        return false;
    FPDF_FORMFILLINFO info{};
    info.version = 2;
    info.xfa_disabled = true;
    const auto form = FPDFDOC_InitFormFillEnvironment(document, &info);
    const auto page = FPDF_LoadPage(document, 0);
    bool edited = false;
    MemoryWriter writer;
    if (page && form) {
        FORM_OnAfterLoadPage(page, form);
        for (int index = 0; index < FPDFPage_GetAnnotCount(page); ++index) {
            const auto annotation = FPDFPage_GetAnnot(page, index);
            const auto name = pdfWideString([&](FPDF_WCHAR *buffer, unsigned long bytes) {
                return FPDFAnnot_GetFormFieldName(form, annotation, buffer, bytes);
            });
            if (name == fieldName && FORM_SetFocusedAnnot(form, annotation)) {
                edited = edit(form, page, annotation);
                FORM_ForceToKillFocus(form);
            }
            FPDFPage_CloseAnnot(annotation);
        }
        if (edited)
            edited = FPDF_SaveAsCopy(document, &writer, FPDF_NO_INCREMENTAL);
        FORM_OnBeforeClosePage(page, form);
    }
    if (page)
        FPDF_ClosePage(page);
    if (form)
        FPDFDOC_ExitFormFillEnvironment(form);
    FPDF_CloseDocument(document);
    return edited && writeFile(destination, writer.bytes);
}

bool fillTextAndSave(const QString &path, const QString &destination,
                     const QString &fieldName, const QString &value)
{
    return editFormAndSave(path, destination, fieldName,
        [&](FPDF_FORMHANDLE form, FPDF_PAGE page, FPDF_ANNOTATION) {
            if (!FORM_SelectAllText(form, page))
                return false;
            FORM_ReplaceSelection(form, page,
                reinterpret_cast<FPDF_WIDESTRING>(value.utf16()));
            return true;
        });
}

bool clickFieldAndSave(const QString &path, const QString &destination,
                      const QString &fieldName)
{
    return editFormAndSave(path, destination, fieldName,
        [](FPDF_FORMHANDLE form, FPDF_PAGE page, FPDF_ANNOTATION annotation) {
            FS_RECTF rect{};
            if (!FPDFAnnot_GetRect(annotation, &rect))
                return false;
            const double x = (rect.left + rect.right) / 2.0;
            const double y = (rect.top + rect.bottom) / 2.0;
            return FORM_OnLButtonDown(form, page, 0, x, y)
                && FORM_OnLButtonUp(form, page, 0, x, y);
        });
}

bool selectOptionAndSave(const QString &path, const QString &destination,
                        const QString &fieldName, int option)
{
    return editFormAndSave(path, destination, fieldName,
        [option](FPDF_FORMHANDLE form, FPDF_PAGE page, FPDF_ANNOTATION) {
            return FORM_SetIndexSelected(form, page, option, true);
        });
}
}

class PDFSplitControllerTest : public QObject
{
    Q_OBJECT
    PDFDocument m_runtime;

private slots:
    void splitModesPreserveNativePages_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<int>("perFile");
        QTest::addColumn<QString>("ranges");
        QTest::addColumn<QStringList>("expected");
        QTest::newRow("each-page") << 0 << 1 << QString() << QStringList{"1", "2", "3", "4", "5"};
        QTest::newRow("every-two") << 1 << 2 << QString() << QStringList{"1,2", "3,4", "5"};
        QTest::newRow("custom-source-order") << 2 << 1 << QString("5,3; 2,1") << QStringList{"3,5", "1,2"};
        QTest::newRow("whole-file") << 1 << 99 << QString() << QStringList{"1,2,3,4,5"};
    }
    void splitModesPreserveNativePages()
    {
        QFETCH(int, mode);
        QFETCH(int, perFile);
        QFETCH(QString, ranges);
        QFETCH(QStringList, expected);
        QTemporaryDir dir;
        const QString source = dir.filePath("source.pdf");
        QVERIFY(createPdf(source));
        const auto original = readFile(source);
        const auto originals = inspectPages(source);
        QCOMPARE(originals.size(), 5);
        PDFSplitController controller;
        controller.setMode(mode);
        controller.setPagesPerFile(perFile);
        controller.setPageRanges(ranges);
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QCOMPARE(controller.pageCount(), 5);
        QCOMPARE(controller.outputCount(), expected.size());
        QVERIFY(controller.canSplit());
        QSignalSpy completed(&controller, &PDFSplitController::completed);
        QSignalSpy finished(&controller, &PDFSplitController::finished);
        QVERIFY(controller.splitTo(QUrl::fromLocalFile(dir.path())));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
        QVERIFY2(finished[0][0].toBool(), qPrintable(controller.statusText()));
        QCOMPARE(completed.size(), 1);
        QCOMPARE(controller.progress(), 1.0);
        const auto outputs = qvariant_cast<QList<QUrl>>(completed[0][1]);
        QCOMPARE(outputs.size(), expected.size());
        QCOMPARE(completed[0][0].toUrl().toLocalFile(), QFileInfo(dir.filePath("source-split")).canonicalFilePath());
        for (int file = 0; file < outputs.size(); ++file) {
            const auto actual = inspectPages(outputs[file].toLocalFile());
            const auto expectedPages = expected[file].split(',');
            QCOMPARE(actual.size(), expectedPages.size());
            for (int page = 0; page < actual.size(); ++page) {
                const auto &wanted = originals[expectedPages[page].toInt() - 1];
                QCOMPARE(actual[page].text, wanted.text);
                QCOMPARE(actual[page].size, wanted.size);
                QCOMPARE(actual[page].rotation, wanted.rotation);
                QCOMPARE(actual[page].images, wanted.images);
                QCOMPARE(actual[page].paths, wanted.paths);
            }
        }
        QCOMPARE(readFile(source), original);
        QCOMPARE(directoryEntries(dir.path()), (QStringList{"source-split", "source.pdf"}));
    }

    void remapsKeptLinksAndRemovesExcludedTargets()
    {
        QTemporaryDir dir;
        const QString source = dir.filePath("links.pdf");
        QVERIFY(createPdf(source));
        PDFSplitController controller;
        controller.setMode(2);
        controller.setPageRanges("1,3");
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QSignalSpy completed(&controller, &PDFSplitController::completed);
        QVERIFY(controller.splitTo(QUrl::fromLocalFile(dir.path())));
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 10000);
        const auto outputs = qvariant_cast<QList<QUrl>>(completed[0][1]);
        PdfHandle document;
        auto pdf = document.value;
        QCOMPARE(qpdf_read(pdf, outputs[0].toLocalFile().toUtf8().constData(), ""), QPDF_SUCCESS);
        const auto page = qpdf_get_page_n(pdf, 0);
        const auto annotations = qpdf_oh_get_key(pdf, page, "/Annots");
        QCOMPARE(qpdf_oh_get_array_n_items(pdf, annotations), 4); // note, disabled link, kept named link, URI
        const auto note = qpdf_oh_get_array_item(pdf, annotations, 0);
        QCOMPARE(QByteArray(qpdf_oh_get_string_value(pdf, qpdf_oh_get_key(pdf, note, "/Contents"))), QByteArray("Keep note"));
        const auto disabled = qpdf_oh_get_array_item(pdf, annotations, 1);
        QVERIFY(!qpdf_oh_has_key(pdf, disabled, "/Dest"));
        QVERIFY(!qpdf_oh_has_key(pdf, disabled, "/A"));
        const auto link = qpdf_oh_get_array_item(pdf, annotations, 2);
        const auto action = qpdf_oh_get_key(pdf, link, "/A");
        const auto destination = qpdf_oh_get_key(pdf, action, "/D");
        QCOMPARE(qpdf_find_page_by_oh(pdf, qpdf_oh_get_array_item(pdf, destination, 0)), 1);
        QCOMPARE(QByteArray(qpdf_oh_get_name(pdf, qpdf_oh_get_array_item(pdf, destination, 1))), QByteArray("/XYZ"));
        QCOMPARE(qpdf_oh_get_numeric_value(pdf, qpdf_oh_get_array_item(pdf, destination, 2)), 12.0);
        QCOMPARE(qpdf_oh_get_numeric_value(pdf, qpdf_oh_get_array_item(pdf, destination, 3)), 34.0);
        QCOMPARE(qpdf_oh_get_numeric_value(pdf, qpdf_oh_get_array_item(pdf, destination, 4)), 1.5);
        QCOMPARE(qpdf_find_page_by_oh(pdf, qpdf_oh_get_key(pdf, link, "/P")), 0);
        const auto uri = qpdf_oh_get_key(pdf, qpdf_oh_get_key(pdf,
            qpdf_oh_get_array_item(pdf, annotations, 3), "/A"), "/URI");
        QCOMPARE(QByteArray(qpdf_oh_get_string_value(pdf, uri)), QByteArray("https://example.com/"));
    }

    void keepsCommentAndPopupWhenDestinationIsExcluded()
    {
        QTemporaryDir dir;
        const auto source = dir.filePath("comment.pdf");
        QVERIFY(writeObjects(source, {
            "<< /Type /Catalog /Pages 2 0 R >>",
            "<< /Type /Pages /Count 2 /Kids [3 0 R 4 0 R] >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Resources << >> /Annots [5 0 R 6 0 R 7 0 R] >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Resources << >> >>",
            "<< /Type /Annot /Subtype /Text /Rect [10 10 30 30] /Contents (Keep this comment) /P 3 0 R /Popup 6 0 R /A << /S /GoTo /D [4 0 R /Fit] >> >>",
            "<< /Type /Annot /Subtype /Popup /Rect [40 40 140 140] /Parent 5 0 R /P 3 0 R >>",
            "<< /Type /Annot /Subtype /Text /Rect [10 10 30 30] /IRT 5 0 R /Contents (Reply) /P 3 0 R >>"
        }));
        PDFSplitController controller;
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QSignalSpy done(&controller, &PDFSplitController::completed);
        QVERIFY(controller.splitTo(QUrl::fromLocalFile(dir.path())));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
        const auto outputs = qvariant_cast<QList<QUrl>>(done[0][1]);
        PdfHandle document;
        auto pdf = document.value;
        QCOMPARE(qpdf_read(pdf, outputs[0].toLocalFile().toUtf8().constData(), ""), QPDF_SUCCESS);
        const auto annotations = qpdf_oh_get_key(pdf, qpdf_get_page_n(pdf, 0), "/Annots");
        QCOMPARE(qpdf_oh_get_array_n_items(pdf, annotations), 3);
        const auto comment = qpdf_oh_get_array_item(pdf, annotations, 0);
        const auto popup = qpdf_oh_get_array_item(pdf, annotations, 1);
        const auto reply = qpdf_oh_get_array_item(pdf, annotations, 2);
        QVERIFY(!qpdf_oh_has_key(pdf, comment, "/A"));
        QCOMPARE(QByteArray(qpdf_oh_get_string_value(pdf, qpdf_oh_get_key(pdf, comment, "/Contents"))), QByteArray("Keep this comment"));
        QCOMPARE(qpdf_oh_get_object_id(pdf, qpdf_oh_get_key(pdf, popup, "/Parent")), qpdf_oh_get_object_id(pdf, comment));
        QCOMPARE(qpdf_oh_get_object_id(pdf, qpdf_oh_get_key(pdf, reply, "/IRT")), qpdf_oh_get_object_id(pdf, comment));
    }

    void enforcesPageOutputAndInputLimits()
    {
        QTemporaryDir dir;
        const auto source = dir.filePath("many.pdf");
        QVERIFY(createManyPages(source, 1001));
        PDFSplitController controller;
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QCOMPARE(controller.pageCount(), 1001);
        QVERIFY(!controller.canSplit());
        QCOMPARE(controller.outputCount(), 0);
        controller.setMode(1);
        controller.setPagesPerFile(2);
        QVERIFY(controller.canSplit());
        QCOMPARE(controller.outputCount(), 501);
        controller.setPagesPerFile(0);
        QVERIFY(!controller.canSplit());
        controller.setPagesPerFile(10001);
        QVERIFY(!controller.canSplit());
        controller.setMode(2);
        controller.setPageRanges(QString(65537, QLatin1Char('1')));
        QVERIFY(!controller.canSplit());
        const auto excessive = dir.filePath("excessive.pdf");
        QVERIFY(createManyPages(excessive, 10001));
        QVERIFY(controller.setSource(QUrl::fromLocalFile(excessive)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QCOMPARE(controller.pageCount(), 0);
        QFile oversized(dir.filePath("oversized.pdf"));
        QVERIFY(oversized.open(QIODevice::WriteOnly));
        QVERIFY(oversized.resize(256LL * 1024 * 1024 + 1));
        oversized.close();
        QVERIFY(!controller.setSource(QUrl::fromLocalFile(oversized.fileName())));
    }

    void rejectsCrossPageAnnotationRelationships()
    {
        QTemporaryDir dir;
        const auto source = dir.filePath("cross-page-reply.pdf");
        QVERIFY(writeObjects(source, {
            "<< /Type /Catalog /Pages 2 0 R >>",
            "<< /Type /Pages /Count 2 /Kids [3 0 R 4 0 R] >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Resources << >> /Annots [5 0 R] >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Resources << >> /Annots [6 0 R] >>",
            "<< /Type /Annot /Subtype /Text /Rect [10 10 30 30] /Contents (Comment on page one) /P 3 0 R >>",
            "<< /Type /Annot /Subtype /Text /Rect [10 10 30 30] /Contents (Reply on page two) /IRT 5 0 R /P 4 0 R >>"
        }));
        const auto original = readFile(source);
        PDFSplitController controller;
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QVERIFY(!controller.canSplit());
        QCOMPARE(controller.pageCount(), 0);
        QVERIFY(!controller.splitTo(QUrl::fromLocalFile(dir.path())));
        QCOMPARE(readFile(source), original);
        QCOMPARE(directoryEntries(dir.path()), QStringList{"cross-page-reply.pdf"});
    }

    void cancellationAfterFirstOutputRemovesWholeBatch()
    {
        QTemporaryDir dir;
        const auto source = dir.filePath("many.pdf");
        QVERIFY(createManyPages(source, 300));
        PDFSplitController controller;
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        bool cancelledAfterProgress = false;
        connect(&controller, &PDFSplitController::changed, this, [&] {
            if (controller.busy() && controller.progress() > 0 && !cancelledAfterProgress) {
                cancelledAfterProgress = true;
                controller.cancel();
            }
        });
        QSignalSpy done(&controller, &PDFSplitController::finished);
        QVERIFY(controller.splitTo(QUrl::fromLocalFile(dir.path())));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
        QVERIFY(cancelledAfterProgress);
        QVERIFY(!done[0][0].toBool());
        QCOMPARE(directoryEntries(dir.path()), QStringList{"many.pdf"});
    }

    void rejectsInvalidRanges_data()
    {
        QTest::addColumn<QString>("ranges");
        for (const auto &value : QStringList{"", " ", "0", "6", "3-1", "1-6", "1,1", "1-3;3-5",
                "1;;2", "1;", ";1", "1,", "1--2", "1.5", "1 2", "all", "9999999999999999999"})
            QTest::newRow(qPrintable(value.isEmpty() ? QString("empty") : value)) << value;
    }
    void rejectsInvalidRanges()
    {
        QFETCH(QString, ranges);
        QTemporaryDir dir;
        const auto source = dir.filePath("source.pdf");
        QVERIFY(createPdf(source));
        PDFSplitController controller;
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        controller.setMode(2);
        controller.setPageRanges(ranges);
        QVERIFY(!controller.canSplit());
        QCOMPARE(controller.outputCount(), 0);
        QVERIFY(!controller.splitTo(QUrl::fromLocalFile(dir.path())));
        QCOMPARE(directoryEntries(dir.path()), QStringList{"source.pdf"});
    }

    void createsUniqueOutputWithoutReplacingFiles()
    {
        QTemporaryDir dir;
        const auto source = dir.filePath("source.pdf");
        QVERIFY(createPdf(source));
        QVERIFY(QDir(dir.path()).mkdir("source-split"));
        QVERIFY(writeFile(dir.filePath("source-split/keep.txt"), "keep"));
        QVERIFY(writeFile(dir.filePath("source-split-1"), "existing file"));
        PDFSplitController controller;
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QSignalSpy done(&controller, &PDFSplitController::completed);
        QVERIFY(controller.splitTo(QUrl::fromLocalFile(dir.path())));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
        QCOMPARE(done[0][0].toUrl().toLocalFile(), QFileInfo(dir.filePath("source-split-2")).canonicalFilePath());
        QCOMPARE(readFile(dir.filePath("source-split/keep.txt")), QByteArray("keep"));
        QCOMPARE(readFile(dir.filePath("source-split-1")), QByteArray("existing file"));
        QVERIFY(controller.splitTo(QUrl::fromLocalFile(dir.path())));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 2, 10000);
        QCOMPARE(done[1][0].toUrl().toLocalFile(), QFileInfo(dir.filePath("source-split-3")).canonicalFilePath());
    }

    void rejectsUnsupportedStructures_data()
    {
        QTest::addColumn<QByteArray>("catalog");
        QTest::addColumn<QByteArray>("annotation");
        QTest::newRow("XFA") << QByteArray("/AcroForm << /XFA (data) >>") << QByteArray();
        QTest::newRow("signature") << QByteArray("/Perms << /DocMDP 1 0 R >>") << QByteArray();
        QTest::newRow("layers") << QByteArray("/OCProperties << >>") << QByteArray();
        QTest::newRow("open-javascript") << QByteArray("/OpenAction << /S /JavaScript /JS (app.alert) >>") << QByteArray();
        QTest::newRow("annot-actions") << QByteArray() << QByteArray("/AA << /E << /S /JavaScript /JS (alert) >> >>");
        QTest::newRow("orphan-widget") << QByteArray() << QByteArray("/Subtype /Widget /FT /Tx");
    }
    void rejectsUnsupportedStructures()
    {
        QFETCH(QByteArray, catalog);
        QFETCH(QByteArray, annotation);
        QTemporaryDir dir;
        const auto source = dir.filePath("unsupported.pdf");
        QVERIFY(createPdf(source, "Page", catalog, annotation));
        const auto original = readFile(source);
        PDFSplitController controller;
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QVERIFY(!controller.canSplit());
        QCOMPARE(controller.pageCount(), 0);
        QVERIFY(!controller.statusText().isEmpty());
        QCOMPARE(readFile(source), original);
        QCOMPARE(directoryEntries(dir.path()), QStringList{"unsupported.pdf"});
    }

    void rejectsBrokenEncryptedRemoteAndDestination()
    {
        QTemporaryDir dir;
        const auto source = dir.filePath("source.pdf");
        const auto broken = dir.filePath("broken.pdf");
        const auto encrypted = dir.filePath("encrypted.pdf");
        QVERIFY(createPdf(source));
        QVERIFY(writeFile(broken, "not a PDF"));
        {
            // qpdf retains the output filename while configuring encryption
            // and writing. Both byte buffers outlive the entire qpdf handle.
            const QByteArray sourcePath = source.toUtf8();
            const QByteArray encryptedPath = encrypted.toUtf8();
            PdfHandle pdf;
            QCOMPARE(qpdf_read(pdf.value, sourcePath.constData(), ""), QPDF_SUCCESS);
            QCOMPARE(qpdf_init_write(pdf.value, encryptedPath.constData()), QPDF_SUCCESS);
            qpdf_set_r6_encryption_parameters2(pdf.value, "secret", "owner", true, true, true,
                true, true, true, qpdf_r3p_full, true);
            QCOMPARE(qpdf_write(pdf.value), QPDF_SUCCESS);
        }
        PDFSplitController controller;
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        for (const QString &path : {broken, encrypted}) {
            QVERIFY(controller.setSource(QUrl::fromLocalFile(path)));
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
            QVERIFY(!controller.canSplit());
            QCOMPARE(controller.fileName(), QFileInfo(path).fileName());
        }
        QVERIFY(!controller.setSource(QUrl("https://example.com/file.pdf")));
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QVERIFY(!controller.splitTo(QUrl::fromLocalFile(source)));
        QVERIFY(!controller.splitTo(QUrl::fromLocalFile(dir.filePath("missing"))));
        QVERIFY(!controller.splitTo(QUrl("https://example.com/")));
        const auto alias = dir.filePath("alias");
        QVERIFY(QFile::link(dir.path(), alias));
        QVERIFY(!controller.splitTo(QUrl::fromLocalFile(alias)));
    }

    void unlocksPasswordAndPreservesEncryption_data()
    {
        QTest::addColumn<QString>("password");
        QTest::addColumn<QByteArray>("userPassword");
        QTest::newRow("user") << QStringLiteral("secret") << QByteArray("secret");
        QTest::newRow("owner") << QStringLiteral("owner") << QByteArray("secret");
        QTest::newRow("vietnamese") << QStringLiteral("Mật khẩu Việt 23")
                                  << QStringLiteral("Mật khẩu Việt 23").toUtf8();
    }
    void unlocksPasswordAndPreservesEncryption()
    {
        QFETCH(QString, password);
        QFETCH(QByteArray, userPassword);
        QTemporaryDir dir;
        const auto plain = dir.filePath("source.pdf");
        const auto encrypted = dir.filePath("encrypted.pdf");
        QVERIFY(createPdf(plain));
        QVERIFY(encryptPdf(plain, encrypted, userPassword));
        const auto original = readFile(encrypted);
        const auto expectedEncryption = encryptionParameters(encrypted, userPassword);
        QVERIFY(!expectedEncryption.isEmpty());
        PDFSplitController controller;
        QVERIFY(controller.setSource(QUrl::fromLocalFile(encrypted)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QVERIFY(controller.passwordRequired());
        QVERIFY(!controller.canSplit());
        QVERIFY(!controller.splitTo(QUrl::fromLocalFile(dir.path())));
        QVERIFY(controller.unlockSource("wrong-password"));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QVERIFY(controller.passwordRequired());
        QVERIFY(!controller.canSplit());
        QVERIFY(!controller.statusText().contains("wrong-password"));
        QVERIFY(controller.unlockSource(password));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QVERIFY(!controller.passwordRequired());
        QVERIFY(controller.encryptedSource());
        QVERIFY2(controller.canSplit(), qPrintable(controller.statusText()));
        QSignalSpy finished(&controller, &PDFSplitController::finished);
        QSignalSpy completed(&controller, &PDFSplitController::completed);
        QVERIFY(controller.splitTo(QUrl::fromLocalFile(dir.path())));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
        QVERIFY2(finished[0][0].toBool(), qPrintable(controller.statusText()));
        QCOMPARE(completed.size(), 1);
        const auto outputs = qvariant_cast<QList<QUrl>>(completed[0][1]);
        QCOMPARE(outputs.size(), 5);
        for (const auto &output : outputs) {
            const QByteArray path = output.toLocalFile().toUtf8();
            PdfHandle noPassword;
            QVERIFY(qpdf_read(noPassword.value, path.constData(), "") != QPDF_SUCCESS);
            QCOMPARE(encryptionParameters(output.toLocalFile(), userPassword), expectedEncryption);
            QCOMPARE(encryptionParameters(output.toLocalFile(), "owner"), expectedEncryption);
            PdfHandle reopened;
            QCOMPARE(qpdf_read(reopened.value, path.constData(), userPassword.constData()), QPDF_SUCCESS);
            QCOMPARE(qpdf_get_num_pages(reopened.value), 1);
            QVERIFY(qpdf_allow_extract_all(reopened.value));
            QVERIFY(qpdf_allow_modify_assembly(reopened.value));
            QVERIFY(!qpdf_allow_print_high_res(reopened.value));
            QVERIFY(!qpdf_allow_modify_other(reopened.value));
        }
        QCOMPARE(readFile(encrypted), original);
    }

    void rejectsRestrictedUserButAcceptsOwner_data()
    {
        QTest::addColumn<bool>("extract");
        QTest::addColumn<bool>("assemble");
        QTest::newRow("no-extraction") << false << true;
        QTest::newRow("no-assembly") << true << false;
        QTest::newRow("neither") << false << false;
    }
    void rejectsRestrictedUserButAcceptsOwner()
    {
        QFETCH(bool, extract);
        QFETCH(bool, assemble);
        QTemporaryDir dir;
        const auto plain = dir.filePath("source.pdf");
        const auto encrypted = dir.filePath("restricted.pdf");
        QVERIFY(createPdf(plain));
        QVERIFY(encryptPdf(plain, encrypted, "secret", extract, assemble));
        const auto original = readFile(encrypted);
        PDFSplitController controller;
        QVERIFY(controller.setSource(QUrl::fromLocalFile(encrypted), "secret"));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QVERIFY(!controller.canSplit());
        QVERIFY(!controller.splitTo(QUrl::fromLocalFile(dir.path())));
        QCOMPARE(directoryEntries(dir.path()), (QStringList{"restricted.pdf", "source.pdf"}));
        QVERIFY(controller.setSource(QUrl::fromLocalFile(encrypted), "owner"));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QVERIFY2(controller.canSplit(), qPrintable(controller.statusText()));
        controller.setMode(2);
        controller.setPageRanges("1");
        QSignalSpy finished(&controller, &PDFSplitController::finished);
        QSignalSpy completed(&controller, &PDFSplitController::completed);
        QVERIFY(controller.splitTo(QUrl::fromLocalFile(dir.path())));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
        QVERIFY2(finished[0][0].toBool(), qPrintable(controller.statusText()));
        const auto outputs = qvariant_cast<QList<QUrl>>(completed[0][1]);
        QCOMPARE(outputs.size(), 1);
        QCOMPARE(encryptionParameters(outputs[0].toLocalFile(), "secret"),
            encryptionParameters(encrypted, "secret"));
        QCOMPARE(readFile(encrypted), original);
    }

    void passwordStateClearsOnSourceChangeAndCancellation()
    {
        QTemporaryDir dir;
        const auto plain = dir.filePath("source.pdf");
        const auto encrypted = dir.filePath("encrypted.pdf");
        const auto other = dir.filePath("other.pdf");
        QVERIFY(createPdf(plain));
        QVERIFY(encryptPdf(plain, encrypted));
        QVERIFY(encryptPdf(plain, other, "different"));
        PDFSplitController controller;
        QVERIFY(controller.setSource(QUrl::fromLocalFile(encrypted), "secret"));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QVERIFY(controller.canSplit());
        QSignalSpy finished(&controller, &PDFSplitController::finished);
        QVERIFY(controller.splitTo(QUrl::fromLocalFile(dir.path())));
        controller.cancel();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QVERIFY(!finished[0][0].toBool());
        QVERIFY(controller.canSplit());
        QCOMPARE(directoryEntries(dir.path()), (QStringList{"encrypted.pdf", "other.pdf", "source.pdf"}));
        QVERIFY(controller.setSource(QUrl::fromLocalFile(other)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QVERIFY(controller.passwordRequired());
        QVERIFY(!controller.canSplit());
        controller.clear();
        QVERIFY(!controller.passwordRequired());
        QVERIFY(!controller.encryptedSource());
        QVERIFY(controller.setSource(QUrl::fromLocalFile(encrypted)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QVERIFY(controller.passwordRequired());
        QVERIFY(controller.setSource(QUrl::fromLocalFile(plain)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QVERIFY(!controller.passwordRequired());
        QVERIFY(!controller.encryptedSource());
        QVERIFY(controller.canSplit());
    }

    void preservesPermissionOnlyEncryption()
    {
        QTemporaryDir dir;
        const auto plain = dir.filePath("source.pdf");
        const auto encrypted = dir.filePath("protected.pdf");
        QVERIFY(createPdf(plain));
        QVERIFY(encryptPdf(plain, encrypted, ""));
        PDFSplitController controller;
        QVERIFY(controller.setSource(QUrl::fromLocalFile(encrypted)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QVERIFY(!controller.passwordRequired());
        QVERIFY(controller.encryptedSource());
        QVERIFY(controller.canSplit());
        controller.setMode(2);
        controller.setPageRanges("2");
        QSignalSpy finished(&controller, &PDFSplitController::finished);
        QSignalSpy completed(&controller, &PDFSplitController::completed);
        QVERIFY(controller.splitTo(QUrl::fromLocalFile(dir.path())));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QVERIFY2(finished[0][0].toBool(), qPrintable(controller.statusText()));
        const auto outputs = qvariant_cast<QList<QUrl>>(completed[0][1]);
        QCOMPARE(encryptionParameters(outputs[0].toLocalFile(), ""),
            encryptionParameters(encrypted, ""));
        QVERIFY(!encryptionParameters(outputs[0].toLocalFile(), "owner").isEmpty());
    }

    void preservesLegacyRc4EncryptionAndCheckbox()
    {
        QTemporaryDir dir;
        const auto source = dir.filePath("legacy-protected.pdf");
        QVERIFY(QFile::copy(QFINDTESTDATA("fixtures/password-1.pdf"), source));
        const auto original = readFile(source);
        const QByteArray password("fixture-open-1");
        const auto encryption = encryptionParameters(source, password);
        QCOMPARE(encryption.value("/V"), QByteArray("2"));
        QCOMPARE(encryption.value("/R"), QByteArray("3"));
        QCOMPARE(encryption.value("/Length"), QByteArray("128"));
        const auto originalWidgets = inspectFormWidgets(source, password);
        QCOMPARE(originalWidgets.size(), 1);
        QCOMPARE(originalWidgets[0].type, FPDF_FORMFIELD_CHECKBOX);
        PDFSplitController controller;
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source), QString::fromUtf8(password)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QVERIFY2(controller.canSplit(), qPrintable(controller.statusText()));
        QSignalSpy finished(&controller, &PDFSplitController::finished);
        QSignalSpy completed(&controller, &PDFSplitController::completed);
        QVERIFY(controller.splitTo(QUrl::fromLocalFile(dir.path())));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QVERIFY2(finished[0][0].toBool(), qPrintable(controller.statusText()));
        QCOMPARE(completed.size(), 1);
        const auto outputs = qvariant_cast<QList<QUrl>>(completed[0][1]);
        QCOMPARE(outputs.size(), 1);
        const auto path = outputs[0].toLocalFile();
        QCOMPARE(encryptionParameters(path, password), encryption);
        QCOMPARE(encryptionParameters(path, "fixture-owner-1"), encryption);
        const QByteArray encoded = path.toUtf8();
        PdfHandle locked;
        QVERIFY(qpdf_read(locked.value, encoded.constData(), "") != QPDF_SUCCESS);
        const auto widgets = inspectFormWidgets(path, password);
        QCOMPARE(widgets.size(), 1);
        QCOMPARE(widgets[0].name, originalWidgets[0].name);
        QCOMPARE(widgets[0].value, originalWidgets[0].value);
        QCOMPARE(widgets[0].checked, originalWidgets[0].checked);
        QVERIFY(hasConsistentFormTree(path, password));
        const auto before = renderFormPage(source, 0, password);
        QVERIFY(!before.isNull());
        QCOMPARE(renderFormPage(path, 0, password), before);
        QCOMPARE(readFile(source), original);
    }

    void preservesOrdinaryAcroForms_data()
    {
        QTest::addColumn<bool>("encrypted");
        QTest::newRow("plain") << false;
        QTest::newRow("encrypted") << true;
    }
    void preservesOrdinaryAcroForms()
    {
        QFETCH(bool, encrypted);
        QTemporaryDir dir;
        const auto plain = dir.filePath("forms.pdf");
        const auto source = encrypted ? dir.filePath("protected-forms.pdf") : plain;
        QVERIFY(writeObjects(plain, ordinaryFormObjects()));
        if (encrypted)
            QVERIFY(encryptPdf(plain, source));
        const auto original = readFile(source);
        const auto password = encrypted ? QByteArray("secret") : QByteArray();
        const auto sourceWidgets = inspectFormWidgets(source, password);
        QCOMPARE(sourceWidgets.size(), 8);
        QVERIFY(hasConsistentFormTree(source, password));
        PDFSplitController controller;
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source), QString::fromUtf8(password)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QVERIFY2(controller.canSplit(), qPrintable(controller.statusText()));
        QSignalSpy finished(&controller, &PDFSplitController::finished);
        QSignalSpy completed(&controller, &PDFSplitController::completed);
        QVERIFY(controller.splitTo(QUrl::fromLocalFile(dir.path())));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
        QVERIFY2(finished[0][0].toBool(), qPrintable(controller.statusText()));
        const auto outputs = qvariant_cast<QList<QUrl>>(completed[0][1]);
        QCOMPARE(outputs.size(), 3);
        for (int page = 0; page < outputs.size(); ++page) {
            const auto path = outputs[page].toLocalFile();
            QVERIFY2(hasConsistentFormTree(path, password), qPrintable(path));
            const auto widgets = inspectFormWidgets(path, password);
            QList<FormWidget> expected;
            for (const auto &widget : sourceWidgets) {
                if (widget.page == page)
                    expected.append(widget);
            }
            QCOMPARE(widgets.size(), expected.size());
            for (int index = 0; index < widgets.size(); ++index) {
                const auto &actual = widgets[index];
                const auto &wanted = expected[index];
                QCOMPARE(actual.name, wanted.name);
                QCOMPARE(actual.type, wanted.type);
                QCOMPARE(actual.flags, wanted.flags);
                QCOMPARE(actual.options, wanted.options);
                QCOMPARE(actual.selected, wanted.selected);
                QCOMPARE(actual.checked, wanted.checked);
                QCOMPARE(actual.value, actual.name == "consent" && page == 0
                    ? QString("Off") : wanted.value);
                QCOMPARE(actual.controls, 1);
            }
            const auto before = renderFormPage(source, page, password);
            QVERIFY(!before.isNull());
            QCOMPARE(renderFormPage(path, 0, password), before);
            if (encrypted)
                QCOMPARE(encryptionParameters(path, password), encryptionParameters(source, password));
            if (page < 2) {
                const QByteArray encoded = path.toUtf8();
                PdfHandle pdf;
                QCOMPARE(qpdf_read(pdf.value, encoded.constData(), password.constData()), QPDF_SUCCESS);
                const auto annotations = qpdf_oh_get_key(pdf.value, qpdf_get_page_n(pdf.value, 0), "/Annots");
                const auto radio = qpdf_oh_get_array_item(pdf.value, annotations, page == 0 ? 2 : 1);
                const auto field = qpdf_oh_get_key(pdf.value, radio, "/Parent");
                const auto defaultValue = qpdf_oh_get_key(pdf.value, field, "/DV");
                QVERIFY(qpdf_oh_is_name_and_equals(pdf.value, defaultValue, page == 0 ? "/YesA" : "/Off"));
            }
            if (page == 2) {
                const QByteArray encoded = path.toUtf8();
                PdfHandle pdf;
                QCOMPARE(qpdf_read(pdf.value, encoded.constData(), password.constData()), QPDF_SUCCESS);
                const auto acroForm = qpdf_oh_get_key(pdf.value, qpdf_get_root(pdf.value), "/AcroForm");
                if (!qpdf_oh_is_null(pdf.value, acroForm)) {
                    const auto fields = qpdf_oh_get_key(pdf.value, acroForm, "/Fields");
                    QVERIFY(qpdf_oh_is_array(pdf.value, fields));
                    QCOMPARE(qpdf_oh_get_array_n_items(pdf.value, fields), 0);
                }
            }
        }
        if (!encrypted) {
            const auto edited = dir.filePath("filled.pdf");
            QVERIFY(fillTextAndSave(outputs[0].toLocalFile(), edited, "person.name", "Edited after split"));
            const auto widgets = inspectFormWidgets(edited);
            QCOMPARE(widgets.size(), 4);
            QCOMPARE(widgets[0].value, QString("Edited after split"));
            QVERIFY(hasConsistentFormTree(edited));
            const auto unchecked = dir.filePath("unchecked.pdf");
            QVERIFY(clickFieldAndSave(edited, unchecked, "accept"));
            QVERIFY(!inspectFormWidgets(unchecked)[1].checked);
            const auto chosen = dir.filePath("choice.pdf");
            QVERIFY(selectOptionAndSave(unchecked, chosen, "category", 0));
            QCOMPARE(inspectFormWidgets(chosen)[3].value, QString("a"));
            const auto list = dir.filePath("list.pdf");
            QVERIFY(selectOptionAndSave(outputs[1].toLocalFile(), list, "regions", 1));
            QCOMPARE(inspectFormWidgets(list)[3].selected, (QList<int>{0, 1, 2}));
            const auto comb = dir.filePath("comb.pdf");
            QVERIFY(fillTextAndSave(list, comb, "person.code", "Z98765"));
            QCOMPARE(inspectFormWidgets(comb)[2].value, QString("Z98765"));
        }
        QCOMPARE(readFile(source), original);
    }

    void keepsSharedFieldsWhenRetainingMultiplePages()
    {
        QTemporaryDir dir;
        const auto source = dir.filePath("shared.pdf");
        QVERIFY(writeObjects(source, ordinaryFormObjects()));
        PDFSplitController controller;
        controller.setMode(2);
        controller.setPageRanges("1-2");
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QVERIFY2(controller.canSplit(), qPrintable(controller.statusText()));
        QSignalSpy finished(&controller, &PDFSplitController::finished);
        QSignalSpy completed(&controller, &PDFSplitController::completed);
        QVERIFY(controller.splitTo(QUrl::fromLocalFile(dir.path())));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QVERIFY2(finished[0][0].toBool(), qPrintable(controller.statusText()));
        const auto outputs = qvariant_cast<QList<QUrl>>(completed[0][1]);
        QCOMPARE(outputs.size(), 1);
        const auto path = outputs[0].toLocalFile();
        QVERIFY(hasConsistentFormTree(path));
        const auto widgets = inspectFormWidgets(path);
        QCOMPARE(widgets.size(), 8);
        QCOMPARE(widgets[0].name, QString("person.name"));
        QCOMPARE(widgets[4].name, QString("person.name"));
        QCOMPARE(widgets[0].controls, 2);
        QCOMPARE(widgets[4].controls, 2);
        QCOMPARE(widgets[2].name, QString("consent"));
        QCOMPARE(widgets[5].name, QString("consent"));
        QCOMPARE(widgets[2].controls, 2);
        QCOMPARE(widgets[5].controls, 2);
        const auto edited = dir.filePath("edited.pdf");
        QVERIFY(fillTextAndSave(path, edited, "person.name", "Both pages updated"));
        const auto afterEdit = inspectFormWidgets(edited);
        QCOMPARE(afterEdit.size(), 8);
        QCOMPARE(afterEdit[0].value, QString("Both pages updated"));
        QCOMPARE(afterEdit[4].value, QString("Both pages updated"));
        QVERIFY(hasConsistentFormTree(edited));
        const auto selected = dir.filePath("radio.pdf");
        QVERIFY(clickFieldAndSave(edited, selected, "consent"));
        const auto afterClick = inspectFormWidgets(selected);
        QCOMPARE(afterClick.size(), 8);
        QVERIFY(afterClick[2].checked);
        QVERIFY(!afterClick[5].checked);
        QCOMPARE(afterClick[2].value, QString("YesA"));
        QCOMPARE(afterClick[5].value, QString("YesA"));
        QVERIFY(hasConsistentFormTree(selected));
    }

    void retainsIndexedRadioExportsAfterPruning_data()
    {
        QTest::addColumn<QString>("ranges");
        QTest::addColumn<QStringList>("exports");
        QTest::addColumn<QByteArray>("value");
        QTest::addColumn<QByteArray>("defaultValue");
        QTest::newRow("first-only") << QString("1") << QStringList{"Alpha"}
            << QByteArray("/Off") << QByteArray("/0");
        QTest::newRow("second-only") << QString("2") << QStringList{"Beta"}
            << QByteArray("/0") << QByteArray("/Off");
        QTest::newRow("both") << QString("1-2") << QStringList{"Alpha", "Beta"}
            << QByteArray("/1") << QByteArray("/0");
    }
    void retainsIndexedRadioExportsAfterPruning()
    {
        QFETCH(QString, ranges);
        QFETCH(QStringList, exports);
        QFETCH(QByteArray, value);
        QFETCH(QByteArray, defaultValue);
        QTemporaryDir dir;
        const auto source = dir.filePath("indexed-radio.pdf");
        auto objects = ordinaryFormObjects();
        objects[12] = "<< /T (consent) /FT /Btn /Ff 32768 /V /1 /DV /0 "
                      "/Opt [(Alpha) (Beta)] /Kids [14 0 R 15 0 R] >>";
        objects[13].replace("/YesA", "/0");
        objects[14].replace("/YesB", "/1");
        QVERIFY(writeObjects(source, objects));
        const auto original = readFile(source);
        const auto originalWidgets = inspectFormWidgets(source);
        QCOMPARE(originalWidgets.size(), 8);
        QCOMPARE(originalWidgets[2].exportValue, QString("Alpha"));
        QCOMPARE(originalWidgets[5].exportValue, QString("Beta"));
        QCOMPARE(originalWidgets[2].value, QString("Beta"));
        QCOMPARE(originalWidgets[5].value, QString("Beta"));
        PDFSplitController controller;
        controller.setMode(2);
        controller.setPageRanges(ranges);
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QVERIFY2(controller.canSplit(), qPrintable(controller.statusText()));
        QSignalSpy finished(&controller, &PDFSplitController::finished);
        QSignalSpy completed(&controller, &PDFSplitController::completed);
        QVERIFY(controller.splitTo(QUrl::fromLocalFile(dir.path())));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QVERIFY2(finished[0][0].toBool(), qPrintable(controller.statusText()));
        QCOMPARE(completed.size(), 1);
        const auto outputs = qvariant_cast<QList<QUrl>>(completed[0][1]);
        QCOMPARE(outputs.size(), 1);
        const auto path = outputs[0].toLocalFile();
        QVERIFY(hasConsistentFormTree(path));
        const auto widgets = inspectFormWidgets(path);
        QStringList actualExports;
        for (const auto &widget : widgets) {
            if (widget.name == "consent")
                actualExports.append(widget.exportValue);
        }
        QCOMPARE(actualExports, exports);
        const QByteArray encoded = path.toUtf8();
        {
            PdfHandle pdf;
            QCOMPARE(qpdf_read(pdf.value, encoded.constData(), ""), QPDF_SUCCESS);
            const auto annotations = qpdf_oh_get_key(pdf.value, qpdf_get_page_n(pdf.value, 0), "/Annots");
            const auto radio = qpdf_oh_get_array_item(pdf.value, annotations, ranges == "2" ? 1 : 2);
            const auto field = qpdf_oh_get_key(pdf.value, radio, "/Parent");
            QCOMPARE(QByteArray(qpdf_oh_unparse(pdf.value, qpdf_oh_get_key(pdf.value, field, "/V"))), value);
            QCOMPARE(QByteArray(qpdf_oh_unparse(pdf.value, qpdf_oh_get_key(pdf.value, field, "/DV"))), defaultValue);
            const auto options = qpdf_oh_get_key(pdf.value, field, "/Opt");
            QCOMPARE(qpdf_oh_get_array_n_items(pdf.value, options), exports.size());
            for (int index = 0; index < exports.size(); ++index) {
                QCOMPARE(QString::fromUtf8(qpdf_oh_get_string_value(pdf.value,
                    qpdf_oh_get_array_item(pdf.value, options, index))), exports[index]);
            }
        }
        const auto edited = dir.filePath("clicked.pdf");
        QVERIFY(clickFieldAndSave(path, edited, "consent"));
        const auto editedWidgets = inspectFormWidgets(edited);
        int radioCount = 0;
        for (const auto &widget : editedWidgets) {
            if (widget.name != "consent")
                continue;
            QVERIFY(radioCount < exports.size());
            QCOMPARE(widget.exportValue, exports[radioCount]);
            QCOMPARE(widget.value, exports[0]);
            QCOMPARE(widget.checked, radioCount == 0);
            ++radioCount;
        }
        QCOMPARE(radioCount, exports.size());
        QVERIFY(hasConsistentFormTree(edited));
        QCOMPARE(readFile(source), original);
    }

    void rejectsUnsafeOrMalformedAcroForms_data()
    {
        QTest::addColumn<int>("object");
        QTest::addColumn<QByteArray>("replacement");
        QTest::newRow("signature-field") << 8 << QByteArray("<< /Parent 8 0 R /T (name) /FT /Sig /Kids [10 0 R 11 0 R] >>");
        QTest::newRow("xfa") << 5 << QByteArray("<< /Fields [8 0 R] /XFA (unsupported) >>");
        QTest::newRow("calculation-order") << 5 << QByteArray("<< /Fields [8 0 R] /CO [9 0 R] >>");
        QTest::newRow("field-action") << 8 << QByteArray("<< /Parent 8 0 R /T (name) /FT /Tx /Kids [10 0 R 11 0 R] /AA << /K << /S /JavaScript /JS (alert) >> >> >>");
        QTest::newRow("parent-action") << 7 << QByteArray("<< /T (person) /Kids [9 0 R 16 0 R] /AA << /C << /S /JavaScript /JS (alert) >> >> >>");
        QTest::newRow("widget-action") << 9 << QByteArray("<< /Type /Annot /Subtype /Widget /Parent 9 0 R /P 3 0 R /Rect [20 330 180 354] /A << /S /SubmitForm /F (https://example.com/) >> >>");
        QTest::newRow("field-cycle") << 7 << QByteArray("<< /T (person) /Kids [8 0 R] >>");
        QTest::newRow("mismatched-parent") << 9 << QByteArray("<< /Type /Annot /Subtype /Widget /Parent 13 0 R /P 3 0 R /Rect [20 330 180 354] >>");
        QTest::newRow("orphan-widget") << 8 << QByteArray("<< /Parent 8 0 R /T (name) /FT /Tx /Kids [10 0 R] >>");
        QTest::newRow("duplicate-field") << 5 << QByteArray("<< /Fields [8 0 R 8 0 R 12 0 R 13 0 R 17 0 R 18 0 R] >>");
    }
    void rejectsUnsafeOrMalformedAcroForms()
    {
        QFETCH(int, object);
        QFETCH(QByteArray, replacement);
        QTemporaryDir dir;
        const auto source = dir.filePath("unsafe-form.pdf");
        auto objects = ordinaryFormObjects();
        objects[object] = replacement;
        QVERIFY(writeObjects(source, objects));
        const auto original = readFile(source);
        PDFSplitController controller;
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QVERIFY(!controller.canSplit());
        QVERIFY(!controller.splitTo(QUrl::fromLocalFile(dir.path())));
        QCOMPARE(directoryEntries(dir.path()), QStringList{"unsafe-form.pdf"});
        QCOMPARE(readFile(source), original);
    }

    void sourceChangedWithSameSizeAndTimestampIsRejected()
    {
        QTemporaryDir dir;
        const auto source = dir.filePath("source.pdf");
        QVERIFY(createPdf(source, "AAAA"));
        PDFSplitController controller;
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        const auto before = QFileInfo(source);
        const auto modified = before.lastModified();
        const auto size = before.size();
        QVERIFY(createPdf(source, "BBBB"));
        QFile changed(source);
        QVERIFY(changed.open(QIODevice::ReadWrite));
        QVERIFY(changed.setFileTime(modified, QFileDevice::FileModificationTime));
        changed.close();
        QCOMPARE(QFileInfo(source).size(), size);
        QSignalSpy done(&controller, &PDFSplitController::finished);
        QVERIFY(controller.splitTo(QUrl::fromLocalFile(dir.path())));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
        QVERIFY(!done[0][0].toBool());
        QCOMPARE(directoryEntries(dir.path()), QStringList{"source.pdf"});
    }

    void cancellationCleansStagingAndKeepsSelection()
    {
        QTemporaryDir dir;
        const auto source = dir.filePath("source.pdf");
        QVERIFY(createPdf(source));
        PDFSplitController controller;
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QSignalSpy done(&controller, &PDFSplitController::finished);
        QSignalSpy completed(&controller, &PDFSplitController::completed);
        QVERIFY(controller.splitTo(QUrl::fromLocalFile(dir.path())));
        QVERIFY(!controller.setSource(QUrl::fromLocalFile(source)));
        QVERIFY(!controller.splitTo(QUrl::fromLocalFile(dir.path())));
        controller.setMode(2);
        QCOMPARE(controller.mode(), 0);
        controller.cancel();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
        QVERIFY(!done[0][0].toBool());
        QVERIFY(completed.isEmpty());
        QCOMPARE(controller.pageCount(), 5);
        QCOMPARE(directoryEntries(dir.path()), QStringList{"source.pdf"});
        QVERIFY(controller.canSplit());
    }

    void clearAndDestructionDuringWork()
    {
        QTemporaryDir dir;
        const auto source = dir.filePath("source.pdf");
        QVERIFY(createPdf(source));
        PDFSplitController controller;
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        controller.clear();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QCOMPARE(controller.pageCount(), 0);
        QVERIFY(controller.fileName().isEmpty());
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QSignalSpy done(&controller, &PDFSplitController::finished);
        QVERIFY(controller.splitTo(QUrl::fromLocalFile(dir.path())));
        controller.clear();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
        QCOMPARE(controller.pageCount(), 0);
        {
            auto worker = std::make_unique<PDFSplitController>();
            QVERIFY(worker->setSource(QUrl::fromLocalFile(source)));
            QTRY_VERIFY_WITH_TIMEOUT(!worker->busy(), 10000);
            QVERIFY(worker->splitTo(QUrl::fromLocalFile(dir.path())));
        }
        // A new inspection is accepted once the destroyed owner's worker exits.
        bool accepted = false;
        QTRY_VERIFY_WITH_TIMEOUT(accepted || (accepted = controller.setSource(QUrl::fromLocalFile(source))), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QCOMPARE(directoryEntries(dir.path()), QStringList{"source.pdf"});
    }
};

QTEST_MAIN(PDFSplitControllerTest)
#include "tst_PDFSplitController.moc"
