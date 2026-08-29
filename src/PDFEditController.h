#pragma once

#include "PDFDocument.h"
#include "PDFCommentAnnotation.h"
#include "PDFSaveWriter.h"

#include <QByteArray>
#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <array>
#include <functional>
#include <memory>

class PDFFormController;
class PDFFormDesignerController;

class PDFEditController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(PDFDocument *document READ document WRITE setDocument NOTIFY documentChanged FINAL)
    Q_PROPERTY(Tool tool READ tool WRITE setTool NOTIFY toolChanged FINAL)
    Q_PROPERTY(ShapeType shapeType READ shapeType WRITE setShapeType NOTIFY shapeTypeChanged FINAL)
    Q_PROPERTY(QColor highlightColor READ highlightColor WRITE setHighlightColor NOTIFY highlightColorChanged FINAL)
    Q_PROPERTY(QColor underlineColor READ underlineColor WRITE setUnderlineColor NOTIFY underlineColorChanged FINAL)
    Q_PROPERTY(QColor squigglyColor READ squigglyColor WRITE setSquigglyColor NOTIFY squigglyColorChanged FINAL)
    Q_PROPERTY(QColor strikeoutColor READ strikeoutColor WRITE setStrikeoutColor NOTIFY strikeoutColorChanged FINAL)
    Q_PROPERTY(QColor drawingCommentColor READ drawingCommentColor WRITE setDrawingCommentColor NOTIFY drawingCommentColorChanged FINAL)
    Q_PROPERTY(QString stampName READ stampName WRITE setStampName NOTIFY stampNameChanged FINAL)
    Q_PROPERTY(QString stampText READ stampText NOTIFY stampNameChanged FINAL)
    Q_PROPERTY(QColor stampColor READ stampColor NOTIFY stampNameChanged FINAL)
    Q_PROPERTY(int pendingDrawingCommentPage READ pendingDrawingCommentPage NOTIFY pendingDrawingCommentPageChanged FINAL)
    Q_PROPERTY(bool hasDrafts READ hasDrafts NOTIFY historyStateChanged FINAL)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyStateChanged FINAL)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyStateChanged FINAL)
    Q_PROPERTY(bool saving READ saving NOTIFY savingChanged FINAL)
    Q_PROPERTY(bool textEditActive READ textEditActive NOTIFY textEditActiveChanged FINAL)
    Q_PROPERTY(QStringList bundledFontFamilies READ bundledFontFamilies CONSTANT FINAL)
    Q_PROPERTY(QStringList availableFontFamilies READ availableFontFamilies NOTIFY textFormatStateChanged FINAL)
    Q_PROPERTY(int selectedPage READ selectedPage NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QString selectedObjectId READ selectedObjectId NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QString selectedKind READ selectedKind NOTIFY selectionChanged FINAL)
    Q_PROPERTY(bool selectedCommentReply READ selectedCommentReply NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QRectF selectedSourceRect READ selectedSourceRect NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QRectF selectedFrameRect READ selectedFrameRect NOTIFY selectedGeometryChanged FINAL)
    Q_PROPERTY(QRectF selectedContentBounds READ selectedContentBounds NOTIFY selectedGeometryChanged FINAL)
    Q_PROPERTY(QVariantList selectedSourceRects READ selectedSourceRects NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QVariantList selectedCharacterSpans READ selectedCharacterSpans NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QVariantList selectedParagraphFormats READ selectedParagraphFormats NOTIFY selectionChanged FINAL)
    Q_PROPERTY(bool selectedHasBaseline READ selectedHasBaseline NOTIFY selectionChanged FINAL)
    Q_PROPERTY(qreal selectedOriginX READ selectedOriginX NOTIFY selectionChanged FINAL)
    Q_PROPERTY(qreal selectedBaselineY READ selectedBaselineY NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QString selectedText READ selectedText NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QString selectedFontFamily READ selectedFontFamily NOTIFY textFormatStateChanged FINAL)
    Q_PROPERTY(qreal selectedFontSizeRatio READ selectedFontSizeRatio NOTIFY textFormatStateChanged FINAL)
    Q_PROPERTY(qreal selectedDisplayFontSizeRatio READ selectedDisplayFontSizeRatio NOTIFY textFormatStateChanged FINAL)
    Q_PROPERTY(int selectedFontStretch READ selectedFontStretch NOTIFY textFormatStateChanged FINAL)
    Q_PROPERTY(bool selectedFontBold READ selectedFontBold NOTIFY textFormatStateChanged FINAL)
    Q_PROPERTY(bool selectedFontItalic READ selectedFontItalic NOTIFY textFormatStateChanged FINAL)
    Q_PROPERTY(bool selectedFontUnderline READ selectedFontUnderline NOTIFY textFormatStateChanged FINAL)
    Q_PROPERTY(QColor selectedColor READ selectedColor NOTIFY textFormatStateChanged FINAL)
    Q_PROPERTY(int selectedParagraphAlignment READ selectedParagraphAlignment NOTIFY textFormatStateChanged FINAL)
    Q_PROPERTY(bool selectedEditable READ selectedEditable NOTIFY selectionChanged FINAL)
    Q_PROPERTY(bool selectedDeletable READ selectedDeletable NOTIFY selectionChanged FINAL)
    Q_PROPERTY(bool selectedDrawingPropertiesEditable READ selectedDrawingPropertiesEditable NOTIFY selectionChanged FINAL)
    Q_PROPERTY(bool selectedDrawingSupportsFill READ selectedDrawingSupportsFill NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QColor selectedDrawingStrokeColor READ selectedDrawingStrokeColor NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QColor selectedDrawingInteriorColor READ selectedDrawingInteriorColor NOTIFY selectionChanged FINAL)
    Q_PROPERTY(bool selectedDrawingHasInterior READ selectedDrawingHasInterior NOTIFY selectionChanged FINAL)
    Q_PROPERTY(qreal selectedDrawingOpacity READ selectedDrawingOpacity NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QString selectedReason READ selectedReason NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QVariantList commentItems READ commentItems NOTIFY commentsChanged FINAL)
    Q_PROPERTY(QVariantList commentPanelItems READ commentPanelItems NOTIFY commentsChanged FINAL)
    Q_PROPERTY(CommentIndexState commentIndexState READ commentIndexState NOTIFY commentIndexStateChanged FINAL)
    Q_PROPERTY(int maximumCommentCharacters READ maximumCommentCharacters CONSTANT FINAL)

public:
    struct DraftObjectMove {
        enum class Kind {
            Text,
            Image
        };

        Kind kind = Kind::Text;
        QVector<int> objectPath;
        QRectF sourceRect;
        QRectF frameRect;
    };

    struct DraftAnnotationDeletion {
        int annotationIndex = -1;
        int annotationSubtype = 0;
    };

    enum CommentIndexState {
        CommentIndexIdle,
        CommentIndexLoading,
        CommentIndexReady,
        CommentIndexFailed,
        CommentIndexLimited
    };
    Q_ENUM(CommentIndexState)

    enum Tool {
        BrowseTool,
        SelectTool,
        EditTextTool,
        AddTextTool,
        AddImageTool,
        AddShapeTool,
        HighlightTool,
        UnderlineTool,
        SquigglyTool,
        StrikeoutTool,
        StickyNoteTool,
        FreeTextTool,
        SquareTool,
        CircleTool,
        LineTool,
        PolygonTool,
        PolyLineTool,
        InkTool,
        StampTool,
        CommentSelectTool,
        // Compatibility aliases for QML/tests written during the initial
        // drawing-comment implementation.
        SquareCommentTool = SquareTool,
        CircleCommentTool = CircleTool,
        LineCommentTool = LineTool,
        PolygonCommentTool = PolygonTool,
        PolyLineCommentTool = PolyLineTool,
        InkCommentTool = InkTool
    };
    Q_ENUM(Tool)

    enum ShapeType {
        RectangleShape,
        EllipseShape,
        LineShape,
        ArrowShape,
        TriangleShape,
        DiamondShape
    };
    Q_ENUM(ShapeType)

    enum FontFilter {
        VietnameseFonts,
        EnglishFonts,
        ChineseFonts,
        JapaneseFonts,
        KoreanFonts
    };
    Q_ENUM(FontFilter)

    enum ParagraphAlignment {
        LeftAlignment,
        RightAlignment,
        CenterAlignment,
        JustifyAlignment
    };
    Q_ENUM(ParagraphAlignment)

    explicit PDFEditController(QObject *parent = nullptr);
    ~PDFEditController() override;

    PDFDocument *document() const;
    void setDocument(PDFDocument *document);
    PDFFormController *formController() const;
    void setFormController(PDFFormController *formController);
    PDFFormDesignerController *formDesignerController() const;
    void setFormDesignerController(
        PDFFormDesignerController *formDesignerController);

    Tool tool() const;
    void setTool(Tool tool);
    ShapeType shapeType() const;
    void setShapeType(ShapeType shapeType);
    QColor highlightColor() const;
    void setHighlightColor(const QColor &color);
    QColor underlineColor() const;
    void setUnderlineColor(const QColor &color);
    QColor squigglyColor() const;
    void setSquigglyColor(const QColor &color);
    QColor strikeoutColor() const;
    void setStrikeoutColor(const QColor &color);
    QColor drawingCommentColor() const;
    void setDrawingCommentColor(const QColor &color);
    QString stampName() const;
    void setStampName(const QString &name);
    QString stampText() const;
    QColor stampColor() const;

    bool hasDrafts() const;
    bool canUndo() const;
    bool canRedo() const;
    bool saving() const;
    bool textEditActive() const;
    QStringList bundledFontFamilies() const;
    QStringList availableFontFamilies() const;

    int selectedPage() const;
    QString selectedObjectId() const;
    QString selectedKind() const;
    bool selectedCommentReply() const;
    QRectF selectedSourceRect() const;
    QRectF selectedFrameRect() const;
    QRectF selectedContentBounds() const;
    QVariantList selectedSourceRects() const;
    QVariantList selectedCharacterSpans() const;
    QVariantList selectedParagraphFormats() const;
    bool selectedHasBaseline() const;
    qreal selectedOriginX() const;
    qreal selectedBaselineY() const;
    QString selectedText() const;
    QString selectedFontFamily() const;
    qreal selectedFontSizeRatio() const;
    qreal selectedDisplayFontSizeRatio() const;
    int selectedFontStretch() const;
    bool selectedFontBold() const;
    bool selectedFontItalic() const;
    bool selectedFontUnderline() const;
    QColor selectedColor() const;
    int selectedParagraphAlignment() const;
    bool selectedEditable() const;
    bool selectedDeletable() const;
    bool selectedDrawingPropertiesEditable() const;
    bool selectedDrawingSupportsFill() const;
    QColor selectedDrawingStrokeColor() const;
    QColor selectedDrawingInteriorColor() const;
    bool selectedDrawingHasInterior() const;
    qreal selectedDrawingOpacity() const;
    QString selectedReason() const;
    QVariantList commentItems() const;
    QVariantList commentPanelItems() const;
    CommentIndexState commentIndexState() const;
    int maximumCommentCharacters() const;
    int pendingDrawingCommentPage() const;
    quint64 draftRevisionForPage(int page) const;
    QVector<QVector<int>> draftedTextObjectPathsForPage(int page) const;
    QVector<DraftObjectMove> draftedObjectMovesForPage(int page) const;
    QVector<DraftAnnotationDeletion> deletedSourceCommentAnnotationsForPage(
        int page) const;
    bool allDraftedTextObjectsSuppressibleForPage(int page) const;

    Q_INVOKABLE QVariantList candidatesForPage(int page);
    Q_INVOKABLE void requestCandidatesForPage(int page);
    Q_INVOKABLE QVariantList draftsForPage(int page) const;
    Q_INVOKABLE QVariantMap selectedDraftPreview() const;
    Q_INVOKABLE QVariantMap candidateAt(int page, qreal x, qreal y);
    Q_INVOKABLE QVariantMap addText(int page, qreal x, qreal y);
    Q_INVOKABLE void requestAddImage(int page, qreal x, qreal y);
    Q_INVOKABLE QVariantMap addImage(int page,
                                     qreal x,
                                     qreal y,
                                     const QUrl &source);
    Q_INVOKABLE QVariantMap addShape(int page,
                                     const QPointF &startPoint,
                                     const QPointF &endPoint);
    Q_INVOKABLE QPointF constrainedShapeEnd(int page,
                                            const QPointF &startPoint,
                                            const QPointF &endPoint) const;
    Q_INVOKABLE bool shapeDragIsValid(int page,
                                      const QPointF &startPoint,
                                      const QPointF &endPoint) const;
    Q_INVOKABLE qreal shapeHitToleranceRatio(
        int page,
        qreal strokeWidthRatio) const;
    Q_INVOKABLE QVariantList textMarkupRectsForSelection(
        int page,
        const QRectF &selectionRect);
    Q_INVOKABLE QVariantMap addHighlight(int page,
                                         const QRectF &selectionRect);
    Q_INVOKABLE QVariantMap addStickyNote(int page,
                                         const QPointF &position);
    Q_INVOKABLE QVariantMap addFreeText(int page,
                                       const QRectF &selectionRect);
    Q_INVOKABLE QVariantMap addStamp(int page,
                                    const QRectF &selectionRect);
    Q_INVOKABLE QVariantMap addDrawingComment(
        int page,
        const QVariantList &vertices);
    Q_INVOKABLE bool claimDrawingCommentInteraction(int page);
    Q_INVOKABLE void releaseDrawingCommentInteraction(int page);
    Q_INVOKABLE QVariantMap addCommentReply(int page,
                                            const QString &parentObjectId,
                                            const QString &text);
    Q_INVOKABLE int addBlankPage();
    Q_INVOKABLE int deleteCurrentPage();
    Q_INVOKABLE QFont fontForDisplay(const QString &fontFamily,
                                     int fontStretch,
                                     bool fontBold,
                                     bool fontItalic,
                                     bool fontUnderline,
                                     qreal pixelSize) const;
    Q_INVOKABLE int displayFontStretch(const QString &fontFamily,
                                       int fontStretch,
                                       bool fontBold,
                                       bool fontItalic,
                                       qreal pixelSize) const;
    Q_INVOKABLE void applyTypingFormatToEditorDocument(QObject *textDocument,
                                                       int start,
                                                       int length) const;
    Q_INVOKABLE bool applyFontStyleToEditorDocument(QObject *textDocument,
                                                    int start,
                                                    int length,
                                                    int style,
                                                    bool enabled) const;
    Q_INVOKABLE bool syncEditorFontStyles(QObject *textDocument);
    Q_INVOKABLE QStringList fontFamiliesForFilter(FontFilter filter) const;
    Q_INVOKABLE bool selectCandidate(int page, const QString &objectId);
    Q_INVOKABLE void requestCommentIndex();
    Q_INVOKABLE bool selectComment(int page, const QString &objectId);
    Q_INVOKABLE bool removeComment(int page, const QString &objectId);
    Q_INVOKABLE int selectRelativeComment(int direction);
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE void beginSelectedTextEdit();
    Q_INVOKABLE void updateSelectedTextPreview(const QString &text,
                                               int cursorPosition = -1);
    Q_INVOKABLE void beginSelectedFrameResize();
    Q_INVOKABLE void updateSelectedFrameRect(const QRectF &frameRect);
    Q_INVOKABLE void updateSelectedContentBounds(const QRectF &contentBounds);
    Q_INVOKABLE void finishSelectedFrameResize();
    Q_INVOKABLE void cancelSelectedFrameResize();
    Q_INVOKABLE void setSelectedTextRange(int start, int length);
    Q_INVOKABLE void setSelectedFontFamily(const QString &fontFamily);
    Q_INVOKABLE void setSelectedFontBold(bool enabled);
    Q_INVOKABLE void setSelectedFontItalic(bool enabled);
    Q_INVOKABLE void setSelectedFontUnderline(bool enabled);
    Q_INVOKABLE void setSelectedColor(const QColor &color);
    Q_INVOKABLE void setSelectedParagraphAlignment(ParagraphAlignment alignment);
    Q_INVOKABLE void finishSelectedTextEdit();
    Q_INVOKABLE void cancelSelectedTextEdit();
    Q_INVOKABLE void commitSelectedText(const QString &text);
    Q_INVOKABLE bool setCommentText(int page,
                                    const QString &objectId,
                                    const QString &text);
    Q_INVOKABLE void setSelectedCommentText(const QString &text);
    Q_INVOKABLE bool setSelectedDrawingStrokeColor(const QColor &color);
    Q_INVOKABLE bool setSelectedDrawingInteriorColor(const QColor &color);
    Q_INVOKABLE bool clearSelectedDrawingInteriorColor();
    Q_INVOKABLE bool setSelectedDrawingOpacity(qreal opacity);
    Q_INVOKABLE void deleteSelectedObject();
    Q_INVOKABLE int undo();
    Q_INVOKABLE int redo();
    Q_INVOKABLE void discardDrafts();
    Q_INVOKABLE bool save();
    Q_INVOKABLE bool saveAs(const QUrl &target);
    Q_INVOKABLE bool requestSave();
    Q_INVOKABLE bool requestSaveAs(const QUrl &target);

signals:
    void documentChanged();
    void toolChanged();
    void shapeTypeChanged();
    void highlightColorChanged();
    void underlineColorChanged();
    void squigglyColorChanged();
    void strikeoutColorChanged();
    void drawingCommentColorChanged();
    void stampNameChanged();
    void pendingDrawingCommentPageChanged();
    void historyStateChanged();
    void savingChanged();
    void textEditActiveChanged();
    void selectionChanged();
    void selectedGeometryChanged();
    void textFormatStateChanged();
    void commentsChanged();
    void commentIndexStateChanged();
    void typingFormatApplied(int start, int length);
    void selectionFontStyleApplied(int start,
                                   int length,
                                   int style,
                                   bool enabled);
    void pageDataChanged(int page);
    void pageCandidatesReady(int page);
    void draftRenderStateChanged(int page, quint64 revision, bool immediate);
    void statusMessage(const QString &message);
    void imageFileRequested(int page, qreal x, qreal y);
    void saveFinished(bool succeeded);

private:
    friend class PDFSaveWriter;

    struct TextObjectLayout {
        QRectF bounds;
        QVector<QRectF> glyphBounds;
        QPointF origin;
        QString text;
        bool hasOrigin = false;
    };

    struct CandidatePart {
        QString objectId;
        QVector<int> objectPath;
        QRectF sourceRect;
        QPointF origin;
        QString text;
        QString separatorAfter;
        bool hasBaseline = false;
    };

    struct CandidateGeometry {
        QRectF sourceRect;
    };

    struct Candidate {
        int page = -1;
        QString objectId;
        int sourceAnnotationIndex = -1;
        QString sourceWidgetId;
        QVector<int> objectPath;
        QVector<CandidatePart> parts;
        QString kind;
        QString label;
        CandidateGeometry geometry;
        QRectF initialDraftFrameRect;
        QPointF origin;
        QString text;
        QVector<QRectF> textGlyphRects;
        bool textMarkupHitTestOnly = false;
        QString sourceAnnotationName;
        bool sourceAnnotationHasName = false;
        QString commentAuthor;
        QDateTime commentCreationDate;
        QDateTime commentModifiedDate;
        QString commentThreadId;
        QString replyToObjectId;
        QString replyToThreadId;
        int replyToSourceAnnotationIndex = -1;
        int replyDirectParentSourceAnnotationIndex = -1;
        bool isReplyAnnotation = false;
        bool commentStateAnnotation = false;
        bool commentTextReadOnly = false;
        bool commentThreadDeletionProtected = false;
        bool replyToParentAdded = false;
        QString fontFamily;
        QString fontIdentity;
        qreal fontSizeRatio = 0.0;
        qreal displayFontSizeRatio = 0.0;
        qreal horizontalScale = 1.0;
        int fontStretch = 100;
        bool fontBold = false;
        bool fontItalic = false;
        bool fontUnderline = false;
        QColor color = Qt::black;
        QColor fillColor = Qt::black;
        QColor strokeColor = Qt::black;
        qreal strokeWidth = 1.0;
        FPDF_TEXT_RENDERMODE textRenderMode = FPDF_TEXTRENDERMODE_FILL;
        bool hasBaseline = false;
        bool editable = false;
        bool renderSuppressible = false;
        bool limited = false;
        bool added = false;
        quint64 addedOrder = 0;
        QImage image;
        ShapeType shapeType = RectangleShape;
        QPointF shapeStartRatio;
        QPointF shapeEndRatio = QPointF(1.0, 1.0);
        qreal shapeStrokeWidthRatio = 0.0;
        QVector<QRectF> highlightRects;
        QVector<std::array<QPointF, 4>> textMarkupQuads;
        QColor highlightColor;
        PDFCommentAnnotation::DrawingType drawingType
            = PDFCommentAnnotation::DrawingType::Square;
        QVector<QPointF> drawingVertices;
        QVector<QVector<QPointF>> inkStrokes;
        PDFCommentAnnotation::CommonProperties commentProperties;
        QByteArray sourceDrawingAppearanceFingerprint;
        bool drawingPropertiesEditable = false;
        QString stampText;
        QString reason;
        int pendingFontSnapshotIndex = -1;
        QString pendingFontExpectedFamily;
        QString pendingBundledFontFamily;
        QString pendingSystemFontFamily;
        QByteArray pendingEmbeddedFontData;
    };

    struct CandidateScanRequest {
        QString filePath;
        int revision = -1;
        int page = -1;
        int sourcePage = -1;
        QSizeF pageSize;
        bool editingAllowed = false;
        bool annotationsAllowed = false;
        QStringList bundledFontFamilies;
        QStringList systemFontFamilies;
        qsizetype maximumEmbeddedFontSnapshotBytes = 0;
        qsizetype maximumCommentIndexBytes = -1;
    };

    struct CommentPageScanResult {
        QVector<Candidate> candidates;
        qsizetype estimatedRetainedBytes = 0;
        bool memoryBudgetExceeded = false;
        bool succeeded = false;
    };

    struct PendingSquigglyCreation {
        QString filePath;
        QRectF selectionRect;
        QSizeF pageSize;
        QColor color;
        int revision = -1;
        int page = -1;
        int sourcePage = -1;
    };

    struct FontResolutionSnapshot {
        QString expectedFamily;
        QString bundledFamily;
        QString systemFamily;
        QString identity;
        QByteArray embeddedFontData;
        int scanIndex = -1;
        bool bold = false;
        bool italic = false;
    };

    struct CharacterFormat {
        QString fontFamily;
        qreal fontSizeRatio = 0.0;
        qreal displayFontSizeRatio = 0.0;
        int fontStretch = 100;
        bool fontBold = false;
        bool fontItalic = false;
        bool fontUnderline = false;
        QColor color = Qt::black;
    };

    struct CharacterSpan {
        qsizetype start = 0;
        qsizetype length = 0;
        CharacterFormat format;
    };

    struct TextChange {
        qsizetype start = 0;
        qsizetype removedLength = 0;
        qsizetype insertedLength = 0;
    };

    enum class ParagraphListStyle {
        None,
        Bullet,
        Decimal
    };

    struct ParagraphFormat {
        qsizetype start = 0;
        qsizetype length = 0;
        ParagraphAlignment alignment = LeftAlignment;
        ParagraphListStyle listStyle = ParagraphListStyle::None;
        int listLevel = 0;
        int listStart = 1;
    };

    struct Draft {
        Candidate candidate;
        PDFCommentAnnotation::CommonProperties commentProperties;
        QString text;
        QVector<CharacterSpan> characterSpans;
        QVector<ParagraphFormat> paragraphFormats;
        QRectF frameRect;
        QRectF contentBounds;
        bool deleted = false;
    };

    struct PageState {
        QSet<int> deletedSourcePages;
        QVector<PDFPageInsertion> pageInsertions;
        QVector<Candidate> addedCandidates;
        QHash<QString, Draft> drafts;
        int currentPage = 0;
        bool hasSelection = false;
        Candidate selectedCandidate;
    };

    struct Command {
        enum class Kind {
            ObjectDraft,
            PageStructure
        };

        Kind kind = Kind::ObjectDraft;
        QString key;
        int page = -1;
        QString objectId;
        bool hadBefore = false;
        Draft before;
        bool hadAfter = false;
        Draft after;
        PageState pageStateBefore;
        PageState pageStateAfter;
    };

    enum class FontStyle {
        Bold,
        Italic,
        Underline
    };

    void resetSession();
    void setPendingDrawingCommentPage(int page);
    bool completeSave(const PDFSaveWriter::Result &result,
                      const QString &targetFilePath,
                      PDFDocument *expectedDocument,
                      int expectedRevision,
                      const QString &expectedSourceFilePath,
                      const QString &temporarySourceFilePath = {});
    void startAsyncSave(
        const QString &targetFilePath,
        PDFDocument *expectedDocument,
        int expectedRevision,
        const QString &expectedSourceFilePath,
        const QString &sourceFilePathOverride = {},
        const QString &temporarySourceFilePath = {});
    void ensureCacheRevision();
    QVector<QRectF> resolvedTextMarkupRectsForSelection(
        int page,
        const QRectF &selectionRect,
        bool *geometryReady);
    QVariantMap addTextMarkupDraft(
        int page,
        const QVector<QRectF> &annotationRects,
        Tool tool,
        const QColor &color);
    QVariantMap materializeTextMarkupDraft(
        int page,
        const QVector<QRectF> &annotationRects,
        Tool tool,
        const QColor &color);
    bool pendingSquigglyCreationIsCurrent(
        const PendingSquigglyCreation &creation) const;
    int pendingSquigglyScanPage() const;
    void completePendingSquigglyCreations(int page);
    const QVector<Candidate> &ensurePageCandidates(int page);
    const QVector<Candidate> *cachedPageCandidates(int page);
    void requestPageCandidateScan(int page);
    void startPendingCandidateScan();
    void completePageCandidateScan(
        const CandidateScanRequest &request,
        QVector<Candidate> candidates,
        bool cancelled);
    void completeDroppedPageCandidateScan(
        const std::shared_ptr<std::atomic_bool> &cancellation);
    void touchPageCandidateCache(int page);
    void prunePageCandidateCache(int protectedPage);
    bool isPageCandidateCachePinned(int page) const;
    void resetCommentIndex();
    void setCommentIndexState(CommentIndexState state);
    bool cacheCommentCandidates(
        int page,
        const QVector<Candidate> &candidates);
    static qsizetype estimatedCommentCandidateBytes(
        const Candidate &candidate);
    static qsizetype estimatedCommentCandidatePageBytes(
        const QVector<Candidate> &candidates);
    CandidateScanRequest candidateScanRequest(int page) const;
    static QVector<Candidate> scanPageFromFile(
        const CandidateScanRequest &request,
        const std::shared_ptr<std::atomic_bool> &cancellation);
    static CommentPageScanResult scanCommentPageLocked(
        FPDF_DOCUMENT document,
        const CandidateScanRequest &request,
        const std::shared_ptr<std::atomic_bool> &cancellation);
    static QVector<Candidate> scanPageLocked(
        FPDF_DOCUMENT document,
        const CandidateScanRequest &request,
        const std::shared_ptr<std::atomic_bool> &cancellation = {});
    static void applyCommentThreadDeletionProtection(
        QVector<Candidate> &candidates);
    QVector<Candidate> finalizePageCandidates(
        QVector<Candidate> candidates,
        int page,
        const QSizeF &pageSize);
    static void appendPageObjectCandidates(
        FPDF_PAGE page,
        FPDF_PAGEOBJECT pageObject,
        const QVector<int> &objectPath,
        FPDF_TEXTPAGE textPage,
        const QHash<FPDF_PAGEOBJECT, TextObjectLayout> &textObjectLayouts,
        const FS_MATRIX &containerTransform,
        const CandidateScanRequest &request,
        QVector<Candidate> &candidates,
        QHash<FPDF_FONT, FontResolutionSnapshot> &fontResolutionCache,
        qsizetype &embeddedFontDataBytes,
        const std::shared_ptr<std::atomic_bool> &cancellation);
    QVector<Candidate> groupTextLineCandidates(QVector<Candidate> candidates,
                                               const QSizeF &pageSize) const;
    static bool appendAnnotationCandidates(
        FPDF_PAGE page,
        const CandidateScanRequest &request,
        QVector<Candidate> &candidates,
        const std::shared_ptr<std::atomic_bool> &cancellation,
        qsizetype &estimatedCommentBytes,
        bool &commentMemoryBudgetExceeded);
    const Candidate *findCandidate(int page, const QString &objectId);
    const Candidate *findCommentCandidate(int page,
                                          const QString &objectId);
    bool candidateDeletable(const Candidate &candidate) const;
    bool drawingPropertiesEditable(const Candidate &candidate) const;
    static bool drawingSupportsFill(const QString &kind);
    QVariantList candidateSourceRects(const Candidate &candidate) const;
    QVariantList characterSpanMaps(const QVector<CharacterSpan> &spans) const;
    QVariantList paragraphFormatMaps(const QVector<ParagraphFormat> &formats) const;
    QVariantMap candidateMap(const Draft &draft) const;
    QVariantList buildCommentItems(bool includeCandidateDetails) const;
    bool documentAllowsEditing() const;
    bool documentAllowsAnnotations() const;
    bool documentAllowsPageAssembly() const;
    QString draftKey(int page, const QString &objectId) const;
    bool commentReplyIsVisible(const Candidate &candidate) const;
    void markDraftStateChanged(int page,
                               bool immediate,
                               bool notifyComments = true);
    void requestDraftRender(int page, bool immediate);
    bool removeCommentCandidate(const Candidate &candidate);
    static void setInitialGeometry(Candidate &candidate, const QRectF &sourceRect);
    void expandInitialTextFrame(Candidate &candidate, const QSizeF &pageSize) const;
    static CharacterFormat characterFormatForCandidate(const Candidate &candidate);
    static CharacterFormat primaryCharacterFormat(const Draft &draft);
    static CharacterFormat characterFormatAtPosition(const Draft &draft,
                                                      qsizetype position,
                                                      bool preferPrevious);
    CharacterFormat selectedCharacterFormatForDisplay() const;
    static QVector<ParagraphFormat> paragraphFormatsForText(
        const QString &text,
        const ParagraphFormat &prototype);
    static Draft initialDraftForCandidate(const Candidate &candidate);
    static QVector<QRectF> highlightRectsForDraft(const Draft &draft);
    static QVector<std::array<QPointF, 4>> textMarkupQuadsForDraft(
        const Draft &draft);
    static QVector<QPointF> drawingVerticesForDraft(const Draft &draft);
    static QVector<QVector<QPointF>> inkStrokesForDraft(const Draft &draft);
    static QPointF layoutOriginForDraft(const Draft &draft);
    static bool sameCharacterFormat(const CharacterFormat &left,
                                    const CharacterFormat &right);
    static bool sameCharacterSpans(const QVector<CharacterSpan> &left,
                                   const QVector<CharacterSpan> &right);
    static bool sameParagraphFormats(const QVector<ParagraphFormat> &left,
                                     const QVector<ParagraphFormat> &right);
    static bool sameDraftContent(const Draft &left, const Draft &right);
    static bool isSourceTextMoveOnly(const Draft &draft);
    static TextChange setDraftText(
        Draft &draft,
        const QString &text,
        const CharacterFormat *typingFormat = nullptr,
        qsizetype typingPosition = -1,
        qsizetype cursorAfterEdit = -1);
    static void applyUnicodeFontFallback(Draft &draft);
    static bool updateCharacterFormats(
        Draft &draft,
        qsizetype start,
        qsizetype length,
        const std::function<void(CharacterFormat &)> &update);
    void beginSelectedTextEditSession(bool expandFrame);
    QRectF expandedTextEditFrame(const Draft &draft);
    Draft selectedDraftForDisplay() const;
    void updateSelectedDraft(const Draft &draft);
    bool updateSelectedDrawingProperties(
        const std::function<void(
            PDFCommentAnnotation::CommonProperties &)> &update);
    void selectedTextRangeForDraft(const Draft &draft,
                                   qsizetype &start,
                                   qsizetype &length) const;
    void selectedParagraphRangeForDraft(const Draft &draft,
                                        qsizetype &first,
                                        qsizetype &last) const;
    void resetTextEditDisplayState();
    void resetTypingFormat();
    void rebaseTypingFormat(const Draft &draft);
    void resetFrameResizeState();
    void setSelectedFontStyle(FontStyle style, bool enabled);
    void applyDraftState(const Command &command, bool useAfterState, bool immediate);
    PageState capturePageState() const;
    void applyPageState(const PageState &state);
    bool removeAddedCandidate(int page, const QString &objectId);
    void updateSelectedTextFromDraft();
    void setSelection(const Candidate *candidate);
    static void snapshotFontResolutionLocked(
        FPDF_PAGEOBJECT textObject,
        Candidate &candidate,
        const CandidateScanRequest &request,
        QHash<FPDF_FONT, FontResolutionSnapshot> &fontResolutionCache,
        qsizetype &embeddedFontDataBytes);
    void resolveCandidateFontFamily(
        Candidate &candidate,
        QHash<int, QString> &resolvedEmbeddedFamilies);
    QString registerEmbeddedFont(
        const QByteArray &fontData,
        const QString &expectedFamily);
    void rememberRejectedFontFingerprint(const QByteArray &fingerprint);
    void clearRegisteredFonts();
    bool isDesignerManagedSourceWidget(const Candidate &candidate) const;
    void notifyManagedWidgetVisibilityChanged(int page);

    QPointer<PDFDocument> m_document;
    QPointer<PDFFormController> m_formController;
    QPointer<PDFFormDesignerController> m_formDesignerController;
    Tool m_tool = BrowseTool;
    ShapeType m_shapeType = RectangleShape;
    QColor m_highlightColor;
    QColor m_underlineColor;
    QColor m_squigglyColor;
    QColor m_strikeoutColor;
    QColor m_drawingCommentColor;
    QString m_stampName = QStringLiteral("Approved");
    int m_pendingDrawingCommentPage = -1;
    int m_cachedRevision = -1;
    QHash<int, QVector<Candidate>> m_pageCandidates;
    QVector<int> m_pageCandidateLru;
    int m_deferredCandidateScanPage = -1;
    int m_candidateScanInFlightPage = -1;
    bool m_candidateScanInFlight = false;
    std::shared_ptr<std::atomic_bool> m_candidateScanCancellation;
    QVector<PendingSquigglyCreation> m_pendingSquigglyCreations;
    QHash<int, QVector<Candidate>> m_indexedCommentCandidates;
    qsizetype m_indexedCommentCandidateBytes = 0;
    CommentIndexState m_commentIndexState = CommentIndexIdle;
    std::shared_ptr<std::atomic_bool> m_commentIndexCancellation;
    QVector<Candidate> m_addedCandidates;
    QVector<PDFPageInsertion> m_pageInsertions;
    QSet<int> m_deletedSourcePages;
    QHash<QString, Draft> m_drafts;
    QHash<int, quint64> m_pageDraftRevisions;
    quint64 m_nextDraftRevision = 0;
    quint64 m_nextAddedObjectId = 0;
    QVector<Command> m_undoStack;
    QVector<Command> m_redoStack;
    QHash<QByteArray, QString> m_registeredFontFamilies;
    QSet<QByteArray> m_rejectedFontFingerprints;
    QVector<QByteArray> m_rejectedFontFingerprintLru;
    QVector<int> m_registeredFontIds;
    qsizetype m_registeredFontBytes = 0;
    Candidate m_selectedCandidate;
    bool m_hasSelection = false;
    bool m_saving = false;
    QString m_selectedText;
    bool m_textEditActive = false;
    qsizetype m_selectedTextRangeStart = 0;
    qsizetype m_selectedTextRangeLength = 0;
    CharacterFormat m_typingCharacterFormat;
    qsizetype m_typingCharacterFormatPosition = -1;
    bool m_typingCharacterFormatActive = false;
    Command m_textEditCommand;
    Draft m_textEditDisplayDraft;
    bool m_textEditDisplayDraftActive = false;
    bool m_textEditFrameAutomaticallyExpanded = false;
    bool m_textEditGeometryExplicitlyChanged = false;
    QRectF m_textEditInitialDisplayFrameRect;
    bool m_frameResizeActive = false;
    bool m_frameResizeOwnsTextEdit = false;
    bool m_frameResizeStartGeometryExplicitlyChanged = false;
    QRectF m_frameResizeStartFrameRect;
    QRectF m_frameResizeStartContentBounds;
};
