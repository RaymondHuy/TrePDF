#pragma once

#include "ApplicationFonts.h"
#include "DraftImageItem.h"
#include "FormFieldLayer.h"
#include "PDFAnnotationUtils.h"
#include "PDFDocument.h"
#include "PDFEditController.h"
#include "PDFFormController.h"
#include "PDFFormDesignerController.h"
#include "PDFFormAppearance.h"
#include "PDFFormView.h"
#include "PDFFormWorker.h"
#include "PDFPageItem.h"
#include "PDFRasterBudget.h"
#include "PDFRasterPlan.h"
#include "PDFSaveWriter.h"
#include "PDFSearchController.h"
#include "PDFTabModel.h"
#include "PDFWidgetAnnotationSuppression.h"
#include "PDFiumSynchronization.h"

#include <QAccessible>
#include <QElapsedTimer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImage>
#include <QInputMethodEvent>
#include <QJSValue>
#include <QLocale>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QRawFont>
#include <QColorSpace>
#include <QRegularExpression>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QQmlComponent>
#include <QQmlEngine>
#include <qqml.h>
#include <QSemaphore>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextLayout>
#include <QThread>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <fpdf_annot.h>
#include <fpdf_edit.h>
#include <fpdf_formfill.h>
#include <fpdf_fwlevent.h>
#include <fpdf_save.h>
#include <fpdf_text.h>
#include <fpdf_transformpage.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace PDFEditControllerTestSupport {


constexpr char kRegularText[] = "Regular PDF text";
constexpr char kBoldText[] = "Bold PDF text";
constexpr char kBottomEdgeText[] = "BOTTOM ABC";
constexpr char kGroupedText[] = "Hello world";
constexpr char kSourceParagraphText[]
    = "Hello world\nParagraph second line\nLast paragraph line";
constexpr char kMixedRegularText[] = "Plain";
constexpr char kMixedBoldText[] = "Strong";
constexpr char kLeftColumnText[] = "Left column";
constexpr char kRightColumnText[] = "Right column";
constexpr char kUprightText[] = "Upright";
constexpr char kObliqueText[] = "Oblique";
constexpr char kStretchedText[] = "Scale";
constexpr char kImageFixtureText[] = "Image fixture text";
constexpr char kFontCollisionTargetText[] = "Wxyz";
constexpr char kLegacyListBoxFieldName[] = "LegacyOfficeList";
constexpr char kLegacyListBoxWidgetId[]
    = "listbox-field-22222222-2222-4222-8222-222222222222";
constexpr char kSourceTextMarkupAppearance[]
    = "q\n"
      "0.13 0.27 0.61 RG\n"
      "2 w\n"
      "7 11 m 133 11 l S\n"
      "% TrePDF source text markup appearance sentinel\n"
      "Q\n";
constexpr int kPdfButtonNoToggleToOff = 1 << 14;
constexpr int kPdfButtonRadio = 1 << 15;
constexpr int kPdfTextDoNotScroll = 1 << 23;
constexpr int kPdfTextComb = 1 << 24;
constexpr qsizetype kCommentIndexBudgetFieldCharacters = 1536 * 1024;

class PdfFileWriter : public FPDF_FILEWRITE
{
public:
    explicit PdfFileWriter(const QString &filePath)
        : m_file(filePath)
    {
        version = 1;
        WriteBlock = &PdfFileWriter::writeBlock;
    }

    bool open()
    {
        return m_file.open(QIODevice::WriteOnly | QIODevice::Truncate);
    }

private:
    static int writeBlock(FPDF_FILEWRITE *fileWriter,
                          const void *data,
                          unsigned long size)
    {
        auto *writer = static_cast<PdfFileWriter *>(fileWriter);
        const qint64 byteCount = static_cast<qint64>(size);
        return writer->m_file.write(static_cast<const char *>(data), byteCount) == byteCount;
    }

    QFile m_file;
};

bool writePdfObjects(const QString &filePath,
                     const QVector<QByteArray> &objects);

bool createLegacyTrePDFRadioButtonPdf(const QString &filePath);

bool createLegacyTrePDFListBoxPdf(const QString &filePath);

bool createDropdownDeleteRegressionPdf(const QString &filePath);

bool createPushButtonActionPdf(
    const QString &filePath,
    const QByteArray &uri = QByteArray("https://example.com/"));

bool createChoiceFieldsPdf(const QString &filePath);

bool createTrePDFDropdownWithExportValuesPdf(const QString &filePath);

QVector<int> selectedChoiceOptions(
    const QString &filePath,
    int annotationIndex);

bool createArialSubsetCollisionSourcePdf(const QString &filePath);

enum class PatternPaintMode {
    Fill,
    Stroke
};

bool createPatternPaintPdf(const QString &filePath, PatternPaintMode mode);

bool createPatternFillPdf(const QString &filePath);

bool createPatternStrokePdf(const QString &filePath);

bool createGrayscaleWhiteFillPdf(const QString &filePath);

bool createIccBasedTextPdf(const QString &filePath);

QImage renderFirstPage(const QString &filePath,
                       const QSize &pixelSize,
                       int renderFlags = 0,
                       const QRect &region = {});

QImage renderFirstPageWithForms(
    const QString &filePath,
    const QSize &pixelSize,
    const QStringList &suppressedWidgetIds = {},
    const QPointF &focusedNormalizedPoint = QPointF(-1.0, -1.0));

class TestablePDFPageItem final : public PDFPageItem
{
public:
    explicit TestablePDFPageItem(QQuickItem *parent = nullptr)
        : PDFPageItem(parent)
    {
    }

    void completeForTest()
    {
        if (!isComponentComplete()) {
            componentComplete();
        }
    }

    QSize texturePixelSizeForTest() const
    {
        return QSize(
            m_textureWidth.load(std::memory_order_relaxed),
            m_textureHeight.load(std::memory_order_relaxed));
    }

    int textureCountForTest() const
    {
        return m_textureCount.load(std::memory_order_relaxed);
    }

    qint64 maximumTextureBytesForTest() const
    {
        return m_maximumTextureBytes.load(std::memory_order_relaxed);
    }

    qint64 totalTextureBytesForTest() const
    {
        return m_totalTextureBytes.load(std::memory_order_relaxed);
    }

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode,
                             UpdatePaintNodeData *data) override
    {
        QSGNode *node = PDFPageItem::updatePaintNode(oldNode, data);
        QSize textureSize;
        int textureCount = 0;
        qint64 maximumTextureBytes = 0;
        qint64 totalTextureBytes = 0;
        const auto inspectNode = [&](const auto &self, QSGNode *current) -> void {
            if (!current) {
                return;
            }
            if (current->type() == QSGNode::GeometryNodeType) {
                auto *textureNode = dynamic_cast<QSGSimpleTextureNode *>(current);
                if (QSGTexture *texture = textureNode ? textureNode->texture() : nullptr) {
                    const QSize currentSize = texture->textureSize();
                    // The page-wide base texture is the first child. Preserve
                    // the original helper's meaning for full-page zoom tests.
                    if (textureCount == 0) {
                        textureSize = currentSize;
                    }
                    ++textureCount;
                    const qint64 bytes = PDFRasterPlan::imageBytes(currentSize);
                    maximumTextureBytes = qMax(maximumTextureBytes, bytes);
                    totalTextureBytes += bytes;
                }
            }
            for (QSGNode *child = current->firstChild(); child;
                 child = child->nextSibling()) {
                self(self, child);
            }
        };
        inspectNode(inspectNode, node);
        m_textureWidth.store(textureSize.width(), std::memory_order_relaxed);
        m_textureHeight.store(textureSize.height(), std::memory_order_relaxed);
        m_textureCount.store(textureCount, std::memory_order_relaxed);
        m_maximumTextureBytes.store(maximumTextureBytes, std::memory_order_relaxed);
        m_totalTextureBytes.store(totalTextureBytes, std::memory_order_relaxed);
        return node;
    }

private:
    std::atomic<int> m_textureWidth = 0;
    std::atomic<int> m_textureHeight = 0;
    std::atomic<int> m_textureCount = 0;
    std::atomic<qint64> m_maximumTextureBytes = 0;
    std::atomic<qint64> m_totalTextureBytes = 0;
};

QImage renderFirstPageInViewer(const QString &filePath,
                               const QSize &logicalSize);

qsizetype changedPixelCount(const QImage &left,
                            const QImage &right,
                            const QRect &area);

QRect changedPixelBounds(const QImage &left,
                         const QImage &right,
                         const QRect &area);

QRect redPixelBounds(const QImage &image, const QRect &area = {});

QRect darkChangedPixelBounds(const QImage &left,
                             const QImage &right,
                             const QRect &area);

bool containsPatternColors(const QImage &image);

bool addTextObject(FPDF_DOCUMENT document,
                   FPDF_PAGE page,
                   const char *fontName,
                   const QString &text,
                   double x,
                   double y,
                   double horizontalScale = 1.0,
                   float fontSize = 24.0F);

bool addTransformedTextObject(FPDF_DOCUMENT document,
                              FPDF_PAGE page,
                              const QString &text,
                              const FS_MATRIX &matrix);

bool addTextObjectWithFont(FPDF_DOCUMENT document,
                           FPDF_PAGE page,
                           FPDF_FONT font,
                           const QString &text,
                           double x,
                           double y);

bool addStyledTextObject(FPDF_DOCUMENT document,
                         FPDF_PAGE page,
                         const QString &text,
                         FPDF_TEXT_RENDERMODE renderMode,
                         const QColor &color,
                         float strokeWidth,
                         double x,
                         double y);

bool createFontStylePdf(const QString &filePath);

struct SourceParagraphTextPart {
    QString text;
    double x = 72.0;
    double y = 740.0;
    const char *fontName = "Helvetica";
    double horizontalScale = 1.0;
    float fontSize = 24.0F;
};

QVector<SourceParagraphTextPart> mixedSourceParagraphParts();

QString sourceParagraphText(const QVector<SourceParagraphTextPart> &parts,
                            qsizetype start,
                            qsizetype count);

bool createSourceParagraphPdf(
    const QString &filePath,
    const QVector<SourceParagraphTextPart> &parts = {
        {QStringLiteral("Hello"), 72.0, 740.0},
        {QStringLiteral("world"), 140.0, 740.0},
        {QStringLiteral("Paragraph second line"), 72.0, 710.0},
        {QStringLiteral("Last paragraph line"), 72.0, 680.0}});

bool createSourceSpacingPdf(const QString &filePath,
                            qreal letterSpacing,
                            qreal wordSpacing,
                            qreal textMatrixScale = 1.0,
                            bool irregularAdjustments = false);

bool createBottomEdgeTextPdf(const QString &filePath);

bool createSquigglyAspectRatioPdf(const QString &filePath);

bool createDifferentPageSizesPdf(const QString &filePath);

bool createStickyNotePopupPdf(const QString &filePath);

bool createTextMarkupCommentPdf(const QString &filePath,
                                const QByteArray &subtypeName,
                                const QByteArray &annotationName,
                                const QByteArray &contents);

bool createProtectedCommentTextPdf(const QString &filePath);

bool createLockedCommentTextPdf(const QString &filePath);

bool createLockedCommentReplyThreadPdf(const QString &filePath);

bool createProtectedFreeTextPdf(const QString &filePath);

bool createPageLocalDuplicateFreeTextNamesPdf(const QString &filePath);

bool createNestedCommentRepliesPdf(const QString &filePath);

bool createCommentStateThreadPdf(const QString &filePath);

bool createReplyCommentStateHistoryPdf(const QString &filePath);

bool createMixedCaseCommentStateHistoryPdf(const QString &filePath);

bool createZeroRectUnderlineReplyPdf(const QString &filePath);

bool createRootCommentOnlyPdf(const QString &filePath);

bool createCommentIndexMemoryBudgetPdf(const QString &filePath);

bool createUnsupportedRootCommentReplyPdf(const QString &filePath);

bool createMixedSubtypeCommentRepliesPdf(const QString &filePath);

bool createUnsupportedMarkupCommentsPdf(const QString &filePath);

bool createSourceInkWithFillAppearancePdf(const QString &filePath);

bool createSourceInkWithMismatchedAppearancePdf(const QString &filePath);

bool createNonMarkupCommentReplyPdf(const QString &filePath);

bool createThreePageTextPdf(const QString &filePath);

bool createStretchedTextPdf(const QString &filePath,
                            qreal horizontalScale = 2.0);

bool createEmbeddedArialPdf(const QString &filePath);

bool createEmbeddedFontPdf(const QString &filePath,
                           const QByteArray &fontData,
                           const QString &text);

bool createGroupedSubsetFontPdf(const QString &filePath);

bool createOffsetCropPdf(const QString &filePath);

bool createRenderModePdf(const QString &filePath);

bool createInvisibleOcrTextPdf(const QString &filePath);

bool createVerticalTextPdf(const QString &filePath);

bool createTopLevelImagePdf(
    const QString &filePath,
    const FS_MATRIX &imageMatrix = {120.0F, 0.0F, 0.0F, 60.0F, 360.0F, 620.0F},
    int pageRotation = 0,
    bool offsetCrop = false);

bool createRotatedPdf(const QString &filePath);

QVariantMap candidateForText(const QVariantList &candidates, const QString &text);

QVariantMap candidateForKind(const QVariantList &candidates, const QString &kind);

QVariantList transformedInkStrokes(const QVariantList &strokes,
                                   const QRectF &sourceFrame,
                                   const QRectF &targetFrame);

QVariantList transformedDrawingVertices(const QVariantList &vertices,
                                        const QRectF &sourceFrame,
                                        const QRectF &targetFrame);

QVariantList waitForCandidates(PDFEditController &controller, int page);

QRectF rectFromCoordinateMap(const QVariant &value);

QRectF boundingRectForCoordinateMaps(const QVariantList &values);

QRectF normalizedGlyphBoundsForText(const QString &filePath,
                                    const QString &text,
                                    int pageIndex = 0);

QString pageText(const QString &filePath, int pageIndex = 0);

QString normalizedPageText(const QString &filePath, int pageIndex = 0);

QString pageObjectText(FPDF_PAGEOBJECT object, FPDF_TEXTPAGE textPage);

struct TextPaintInspection {
    bool readable = false;
    bool found = false;
    int objectType = FPDF_PAGEOBJ_UNKNOWN;
    FPDF_TEXT_RENDERMODE renderMode = FPDF_TEXTRENDERMODE_UNKNOWN;
    bool hasFillColor = false;
    QColor fillColor;
};

TextPaintInspection inspectTopLevelTextPaint(const QString &filePath,
                                             const QString &targetText);

QString baseFontName(FPDF_PAGEOBJECT textObject);

QString fontFamilyName(FPDF_FONT font);

bool hasSubsetFontPrefix(const QString &baseName);

QString compactFontName(const QString &name);

struct TextObjectFontInfo {
    bool found = false;
    QString baseName;
    QString familyName;
    size_t dataSize = 0;
    int weight = -1;
    bool italic = false;
    int embedded = -1;
};

TextObjectFontInfo textObjectFontInfo(const QString &filePath,
                                      const QString &text);

struct TextCharacterInfo {
    QString text;
    TextObjectFontInfo font;
    QByteArray fontData;
    float pointSize = 0.0F;
    FS_MATRIX matrix{};
    QColor color;
};

QVector<TextCharacterInfo> textCharacterRangeInfo(const QString &filePath,
                                                const QString &text);

bool textObjectsShareFont(const QString &filePath,
                          const QString &firstText,
                          const QString &secondText);

int topLevelPageObjectCount(const QString &filePath, int pageIndex = 0);

QStringList topLevelTextObjects(const QString &filePath);

bool textObjectOrigin(const QString &filePath,
                      const QString &text,
                      QPointF &origin);

bool textObjectMatrix(const QString &filePath,
                      const QString &text,
                      FS_MATRIX &matrix);

qreal horizontalScale(const FS_MATRIX &matrix);

bool strokeTextAppearance(const QString &filePath,
                          const QString &text,
                          QColor &strokeColor,
                          qreal &strokeWidth);

struct RichSaveInspection {
    bool readable = false;
    bool redRun = false;
    bool boldItalicRun = false;
    bool underlinePath = false;
    bool redAndBlueShareBaseline = false;
    bool greenIsNextLine = false;
};

bool createTextUnderlineSourcePdf(const QString &filePath,
                                 const QByteArray &underlineCommands = {});

bool createSavedTextUnderlinePdf(const QString &filePath, bool legacy);

struct TextUnderlineInspection {
    bool readable = false;
    int textIndex = -1;
    int textCount = 0;
    QPointF textOrigin;
    QVector<int> underlineIndexes;
    QVector<QLineF> underlines;
    QVector<QLineF> unrelatedLines;
};

TextUnderlineInspection inspectTextUnderlinePaths(const QString &filePath);

void addTextUnderlineFixtureRows();

RichSaveInspection inspectRichSave(const QString &filePath);

struct HighlightSaveInspection {
    bool readable = false;
    int count = 0;
    QColor color;
    int flags = 0;
    FS_RECTF rect{};
    QVector<FS_QUADPOINTSF> quadPoints;
    QString author;
    QString creationDate;
    QString modificationDate;
    QString contents;
    bool hasAppearance = false;
    QString appearance;
    int appearanceObjectCount = 0;
    int appearancePathCount = 0;
};

struct StickyNoteSaveInspection {
    bool readable = false;
    int count = 0;
    QColor color;
    int flags = 0;
    FS_RECTF rect{};
    QString contents;
    QStringList names;
    QString author;
    QString creationDate;
    QString modificationDate;
};

QString annotationDictionaryString(FPDF_ANNOTATION annotation,
                                   FPDF_BYTESTRING key);

QString annotationAppearanceString(FPDF_ANNOTATION annotation);

struct CommentAnnotationInspection {
    bool readable = false;
    int annotationCount = 0;
    int annotationIndex = -1;
    int subtype = FPDF_ANNOT_UNKNOWN;
    FS_RECTF rect{};
    QString contents;
    QString richContents;
    QString name;
    QString appearanceName;
    QString author;
    QString subject;
    QString creationDate;
    QString modificationDate;
    QString defaultAppearance;
    int flags = 0;
    bool hasAppearance = false;
    QString appearance;
    int popupIndex = -1;
    bool popupParentMatches = false;
    int parentIndex = -1;
    QString parentName;
    QString relation;
    QVector<FS_QUADPOINTSF> quadPoints;
};

struct CommentStateTransitionInspection {
    int annotationIndex = -1;
    int parentIndex = -1;
    QString name;
    QString parentName;
    QString state;
    QString stateModel;
    QString relation;
    QString author;
    QString modificationDate;
};

struct CommentStateInspection {
    bool readable = false;
    int annotationCount = 0;
    QVector<CommentStateTransitionInspection> transitions;
};

struct PdfDictionaryRange {
    qsizetype start = -1;
    qsizetype end = -1;

    bool isValid() const
    {
        return start >= 0 && end > start;
    }
};

qsizetype skipPdfLiteralString(const QByteArray &data, qsizetype offset);

qsizetype skipPdfHexString(const QByteArray &data, qsizetype offset);

qsizetype skipPdfComment(const QByteArray &data, qsizetype offset);

PdfDictionaryRange pdfDictionaryRangeAt(const QByteArray &data,
                                        qsizetype dictionaryStart);

bool isPdfWhitespace(char value);

bool isPdfDelimiter(char value);

qsizetype skipPdfWhitespaceAndComments(const QByteArray &data,
                                       qsizetype offset,
                                       qsizetype end);

qsizetype pdfDirectValueOffset(
    const QByteArray &data,
    const PdfDictionaryRange &parent,
    const QByteArray &key);

PdfDictionaryRange pdfDirectDictionaryValue(
    const QByteArray &data,
    const PdfDictionaryRange &parent,
    const QByteArray &key);

struct PdfIndirectReference {
    int objectNumber = 0;
    int generation = 0;

    bool isValid() const
    {
        return objectNumber > 0 && generation >= 0;
    }
};

PdfIndirectReference pdfDirectIndirectReference(
    const QByteArray &data,
    const PdfDictionaryRange &parent,
    const QByteArray &key);

PdfDictionaryRange pdfDictionaryContainingOffset(const QByteArray &data,
                                                 qsizetype offset);

QRegularExpressionMatch lastRegularExpressionMatch(
    const QRegularExpression &expression,
    const QString &text);

QByteArray annotationDictionaryBytes(const QString &filePath,
                                     const QString &targetName);

PdfDictionaryRange pdfIndirectObjectDictionary(
    const QByteArray &data,
    int objectNumber,
    int generation,
    bool *isStream);

struct AnnotationAppearanceStructureInspection {
    bool readable = false;
    bool annotationFound = false;
    bool normalAppearanceIsIndirect = false;
    int normalAppearanceObjectNumber = -1;
    int normalAppearanceGeneration = -1;
    bool appearanceObjectFound = false;
    bool appearanceIsStream = false;
    bool typeIsXObject = false;
    bool subtypeIsForm = false;
    bool matrixIsIdentity = false;
    QVector<qreal> boundingBox;
    bool hasResources = false;
    bool hasExtGState = false;
    bool hasGraphicsState = false;
    bool graphicsStateTypeIsExtGState = false;
    bool blendModeIsNormal = false;
    bool alphaSourceIsFalse = false;
    qreal strokingOpacity = std::numeric_limits<qreal>::quiet_NaN();
    qreal nonStrokingOpacity = std::numeric_limits<qreal>::quiet_NaN();
};

// FPDFAnnot_GetAP() exposes only decoded drawing commands. Inspect the final
// serialized object graph as well so a named graphics state cannot go unresolved.
AnnotationAppearanceStructureInspection inspectAnnotationAppearanceStructure(
    const QString &filePath,
    const QString &targetName);

CommentAnnotationInspection inspectCommentAnnotation(
    const QString &filePath,
    const QString &targetName,
    int pageIndex = 0);

struct AcroFormFieldInspection {
    bool readable = false;
    int formType = FORMTYPE_NONE;
    int widgetCount = 0;
    int fieldType = FPDF_FORMFIELD_UNKNOWN;
    int fieldFlags = 0;
    QString widgetId;
    QString name;
    QString value;
    QString defaultValue;
    QStringList options;
    QVector<int> selectedIndices;
    FS_RECTF rect{};
    bool hasAppearance = false;
    bool hasMaxLength = false;
    float maxLength = 0.0f;
    QString exportValue;
    bool checked = false;
    int annotationFlags = 0;
    int controlCount = -1;
    int controlIndex = -1;
    bool hasParent = false;
    bool hasLocalFieldType = false;
    bool hasLocalFieldFlags = false;
    bool hasLocalValue = false;
    bool hasAppearanceVersion = false;
    float appearanceVersion = 0.0f;
    QString appearanceStream;
    bool pointHit = false;
    bool focusHandled = false;
};

AcroFormFieldInspection inspectAcroFormField(
    const QString &filePath,
    const QString &targetFieldName = {},
    int pageIndex = 0,
    const QString &targetWidgetId = {});

QVector<bool> clickAcroFormFieldTwice(
    const QString &filePath,
    const QString &targetFieldName,
    int pageIndex = 0);

HighlightSaveInspection inspectTextMarkupAnnotations(
    const QString &filePath,
    FPDF_ANNOTATION_SUBTYPE subtype,
    int pageIndex = 0);

HighlightSaveInspection inspectHighlights(const QString &filePath,
                                          int pageIndex = 0);

StickyNoteSaveInspection inspectStickyNotes(const QString &filePath,
                                            int pageIndex = 0);

CommentStateInspection inspectCommentStateTransitions(
    const QString &filePath,
    int pageIndex = 0);

struct CommentReplyInspection {
    bool readable = false;
    int textAnnotationCount = 0;
    int rootIndex = -1;
    int replyIndex = -1;
    QString rootContents;
    QString replyContents;
    QString relation;
    QString parentThreadId;
    QString replyToThreadId;
};

CommentReplyInspection inspectCommentReply(const QString &filePath,
                                           int pageIndex = 0);

int annotationSubtypeCount(
    const QString &filePath,
    FPDF_ANNOTATION_SUBTYPE subtype,
    int pageIndex = 0);

struct InkAnnotationInspection {
    bool readable = false;
    int count = 0;
    bool annotationFound = false;
    QVector<QVector<QPointF>> paths;
};

InkAnnotationInspection inspectInkAnnotation(
    const QString &filePath,
    const QString &targetName,
    int pageIndex = 0);

bool hasSingleBidirectionalCommentPopup(const QString &filePath,
                                        int pageIndex = 0);

struct ShapeSaveInspection {
    bool readable = false;
    int count = 0;
    int managedCount = 0;
    int filledCount = 0;
    int openCount = 0;
    int bezierCount = 0;
    int blackStrokeCount = 0;
    int transparentFillCount = 0;
    QVector<QColor> strokeColors;
    QVector<QColor> fillColors;
    QVector<QVector<QPointF>> normalizedPathPoints;
};

ShapeSaveInspection inspectShapePaths(const QString &filePath);

bool createUnmarkedShapeContentPdf(const QString &filePath,
                                  const QByteArray &content,
                                  const QByteArray &pageGeometry =
                                      QByteArray("/MediaBox [0 0 612 792]"));

bool tamperMarkedShapePath(const QString &sourcePath,
                          const QString &targetPath,
                          bool changeGeometry);

struct ImageSaveInspection {
    bool readable = false;
    bool pixelsReadable = false;
    bool matrixReadable = false;
    int count = 0;
    int firstTopLevelIndex = -1;
    unsigned int pixelWidth = 0;
    unsigned int pixelHeight = 0;
    QColor topLeftPixel;
    QColor bottomLeftPixel;
    QImage pixels;
    QVector<QPointF> normalizedCorners;
    FS_MATRIX matrix{};
};

ImageSaveInspection inspectImages(const QString &filePath);

class TestableFormFieldLayer final : public FormFieldLayer
{
public:
    using FormFieldLayer::mouseMoveEvent;
    using FormFieldLayer::mousePressEvent;
    using FormFieldLayer::mouseReleaseEvent;
};

bool registerQmlTestTypes(const QString &themePath);

} // namespace PDFEditControllerTestSupport
