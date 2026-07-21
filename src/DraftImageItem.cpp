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

void DraftImageItem::paint(QPainter *painter)
{
    if (m_image.isNull()) {
        return;
    }

    painter->setRenderHint(QPainter::SmoothPixmapTransform, smooth());
    painter->drawImage(boundingRect(), m_image);
}
