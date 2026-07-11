#pragma once

#include "PDFDocument.h"

#include <QColor>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QRectF>
#include <QString>
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
    Q_PROPERTY(int selectedPage READ selectedPage NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QString selectedObjectId READ selectedObjectId NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QString selectedKind READ selectedKind NOTIFY selectionChanged FINAL)
    Q_PROPERTY(qreal selectedX READ selectedX NOTIFY selectionChanged FINAL)
    Q_PROPERTY(qreal selectedY READ selectedY NOTIFY selectionChanged FINAL)
    Q_PROPERTY(qreal selectedWidth READ selectedWidth NOTIFY selectionChanged FINAL)
    Q_PROPERTY(qreal selectedHeight READ selectedHeight NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QString selectedText READ selectedText NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QString selectedFontFamily READ selectedFontFamily NOTIFY selectionChanged FINAL)
    Q_PROPERTY(qreal selectedFontSizeRatio READ selectedFontSizeRatio NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QColor selectedColor READ selectedColor NOTIFY selectionChanged FINAL)
    Q_PROPERTY(bool selectedEditable READ selectedEditable NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QString selectedReason READ selectedReason NOTIFY selectionChanged FINAL)

public:
    enum Tool {
        BrowseTool,
        SelectTool,
        EditTextTool
    };
    Q_ENUM(Tool)

    explicit PDFEditController(QObject *parent = nullptr);

    PDFDocument *document() const;
    void setDocument(PDFDocument *document);

    Tool tool() const;
    void setTool(Tool tool);

    bool hasDrafts() const;
    bool canUndo() const;
    bool canRedo() const;
    bool textEditActive() const;

    int selectedPage() const;
    QString selectedObjectId() const;
    QString selectedKind() const;
    qreal selectedX() const;
    qreal selectedY() const;
    qreal selectedWidth() const;
    qreal selectedHeight() const;
    QString selectedText() const;
    QString selectedFontFamily() const;
    qreal selectedFontSizeRatio() const;
    QColor selectedColor() const;
    bool selectedEditable() const;
    QString selectedReason() const;

    Q_INVOKABLE QVariantList candidatesForPage(int page);
    Q_INVOKABLE QVariantList draftsForPage(int page) const;
    Q_INVOKABLE QVariantMap candidateAt(int page, qreal x, qreal y);
    Q_INVOKABLE bool selectCandidate(int page, const QString &objectId);
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE void beginSelectedTextEdit();
    Q_INVOKABLE void updateSelectedTextPreview(const QString &text);
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
    void statusMessage(const QString &message);

private:
    struct Candidate {
        int page = -1;
        QString objectId;
        QString kind;
        QString label;
        QRectF rect;
        QString text;
        QString fontFamily;
        qreal fontSizeRatio = 0.0;
        QColor color = Qt::black;
        bool editable = false;
        bool limited = false;
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
        bool hadAfter = false;
        QString afterText;
    };

    void resetSession();
    void ensureCacheRevision();
    const QVector<Candidate> &ensurePageCandidates(int page);
    QVector<Candidate> scanPage(int page) const;
    void appendPageObjectCandidates(FPDF_PAGE page,
                                    FPDF_PAGEOBJECT pageObject,
                                    const QString &path,
                                    FPDF_TEXTPAGE textPage,
                                    const FS_MATRIX &containerTransform,
                                    const QSizeF &pageSize,
                                    QVector<Candidate> &candidates) const;
    void appendAnnotationCandidates(FPDF_PAGE page,
                                    int pageIndex,
                                    const QSizeF &pageSize,
                                    QVector<Candidate> &candidates) const;
    const Candidate *findCandidate(int page, const QString &objectId);
    QVariantMap candidateMap(const Candidate &candidate, const QString &text) const;
    bool documentAllowsEditing() const;
    QString draftKey(int page, const QString &objectId) const;
    void applyDraftState(const Command &command, bool useAfterState);
    void updateSelectedTextFromDraft();
    void setSelection(const Candidate *candidate);

    QPointer<PDFDocument> m_document;
    Tool m_tool = BrowseTool;
    int m_cachedRevision = -1;
    QHash<int, QVector<Candidate>> m_pageCandidates;
    QHash<QString, Draft> m_drafts;
    QVector<Command> m_undoStack;
    QVector<Command> m_redoStack;
    Candidate m_selectedCandidate;
    bool m_hasSelection = false;
    QString m_selectedText;
    bool m_textEditActive = false;
    Command m_textEditCommand;
};
