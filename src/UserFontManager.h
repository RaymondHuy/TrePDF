#pragma once

#include <QFont>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

// Imports private application fonts; it never installs fonts into the OS.
class UserFontManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString error READ error NOTIFY errorChanged FINAL)
    Q_PROPERTY(QString storageDirectory READ storageDirectory CONSTANT FINAL)
    Q_PROPERTY(QString previewFamily READ previewFamily NOTIFY previewChanged FINAL)
    Q_PROPERTY(QString previewError READ previewError NOTIFY previewChanged FINAL)
    Q_PROPERTY(QFont previewFont READ previewFont NOTIFY previewChanged FINAL)

public:
    explicit UserFontManager(QObject *parent = nullptr);
    ~UserFontManager() override;

    QString error() const { return m_error; }
    QString storageDirectory() const;
    QString previewFamily() const { return m_previewFamily; }
    QString previewError() const { return m_previewError; }
    QFont previewFont() const { return m_previewFont; }
    Q_INVOKABLE QString fileName(const QUrl &source) const;
    Q_INVOKABLE void clearError();
    Q_INVOKABLE bool preparePreview(const QUrl &source, bool bold, bool italic);
    Q_INVOKABLE void clearPreview();
    Q_INVOKABLE bool importFonts(const QString &family, const QUrl &regular,
                                const QUrl &bold, const QUrl &italic,
                                const QUrl &boldItalic);

signals:
    void errorChanged();
    void fontsChanged();
    void previewChanged();

private:
    bool fail(const QString &message);
    void replacePreview(const QString &family, const QString &error,
                        bool bold = false, bool italic = false);
    QString m_error;
    QString m_previewFamily;
    QString m_previewError;
    QFont m_previewFont;
    quint64 m_previewRevision = 0;
};
