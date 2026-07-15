#pragma once

#include "PDFDocument.h"

#include <QByteArray>
#include <QColor>
#include <QFont>
#include <QHash>
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

class PDFEditController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(PDFDocument *document READ document WRITE setDocument NOTIFY documentChanged FINAL)
    Q_PROPERTY(Tool tool READ tool WRITE setTool NOTIFY toolChanged FINAL)
    Q_PROPERTY(bool hasDrafts READ hasDrafts NOTIFY historyStateChanged FINAL)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyStateChanged FINAL)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyStateChanged FINAL)
    Q_PROPERTY(bool textEditActive READ textEditActive NOTIFY textEditActiveChanged FINAL)
    Q_PROPERTY(QStringList bundledFontFamilies READ bundledFontFamilies CONSTANT FINAL)
    Q_PROPERTY(QStringList availableFontFamilies READ availableFontFamilies CONSTANT FINAL)
    Q_PROPERTY(int selectedPage READ selectedPage NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QString selectedObjectId READ selectedObjectId NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QString selectedKind READ selectedKind NOTIFY selectionChanged FINAL)
    Q_PROPERTY(qreal selectedX READ selectedX NOTIFY selectionChanged FINAL)
    Q_PROPERTY(qreal selectedY READ selectedY NOTIFY selectionChanged FINAL)
    Q_PROPERTY(qreal selectedWidth READ selectedWidth NOTIFY selectionChanged FINAL)
    Q_PROPERTY(qreal selectedHeight READ selectedHeight NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QVariantList selectedSourceRects READ selectedSourceRects NOTIFY selectionChanged FINAL)
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
        AddTextTool
    };
    Q_ENUM(Tool)

    explicit PDFEditController(QObject *parent = nullptr);
    ~PDFEditController() override;

    PDFDocument *document() const;
    void setDocument(PDFDocument *document);

    Tool tool() const;
    void setTool(Tool tool);

    bool hasDrafts() const;
    bool canUndo() const;
    bool canRedo() const;
    bool textEditActive() const;
    QStringList bundledFontFamilies() const;
    QStringList availableFontFamilies() const;

    int selectedPage() const;
    QString selectedObjectId() const;
    QString selectedKind() const;
    qreal selectedX() const;
    qreal selectedY() const;
    qreal selectedWidth() const;
    qreal selectedHeight() const;
    QVariantList selectedSourceRects() const;
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
    Q_INVOKABLE QFont fontForDisplay(const QString &fontFamily,
                                     int fontStretch,
                                     bool fontBold,
                                     bool fontItalic,
                                     bool fontUnderline,
                                     qreal pixelSize) const;
    Q_INVOKABLE bool selectCandidate(int page, const QString &objectId);
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE void beginSelectedTextEdit();
    Q_INVOKABLE void updateSelectedTextPreview(const QString &text);
    Q_INVOKABLE void setSelectedFontFamily(const QString &fontFamily);
    Q_INVOKABLE void setSelectedFontBold(bool enabled);
    Q_INVOKABLE void setSelectedFontItalic(bool enabled);
    Q_INVOKABLE void setSelectedFontUnderline(bool enabled);
    Q_INVOKABLE void finishSelectedTextEdit();
    Q_INVOKABLE void cancelSelectedTextEdit();
    Q_INVOKABLE void commitSelectedText(const QString &text);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void discardDrafts();

signals:
    void documentChanged();
    void toolChanged();
    void historyStateChanged();
    void textEditActiveChanged();
    void selectionChanged();
    void pageDataChanged(int page);
    void draftRenderStateChanged(int page, quint64 revision, bool immediate);
    void statusMessage(const QString &message);

private:
    struct TextObjectLayout {
        QRectF bounds;
        QPointF origin;
        QString text;
        bool hasOrigin = false;
    };

    struct CandidatePart {
        QString objectId;
        QVector<int> objectPath;
        QRectF rect;
        QPointF origin;
        QString text;
        QString separatorAfter;
        bool hasBaseline = false;
    };

    struct Candidate {
        int page = -1;
        QString objectId;
        QVector<int> objectPath;
        QVector<CandidatePart> parts;
        QString kind;
        QString label;
        QRectF rect;
        QPointF origin;
        QString text;
        QString fontFamily;
        QString fontIdentity;
        qreal fontSizeRatio = 0.0;
        qreal displayFontSizeRatio = 0.0;
        int fontStretch = 100;
        bool fontBold = false;
        bool fontItalic = false;
        bool fontUnderline = false;
        QColor color = Qt::black;
        bool hasBaseline = false;
        bool editable = false;
        bool renderSuppressible = false;
        bool limited = false;
        bool added = false;
        quint64 addedOrder = 0;
        QString reason;
    };

    struct Draft {
        Candidate candidate;
        QString text;
    };

    struct Command {
        QString key;
        int page = -1;
        QString objectId;
        bool hadBefore = false;
        QString beforeText;
        QString beforeFontFamily;
        bool beforeFontBold = false;
        bool beforeFontItalic = false;
        bool beforeFontUnderline = false;
        bool hadAfter = false;
        QString afterText;
        QString afterFontFamily;
        bool afterFontBold = false;
        bool afterFontItalic = false;
        bool afterFontUnderline = false;
        Candidate candidate;
    };

    enum class FontStyle {
        Bold,
        Italic,
        Underline
    };

    void resetSession();
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
    QVariantMap candidateMap(const Candidate &candidate, const QString &text) const;
    bool documentAllowsEditing() const;
    QString draftKey(int page, const QString &objectId) const;
    void markDraftStateChanged(int page, bool immediate);
    void requestDraftRender(int page, bool immediate);
    static bool sameDraftFormatting(const Candidate &left, const Candidate &right);
    const Candidate &selectedCandidateForDisplay() const;
    void updateSelectedDraftCandidate(const Candidate &candidate);
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
    quint64 m_nextAddedTextId = 0;
    QVector<Command> m_undoStack;
    QVector<Command> m_redoStack;
    QHash<QString, QString> m_pdfEmbeddedFontFamilyCache;
    QHash<QByteArray, QString> m_registeredFontFamilies;
    QVector<int> m_registeredFontIds;
    qsizetype m_registeredFontBytes = 0;
    Candidate m_selectedCandidate;
    bool m_hasSelection = false;
    QString m_selectedText;
    bool m_textEditActive = false;
    Command m_textEditCommand;
};
