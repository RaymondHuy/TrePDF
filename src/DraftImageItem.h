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
    Q_PROPERTY(int imageQuarterTurns READ imageQuarterTurns WRITE setImageQuarterTurns NOTIFY imageQuarterTurnsChanged FINAL)

public:
    explicit DraftImageItem(QQuickItem *parent = nullptr);

    QVariant sourceImage() const;
    void setSourceImage(const QVariant &sourceImage);
    int imageQuarterTurns() const;
    void setImageQuarterTurns(int quarterTurns);

    void paint(QPainter *painter) override;

signals:
    void sourceImageChanged();
    void imageQuarterTurnsChanged();

private:
    QImage m_image;
    int m_imageQuarterTurns = 0;
};
