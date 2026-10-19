#include "PDFOCRPipeline.h"

#include <QCoreApplication>

#ifdef TREPDF_HAS_OCR
#include "OCRLanguageManager.h"
#include "PDFiumDocument.h"
#include "PDFiumSynchronization.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSet>
#include <QTemporaryDir>
#include <QTransform>

#include <fpdf_annot.h>
#include <fpdf_text.h>
#include <qpdf/qpdf-c.h>
#include <leptonica/allheaders.h>
#include <tesseract/baseapi.h>
#include <tesseract/ocrclass.h>
#include <tesseract/renderer.h>
#include <tesseract/resultiterator.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace PDFOCRPipeline {
namespace {

struct SourceDocument {
    PDFiumDocument handle;
    ~SourceDocument()
    {
        if (handle) {
            PDFiumProcessLock lock;
            handle.reset();
        }
    }
};

struct Page {
    FPDF_PAGE handle = nullptr;
    ~Page() { if (handle) FPDF_ClosePage(handle); }
};

struct TextPage {
    FPDF_TEXTPAGE handle = nullptr;
    ~TextPage() { if (handle) FPDFText_ClosePage(handle); }
};

struct Bitmap {
    FPDF_BITMAP handle = nullptr;
    ~Bitmap() { if (handle) FPDFBitmap_Destroy(handle); }
};

struct QPdfDocument {
    qpdf_data handle = qpdf_init();
    QPdfDocument()
    {
        if (handle) {
            qpdf_silence_errors(handle);
            qpdf_set_suppress_warnings(handle, QPDF_TRUE);
            qpdf_set_attempt_recovery(handle, QPDF_FALSE);
        }
    }
    ~QPdfDocument()
    {
        if (handle)
            qpdf_get_error(handle);
        qpdf_cleanup(&handle);
    }
};

struct PixOwner {
    Pix *handle = nullptr;
    ~PixOwner() { pixDestroy(&handle); }
};

struct StagingGuard {
    QString path;
    bool keep = false;
    ~StagingGuard() { if (!keep) QFile::remove(path); }
};

bool cancelled(const Request &request)
{
    return request.cancellation && request.cancellation->load(std::memory_order_relaxed);
}

// Validate the bytes that Tesseract will actually read, rather than hashing a
// cache path and reopening that mutable path later. Reading and writing remain
// bounded and cancellable, and the private destination is never published.
bool snapshotFile(const Request &request, const QString &sourcePath,
                  const QString &destinationPath, qint64 expectedBytes,
                  const QByteArray &expectedHash)
{
    constexpr qint64 maximumModelBytes = 128 * 1024 * 1024;
    const QFileInfo info(sourcePath);
    if (sourcePath.isEmpty() || sourcePath.contains(QChar::Null)
        || !info.isAbsolute() || !info.isFile() || info.isSymLink()
        || expectedBytes <= 0 || expectedBytes > maximumModelBytes
        || expectedHash.size() != 32 || info.size() != expectedBytes)
        return false;
    QFile source(sourcePath);
    QFile destination(destinationPath);
    if (!source.open(QIODevice::ReadOnly) || source.size() != expectedBytes
        || !destination.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return false;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 total = 0;
    while (!source.atEnd()) {
        if (cancelled(request))
            return false;
        const QByteArray chunk = source.read(64 * 1024);
        if (chunk.isEmpty() || chunk.size() > expectedBytes - total
            || destination.write(chunk) != chunk.size())
            return false;
        total += chunk.size();
        hash.addData(chunk);
    }
    return !cancelled(request) && source.error() == QFileDevice::NoError
        && total == expectedBytes && hash.result() == expectedHash
        && destination.flush();
}

bool snapshotLanguageData(const Request &request, const QTemporaryDir &directory)
{
    const QStringList codes = request.language.split(QLatin1Char('+'));
    if (!request.modelPaths.isEmpty() && request.modelPaths.size() != codes.size())
        return false;
    const QDir bundled(request.tessdataPath);
    for (const QString &code : codes) {
        const QString filename = code + QStringLiteral(".traineddata");
        const QString path = request.modelPaths.isEmpty()
            ? bundled.absoluteFilePath(filename) : request.modelPaths.value(code);
        if (!snapshotFile(request, path, directory.filePath(filename),
                          OCRLanguageManager::modelBytes(code),
                          OCRLanguageManager::modelSha256(code)))
            return false;
    }
    // This tiny Unicode font is shipped by the pinned Tesseract 5.5.3 package.
    // It also needs a stable private copy for TessPDFRenderer after recognition.
    return snapshotFile(request, bundled.absoluteFilePath(QStringLiteral("pdf.ttf")),
                        directory.filePath(QStringLiteral("pdf.ttf")), 572,
                        QByteArray::fromHex("c7845420925a23d88ed830a63957b8af85a66a8daf8d9fc90e843673b2ef1a59"));
}

bool nonWhitespace(char32_t value)
{
    return value != 0 && value <= 0x10ffff && !QChar::isSpace(value);
}

// -1 is an unreadable/oversized text page, 0 is empty, 1 contains source text.
int existingText(FPDF_PAGE page)
{
    TextPage text{FPDFText_LoadPage(page)};
    if (!text.handle)
        return -1;
    const int count = FPDFText_CountChars(text.handle);
    // A huge existing layer need not be traversed to decide that OCR must not
    // duplicate it. PDFium still owns parsing limits for the source document.
    if (count > maximumCharactersPerPage)
        return 1;
    if (count < 0)
        return -1;
    for (int i = 0; i < count; ++i) {
        if (nonWhitespace(FPDFText_GetUnicode(text.handle, i)))
            return 1;
    }
    return 0;
}

QImage render(FPDF_PAGE page, int width, int height, bool annotations)
{
    QImage image(width, height, QImage::Format_RGB32);
    if (image.isNull())
        return {};
    image.fill(Qt::white);
    Bitmap bitmap{FPDFBitmap_CreateEx(width, height, FPDFBitmap_BGRx,
                                     image.bits(), int(image.bytesPerLine()))};
    if (!bitmap.handle)
        return {};
    FPDF_RenderPageBitmap(bitmap.handle, page, 0, 0, width, height, 0,
                         annotations ? FPDF_ANNOT : 0);
    return image;
}

// Tesseract's text-only page uses image pixels * 72 / DPI and bottom-left
// coordinates. PDFium's inverse display transform accounts for CropBox and
// intrinsic /Rotate, including pages whose visible origin is not (0, 0).
bool layerTransform(FPDF_PAGE page, int width, int height, QTransform &transform)
{
    QPointF origin, right, top;
    const auto map = [&](int x, int y, QPointF &point) {
        double pageX = 0, pageY = 0;
        if (!FPDF_DeviceToPage(page, 0, 0, width, height, 0,
                               x, y, &pageX, &pageY)
            || !std::isfinite(pageX) || !std::isfinite(pageY)) {
            return false;
        }
        point = QPointF(pageX, pageY);
        return true;
    };
    if (!map(0, height, origin) || !map(width, height, right)
        || !map(0, 0, top)) {
        return false;
    }
    const double layerWidth = width * 72.0 / renderDpi;
    const double layerHeight = height * 72.0 / renderDpi;
    transform = QTransform((right.x() - origin.x()) / layerWidth,
                           (right.y() - origin.y()) / layerWidth,
                           (top.x() - origin.x()) / layerHeight,
                           (top.y() - origin.y()) / layerHeight,
                           origin.x(), origin.y());
    return transform.isInvertible();
}

qpdf_oh copyDictionary(qpdf_data document, qpdf_oh source)
{
    const qpdf_oh copy = qpdf_oh_new_dictionary(document);
    if (qpdf_oh_is_null(document, source))
        return copy;
    if (!qpdf_oh_is_dictionary(document, source))
        return 0;
    // The C API permits only one active dictionary iterator. Snapshot the keys
    // before accessing children; shallow copy keeps unrelated resources shared.
    QList<QByteArray> keys;
    qpdf_oh_begin_dict_key_iter(document, source);
    while (qpdf_oh_dict_more_keys(document)) {
        keys.append(QByteArray(qpdf_oh_dict_next_key(document)));
        if (keys.size() > 65536)
            return 0;
    }
    for (const auto &key : keys)
        qpdf_oh_replace_key(document, copy, key.constData(),
                            qpdf_oh_get_key(document, source, key.constData()));
    return copy;
}

qpdf_oh inheritedResources(qpdf_data document, qpdf_oh page)
{
    qpdf_oh current = page;
    for (int depth = 0; depth < 128; ++depth) {
        if (!qpdf_oh_is_dictionary(document, current))
            return qpdf_oh_new_null(document);
        if (qpdf_oh_has_key(document, current, "/Resources"))
            return qpdf_oh_get_key(document, current, "/Resources");
        current = qpdf_oh_get_key(document, current, "/Parent");
    }
    return 0;
}

qpdf_oh newStream(qpdf_data document, const QByteArray &bytes)
{
    const qpdf_oh stream = qpdf_oh_new_stream(document);
    const qpdf_oh null = qpdf_oh_new_null(document);
    qpdf_oh_replace_stream_data(document, stream,
        reinterpret_cast<const unsigned char *>(bytes.constData()), size_t(bytes.size()), null, null);
    return stream;
}

QByteArray number(double value)
{
    // Locale-independent fixed notation; PDF numeric tokens cannot contain an
    // exponent. These coefficients retain considerably more than pixel accuracy.
    return QByteArray::number(value == 0 ? 0.0 : value, 'f', 12);
}

bool importLayer(qpdf_data destination, int sourcePage, const QString &layerPath,
                 int width, int height, const QTransform &transform)
{
    QPdfDocument layer;
    const QByteArray path = QFile::encodeName(layerPath);
    if (!layer.handle || qpdf_read(layer.handle, path.constData(), "") != QPDF_SUCCESS
        || qpdf_get_num_pages(layer.handle) != 1) {
        return false;
    }
    const qpdf_oh layerPage = qpdf_get_page_n(layer.handle, 0);
    unsigned char *buffer = nullptr;
    size_t length = 0;
    const auto contentStatus = qpdf_oh_get_page_content_data(layer.handle, layerPage, &buffer, &length);
    if (contentStatus != QPDF_SUCCESS || length == 0 || length > 16 * 1024 * 1024) {
        qpdf_oh_free_buffer(&buffer);
        return false;
    }
    const QByteArray contents(reinterpret_cast<const char *>(buffer), qsizetype(length));
    qpdf_oh_free_buffer(&buffer);
    // Resources must be indirect for copy_foreign_object. Source stream files
    // stay in the temporary directory until the destination has been serialized.
    const qpdf_oh resources = qpdf_make_indirect_object(layer.handle,
        qpdf_oh_get_key(layer.handle, layerPage, "/Resources"));
    const qpdf_oh importedResources = qpdf_oh_copy_foreign_object(destination, layer.handle, resources);
    const qpdf_oh form = newStream(destination, contents);
    const qpdf_oh formDictionary = qpdf_oh_get_dict(destination, form);
    qpdf_oh_replace_key(destination, formDictionary, "/Type", qpdf_oh_new_name(destination, "/XObject"));
    qpdf_oh_replace_key(destination, formDictionary, "/Subtype", qpdf_oh_new_name(destination, "/Form"));
    qpdf_oh_replace_key(destination, formDictionary, "/Resources", importedResources);
    const QByteArray box = "[0 0 " + number(width * 72.0 / renderDpi) + ' '
        + number(height * 72.0 / renderDpi) + ']';
    qpdf_oh_replace_key(destination, formDictionary, "/BBox", qpdf_oh_parse(destination, box.constData()));
    qpdf_oh_replace_key(destination, formDictionary, "/TrePDFOCR", qpdf_oh_new_integer(destination, 1));

    const qpdf_oh page = qpdf_get_page_n(destination, size_t(sourcePage));
    const qpdf_oh pageResources = copyDictionary(destination, inheritedResources(destination, page));
    if (!pageResources)
        return false;
    const qpdf_oh xobjects = copyDictionary(destination,
        qpdf_oh_get_key(destination, pageResources, "/XObject"));
    if (!xobjects)
        return false;
    QByteArray name;
    for (int suffix = 0; suffix < 10000; ++suffix) {
        name = "/TrePDFOCR" + QByteArray::number(suffix);
        if (!qpdf_oh_has_key(destination, xobjects, name.constData()))
            break;
        name.clear();
    }
    if (name.isEmpty())
        return false;
    qpdf_oh_replace_key(destination, xobjects, name.constData(), form);
    qpdf_oh_replace_key(destination, pageResources, "/XObject", xobjects);
    qpdf_oh_replace_key(destination, page, "/Resources", pageResources);
    const QByteArray command = "q\n/TrePDFOCR << /Version 1 >> BDC\n"
        + number(transform.m11()) + ' ' + number(transform.m12()) + ' '
        + number(transform.m21()) + ' ' + number(transform.m22()) + ' '
        + number(transform.dx()) + ' ' + number(transform.dy()) + " cm\n"
        + name + " Do\nEMC\nQ\n";
    const qpdf_oh contentArray = qpdf_oh_new_array(destination);
    // Prepending the isolated invisible layer ensures arbitrary graphics state
    // left by the original streams cannot clip/transform the OCR. The original
    // streams retain their order and bytes, and begin with the same state.
    qpdf_oh_append_item(destination, contentArray, newStream(destination, command));
    const qpdf_oh existing = qpdf_oh_get_key(destination, page, "/Contents");
    if (qpdf_oh_is_array(destination, existing)) {
        const int count = qpdf_oh_get_array_n_items(destination, existing);
        if (count > 65536)
            return false;
        for (int i = 0; i < count; ++i)
            qpdf_oh_append_item(destination, contentArray, qpdf_oh_get_array_item(destination, existing, i));
    } else if (qpdf_oh_is_stream(destination, existing)) {
        qpdf_oh_append_item(destination, contentArray, existing);
    } else if (!qpdf_oh_is_null(destination, existing)) {
        return false;
    }
    qpdf_oh_replace_key(destination, page, "/Contents", contentArray);
    return !qpdf_has_error(destination) && !qpdf_has_error(layer.handle);
}

bool validateOutput(const Request &request, FPDF_DOCUMENT source,
                    const QString &outputPath, const QVector<int> &recognized)
{
    // The whole transaction is worker-side. A bounded thumbnail comparison
    // catches accidental visible OCR paint or content damage before committing.
    // Acquire per page, allowing viewer renders between validation transactions.
    SourceDocument output;
    {
        PDFiumProcessLock lock;
        output.handle = PDFiumDocument::open(outputPath, request.password);
        if (!output.handle || FPDF_GetPageCount(source) != FPDF_GetPageCount(output.handle)
            || FPDF_GetSecurityHandlerRevision(source)
                != FPDF_GetSecurityHandlerRevision(output.handle)) {
            return false;
        }
    }
    for (int index : recognized) {
        if (cancelled(request))
            return false;
        PDFiumProcessLock lock;
        Page before{FPDF_LoadPage(source, index)};
        Page after{FPDF_LoadPage(output.handle, index)};
        if (!before.handle || !after.handle || existingText(after.handle) != 1
            || FPDF_GetPageWidthF(before.handle) != FPDF_GetPageWidthF(after.handle)
            || FPDF_GetPageHeightF(before.handle) != FPDF_GetPageHeightF(after.handle)
            || FPDFPage_GetAnnotCount(before.handle) != FPDFPage_GetAnnotCount(after.handle)) {
            return false;
        }
        const double pageWidth = FPDF_GetPageWidthF(before.handle);
        const double pageHeight = FPDF_GetPageHeightF(before.handle);
        const double scale = std::min(1.0, 1200.0 / std::max(pageWidth, pageHeight));
        const int width = std::max(1, int(std::ceil(pageWidth * scale)));
        const int height = std::max(1, int(std::ceil(pageHeight * scale)));
        const QImage first = render(before.handle, width, height, true);
        const QImage second = render(after.handle, width, height, true);
        if (first.isNull() || second.isNull() || first != second)
            return false;
    }
    return true;
}

} // namespace

Result run(const Request &request, const QString &stagingOutputPath,
           const std::function<void(int)> &progress) try
{
    Result result;
    const auto fail = [&](const QString &error) {
        result.outcome = cancelled(request) ? Result::Cancelled : Result::Failed;
        result.error = error;
        return result;
    };
    if (cancelled(request)) {
        result.outcome = Result::Cancelled;
        return result;
    }
    const QFileInfo sourceInfo(request.sourcePath);
    const QFileInfo outputInfo(stagingOutputPath);
    if (!sourceInfo.isFile() || request.sourcePath.contains(QChar::Null)
        || request.password.contains('\0') || stagingOutputPath.isEmpty()
        || stagingOutputPath.contains(QChar::Null)
        || outputInfo.exists() || outputInfo.isSymLink()
        || sourceInfo.absoluteFilePath() == outputInfo.absoluteFilePath()
        || request.sourcePages.isEmpty() || request.sourcePages.size() > maximumPages
        || !OCRLanguageManager::validSelection(request.language)) {
        //% "Choose a valid page range, language and separate OCR output file."
        return fail(qtTrId("OCRPipelineInvalidRequest"));
    }
    StagingGuard staging{stagingOutputPath};
    // The engine and renderers are destroyed before this private directory.
    // No network or shared-cache mutation is allowed on the recognition path.
    QTemporaryDir languageData;
    if (!languageData.isValid() || request.tessdataPath.isEmpty()
        || request.tessdataPath.contains(QChar::Null)
        || !snapshotLanguageData(request, languageData)) {
        //% "The OCR language files are missing or damaged. Open Languages and prepare them again. If English or Vietnamese is affected, reinstall TrePDF."
        return fail(qtTrId("OCRPipelineModelsMissing"));
    }
    SourceDocument source;
    int pageCount = 0;
    {
        PDFiumProcessLock lock;
        source.handle = PDFiumDocument::open(request.sourcePath, request.password);
        if (source.handle)
            pageCount = FPDF_GetPageCount(source.handle);
    }
    if (!source.handle || pageCount <= 0) {
        //% "The source PDF could not be read for OCR."
        return fail(qtTrId("OCRPipelineReadFailed"));
    }
    int previous = -1;
    for (int page : request.sourcePages) {
        if (page <= previous || page >= pageCount)
            return fail(qtTrId("OCRPipelineInvalidRequest"));
        previous = page;
    }
    // qpdf keeps imported stream input sources alive until its destination is
    // destroyed. Declare the directory first so Windows can close those file
    // handles before QTemporaryDir attempts to remove the text-layer files.
    QTemporaryDir layers;
    if (!layers.isValid()) {
        //% "The searchable PDF could not be written."
        return fail(qtTrId("OCRPipelineWriteFailed"));
    }
    QPdfDocument destination;
    const QByteArray sourcePath = QFile::encodeName(request.sourcePath);
    if (!destination.handle
        || qpdf_read(destination.handle, sourcePath.constData(), request.password.constData()) != QPDF_SUCCESS
        || qpdf_get_num_pages(destination.handle) != pageCount) {
        return fail(qtTrId("OCRPipelineReadFailed"));
    }
    if (qpdf_is_encrypted(destination.handle)
        && (!qpdf_allow_extract_all(destination.handle) || !qpdf_allow_modify_all(destination.handle))) {
        //% "This PDF does not allow the content changes and text extraction required for OCR."
        return fail(qtTrId("OCRPipelineUnsupported"));
    }
    tesseract::TessBaseAPI engine;
    const QByteArray dataPath = QFile::encodeName(languageData.path());
    const QByteArray language = request.language.toLatin1();
    if (engine.Init(dataPath.constData(), language.constData(), tesseract::OEM_LSTM_ONLY) != 0)
        return fail(qtTrId("OCRPipelineModelsMissing"));
    engine.SetPageSegMode(tesseract::PSM_AUTO);
#ifdef Q_OS_WIN
    engine.SetVariable("debug_file", "NUL");
#else
    engine.SetVariable("debug_file", "/dev/null");
#endif
    QVector<int> recognized;
    int totalCharacters = 0;
    int completed = 0;
    for (int index : request.sourcePages) {
        if (cancelled(request))
            return fail({});
        QImage image;
        QTransform transform;
        bool skip = false;
        {
            PDFiumProcessLock lock;
            Page page{FPDF_LoadPage(source.handle, index)};
            if (!page.handle)
                return fail(qtTrId("OCRPipelineReadFailed"));
            const int textState = existingText(page.handle);
            if (textState < 0)
                return fail(qtTrId("OCRPipelineReadFailed"));
            skip = textState > 0;
            if (!skip) {
                const double width = std::ceil(FPDF_GetPageWidthF(page.handle) * renderDpi / 72.0);
                const double height = std::ceil(FPDF_GetPageHeightF(page.handle) * renderDpi / 72.0);
                if (!std::isfinite(width) || !std::isfinite(height) || width < 1 || height < 1
                    || width > maximumRenderDimension || height > maximumRenderDimension
                    || width * height > maximumRenderPixels) {
                    //% "A page exceeds the size or text limits supported by OCR."
                    return fail(qtTrId("OCRPipelineLimitExceeded"));
                }
                if (!layerTransform(page.handle, int(width), int(height), transform))
                    return fail(qtTrId("OCRPipelineReadFailed"));
                // Exclude annotation appearances: they already carry their own
                // semantic contents and must not be duplicated into page text.
                image = render(page.handle, int(width), int(height), false);
                if (image.isNull())
                    return fail(qtTrId("OCRPipelineLimitExceeded"));
            }
        }
        if (skip) {
            ++result.skippedPages;
        } else {
            PixOwner pix{pixCreate(image.width(), image.height(), 8)};
            if (!pix.handle)
                return fail(qtTrId("OCRPipelineLimitExceeded"));
            pixSetResolution(pix.handle, renderDpi, renderDpi);
            const int wordsPerLine = pixGetWpl(pix.handle);
            l_uint32 *data = pixGetData(pix.handle);
            for (int y = 0; y < image.height(); ++y) {
                if (cancelled(request))
                    return fail({});
                const auto *input = reinterpret_cast<const QRgb *>(image.constScanLine(y));
                l_uint32 *line = data + y * wordsPerLine;
                for (int x = 0; x < image.width(); ++x)
                    SET_DATA_BYTE(line, x, qGray(input[x]));
            }
            const int width = image.width(), height = image.height();
            image = {}; // release the full RGB render before recognition
            engine.SetImage(pix.handle);
            engine.SetSourceResolution(renderDpi);
            tesseract::ETEXT_DESC monitor;
            monitor.cancel_this = request.cancellation.get();
            monitor.cancel = [](void *data, int) {
                return data && static_cast<std::atomic_bool *>(data)->load(std::memory_order_relaxed);
            };
            monitor.set_deadline_msecs(120000);
            if (engine.Recognize(&monitor) != 0 || cancelled(request)) {
                //% "Text recognition could not finish. Try fewer pages or a clearer scan."
                return fail(qtTrId("OCRPipelineRecognitionFailed"));
            }
            std::unique_ptr<tesseract::ResultIterator> iterator(engine.GetIterator());
            int characters = 0;
            if (iterator) {
                do {
                    std::unique_ptr<char[]> symbol(iterator->GetUTF8Text(tesseract::RIL_SYMBOL));
                    if (!symbol)
                        continue;
                    const QString text = QString::fromUtf8(symbol.get());
                    for (char32_t codepoint : text.toUcs4()) {
                        if (nonWhitespace(codepoint))
                            ++characters;
                    }
                    if (characters > maximumCharactersPerPage)
                        return fail(qtTrId("OCRPipelineLimitExceeded"));
                } while (iterator->Next(tesseract::RIL_SYMBOL));
            }
            iterator.reset();
            totalCharacters += characters;
            if (totalCharacters > maximumCharactersPerDocument)
                return fail(qtTrId("OCRPipelineLimitExceeded"));
            if (characters == 0) {
                ++result.emptyPages;
            } else {
                const QString base = layers.filePath(QStringLiteral("layer-%1").arg(index));
                const QByteArray encodedBase = QFile::encodeName(base);
                {
                    tesseract::TessPDFRenderer renderer(encodedBase.constData(), dataPath.constData(), true);
                    if (!renderer.BeginDocument("TrePDF OCR") || !renderer.AddImage(&engine)
                        || !renderer.EndDocument() || !renderer.happy()) {
                        return fail(qtTrId("OCRPipelineWriteFailed"));
                    }
                }
                if (cancelled(request))
                    return fail({});
                if (!importLayer(destination.handle, index, base + QStringLiteral(".pdf"),
                                 width, height, transform)) {
                    return fail(qtTrId("OCRPipelineWriteFailed"));
                }
                recognized.append(index);
                ++result.recognizedPages;
            }
            engine.Clear();
        }
        ++completed;
        if (progress)
            progress(completed);
    }
    if (cancelled(request))
        return fail({});
    if (recognized.isEmpty()) {
        result.outcome = Result::NoText;
        return result;
    }
    const QByteArray outputPath = QFile::encodeName(stagingOutputPath);
    if (qpdf_init_write(destination.handle, outputPath.constData()) != QPDF_SUCCESS)
        return fail(qtTrId("OCRPipelineWriteFailed"));
    qpdf_set_preserve_encryption(destination.handle, QPDF_TRUE);
    qpdf_set_object_stream_mode(destination.handle, qpdf_o_preserve);
    qpdf_set_stream_data_mode(destination.handle, qpdf_s_preserve);
    if (qpdf_has_error(destination.handle) || qpdf_write(destination.handle) != QPDF_SUCCESS)
        return fail(qtTrId("OCRPipelineWriteFailed"));
    if (cancelled(request))
        return fail({});
    if (!validateOutput(request, source.handle, stagingOutputPath, recognized)) {
        //% "The OCR output did not pass validation. The original PDF is unchanged."
        return fail(qtTrId("OCRPipelineValidationFailed"));
    }
    if (cancelled(request))
        return fail({});
    staging.keep = true;
    result.outcome = Result::Succeeded;
    return result;
}
catch (...)
{
    // Never propagate third-party allocation/parse failures across the worker
    // boundary or include source text/password-bearing diagnostics in the UI.
    Result result;
    result.outcome = cancelled(request) ? Result::Cancelled : Result::Failed;
    result.error = qtTrId("OCRPipelineRecognitionFailed");
    return result;
}

} // namespace PDFOCRPipeline

#else

PDFOCRPipeline::Result PDFOCRPipeline::run(const Request &, const QString &,
                                         const std::function<void(int)> &)
{
    Result result;
    //% "OCR is not available in this build."
    result.error = qtTrId("OCRPipelineUnavailable");
    return result;
}

#endif
