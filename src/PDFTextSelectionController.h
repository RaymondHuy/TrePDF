#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QtQml/qqmlregistration.h>

class PDFTextSelectionItem;

// One controller belongs to one viewport; only its current page owns a selection.
class PDFTextSelectionController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString selectedText READ selectedText NOTIFY selectionChanged FINAL)
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged FINAL)
    Q_PROPERTY(int selectedPage READ selectedPage NOTIFY selectionChanged FINAL)

public:
    explicit PDFTextSelectionController(QObject *parent = nullptr);

    QString selectedText() const;
    bool hasSelection() const;
    int selectedPage() const;

    Q_INVOKABLE void clear();
    Q_INVOKABLE bool copy();

signals:
    void selectionChanged();

private:
    friend class PDFTextSelectionItem;
    void setSelection(PDFTextSelectionItem *owner, const QString &text);
    void release(PDFTextSelectionItem *owner);

    QPointer<PDFTextSelectionItem> m_owner;
    QString m_selectedText;
    int m_selectedPage = -1;
};
