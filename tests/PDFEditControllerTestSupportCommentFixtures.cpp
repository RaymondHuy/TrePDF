#include "PDFEditControllerTestSupport.h"

namespace PDFEditControllerTestSupport {


bool createSquigglyAspectRatioPdf(const QString &filePath)
{
    FPDF_DOCUMENT document = FPDF_CreateNewDocument();
    if (!document) {
        return false;
    }

    const QSizeF pageSizes[] = {
        QSizeF(300.0, 800.0),
        QSizeF(800.0, 300.0),
    };
    // Both pages use the same point-space glyph positions. The inter-glyph
    // gap is below the configured 1.25-glyph-height limit, while the old
    // mixed-axis normalized comparison split it only on the tall page.
    bool succeeded = true;
    for (qsizetype pageIndex = 0;
         pageIndex < std::size(pageSizes);
         ++pageIndex) {
        const QSizeF pageSize = pageSizes[pageIndex];
        FPDF_PAGE page = FPDFPage_New(
            document,
            static_cast<int>(pageIndex),
            pageSize.width(),
            pageSize.height());
        const bool pageSucceeded = page
            && addTextObject(document,
                             page,
                             "Helvetica",
                             QStringLiteral("A"),
                             40.0,
                             150.0)
            && addTextObject(document,
                             page,
                             "Helvetica",
                             QStringLiteral("B"),
                             72.0,
                             150.0)
            && FPDFPage_GenerateContent(page);
        if (page) {
            FPDF_ClosePage(page);
        }
        if (!pageSucceeded) {
            succeeded = false;
            break;
        }
    }

    if (succeeded) {
        PdfFileWriter writer(filePath);
        succeeded = writer.open()
            && FPDF_SaveAsCopy(document, &writer, FPDF_NO_INCREMENTAL);
    }
    FPDF_CloseDocument(document);
    return succeeded;
}

bool createStickyNotePopupPdf(const QString &filePath)
{
    const QByteArray appearanceContent(
        "q 1 0.92 0 rg 0 0 24 24 re f "
        "0.2 0.2 0.2 RG 1 w 3 7 m 21 7 l S Q");
    const QByteArray appearanceObject
        = QByteArray(
              "<< /Type /XObject /Subtype /Form /FormType 1 "
              "/BBox [0 0 24 24] /Resources << >> /Length ")
        + QByteArray::number(appearanceContent.size())
        + QByteArray(" >>\nstream\n")
        + appearanceContent
        + QByteArray("\nendstream");
    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << >> /Annots [4 0 R 5 0 R 6 0 R 7 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [32 144 56 168] "
                "/Contents (Reverse-only popup comment) "
                "/T (Alice Example) "
                "/CreationDate (D:20240102030405+07'00') "
                "/NM (reverse-only-popup-comment) /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Popup /P 3 0 R "
                "/Rect [64 72 184 152] /Parent 4 0 R /Open true /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [132 144 156 168] "
                "/Contents (Preserved popup comment) "
                "/RC (<body xmlns=\"http://www.w3.org/1999/xhtml\">"
                "<p>Preserved popup comment</p></body>) "
                "/T (Bob Example) "
                "/CreationDate (D:20240203040506+07'00') "
                "/M (D:20240304050607+07'00') "
                "/NM (preserved-popup-comment) /F 4 /Popup 7 0 R "
                "/AP << /N 8 0 R >> >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Popup /P 3 0 R "
                "/Rect [72 32 192 112] /Parent 6 0 R /Open false /F 4 >>"),
            appearanceObject,
        });
}

bool createTextMarkupCommentPdf(const QString &filePath,
                                const QByteArray &subtypeName,
                                const QByteArray &annotationName,
                                const QByteArray &contents)
{
    const QByteArray appearanceContent(kSourceTextMarkupAppearance);
    const QByteArray appearanceObject
        = QByteArray(
              "<< /Type /XObject /Subtype /Form /FormType 1 "
              "/BBox [0 0 164 48] /Resources << >> /Length ")
        + QByteArray::number(appearanceContent.size())
        + QByteArray(" >>\nstream\n")
        + appearanceContent
        + QByteArray("endstream");
    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << >> /Annots [4 0 R] >>"),
            QByteArray("<< /Type /Annot /Subtype /")
                + subtypeName
                + QByteArray(
                    " /P 3 0 R /Rect [18 96 182 144] "
                    "/QuadPoints ["
                    "20 142 90 144 18 122 88 120 "
                    "106 118 180 116 108 98 182 96] "
                    "/Contents (")
                + contents
                + QByteArray(
                    ") /T (Markup Author) "
                    "/CreationDate (D:20240405060708+07'00') "
                    "/M (D:20240506070809+07'00') /NM (")
                + annotationName
                + QByteArray(
                    ") /C [1 0.9 0] /CA 0.55 /F 4 "
                    "/AP << /N 5 0 R >> >>"),
            appearanceObject,
        });
}

bool createProtectedCommentTextPdf(const QString &filePath)
{
    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << >> /Annots [4 0 R 5 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [24 144 48 168] /Contents (Read-only comment) "
                "/NM (read-only-comment) /F 64 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [60 144 84 168] /Contents (Locked comment) "
                "/NM (locked-contents-comment) /F 512 >>"),
    });
}

bool createLockedCommentTextPdf(const QString &filePath)
{
    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << >> /Annots [4 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [24 144 48 168] /Contents (Locked annotation) "
                "/NM (locked-annotation) /F 132 >>"),
    });
}

bool createLockedCommentReplyThreadPdf(const QString &filePath)
{
    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << >> /Annots [4 0 R 5 0 R 6 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [24 144 48 168] /Contents (Root comment) "
                "/NM (locked-thread-root) /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [52 144 76 168] /Contents (First reply) "
                "/NM (locked-thread-first-reply) "
                "/IRT 4 0 R /RT /R /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [80 144 104 168] /Contents (Locked nested reply) "
                "/NM (locked-thread-nested-reply) "
                "/IRT 5 0 R /RT /R /F 132 >>"),
        });
}

bool createProtectedFreeTextPdf(const QString &filePath)
{
    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << /Font << /Helv 6 0 R >> >> "
                "/Annots [4 0 R 5 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /FreeText /P 3 0 R "
                "/Rect [20 120 90 160] /Contents (Read-only free text) "
                "/DA (/Helv 12 Tf 0 g) /NM (read-only-free-text) /F 64 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /FreeText /P 3 0 R "
                "/Rect [105 120 180 160] /Contents (Locked free text) "
                "/DA (/Helv 12 Tf 0 g) /NM (locked-free-text) /F 512 >>"),
            QByteArray(
                "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
                "/Encoding /WinAnsiEncoding >>"),
        });
}

bool createPageLocalDuplicateFreeTextNamesPdf(const QString &filePath)
{
    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray(
                "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << /Font << /Helv 7 0 R >> >> "
                "/Annots [5 0 R] >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << /Font << /Helv 7 0 R >> >> "
                "/Annots [6 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /FreeText /P 3 0 R "
                "/Rect [20 120 180 170] /Contents (Page zero text) "
                "/DA (/Helv 12 Tf 0 g) /NM (page-local-name) /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /FreeText /P 4 0 R "
                "/Rect [20 120 180 170] /Contents (Page one text) "
                "/DA (/Helv 12 Tf 0 g) /NM (page-local-name) /F 4 >>"),
            QByteArray(
                "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
                "/Encoding /WinAnsiEncoding >>"),
        });
}

bool createNestedCommentRepliesPdf(const QString &filePath)
{
    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << >> /Annots [4 0 R 5 0 R 6 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [24 144 48 168] /Contents (Root comment) "
                "/CreationDate (D:20240101090000Z) /NM (root-comment) /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [52 144 76 168] /Contents (First reply) "
                "/CreationDate (D:20240101100000Z) /NM (first-reply) "
                "/IRT 4 0 R /RT /R /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [80 144 104 168] /Contents (Nested reply) "
                "/CreationDate (D:20240101110000Z) /NM (nested-reply) "
                "/IRT 5 0 R /RT /R /F 4 >>"),
        });
}

bool createCommentStateThreadPdf(const QString &filePath)
{
    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << >> /Annots [4 0 R 5 0 R 6 0 R 7 0 R 8 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [24 144 48 168] /Contents (Root comment) "
                "/CreationDate (D:20240101090000Z) /NM (state-root) /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [0 0 0 0] /Contents (Visible reply) "
                "/CreationDate (D:20240101100000Z) /NM (visible-reply) "
                "/IRT 4 0 R /RT /R /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [0 0 0 0] /Contents (Accepted set by Foxit User) "
                "/CreationDate (D:20240101110000Z) /NM (accepted-state) "
                "/IRT 4 0 R /State /Accepted /StateModel /Review /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [0 0 0 0] /Contents (Rejected set by Foxit User) "
                "/CreationDate (D:20240101120000Z) /NM (rejected-state) "
                "/IRT 6 0 R /State (Rejected) /StateModel (Review) /F 132 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [0 0 0 0] /Contents (None set by Foxit User) "
                "/CreationDate (D:20240101130000Z) /NM (unmarked-state) "
                "/IRT 4 0 R /State (Unmarked) /StateModel (Marked) /F 30 >>"),
        });
}

bool createReplyCommentStateHistoryPdf(const QString &filePath)
{
    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << >> "
                "/Annots [4 0 R 5 0 R 9 0 R 6 0 R 10 0 R 8 0 R 7 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [24 144 48 168] /Contents (Root comment) "
                "/T (trepdfreviewer) /CreationDate (D:20240101090000Z) "
                "/NM (reply-state-root) "
                "/TrePDFThreadId (reply-state-root) /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [0 0 0 0] /Contents (Reply comment) "
                "/T (trepdfreviewer) /CreationDate (D:20240101093000Z) "
                "/NM (reply-state-child) "
                "/TrePDFThreadId (reply-state-child) "
                "/TrePDFReplyTo (reply-state-root) "
                "/IRT 4 0 R /RT /R /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [0 0 0 0] /Contents (Accepted root state) "
                "/T (TREPDFREVIEWER) /M (D:20240101100000Z) "
                "/NM (root-accepted) /IRT 4 0 R "
                "/State /Accepted /StateModel /Review /F 30 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [0 0 0 0] /Contents (Cancelled root state) "
                "/T (TREPDFREVIEWER) /M (D:20240101110000Z) "
                "/NM (root-cancelled) /IRT 6 0 R "
                "/State /Cancelled /StateModel /Review /F 30 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [0 0 0 0] /Contents (Rejected reply state) "
                "/T (trepdfreviewer) /M (D:20240101120000Z) "
                "/NM (reply-rejected-lower) /IRT 5 0 R "
                "/State /Rejected /StateModel /Review /F 30 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [0 0 0 0] /Contents (Deferred reply state) "
                "/T (TREPDFREVIEWER) /M (D:20240101130000Z) "
                "/NM (reply-deferred-upper) /IRT 5 0 R "
                "/State (Deferred) /StateModel (Review) /F 30 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [0 0 0 0] /Contents (Marked reply state) "
                "/T (TREPDFREVIEWER) /M (D:20240101140000Z) "
                "/NM (reply-marked) /IRT 5 0 R "
                "/State /Marked /StateModel /Marked /F 30 >>"),
        });
}

bool createMixedCaseCommentStateHistoryPdf(const QString &filePath)
{
    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << >> "
                "/Annots [4 0 R 5 0 R 7 0 R 6 0 R 8 0 R 9 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [24 144 48 168] /Contents (Root comment) "
                "/T (trepdfreviewer) /CreationDate (D:20240101090000Z) "
                "/NM (mixed-case-root) /TrePDFThreadId (mixed-case-root) "
                "/F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [0 0 0 0] /Contents (Accepted set by TREPDFREVIEWER) "
                "/T (TREPDFREVIEWER) /M (D:20240101100000Z) "
                "/NM (foxit-accepted) /IRT 4 0 R "
                "/State /Accepted /StateModel /Review /F 30 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [0 0 0 0] /Contents (Rejected set by TREPDFREVIEWER) "
                "/T (TREPDFREVIEWER) /M (D:20240101110000Z) "
                "/NM (foxit-rejected) /IRT 5 0 R "
                "/State /Rejected /StateModel /Review /F 30 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [0 0 0 0] /Contents (Cancelled set by TREPDFREVIEWER) "
                "/T (TREPDFREVIEWER) /M (D:20240101120000Z) "
                "/NM (foxit-cancelled) /IRT 6 0 R "
                "/State /Cancelled /StateModel /Review /F 30 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [0 0 0 0] /Contents (Completed set by trepdfreviewer) "
                "/T (trepdfreviewer) /M (D:20240102120000Z) "
                "/NM (trepdf-completed) /IRT 4 0 R "
                "/State /Completed /StateModel /Review /F 30 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [0 0 0 0] /Contents (Future set by TREPDFREVIEWER) "
                "/T (TREPDFREVIEWER) /M (D:20240101113000Z) "
                "/NM (foxit-older-fork) /IRT 6 0 R "
                "/State /Future /StateModel /Review /F 30 >>"),
        });
}

bool createZeroRectUnderlineReplyPdf(const QString &filePath)
{
    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << >> /Annots [4 0 R 5 0 R 6 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Underline /P 3 0 R "
                "/Rect [20 120 180 132] "
                "/QuadPoints [20 132 180 132 20 120 180 120] "
                "/Contents (Root underline) "
                "/CreationDate (D:20240101090000Z) "
                "/NM (foxit-underline-root) /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [0 0 0 0] /Contents (Foxit underline reply) "
                "/CreationDate (D:20240101100000Z) "
                "/NM (foxit-underline-reply) "
                "/IRT 4 0 R /RT /R /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [0 0 0 0] /Contents (Zero rect root) "
                "/CreationDate (D:20240101110000Z) "
                "/NM (zero-rect-root) /F 4 >>"),
        });
}

bool createRootCommentOnlyPdf(const QString &filePath)
{
    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << >> /Annots [4 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [24 144 48 168] /Contents (Root comment) "
                "/CreationDate (D:20240101090000Z) /NM (root-comment) /F 4 >>"),
        });
}

bool createCommentIndexMemoryBudgetPdf(const QString &filePath)
{
    const QByteArray largeString(
        kCommentIndexBudgetFieldCharacters,
        'x');
    const QByteArray largeStringObject
        = QByteArray("(") + largeString + QByteArray(")");
    const auto largeAnnotationObject = [](int pageObject,
                                          int index,
                                          int replyParentObject = -1) {
        QByteArray annotation = QByteArray(
                   "<< /Type /Annot /Subtype /Text /P ")
            + QByteArray::number(pageObject)
            + QByteArray(" 0 R /Rect [24 144 48 168] "
                         "/Contents 10 0 R /T 10 0 R /Subj 10 0 R "
                         "/NM (memory-budget-comment-")
            + QByteArray::number(index)
            + QByteArray(")");
        if (replyParentObject >= 0) {
            annotation += QByteArray(" /IRT ")
                + QByteArray::number(replyParentObject)
                + QByteArray(" 0 R /RT /R");
        }
        annotation += QByteArray(" /F 4 >>");
        return annotation;
    };

    // The same indirect string is intentionally referenced by several fields.
    // The fixture stays compact while each indexed Candidate must own the
    // decoded Contents, author, and subject strings independently.
    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray(
                "<< /Type /Pages /Kids [3 0 R 5 0 R 8 0 R] /Count 3 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << >> /Annots [4 0 R] >>"),
            largeAnnotationObject(3, 0),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << >> /Annots [6 0 R 7 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 5 0 R "
                "/Rect [24 144 48 168] /Contents (Small root) "
                "/NM (memory-budget-small-root) /F 4 >>"),
            largeAnnotationObject(5, 1, 6),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << >> /Annots [9 0 R] >>"),
            largeAnnotationObject(8, 2),
            largeStringObject,
        });
}

bool createUnsupportedRootCommentReplyPdf(const QString &filePath)
{
    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << >> /Annots [4 0 R 5 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Link /P 3 0 R "
                "/Rect [24 144 48 168] /Border [0 0 0] /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [52 144 76 168] /Contents (Unresolved reply) "
                "/NM (unresolved-reply) /IRT 4 0 R /RT /R /F 4 >>"),
        });
}

bool createMixedSubtypeCommentRepliesPdf(const QString &filePath)
{
    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Version /2.0 /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << /Font << /Helv 10 0 R >> >> "
                "/Annots [4 0 R 5 0 R 6 0 R 7 0 R 8 0 R 9 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [24 144 48 168] /Contents (Mixed root) "
                "/NM (mixed-root) /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /FreeText /P 3 0 R "
                "/Rect [52 144 100 168] /Contents (FreeText reply) "
                "/DA (/Helv 10 Tf 0 g) /NM (mixed-free-text) "
                "/IRT 4 0 R /RT /R /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [104 144 128 168] "
                "/Contents (Text after FreeText) /NM (mixed-text) "
                "/IRT 5 0 R /RT /R /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Stamp /P 3 0 R "
                "/Rect [24 100 64 128] /Contents (Stamp reply) "
                "/Name /Draft /NM (mixed-stamp) /IRT 4 0 R /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Ink /P 3 0 R "
                "/Rect [72 96 112 128] /Contents (Ink reply) "
                "/InkList [[76 104 84 112 92 104]] /NM (mixed-ink) "
                "/IRT 4 0 R /RT /R /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Projection /P 3 0 R "
                "/Rect [120 96 160 128] /Contents (Projection reply) "
                "/NM (mixed-projection) /IRT 4 0 R /RT /R /F 4 >>"),
            QByteArray(
                "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
                "/Encoding /WinAnsiEncoding >>"),
        });
}

bool createUnsupportedMarkupCommentsPdf(const QString &filePath)
{
    struct AnnotationDefinition {
        QByteArray subtype;
        QByteArray contents;
        QByteArray subtypeData;
    };
    const QVector<AnnotationDefinition> definitions{
        {"Line", "unsupported-line", "/L [20 520 120 540]"},
        {"Square", "unsupported-square", {}},
        {"Circle", "unsupported-circle", {}},
        {"Polygon", "unsupported-polygon",
         "/Vertices [20 400 120 400 70 430]"},
        {"PolyLine", "unsupported-polyline",
         "/Vertices [20 360 70 390 120 360]"},
        {"Stamp", "unsupported-stamp", "/Name /Draft"},
        {"Caret", "unsupported-caret", "/Sy /P"},
        {"Ink", "unsupported-ink",
         // Foxit commonly repeats consecutive samples in otherwise valid
         // Ink paths. Rehydration must remove those samples without making
         // the comment read-only.
         "/InkList [[24 274 24 274 60 288 60 288 112 276] "
         "[30 282 30 282 72 274 72 274]]"},
        {"FileAttachment", "unsupported-file-attachment",
         "/FS << /Type /Filespec /F (sample.txt) >>"},
        {"Sound", "unsupported-sound", "/Sound 16 0 R"},
        {"Redact", "unsupported-redact",
         "/QuadPoints [20 140 120 140 20 120 120 120]"},
        {"Projection", "unsupported-projection", {}},
    };

    QByteArray annotationReferences;
    for (qsizetype index = 0; index < definitions.size(); ++index) {
        annotationReferences += QByteArray::number(index + 4) + " 0 R ";
    }
    QVector<QByteArray> objects{
        QByteArray("<< /Type /Catalog /Version /2.0 /Pages 2 0 R >>"),
        QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
        QByteArray(
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 600] "
            "/Resources << >> /Annots [")
            + annotationReferences + QByteArray("] >>"),
    };
    for (qsizetype index = 0; index < definitions.size(); ++index) {
        const AnnotationDefinition &definition = definitions.at(index);
        const int top = 560 - static_cast<int>(index) * 38;
        const int bottom = top - 24;
        QByteArray annotation(
            "<< /Type /Annot /P 3 0 R /Subtype /");
        annotation += definition.subtype;
        annotation += " /Rect [20 ";
        annotation += QByteArray::number(bottom);
        annotation += " 120 ";
        annotation += QByteArray::number(top);
        annotation += "] /Contents (";
        annotation += definition.contents;
        annotation += ") /T (Markup author) ";
        annotation += "/CreationDate (D:20240101090000Z) ";
        annotation += "/M (D:20240101100000Z) /NM (unsupported-";
        annotation += QByteArray::number(index);
        annotation += ") /F 4 ";
        if (index < 6 || definition.subtype == QByteArrayLiteral("Ink")) {
            annotation += "/C [.1 .2 .3] /CA .75 "
                "/BS << /Type /Border /W 2 /S /S >> "
                "/AP << /N 17 0 R >> ";
        }
        if (definition.subtype == QByteArrayLiteral("Ink")) {
            annotation += "/RC (ink-rich-original) "
                "/CustomInkKey (keep-me) ";
        }
        annotation += definition.subtypeData;
        annotation += " >>";
        objects.append(std::move(annotation));
    }
    objects.append(QByteArray(
        "<< /Type /Sound /R 8000 /C 1 /B 8 /E /Unsigned /Length 0 >>\n"
        "stream\n\nendstream"));
    const QByteArray drawingAppearance(
        "q\n/GS gs\n.1 .2 .3 RG\n2 w\n0 0 m\n100 24 l\nS\nQ\n");
    objects.append(
        QByteArray(
            "<< /Type /XObject /Subtype /Form /FormType 1 "
            "/BBox [0 0 100 24] /Matrix [1 0 0 1 0 0] "
            "/Resources << /ExtGState << /GS << /Type /ExtGState "
            "/CA .75 /ca .75 /AIS false /BM /Normal >> >> >> /Length ")
        + QByteArray::number(drawingAppearance.size())
        + QByteArray(" >>\nstream\n")
        + drawingAppearance
        + QByteArray("endstream"));
    return writePdfObjects(filePath, objects);
}

bool createSourceInkWithFillAppearancePdf(const QString &filePath)
{
    const QByteArray fillAppearance(
        "q .1 .2 .3 RG 2 w 20 300 m 120 324 l S "
        ".4 .5 .6 rg 30 305 30 15 re f Q");
    const QByteArray appearanceObject
        = QByteArray(
              "<< /Type /XObject /Subtype /Form /FormType 1 "
              "/BBox [20 300 120 324] /Matrix [1 0 0 1 0 0] "
              "/Resources << >> /Length ")
        + QByteArray::number(fillAppearance.size())
        + QByteArray(" >>\nstream\n")
        + fillAppearance
        + QByteArray("\nendstream");
    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 600] "
                "/Resources << >> /Annots [4 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Ink /P 3 0 R "
                "/Rect [20 300 120 324] /Contents (Filled ink) "
                "/T (Ink author) "
                "/CreationDate (D:20240101090000Z) "
                "/M (D:20240101100000Z) /NM (source-ink-fill) /F 4 "
                "/C [.1 .2 .3] /CA .75 /IC [.4 .5 .6] "
                "/BS << /Type /Border /W 2 /S /S >> "
                "/AP << /N 5 0 R >> "
                "/InkList [[24 310 60 316 112 306]] >>"),
            appearanceObject,
        });
}

bool createSourceInkWithMismatchedAppearancePdf(const QString &filePath)
{
    const QByteArray appearance(
        "q\n"
        "1 0 0 RG\n"
        "2 w\n"
        "4 10 m\n"
        "40 16 l\n"
        "92 6 l\n"
        "S\n"
        "Q\n");
    const QByteArray appearanceObject
        = QByteArray(
              "<< /Type /XObject /Subtype /Form /FormType 1 "
              "/BBox [0 0 200 48] /Matrix [1 0 0 1 0 0] "
              "/Resources << >> /Length ")
        + QByteArray::number(appearance.size())
        + QByteArray(" >>\nstream\n")
        + appearance
        + QByteArray("endstream");
    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 600] "
                "/Resources << >> /Annots [4 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Ink /P 3 0 R "
                "/Rect [20 300 120 324] /Contents (Mismatch ink) "
                "/T (Ink author) "
                "/NM (ink:11111111-2222-4333-8444-555555555555) "
                "/TrePDFThreadId "
                "(ink:11111111-2222-4333-8444-555555555555) /F 4 "
                "/C [1 0 0] /CA 1 "
                "/BS << /Type /Border /W 2 /S /S >> "
                "/InkList [[24 310 60 316 112 306]] "
                "/AP << /N 5 0 R >> >>"),
            appearanceObject,
        });
}

bool createNonMarkupCommentReplyPdf(const QString &filePath)
{
    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << >> /Annots [4 0 R 5 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [24 144 48 168] /Contents (Protected root) "
                "/NM (protected-root) /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Link /P 3 0 R "
                "/Rect [52 144 100 168] /Border [0 0 0] "
                "/IRT 4 0 R /RT /R /F 4 >>"),
        });
}

} // namespace PDFEditControllerTestSupport
