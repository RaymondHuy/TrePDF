#pragma once

#include "PDFDocument.h"
#include "PDFSaveWriter.h"

#include <QByteArray>
#include <QColor>
#include <QFont>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <QtQml/qqmlregistration.h>

#include <functional>

class PDFEditController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(PDFDocument *document READ document WRITE setDocument NOTIFY documentChanged FINAL)
    Q_PROPERTY(Tool tool READ tool WRITE setTool NOTIFY toolChanged FINAL)
    Q_PROPERTY(bool hasDrafts READ hasDrafts NOTIFY historyStateChanged FINAL)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyStateChanged FINAL)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyStateChanged FINAL)
    Q_PROPERTY(bool saving READ saving NOTIFY savingChanged FINAL)
    Q_PROPERTY(bool textEditActive READ textEditActive NOTIFY textEditActiveChanged FINAL)
    Q_PROPERTY(QStringList bundledFontFamilies READ bundledFontFamilies CONSTANT FINAL)
    Q_PROPERTY(QStringList availableFontFamilies READ availableFontFamilies NOTIFY selectionChanged FINAL)
    Q_PROPERTY(int selectedPage READ selectedPage NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QString selectedObjectId READ selectedObjectId NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QString selectedKind READ selectedKind NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QRectF selectedSourceRect READ selectedSourceRect NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QRectF selectedFrameRect READ selectedFrameRect NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QRectF selectedContentBounds READ selectedContentBounds NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QVariantList selectedSourceRects READ selectedSourceRects NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QVariantList selectedCharacterSpans READ selectedCharacterSpans NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QVariantList selectedParagraphFormats READ selectedParagraphFormats NOTIFY selectionChanged FINAL)
    Q_PROPERTY(bool selectedHasBaseline READ selectedHasBaseline NOTIFY selectionChanged FINAL)
    Q_PROPERTY(qreal selectedOriginX READ selectedOriginX NOTIFY selectionChanged FINAL)
    Q_PROPERTY(qreal selectedBaselineY READ selectedBaselineY NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QString selectedText READ selectedText NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QString selectedFontFamily READ selectedFontFamily NOTIFY selectionChanged FINAL)
    Q_PROPERTY(qreal selectedFontSizeRatio READ selectedFontSizeRatio NOTIFY selectionChanged FINAL)
    Q_PROPERTY(qreal selectedDisplayFontSizeRatio READ selectedDisplayFontSizeRatio NOTIFY selectionChanged FINAL)
    Q_PROPERTY(int selectedFontStretch READ selectedFontStretch NOTIFY selectionChanged FINAL)
    Q_PROPERTY(bool selectedFontBold READ selectedFontBold NOTIFY selectionChanged FINAL)
    Q_PROPERTY(bool selectedFontItalic READ selectedFontItalic NOTIFY selectionChanged FINAL)
    Q_PROPERTY(bool selectedFontUnderline READ selectedFontUnderline NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QColor selectedColor READ selectedColor NOTIFY selectionChanged FINAL)
    Q_PROPERTY(bool selectedEditable READ selectedEditable NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QString selectedReason READ selectedReason NOTIFY selectionChanged FINAL)

public:
    enum Tool {
        BrowseTool,
        SelectTool,
        EditTextTool,
        AddTextTool,
        AddImageTool
    };
    Q_ENUM(Tool)

    enum FontFilter {
        VietnameseFonts,
        EnglishFonts,
        ChineseFonts,
        JapaneseFonts,
        KoreanFonts
    };
    Q_ENUM(FontFilter)

    explicit PDFEditController(QObject *parent = nullptr);
    ~PDFEditController() override;

    PDFDocument *document() const;
    void setDocument(PDFDocument *document);

    Tool tool() const;
    void setTool(Tool tool);

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
    bool selectedEditable() const;
    QString selectedReason() const;
    quint64 draftRevisionForPage(int page) const;
    QVector<QVector<int>> draftedTextObjectPathsForPage(int page) const;
    bool allDraftedTextObjectsSuppressibleForPage(int page) const;

    Q_INVOKABLE QVariantList candidatesForPage(int page);
    Q_INVOKABLE QVariantList draftsForPage(int page) const;
    Q_INVOKABLE QVariantMap candidateAt(int page, qreal x, qreal y);
    Q_INVOKABLE QVariantMap addText(int page, qreal x, qreal y);
    Q_INVOKABLE void requestAddImage(int page, qreal x, qreal y);
    Q_INVOKABLE QVariantMap addImage(int page,
                                     qreal x,
                                     qreal y,
                                     const QUrl &source);
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
    Q_INVOKABLE QStringList fontFamiliesForFilter(FontFilter filter) const;
    Q_INVOKABLE bool selectCandidate(int page, const QString &objectId);
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE void beginSelectedTextEdit();
    Q_INVOKABLE void updateSelectedTextPreview(const QString &text);
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
    Q_INVOKABLE void finishSelectedTextEdit();
    Q_INVOKABLE void cancelSelectedTextEdit();
    Q_INVOKABLE void commitSelectedText(const QString &text);
    Q_INVOKABLE void deleteSelectedObject();
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void discardDrafts();
    Q_INVOKABLE bool save();
    Q_INVOKABLE bool saveAs(const QUrl &target);
    Q_INVOKABLE bool requestSave();
    Q_INVOKABLE bool requestSaveAs(const QUrl &target);

signals:
    void documentChanged();
    void toolChanged();
    void historyStateChanged();
    void savingChanged();
    void textEditActiveChanged();
    void selectionChanged();
    void pageDataChanged(int page);
    void draftRenderStateChanged(int page, quint64 revision, bool immediate);
    void statusMessage(const QString &message);
    void imageFileRequested(int page, qreal x, qreal y);
    void saveFinished(bool succeeded);

private:
    friend class PDFSaveWriter;

    struct TextObjectLayout {
        QRectF bounds;
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
        QVector<int> objectPath;
        QVector<CandidatePart> parts;
        QString kind;
        QString label;
        CandidateGeometry geometry;
        QRectF initialDraftFrameRect;
        QPointF origin;
        QString text;
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
        QString reason;
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

    enum class ParagraphAlignment {
        Left,
        Center,
        Right,
        Justify
    };

    enum class ParagraphListStyle {
        None,
        Bullet,
        Decimal
    };

    struct ParagraphFormat {
        qsizetype start = 0;
        qsizetype length = 0;
        ParagraphAlignment alignment = ParagraphAlignment::Left;
        ParagraphListStyle listStyle = ParagraphListStyle::None;
        int listLevel = 0;
        int listStart = 1;
    };

    struct Draft {
        Candidate candidate;
        QString text;
        QVector<CharacterSpan> characterSpans;
        QVector<ParagraphFormat> paragraphFormats;
        QRectF frameRect;
        QRectF contentBounds;
        bool deleted = false;
    };

    struct Command {
        QString key;
        int page = -1;
        QString objectId;
        bool hadBefore = false;
        Draft before;
        bool hadAfter = false;
        Draft after;
    };

    enum class FontStyle {
        Bold,
        Italic,
        Underline
    };

    void resetSession();
    bool completeSave(const PDFSaveWriter::Result &result,
                      const QString &targetFilePath,
                      PDFDocument *expectedDocument,
                      int expectedRevision,
                      const QString &expectedSourceFilePath);
    void ensureCacheRevision();
    const QVector<Candidate> &ensurePageCandidates(int page);
    QVector<Candidate> scanPage(int page);
    void appendPageObjectCandidates(FPDF_PAGE page,
                                    FPDF_PAGEOBJECT pageObject,
                                    const QVector<int> &objectPath,
                                    FPDF_TEXTPAGE textPage,
                                    const QHash<FPDF_PAGEOBJECT, TextObjectLayout> &textObjectLayouts,
                                    const FS_MATRIX &containerTransform,
                                    const QSizeF &pageSize,
                                    QVector<Candidate> &candidates);
    QVector<Candidate> groupTextLineCandidates(QVector<Candidate> candidates,
                                               const QSizeF &pageSize) const;
    void appendAnnotationCandidates(FPDF_PAGE page,
                                    int pageIndex,
                                    const QSizeF &pageSize,
                                    QVector<Candidate> &candidates) const;
    const Candidate *findCandidate(int page, const QString &objectId);
    QVariantList candidateSourceRects(const Candidate &candidate) const;
    QVariantList characterSpanMaps(const QVector<CharacterSpan> &spans) const;
    QVariantList paragraphFormatMaps(const QVector<ParagraphFormat> &formats) const;
    QVariantMap candidateMap(const Draft &draft) const;
    bool documentAllowsEditing() const;
    QString draftKey(int page, const QString &objectId) const;
    void markDraftStateChanged(int page, bool immediate);
    void requestDraftRender(int page, bool immediate);
    static void setInitialGeometry(Candidate &candidate, const QRectF &sourceRect);
    void expandInitialTextFrame(Candidate &candidate, const QSizeF &pageSize) const;
    static CharacterFormat characterFormatForCandidate(const Candidate &candidate);
    static CharacterFormat primaryCharacterFormat(const Draft &draft);
    static QVector<ParagraphFormat> paragraphFormatsForText(
        const QString &text,
        const ParagraphFormat &prototype);
    static Draft initialDraftForCandidate(const Candidate &candidate);
    static QPointF layoutOriginForDraft(const Draft &draft);
    static bool sameCharacterFormat(const CharacterFormat &left,
                                    const CharacterFormat &right);
    static bool sameCharacterSpans(const QVector<CharacterSpan> &left,
                                   const QVector<CharacterSpan> &right);
    static bool sameParagraphFormats(const QVector<ParagraphFormat> &left,
                                     const QVector<ParagraphFormat> &right);
    static bool sameDraftContent(const Draft &left, const Draft &right);
    static void setDraftText(Draft &draft, const QString &text);
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
    void selectedTextRangeForDraft(const Draft &draft,
                                   qsizetype &start,
                                   qsizetype &length) const;
    void resetTextEditDisplayState();
    void resetFrameResizeState();
    void setSelectedFontStyle(FontStyle style, bool enabled);
    void applyDraftState(const Command &command, bool useAfterState, bool immediate);
    bool removeAddedCandidate(int page, const QString &objectId);
    void updateSelectedTextFromDraft();
    void setSelection(const Candidate *candidate);
    QString resolvedFontFamily(FPDF_PAGEOBJECT textObject, const QString &text);
    QString registerEmbeddedFont(FPDF_FONT font, const QString &expectedFamily);
    void clearRegisteredFonts();

    QPointer<PDFDocument> m_document;
    Tool m_tool = BrowseTool;
    int m_cachedRevision = -1;
    QHash<int, QVector<Candidate>> m_pageCandidates;
    QVector<Candidate> m_addedCandidates;
    QHash<QString, Draft> m_drafts;
    QHash<int, quint64> m_pageDraftRevisions;
    quint64 m_nextDraftRevision = 0;
    quint64 m_nextAddedObjectId = 0;
    QVector<Command> m_undoStack;
    QVector<Command> m_redoStack;
    QHash<QString, QString> m_pdfEmbeddedFontFamilyCache;
    QHash<QByteArray, QString> m_registeredFontFamilies;
    QVector<int> m_registeredFontIds;
    qsizetype m_registeredFontBytes = 0;
    Candidate m_selectedCandidate;
    bool m_hasSelection = false;
    bool m_saving = false;
    QString m_selectedText;
    bool m_textEditActive = false;
    qsizetype m_selectedTextRangeStart = 0;
    qsizetype m_selectedTextRangeLength = 0;
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
