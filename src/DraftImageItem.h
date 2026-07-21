#pragma once

#include <QImage>
#include <QQuickPaintedItem>
#include <QVariant>
#include <QtQml/qqmlregistration.h>

class DraftImageItem : public QQuickPaintedItem
{
    Q_OBJECT
    QML_NAMED_ELEMENT(DraftImageView)
    Q_PROPERTY(QVariant sourceImage READ sourceImage WRITE setSourceImage NOTIFY sourceImageChanged FINAL)

public:
    explicit DraftImageItem(QQuickItem *parent = nullptr);

    QVariant sourceImage() const;
    void setSourceImage(const QVariant &sourceImage);

    void paint(QPainter *painter) override;

signals:
    void sourceImageChanged();

private:
    QImage m_image;
};
