#include "PDFTextSelectionController.h"

#include "PDFTextSelectionItem.h"

#include <QClipboard>
#include <QGuiApplication>

PDFTextSelectionController::PDFTextSelectionController(QObject *parent)
    : QObject(parent)
{
}

QString PDFTextSelectionController::selectedText() const
{
    return m_selectedText;
}

bool PDFTextSelectionController::hasSelection() const
{
    return m_owner && !m_selectedText.isEmpty();
}

int PDFTextSelectionController::selectedPage() const
{
    return m_selectedPage;
}

void PDFTextSelectionController::clear()
{
    const QPointer<PDFTextSelectionItem> previous = m_owner;
    const bool changed = previous || !m_selectedText.isEmpty();
    m_owner.clear();
    m_selectedText.clear();
    m_selectedPage = -1;
    if (previous) {
        previous->clearLocalSelection();
    }
    if (changed) {
        emit selectionChanged();
    }
}

bool PDFTextSelectionController::copy()
{
    if (!hasSelection() || !m_owner->canCopy()) {
        clear();
        return false;
    }
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard) {
        return false;
    }
    clipboard->setText(m_selectedText, QClipboard::Clipboard);
    return true;
}

void PDFTextSelectionController::setSelection(PDFTextSelectionItem *owner,
                                             const QString &text)
{
    if (!owner || !owner->canCopy() || text.isEmpty()) {
        clear();
        return;
    }
    if (m_owner && m_owner != owner) {
        m_owner->clearLocalSelection();
    }
    const bool changed = m_owner != owner || m_selectedText != text
        || m_selectedPage != owner->page();
    m_owner = owner;
    m_selectedText = text;
    m_selectedPage = owner->page();
    if (changed) {
        emit selectionChanged();
    }
}

void PDFTextSelectionController::release(PDFTextSelectionItem *owner)
{
    if (m_owner == owner) {
        clear();
    }
}
