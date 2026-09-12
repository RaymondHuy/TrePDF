#include "DraftImageItem.h"

#include <QPainter>

DraftImageItem::DraftImageItem(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    setFillColor(Qt::transparent);
}

QVariant DraftImageItem::sourceImage() const
{
    return m_image;
}

void DraftImageItem::setSourceImage(const QVariant &sourceImage)
{
    const QImage image = sourceImage.value<QImage>();
    if (m_image.cacheKey() == image.cacheKey()) {
        return;
    }

    m_image = image;
    update();
    emit sourceImageChanged();
}

int DraftImageItem::imageQuarterTurns() const
{
    return m_imageQuarterTurns;
}

void DraftImageItem::setImageQuarterTurns(int quarterTurns)
{
    const int normalized = ((quarterTurns % 4) + 4) % 4;
    if (m_imageQuarterTurns == normalized) {
        return;
    }

    m_imageQuarterTurns = normalized;
    update();
    emit imageQuarterTurnsChanged();
}

void DraftImageItem::paint(QPainter *painter)
{
    if (m_image.isNull()) {
        return;
    }

    const QRectF bounds = boundingRect();
    const QSizeF unrotatedSize = m_imageQuarterTurns % 2 == 0
        ? bounds.size() : QSizeF(bounds.height(), bounds.width());
    painter->save();
    painter->setRenderHint(QPainter::SmoothPixmapTransform, smooth());
    painter->translate(bounds.center());
    painter->rotate(m_imageQuarterTurns * 90.0);
    painter->drawImage(
        QRectF(QPointF(-unrotatedSize.width() / 2.0, -unrotatedSize.height() / 2.0),
               unrotatedSize),
        m_image);
    painter->restore();
}
