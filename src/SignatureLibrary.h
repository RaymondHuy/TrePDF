#pragma once

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <QtQml/qqmlregistration.h>

class PDFEditController;

// Local visual signatures. These images do not create certificate signatures.
class SignatureLibrary : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QUrl previewUrl READ previewUrl NOTIFY previewChanged FINAL)
    Q_PROPERTY(bool hasPreview READ hasPreview NOTIFY previewChanged FINAL)
    Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged FINAL)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged FINAL)

public:
    explicit SignatureLibrary(QObject *parent = nullptr);
    explicit SignatureLibrary(const QString &storagePath, QObject *parent = nullptr);

    QUrl previewUrl() const { return m_previewUrl; }
    bool hasPreview() const { return !m_preview.isNull(); }
    QVariantList entries() const;
    QString error() const { return m_error; }

    Q_INVOKABLE bool prepareText(const QString &text, int style, const QColor &color);
    Q_INVOKABLE bool prepareDrawing(const QVariantList &strokes,
                                   const QColor &color, qreal strokeWidth);
    Q_INVOKABLE bool prepareImage(const QUrl &source, bool removeWhite);
    Q_INVOKABLE bool prepareDate(const QString &isoDate, int format);
    Q_INVOKABLE bool prepareCheck(bool cross);
    Q_INVOKABLE void clearPreview();
    Q_INVOKABLE bool savePreview(const QString &name, const QString &kind);
    Q_INVOKABLE bool selectEntry(const QString &id);
    Q_INVOKABLE bool removeEntry(const QString &id);
    Q_INVOKABLE QVariantMap place(PDFEditController *controller, int page,
                                 qreal x, qreal y, qreal widthPoints);

signals:
    void previewChanged();
    void entriesChanged();
    void errorChanged();

private:
    struct Entry {
        QString id;
        QString name;
        QString kind;
        QByteArray png;
    };

    bool setPreview(QImage image);
    bool fail(const QString &message);
    void setError(const QString &message);
    void load();
    bool persist(const QVector<Entry> &entries);

    QString m_storagePath;
    QVector<Entry> m_entries;
    QImage m_preview;
    QByteArray m_previewPng;
    QUrl m_previewUrl;
    QString m_error;
    bool m_storageReadable = true;
};
