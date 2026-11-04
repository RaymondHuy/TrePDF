#pragma once

#include "PDFDocument.h"
#include "PDFCommentAnnotation.h"
#include "PDFSaveWriter.h"
#include "PDFWebLinkScanner.h"
#include "PDFWatermark.h"

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
class FontDownloadService;

class PDFEditController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(PDFDocument *document READ document WRITE setDocument NOTIFY documentChanged FINAL)
    Q_PROPERTY(Tool tool READ tool WRITE setTool NOTIFY toolChanged FINAL)
    Q_PROPERTY(bool footerMode READ footerMode WRITE setFooterMode NOTIFY footerModeChanged FINAL)
    Q_PROPERTY(bool headerSelectionOnly READ headerSelectionOnly WRITE setHeaderSelectionOnly NOTIFY headerSelectionOnlyChanged FINAL)
    Q_PROPERTY(bool headerSyncAvailable READ headerSyncAvailable NOTIFY headerSyncChanged FINAL)
    Q_PROPERTY(bool headerSyncLoading READ headerSyncLoading NOTIFY headerSyncChanged FINAL)
    Q_PROPERTY(int headerSyncMatchCount READ headerSyncMatchCount NOTIFY headerSyncChanged FINAL)
    Q_PROPERTY(QString headerSyncMatchPages READ headerSyncMatchPages NOTIFY headerSyncChanged FINAL)
    Q_PROPERTY(QString headerSyncKey READ headerSyncKey NOTIFY headerSyncChanged FINAL)
    Q_PROPERTY(bool headerSyncCanApply READ headerSyncCanApply NOTIFY headerSyncChanged FINAL)
    Q_PROPERTY(QString headerSyncError READ headerSyncError NOTIFY headerSyncChanged FINAL)
    Q_PROPERTY(ShapeType shapeType READ shapeType WRITE setShapeType NOTIFY shapeTypeChanged FINAL)
    Q_PROPERTY(int tableRows READ tableRows WRITE setTableRows NOTIFY tableDimensionsChanged FINAL)
    Q_PROPERTY(int tableColumns READ tableColumns WRITE setTableColumns NOTIFY tableDimensionsChanged FINAL)
    Q_PROPERTY(bool imageAspectRatioLocked READ imageAspectRatioLocked WRITE setImageAspectRatioLocked NOTIFY imageAspectRatioLockedChanged FINAL)
    Q_PROPERTY(QColor highlightColor READ highlightColor WRITE setHighlightColor NOTIFY highlightColorChanged FINAL)
    Q_PROPERTY(QColor underlineColor READ underlineColor WRITE setUnderlineColor NOTIFY underlineColorChanged FINAL)
    Q_PROPERTY(QColor squigglyColor READ squigglyColor WRITE setSquigglyColor NOTIFY squigglyColorChanged FINAL)
    Q_PROPERTY(QColor strikeoutColor READ strikeoutColor WRITE setStrikeoutColor NOTIFY strikeoutColorChanged FINAL)
    Q_PROPERTY(QColor drawingCommentColor READ drawingCommentColor WRITE setDrawingCommentColor NOTIFY drawingCommentColorChanged FINAL)
    Q_PROPERTY(QString stampName READ stampName WRITE setStampName NOTIFY stampNameChanged FINAL)
    Q_PROPERTY(QString stampText READ stampText NOTIFY stampNameChanged FINAL)
    Q_PROPERTY(QColor stampColor READ stampColor NOTIFY stampNameChanged FINAL)
    Q_PROPERTY(QString stampArtwork READ stampArtwork NOTIFY stampNameChanged FINAL)
    Q_PROPERTY(QVariantList stampPresets READ stampPresets CONSTANT FINAL)
    Q_PROPERTY(int pendingDrawingCommentPage READ pendingDrawingCommentPage NOTIFY pendingDrawingCommentPageChanged FINAL)
    Q_PROPERTY(bool hasDrafts READ hasDrafts NOTIFY historyStateChanged FINAL)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyStateChanged FINAL)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyStateChanged FINAL)
    Q_PROPERTY(bool saving READ saving NOTIFY savingChanged FINAL)
    Q_PROPERTY(bool canSetOpenPassword READ canSetOpenPassword NOTIFY passwordProtectionChanged FINAL)
    Q_PROPERTY(bool passwordChangePending READ passwordChangePending NOTIFY passwordProtectionChanged FINAL)
    Q_PROPERTY(bool watermarksReady READ watermarksReady NOTIFY watermarksChanged FINAL)
    Q_PROPERTY(bool loadingWatermarks READ loadingWatermarks NOTIFY watermarksChanged FINAL)
    Q_PROPERTY(bool watermarkChangesPending READ watermarkChangesPending NOTIFY watermarksChanged FINAL)
    Q_PROPERTY(QVariantList watermarks READ watermarks NOTIFY watermarksChanged FINAL)
    Q_PROPERTY(bool creatingWebLinks READ creatingWebLinks NOTIFY creatingWebLinksChanged FINAL)
    Q_PROPERTY(bool removingWebLinks READ removingWebLinks NOTIFY creatingWebLinksChanged FINAL)
    Q_PROPERTY(bool loadingEditableLinks READ loadingEditableLinks NOTIFY editableLinksChanged FINAL)
    Q_PROPERTY(quint64 webLinksRevision READ webLinksRevision NOTIFY webLinksChanged FINAL)
    Q_PROPERTY(bool fontDownloadPending READ fontDownloadPending NOTIFY fontDownloadStateChanged FINAL)
    Q_PROPERTY(bool fontDownloadForEdit READ fontDownloadForEdit NOTIFY fontDownloadStateChanged FINAL)
    Q_PROPERTY(bool downloadingFont READ downloadingFont NOTIFY fontDownloadStateChanged FINAL)
    Q_PROPERTY(QString missingFontFamily READ missingFontFamily NOTIFY fontDownloadStateChanged FINAL)
    Q_PROPERTY(QStringList missingFontFamilies READ missingFontFamilies NOTIFY fontDownloadStateChanged FINAL)
    Q_PROPERTY(QString textEditFallbackFamily READ textEditFallbackFamily NOTIFY fontDownloadStateChanged FINAL)
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
    Q_PROPERTY(qreal selectedLetterSpacing READ selectedLetterSpacing NOTIFY textFormatStateChanged FINAL)
    Q_PROPERTY(qreal selectedWordSpacing READ selectedWordSpacing NOTIFY textFormatStateChanged FINAL)
    Q_PROPERTY(qreal selectedFontSize READ selectedFontSize NOTIFY textFormatStateChanged FINAL)
    Q_PROPERTY(qreal selectedFontSizeRatio READ selectedFontSizeRatio NOTIFY textFormatStateChanged FINAL)
    Q_PROPERTY(qreal selectedDisplayFontSizeRatio READ selectedDisplayFontSizeRatio NOTIFY textFormatStateChanged FINAL)
    Q_PROPERTY(int selectedFontStretch READ selectedFontStretch NOTIFY textFormatStateChanged FINAL)
    Q_PROPERTY(bool selectedFontBold READ selectedFontBold NOTIFY textFormatStateChanged FINAL)
    Q_PROPERTY(bool selectedFontItalic READ selectedFontItalic NOTIFY textFormatStateChanged FINAL)
    Q_PROPERTY(bool selectedFontUnderline READ selectedFontUnderline NOTIFY textFormatStateChanged FINAL)
    Q_PROPERTY(QColor selectedColor READ selectedColor NOTIFY textFormatStateChanged FINAL)
    Q_PROPERTY(int selectedParagraphAlignment READ selectedParagraphAlignment NOTIFY textFormatStateChanged FINAL)
    Q_PROPERTY(qreal selectedLineSpacing READ selectedLineSpacing NOTIFY textFormatStateChanged FINAL)
    Q_PROPERTY(qreal selectedParagraphSpaceBefore READ selectedParagraphSpaceBefore NOTIFY textFormatStateChanged FINAL)
    Q_PROPERTY(qreal selectedParagraphSpaceAfter READ selectedParagraphSpaceAfter NOTIFY textFormatStateChanged FINAL)
    Q_PROPERTY(bool selectedEditable READ selectedEditable NOTIFY selectionChanged FINAL)
    Q_PROPERTY(bool selectedImageResizable READ selectedImageResizable NOTIFY selectionChanged FINAL)
    Q_PROPERTY(bool selectedImageRotatable READ selectedImageRotatable NOTIFY selectionChanged FINAL)
    Q_PROPERTY(bool selectedImageCopyable READ selectedImageCopyable NOTIFY selectedImageCopyableChanged FINAL)
    Q_PROPERTY(bool clipboardHasImage READ clipboardHasImage NOTIFY clipboardImageChanged FINAL)
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
        // Images may translate, scale and rotate; text and paths only translate.
        enum class Kind {
            Text,
            Image,
            Path
        };

        Kind kind = Kind::Text;
        QVector<int> objectPath;
        QRectF sourceRect;
        QRectF frameRect;
        int imageQuarterTurns = 0;
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

    enum CommentStatus {
        NoCommentStatus,
        AcceptedCommentStatus,
        RejectedCommentStatus,
        CancelledCommentStatus,
        CompletedCommentStatus,
        DeferredCommentStatus,
        FutureCommentStatus
    };
    Q_ENUM(CommentStatus)

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
        LinkTool,
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
        DiamondShape,
        TableShape
    };
    Q_ENUM(ShapeType)

    static constexpr int MaximumTableDimension = 20;

    enum FontFilter {
        VietnameseFonts,
        EnglishFonts,
        ChineseFonts,
        JapaneseFonts,
        KoreanFonts,
        SymbolFonts
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
    bool footerMode() const { return m_footerMode; }
    void setFooterMode(bool enabled);
    Q_INVOKABLE qreal selectionTopRatio(int page) const;
    Q_INVOKABLE qreal selectionBottomRatio(int page) const;
    bool headerSelectionOnly() const { return m_headerSelectionOnly; }
    bool headerSyncAvailable() const;
    bool headerSyncLoading() const { return m_headerSyncLoading; }
    int headerSyncMatchCount() const;
    QString headerSyncMatchPages() const;
    QString headerSyncKey() const;
    bool headerSyncCanApply() const;
    QString headerSyncError() const { return m_headerSyncError; }
    Q_INVOKABLE void findSimilarHeaders();
    Q_INVOKABLE bool applyHeaderChanges(const QString &pages = QString());
    Q_INVOKABLE void cancelHeaderSync();
    void setHeaderSelectionOnly(bool enabled);
    Q_INVOKABLE bool candidateAllowedInSelection(int page, const QString &objectId);
    ShapeType shapeType() const;
    void setShapeType(ShapeType shapeType);
    int tableRows() const { return m_tableRows; }
    int tableColumns() const { return m_tableColumns; }
    void setTableRows(int rows);
    void setTableColumns(int columns);
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
    QString stampArtwork() const;
    QVariantList stampPresets() const;
    bool imageAspectRatioLocked() const;
    void setImageAspectRatioLocked(bool locked);

    bool hasDrafts() const;
    bool canUndo() const;
    bool canRedo() const;
    bool saving() const;
    bool canSetOpenPassword() const;
    bool passwordChangePending() const;
    Q_INVOKABLE bool setOpenPassword(const QString &password);
    bool watermarksReady() const { return m_watermarksReady; }
    bool loadingWatermarks() const { return bool(m_watermarkCancellation); }
    bool watermarkChangesPending() const { return !m_watermarkChanges.isEmpty(); }
    QVariantList watermarks() const;
    Q_INVOKABLE void loadWatermarks();
    Q_INVOKABLE bool applyWatermark(const QVariantMap &settings, const QString &id = QString());
    Q_INVOKABLE bool removeWatermark(const QString &id);
    Q_INVOKABLE QVariantMap loadWatermarkImage(const QUrl &url);
    Q_INVOKABLE QString watermarkPreview(const QVariantMap &settings, const QString &id = QString());
    QVector<PDFWatermarks::PageChange> watermarkChangesForPage(int page) const;
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
    qreal selectedFontSize() const;
    qreal selectedLetterSpacing() const;
    qreal selectedWordSpacing() const;
    qreal selectedFontSizeRatio() const;
    qreal selectedDisplayFontSizeRatio() const;
    int selectedFontStretch() const;
    bool selectedFontBold() const;
    bool selectedFontItalic() const;
    bool selectedFontUnderline() const;
    QColor selectedColor() const;
    int selectedParagraphAlignment() const;
    qreal selectedLineSpacing() const;
    qreal selectedParagraphSpaceBefore() const;
    qreal selectedParagraphSpaceAfter() const;
    bool selectedEditable() const;
    bool selectedImageResizable() const;
    bool selectedImageRotatable() const;
    bool selectedImageCopyable() const;
    bool clipboardHasImage() const;
    Q_INVOKABLE bool copySelectedImage();
    Q_INVOKABLE QVariantMap pasteImageFromClipboard(int page);
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
    QVector<DraftAnnotationDeletion> deletedSourceWebLinkAnnotationsForPage(int page) const;
    bool allDraftedTextObjectsSuppressibleForPage(int page) const;

    Q_INVOKABLE QVariantList candidatesForPage(int page);
    Q_INVOKABLE bool autoCreateWebLinks();
    Q_INVOKABLE bool removeWebLinks();
    Q_INVOKABLE bool beginLinkEditing();
    Q_INVOKABLE QVariantList editableLinksForPage(int page) const;
    Q_INVOKABLE QVariantMap linkProperties(int page, const QString &id) const;
    Q_INVOKABLE QString addLink(int page, const QRectF &rect, const QVariantMap &properties);
    Q_INVOKABLE bool updateLink(int page, const QString &id, const QRectF &rect,
                                const QVariantMap &properties);
    Q_INVOKABLE bool deleteLink(int page, const QString &id);
    Q_INVOKABLE bool activateManualLink(int page, const QString &id, quint64 revision);
    bool loadingEditableLinks() const { return bool(m_manualLinkCancellation); }
    Q_INVOKABLE void cancelWebLinkCreation();
    Q_INVOKABLE QVariantList webLinkDraftsForPage(int page) const;
    Q_INVOKABLE bool activateWebLink(int page, int index, quint64 revision);
    bool creatingWebLinks() const { return m_webLinkCancellation && !m_removingWebLinks; }
    bool removingWebLinks() const { return m_webLinkCancellation && m_removingWebLinks; }
    quint64 webLinksRevision() const { return m_webLinksRevision; }
    Q_INVOKABLE void requestCandidatesForPage(int page);
    Q_INVOKABLE QVariantList draftsForPage(int page) const;
    Q_INVOKABLE QVariantMap selectedDraftPreview() const;
    Q_INVOKABLE QVariantMap candidateAt(int page, qreal x, qreal y);
    Q_INVOKABLE QVariantMap addText(int page, qreal x, qreal y);
    Q_INVOKABLE QString headerTextFontFamily() const;
    Q_INVOKABLE QStringList headerTextFontFamilies(int filter, bool bold, bool italic) const;
    Q_INVOKABLE void initializeHeaderTextEditor(QObject *textDocument,
                                                const QVariantList &colors,
                                                int typingPosition,
                                                const QColor &typingColor,
                                                const QVariantList &families = {},
                                                int typingFamilyPosition = -1,
                                                const QString &typingFamily = {}) const;
    Q_INVOKABLE QVariantMap headerTextEditorState(QObject *textDocument) const;
    Q_INVOKABLE void setHeaderTextEditorColor(QObject *textDocument,
                                              const QColor &color) const;
    Q_INVOKABLE void setHeaderTextEditorFontFamily(QObject *textDocument,
                                                   const QString &family) const;
    Q_INVOKABLE bool addHeaderText(const QString &text, const QString &pages,
                                  qreal xPoints = 24.0, qreal yPoints = 16.0,
                                  qreal widthPoints = 0.0,
                                  const QVariantMap &format = {});
    Q_INVOKABLE QString headerPageNumberText(int page, int firstPage, int lastPage,
                                             int romanStartPage, int decimalStartPage) const;
    Q_INVOKABLE bool addHeaderPageNumbers(int firstPage, int lastPage,
                                         int romanStartPage, int decimalStartPage,
                                         qreal xPoints, qreal yPoints, qreal widthPoints,
                                         const QVariantMap &format = {});
    Q_INVOKABLE QVariantMap prepareHeaderImage(const QUrl &source);
    Q_INVOKABLE void clearHeaderImage();
    Q_INVOKABLE bool addHeaderImage(const QString &pages, qreal xPoints, qreal yPoints,
                                   qreal widthPoints, qreal heightPoints,
                                   int quarterTurns = 0);
    Q_INVOKABLE void requestAddImage(int page, qreal x, qreal y);
    Q_INVOKABLE QVariantMap addImage(int page,
                                     qreal x,
                                     qreal y,
                                     const QUrl &source);
    // Inserts snapshotted pixels through the normal image draft/history path.
    // A zero width keeps Add Image's default sizing; positive widths are PDF points.
    QVariantMap addImageData(int page, qreal x, qreal y, QImage image,
                            qreal widthPoints = 0.0, qreal heightPoints = 0.0);
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
                                     qreal pixelSize,
                                     qreal letterSpacing = 0.0,
                                     qreal wordSpacing = 0.0,
                                     bool sourceSpacing = false) const;
    Q_INVOKABLE int displayFontStretch(const QString &fontFamily,
                                       int fontStretch,
                                       bool fontBold,
                                       bool fontItalic,
                                       qreal pixelSize) const;
    Q_INVOKABLE void applyTypingFormatToEditorDocument(QObject *textDocument,
                                                       int start,
                                                       int length,
                                                       qreal displayedPageHeight = 0.0) const;
    Q_INVOKABLE void prepareEditorVirtualSpaces();
    Q_INVOKABLE void initializeEditorFontSizes(QObject *textDocument,
                                               qreal displayedPageHeight = 0.0) const;
    Q_INVOKABLE bool applyFontFamilyToEditorDocument(
        QObject *textDocument, int start, int length, const QString &family, qreal displayedPageHeight) const;
    Q_INVOKABLE bool applyFontSizeToEditorDocument(QObject *textDocument,
                                                   int start,
                                                   int length,
                                                   qreal fontSizeRatio,
                                                   qreal displayedPageHeight) const;
    Q_INVOKABLE bool applyFontStyleToEditorDocument(QObject *textDocument,
                                                    int start,
                                                    int length,
                                                    int style,
                                                    bool enabled) const;
    Q_INVOKABLE bool applyTextSpacingToEditorDocument(QObject *textDocument,
                                                       int start, int length,
                                                       qreal letterSpacingRatio,
                                                       qreal wordSpacingRatio,
                                                       qreal displayedPageHeight) const;
    Q_INVOKABLE void initializeEditorParagraphFormats(QObject *textDocument,
                                                       qreal displayedPageHeight) const;
    Q_INVOKABLE bool applyParagraphFormatsToEditorDocument(QObject *textDocument,
                                                           qreal displayedPageHeight) const;
    Q_INVOKABLE bool syncEditorFontStyles(QObject *textDocument);
    Q_INVOKABLE QStringList fontFamiliesForFilter(FontFilter filter) const;
    Q_INVOKABLE bool selectCandidate(int page, const QString &objectId);
    Q_INVOKABLE void requestCommentIndex();
    Q_INVOKABLE bool selectComment(int page, const QString &objectId);
    Q_INVOKABLE bool removeComment(int page, const QString &objectId);
    Q_INVOKABLE bool setCommentStatus(int page,
                                      const QString &objectId,
                                      CommentStatus status);
    Q_INVOKABLE bool setCommentMarked(int page,
                                      const QString &objectId,
                                      bool marked);
    Q_INVOKABLE int selectRelativeComment(int direction);
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE void beginSelectedTextEdit();
    Q_INVOKABLE void normalizeEditorSourceSpacing(QObject *textDocument,
                                                  qreal displayedPageHeight) const;
    Q_INVOKABLE void updateSelectedTextPreview(const QString &text,
                                               int cursorPosition = -1);
    Q_INVOKABLE void beginSelectedFrameResize();
    Q_INVOKABLE void updateSelectedFrameRect(const QRectF &frameRect);
    Q_INVOKABLE void updateSelectedTableDivider(bool column, int index, qreal position);
    Q_INVOKABLE void updateSelectedContentBounds(const QRectF &contentBounds);
    Q_INVOKABLE void finishSelectedFrameResize();
    Q_INVOKABLE void cancelSelectedFrameResize();
    Q_INVOKABLE void rotateSelectedImage(int quarterTurns);
    Q_INVOKABLE void setSelectedTextRange(int start, int length);
    Q_INVOKABLE void setSelectedFontFamily(const QString &fontFamily);
    Q_INVOKABLE void setSelectedFontSize(qreal points);
    Q_INVOKABLE void setSelectedLetterSpacing(qreal points);
    Q_INVOKABLE void setSelectedWordSpacing(qreal points);
    Q_INVOKABLE void setSelectedFontBold(bool enabled);
    Q_INVOKABLE void setSelectedFontItalic(bool enabled);
    Q_INVOKABLE void setSelectedFontUnderline(bool enabled);
    Q_INVOKABLE void setSelectedColor(const QColor &color);
    Q_INVOKABLE void setSelectedParagraphAlignment(ParagraphAlignment alignment);
    Q_INVOKABLE void setSelectedLineSpacing(qreal factor);
    Q_INVOKABLE void setSelectedParagraphSpaceBefore(qreal points);
    Q_INVOKABLE void setSelectedParagraphSpaceAfter(qreal points);
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
    Q_INVOKABLE void respondToFontDownload(bool accepted);
    Q_INVOKABLE void cancelFontDownload();
    Q_INVOKABLE bool requestSelectedTextEdit(const QString &styleAction = QString());
    Q_INVOKABLE void cancelTextEditPreparation();
    Q_INVOKABLE bool continueTextEditWithFallback();
    bool fontDownloadPending() const { return bool(m_pendingFontSave) || bool(m_pendingFontEdit); }
    bool fontDownloadForEdit() const { return bool(m_pendingFontEdit); }
    bool downloadingFont() const { return m_downloadingFont; }
    QString missingFontFamily() const;
    QString textEditFallbackFamily() const;
    QStringList missingFontFamilies() const;

signals:
    void documentChanged();
    void toolChanged();
    void footerModeChanged();
    void headerSelectionOnlyChanged();
    void headerSyncChanged();
    void shapeTypeChanged();
    void tableDimensionsChanged();
    void imageAspectRatioLockedChanged();
    void clipboardImageChanged();
    void selectedImageCopyableChanged();
    void highlightColorChanged();
    void underlineColorChanged();
    void squigglyColorChanged();
    void strikeoutColorChanged();
    void drawingCommentColorChanged();
    void stampNameChanged();
    void pendingDrawingCommentPageChanged();
    void historyStateChanged();
    void savingChanged();
    void passwordProtectionChanged();
    void watermarksChanged();
    void creatingWebLinksChanged();
    void editableLinksChanged();
    void webLinksChanged();
    void textEditActiveChanged();
    void selectionChanged();
    void selectedGeometryChanged();
    void textFormatStateChanged();
    void commentsChanged();
    void commentIndexStateChanged();
    void typingFormatApplied(int start, int length);
    void selectionTextSpacingApplied(int start, int length,
                                      qreal letterSpacingRatio, qreal wordSpacingRatio);
    void selectionParagraphSpacingApplied();
    void selectionFontFamilyApplied(int start, int length, const QString &family);
    void selectionFontSizeApplied(int start, int length, qreal fontSizeRatio);
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
    void fontDownloadRequested();
    void fontDownloadFailed(const QString &message);
    void fontDownloadStateChanged();
    void textEditPreparationReady(int page, const QString &objectId, const QString &styleAction);

private:
    friend class PDFSaveWriter;

    struct TextSpacingSample {
        qreal residual = 0.0;
        bool followsSpace = false;
    };

    struct SourceTextPosition {
        qsizetype start = 0;
        qsizetype length = 0;
        qreal x = 0.0;
        qreal advance = 0.0;
        qreal y = 0.0;
    };

    struct TextObjectLayout {
        QRectF bounds;
        QVector<QRectF> glyphBounds;
        QPointF origin;
        QString text;
        QString encodedText;
        bool hasOrigin = false;
        QVector<TextSpacingSample> spacingSamples;
        QPointF previousSpacingOrigin;
        qreal previousSpacingAdvance = 0.0;
        qreal spacingEmSize = 0.0;
        bool previousSpacingWasSpace = false;
        bool hasSpacingOrigin = false;
        bool spacingUnsupported = false;
        QVector<SourceTextPosition> sourcePositions;
        bool positionsUnsupported = false;
    };

    struct CharacterFormat {
        QString fontFamily;
        qreal fontSizeRatio = 0.0;
        qreal displayFontSizeRatio = 0.0;
        qreal letterSpacingRatio = 0.0;
        qreal wordSpacingRatio = 0.0;
        // Per-cluster source adjustment, separate from user tracking controls.
        qreal sourceAdvanceRatio = 0.0;
        // Horizontal displacement per unit of glyph height above the baseline.
        qreal textShear = 0.0;
        // Editor-only U+0020 cell with fixed physical width; never PDF text.
        qreal virtualSpaceRatio = 0.0;
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

    struct CandidatePart {
        QString objectId;
        QVector<int> objectPath;
        QVector<int> underlineObjectPath;
        QRectF underlineRect;
        QRectF sourceRect;
        QPointF origin;
        QString text;
        QString separatorAfter;
        bool hasBaseline = false;
    };

    struct CandidateGeometry {
        QRectF sourceRect;
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
        qreal lineSpacing = 1.0;
        qreal spaceBeforeRatio = 0.0;
        qreal spaceAfterRatio = 0.0;
        ParagraphListStyle listStyle = ParagraphListStyle::None;
        int listLevel = 0;
        int listStart = 1;
        qreal firstLineIndentRatio = 0.0;
    };

    struct Candidate {
        int page = -1;
        QString objectId;
        int sourceAnnotationIndex = -1;
        QString sourceWidgetId;
        QVector<int> objectPath;
        QVector<CandidatePart> parts;
        QString kind;
        QString headerGroupId;
        QString headerRole;
        QByteArray headerImageDigest;
        bool headerMetadataInvalid = false;
        QString label;
        CandidateGeometry geometry;
        QRectF initialDraftFrameRect;
        QPointF origin;
        QString text;
        QVector<QRectF> textGlyphRects;
        QVector<CharacterSpan> sourceCharacterSpans;
        QVector<SourceTextPosition> pendingSourcePositions;
        QVector<ParagraphFormat> sourceParagraphFormats;
        QVector<qsizetype> sourceParagraphLineBreaks;
        QString paragraphLayoutGroup;
        int paragraphLayoutIndex = -1;
        int paragraphLayoutCount = 0;
        int paragraphLayoutStart = -1;
        int paragraphLayoutLength = 0;
        QString paragraphLayoutText;
        QByteArray paragraphLayoutModel;
        QPointF paragraphLayoutAnchor;
        QString spacingGroup;
        bool invalidSpacingMetadata = false;
        QString spacingText;
        int spacingIndex = -1;
        int spacingCount = 0;
        int spacingStart = -1;
        qreal spacingLetterPoints = 0.0;
        qreal spacingWordPoints = 0.0;
        qreal spacingSourceAdvancePoints = 0.0;
        qreal spacingOriginOffset = 0.0;
        QVector<CharacterSpan> spacingRuns;
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
        QString commentState;
        QString commentStateModel;
        QString commentStateAuthor;
        CommentStatus commentStatus = NoCommentStatus;
        bool commentMarked = false;
        int commentStatusAnnotationIndex = -1;
        int commentMarkedAnnotationIndex = -1;
        bool commentTextReadOnly = false;
        bool commentThreadDeletionProtected = false;
        bool replyToParentAdded = false;
        QString fontFamily;
        QString fontIdentity;
        qreal fontSizeRatio = 0.0;
        qreal displayFontSizeRatio = 0.0;
        qreal horizontalScale = 1.0;
        qreal textShear = 0.0;
        bool preservePhysicalFontSizes = false;
        qreal letterSpacingRatio = 0.0;
        qreal wordSpacingRatio = 0.0;
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
        bool imageResizable = false;
        bool renderSuppressible = false;
        bool limited = false;
        bool added = false;
        quint64 addedOrder = 0;
        QImage image;
        ShapeType shapeType = RectangleShape;
        int tableRows = 0;
        int tableColumns = 0;
        // Interior dividers relative to the grid's inner frame; empty means equal cells.
        QVector<qreal> tableRowStops;
        QVector<qreal> tableColumnStops;
        QPointF shapeStartRatio;
        QPointF shapeEndRatio = QPointF(1.0, 1.0);
        qreal shapeStrokeWidthRatio = 0.0;
        QByteArray sourceShapeMetadata;
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
        QString stampDetail;
        QString stampArtwork;
        QString reason;
        int pendingFontSnapshotIndex = -1;
        QString pendingFontExpectedFamily;
        QString pendingBundledFontFamily;
        QString pendingSystemFontFamily;
        QByteArray pendingEmbeddedFontData;
    };

    struct CandidateScanRequest {
        QString filePath;
        QUuid documentId;
        QByteArray password;
        int revision = -1;
        int page = -1;
        int sourcePage = -1;
        QSizeF pageSize;
        bool editingAllowed = false;
        bool annotationsAllowed = false;
        QString commentAuthor;
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

    struct PendingTextMarkupCreation {
        QString filePath;
        QRectF selectionRect;
        QSizeF pageSize;
        QColor color;
        Tool tool = HighlightTool;
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

    struct TextChange {
        qsizetype start = 0;
        qsizetype removedLength = 0;
        qsizetype insertedLength = 0;
    };

    struct Draft {
        Candidate candidate;
        QVector<qreal> tableRowStops;
        QVector<qreal> tableColumnStops;
        PDFCommentAnnotation::CommonProperties commentProperties;
        QString text;
        QVector<CharacterSpan> characterSpans;
        QVector<ParagraphFormat> paragraphFormats;
        QVector<qsizetype> sourceLineBreaks;
        QRectF frameRect;
        QRectF contentBounds;
        // Clockwise turns relative to the source image, in displayed page coordinates.
        int imageQuarterTurns = 0;
        CommentStatus commentStatus = NoCommentStatus;
        bool commentMarked = false;
        QString commentStateAuthor;
        QDateTime commentStateModifiedDate;
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

    struct DraftChange {
        QString key;
        int page = -1;
        QString objectId;
        bool hadBefore = false;
        Draft before;
        bool hadAfter = false;
        Draft after;
    };

    struct Command {
        enum class Kind {
            ObjectDraft,
            ObjectDraftBatch,
            HeaderDraftBatch,
            PageStructure,
            WebLinks,
            Watermarks
        };

        Kind kind = Kind::ObjectDraft;
        QString key;
        int page = -1;
        QString objectId;
        bool hadBefore = false;
        Draft before;
        bool hadAfter = false;
        Draft after;
        // A batch of newly added objects, committed and restored atomically.
        QVector<Draft> addedDrafts;
        QVector<DraftChange> changedDrafts;
        PageState pageStateBefore;
        PageState pageStateAfter;
        QVector<PDFWebLinks::Link> webLinksBefore;
        QVector<PDFWebLinks::Link> webLinksAfter;
        QVector<PDFWebLinks::SourceLink> removedWebLinksBefore;
        QVector<PDFWebLinks::SourceLink> removedWebLinksAfter;
        QVector<PDFWebLinks::ManualLink> manualLinksBefore;
        QVector<PDFWebLinks::ManualLink> manualLinksAfter;
        QVector<PDFWatermarks::Change> watermarksBefore;
        QVector<PDFWatermarks::Change> watermarksAfter;
    };

    enum class FontStyle {
        Bold,
        Italic,
        Underline
    };

    void resetSession();
    void initializeImageClipboard();
    void cancelImageCopy();
    void resetHeaderSync();
    void captureHeaderSyncSelection();
    void validateHeaderSyncHistory();
    void scanNextHeaderPage();
    void collectHeaderMatches(QVector<Candidate> candidates, int page);
    bool headerSyncDraftEligible(const Draft &draft, bool includeContentBounds = true) const;
    bool measureHeaderSyncText(Draft &draft) const;
    bool headerSyncDraftMatches(const Draft &draft, bool &pageNumber) const;
    Draft currentHeaderSyncDraft() const;
    bool headerSyncHistoryCurrent() const;
    void applyHeaderDraftBatch(const Command &command, bool useAfterState, bool immediate);
    void resetWatermarks();
    void invalidateWatermarks();
    bool canChangeWatermarks();
    bool prepareWatermarkSpec(const QVariantMap &settings, const QString &id,
                              PDFWatermarks::Spec &spec) const;
    QVector<PDFWatermarks::Group> effectiveWatermarks() const;
    void commitWatermarkChanges(QVector<PDFWatermarks::Change> changes);
    bool hasNonWebLinkDrafts() const;
    void invalidateWebLinks();
    bool resolveManualLink(int page, const QString &id, PDFWebLinks::ManualLink &link) const;
    QVariantMap manualLinkProperties(const PDFWebLinks::ManualLink &link,
                                     bool draft, bool automatic) const;
    bool validateManualLink(int page, const QRectF &rect, const QVariantMap &properties,
                            PDFWebLinks::ManualLink &link) const;
    Command captureLinkCommand() const;
    void commitLinkCommand(Command command);
    bool removeAutoLinkRegion(const QString &id);
    void clearFontRecovery();
    void finishFontDownload(bool succeeded, const QString &error);
    bool textEditPreparationIsCurrent() const;
    void continueTextEditPreparation();
    void finishTextEditFontDownload(bool succeeded, const QString &error);
    QRectF rotatedImageFrame(const Draft &draft, int quarterTurns) const;
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
        Tool tool,
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
    bool pendingTextMarkupCreationIsCurrent(
        const PendingTextMarkupCreation &creation) const;
    int pendingTextMarkupScanPage() const;
    void completePendingTextMarkupCreations(int page);
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
    bool commentStateEditable(const Candidate &candidate) const;
    QString canonicalCommentStateAuthor(
        const QString &requestedAuthor) const;
    static std::optional<CommentStatus> commentStatusForPdfName(
        const QString &state);
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
    void preserveSourceTextPositions(Candidate &candidate, const QSizeF &pageSize) const;
    static void inferSourceTextSpacing(Candidate &candidate,
                                       const TextObjectLayout &layout,
                                       const QSizeF &pageSize);
    static void readSourceTextSpacing(FPDF_PAGEOBJECT object, Candidate &candidate,
                                      const QSizeF &pageSize, const QString &encodedSourceText);
    static void changeFormatFamily(CharacterFormat &format, const QString &family);
    static CharacterFormat characterFormatForCandidate(const Candidate &candidate);
    static CharacterFormat primaryCharacterFormat(const Draft &draft);
    bool addHeaderTextBatch(const QList<int> &pages, const QStringList &texts,
                            qreal xPoints, qreal yPoints, qreal widthPoints,
                            const QVariantMap &format, bool pageNumbers = false);
    static CharacterFormat characterFormatAtPosition(const Draft &draft,
                                                      qsizetype position,
                                                      bool preferPrevious);
    CharacterFormat selectedCharacterFormatForDisplay() const;
    void setSelectedTextSpacing(qreal points, bool wordSpacing);
    enum class ParagraphSpacing { Line, Before, After };
    qreal selectedParagraphSpacing(ParagraphSpacing spacing) const;
    void setSelectedParagraphSpacing(qreal value, ParagraphSpacing spacing);
    static void expandSourceParagraphRange(const Draft &draft,
                                           qsizetype &first, qsizetype &last);
    static bool normalizeSourceParagraphBreaks(Draft &draft,
                                               qsizetype &first, qsizetype &last);
    static void readSourceParagraphLayout(FPDF_PAGE page, FPDF_PAGEOBJECT object,
                                          Candidate &candidate, const QSizeF &pageSize,
                                          const QString &encodedSourceText);
    QVector<Candidate> restoreSourceParagraphLayouts(QVector<Candidate> candidates,
                                                     const QSizeF &pageSize) const;
    static QVector<ParagraphFormat> paragraphFormatsForText(
        const QString &text,
        const ParagraphFormat &prototype);
    static Draft initialDraftForCandidate(const Candidate &candidate);
    static Draft collapseEditorVirtualSpaces(const Draft &draft);
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
        const QString &expectedFamily,
        bool substitute = false);
    void rememberRejectedFontFingerprint(const QByteArray &fingerprint);
    void clearRegisteredFonts();
    bool isDesignerManagedSourceWidget(const Candidate &candidate) const;
    bool candidateMatchesSelectionScope(const Candidate &candidate) const;
    void notifyManagedWidgetVisibilityChanged(int page);

    QPointer<PDFDocument> m_document;
    QPointer<PDFFormController> m_formController;
    QPointer<PDFFormDesignerController> m_formDesignerController;
    Tool m_tool = BrowseTool;
    bool m_headerSelectionOnly = false;
    bool m_footerMode = false;
    ShapeType m_shapeType = RectangleShape;
    int m_tableRows = 3;
    int m_tableColumns = 3;
    QColor m_highlightColor;
    QColor m_underlineColor;
    QColor m_squigglyColor;
    QColor m_strikeoutColor;
    QColor m_drawingCommentColor;
    QString m_stampName = QStringLiteral("Approved");
    mutable QVariantList m_stampPresetItems;
    int m_pendingDrawingCommentPage = -1;
    int m_cachedRevision = -1;
    QHash<int, QVector<Candidate>> m_pageCandidates;
    QVector<int> m_pageCandidateLru;
    int m_deferredCandidateScanPage = -1;
    int m_candidateScanInFlightPage = -1;
    bool m_candidateScanInFlight = false;
    std::shared_ptr<std::atomic_bool> m_candidateScanCancellation;
    QVector<PendingTextMarkupCreation> m_pendingTextMarkupCreations;
    QHash<int, QVector<Candidate>> m_indexedCommentCandidates;
    qsizetype m_indexedCommentCandidateBytes = 0;
    CommentIndexState m_commentIndexState = CommentIndexIdle;
    std::shared_ptr<std::atomic_bool> m_commentIndexCancellation;
    QVector<Candidate> m_addedCandidates;
    QVector<PDFPageInsertion> m_pageInsertions;
    QSet<int> m_deletedSourcePages;
    QHash<QString, Draft> m_drafts;
    QVector<PDFWebLinks::Link> m_autoWebLinks;
    QVector<PDFWebLinks::ManualLink> m_manualLinks;
    QVector<PDFWebLinks::ManualLink> m_sourceManualLinks;
    bool m_manualSourceLinksReady = false;
    std::shared_ptr<std::atomic_bool> m_manualLinkCancellation;
    QVector<PDFWebLinks::SourceLink> m_removedWebLinks;
    QSet<int> m_removedWebLinkPreviewPages;
    bool m_removingWebLinks = false;
    std::shared_ptr<std::atomic_bool> m_webLinkCancellation;
    quint64 m_webLinksRevision = 0;
    quint64 m_pendingWebLinkRequest = 0;
    QHash<int, quint64> m_pageDraftRevisions;
    quint64 m_nextDraftRevision = 0;
    quint64 m_nextAddedObjectId = 0;
    // Temporary placement pixels, shared with committed image drafts by QImage.
    QImage m_headerImage;
    bool m_headerSyncValid = false;
    bool m_headerSyncLoading = false;
    bool m_headerSyncReady = false;
    bool m_headerSyncPreserveNumbers = false;
    bool m_headerSyncApplying = false;
    bool m_headerSyncHadBefore = false;
    Draft m_headerSyncBaseline;
    QVector<Draft> m_headerSyncMatches;
    QVector<CandidateScanRequest> m_headerSyncRequests;
    qsizetype m_headerSyncRequestIndex = 0;
    qsizetype m_headerSyncUndoIndex = 0;
    qsizetype m_headerSyncRetainedBytes = 0;
    QString m_headerSyncError;
    std::shared_ptr<std::atomic_bool> m_headerSyncCancellation;
    QVector<Command> m_undoStack;
    QVector<Command> m_redoStack;
    QHash<QByteArray, QString> m_registeredFontFamilies;
    QSet<QByteArray> m_rejectedFontFingerprints;
    QVector<QByteArray> m_rejectedFontFingerprintLru;
    qsizetype m_registeredFontBytes = 0;
    Candidate m_selectedCandidate;
    bool m_hasSelection = false;
    bool m_saving = false;
    // Session-only save setting; never copied into content history or exposed to QML.
    QByteArray m_pendingOpenPassword;
    bool m_watermarksReady = false;
    std::shared_ptr<std::atomic_bool> m_watermarkCancellation;
    QVector<PDFWatermarks::Group> m_sourceWatermarks;
    QVector<PDFWatermarks::Change> m_watermarkChanges;
    QSet<int> m_watermarkPreviewPages;
    QString m_watermarkImageToken;
    QImage m_watermarkDialogImage;
    struct PendingFontSave {
        PDFSaveWriter::Result failure;
        QString targetFilePath;
        QPointer<PDFDocument> document;
        int revision = -1;
        QString sourceFilePath;
        QString temporarySourceFilePath;
    };
    std::unique_ptr<PendingFontSave> m_pendingFontSave;
    struct TextEditFontRequirement {
        QString family;
        QString text;
        bool bold = false;
        bool italic = false;
    };
    struct PendingFontEdit {
        QPointer<PDFDocument> document;
        int revision = -1;
        QString sourceFilePath;
        int page = -1;
        QString objectId;
        Tool tool = BrowseTool;
        quint64 draftRevision = 0;
        QString styleAction;
        QVector<TextEditFontRequirement> requirements;
        QVector<TextEditFontRequirement> fallbackRequirements;
        QSet<QString> missingDisplayFaces;
        QString fallbackFamily;
        QStringList missingFamilies;
        QSet<QString> attemptedFaces;
        bool accepted = false;
    };
    std::unique_ptr<PendingFontEdit> m_pendingFontEdit;
    quint64 m_textEditPreparationGeneration = 0;
    FontDownloadService *m_fontDownloadService = nullptr;
    bool m_allowFontRecovery = false;
    bool m_downloadingFont = false;
    QSet<QString> m_attemptedFontDownloads;
    bool m_imageAspectRatioLocked = true;
    std::shared_ptr<std::atomic_bool> m_imageCopyCancellation;
    QString m_selectedText;
    bool m_textEditActive = false;
    qsizetype m_selectedTextRangeStart = 0;
    qsizetype m_selectedTextRangeLength = 0;
    bool m_editorVirtualSpacesPrepared = false;
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
    QVector<qreal> m_frameResizeStartTableRowStops;
    QVector<qreal> m_frameResizeStartTableColumnStops;
};
