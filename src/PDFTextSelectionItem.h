#pragma once

#include <QByteArray>
#include <QPointer>
#include <QQuickItem>
#include <QRectF>
#include <QTimer>
#include <QVector>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <memory>

class PDFDocument;
class PDFTextSelectionController;

class PDFTextSelectionItem : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(PDFDocument *document READ document WRITE setDocument
                   NOTIFY documentChanged FINAL)
    Q_PROPERTY(PDFTextSelectionController *controller READ controller WRITE setController
                   NOTIFY controllerChanged FINAL)
    Q_PROPERTY(int page READ page WRITE setPage NOTIFY pageChanged FINAL)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged FINAL)
    Q_PROPERTY(bool ready READ ready NOTIFY dataChanged FINAL)
    Q_PROPERTY(bool limited READ limited NOTIFY dataChanged FINAL)

public:
    explicit PDFTextSelectionItem(QQuickItem *parent = nullptr);
    ~PDFTextSelectionItem() override;

    PDFDocument *document() const;
    PDFTextSelectionController *controller() const;
    int page() const;
    bool active() const;
    bool ready() const;
    bool limited() const;

    void setDocument(PDFDocument *document);
    void setController(PDFTextSelectionController *controller);
    void setPage(int page);
    void setActive(bool active);

signals:
    void documentChanged();
    void controllerChanged();
    void pageChanged();
    void activeChanged();
    void dataChanged();
    void selectionStarted();
    void contextMenuRequested(QPointF position);

protected:
    bool event(QEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseUngrabEvent() override;
    void keyPressEvent(QKeyEvent *event) override;
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override;

private:
    friend class PDFTextSelectionController;
    struct Character {
        int offset = 0;
        int length = 0;
        QRectF rect;
        bool hitTestable = false;
    };
    struct PageData {
        QString text;
        QVector<Character> characters;
        QVector<QRectF> blockedRects;
        bool ready = false;
        bool limited = false;
    };
    struct PendingWork {
        QString filePath;
        QByteArray password;
        int revision = -1;
        int displayPage = -1;
        int sourcePage = -1;
        QSizeF pageSize;
        std::shared_ptr<std::atomic_bool> cancellation;
    };

    static PageData extractPage(const PendingWork &work);
    void refresh();
    void cancelWork();
    void dispatchPendingWork();
    bool canCopy() const;
    void clearLocalSelection();
    void selectRange(int first, int last);
    int characterAt(const QPointF &position, bool nearest = false) const;
    bool blockedAt(const QPointF &position) const;
    bool selectedAt(const QPointF &position) const;
    QRectF itemRect(const QRectF &normalizedRect) const;

    QPointer<PDFDocument> m_document;
    QPointer<PDFTextSelectionController> m_controller;
    int m_page = 0;
    bool m_active = false;
    PageData m_data;
    int m_anchor = -1;
    int m_first = -1;
    int m_last = -1;
    QPointF m_pressPosition;
    bool m_dragging = false;
    std::shared_ptr<std::atomic_bool> m_cancellation;
    std::shared_ptr<PendingWork> m_pendingWork;
    QTimer m_dispatchRetry;
};
