#pragma once

#include <QObject>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QUrl>
#include <QVector>
#include <QtQml/qqmlregistration.h>

#include <fpdfview.h>

class PDFEditController;

class PDFDocument : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QUrl source READ source NOTIFY sourceChanged FINAL)
    Q_PROPERTY(int pageCount READ pageCount NOTIFY pageCountChanged FINAL)
    Q_PROPERTY(int currentPage READ currentPage WRITE setCurrentPage NOTIFY currentPageChanged FINAL)
    Q_PROPERTY(bool loaded READ isLoaded NOTIFY loadedChanged FINAL)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged FINAL)
    Q_PROPERTY(qreal currentPageWidth READ currentPageWidth NOTIFY currentPageSizeChanged FINAL)
    Q_PROPERTY(qreal currentPageHeight READ currentPageHeight NOTIFY currentPageSizeChanged FINAL)
    Q_PROPERTY(qreal maximumPageWidth READ maximumPageWidth NOTIFY pageCountChanged FINAL)

public:
    explicit PDFDocument(QObject *parent = nullptr);
    ~PDFDocument() override;

    QUrl source() const;
    int pageCount() const;
    int currentPage() const;
    bool isLoaded() const;
    QString statusText() const;
    qreal currentPageWidth() const;
    qreal currentPageHeight() const;
    qreal maximumPageWidth() const;

    Q_INVOKABLE bool open(const QUrl &source);
    Q_INVOKABLE void close();
    Q_INVOKABLE void setCurrentPage(int page);
    Q_INVOKABLE void nextPage();
    Q_INVOKABLE void previousPage();
    Q_INVOKABLE QSizeF pageSize(int page) const;

    QString renderFilePath() const;
    int renderRevision() const;

signals:
    void sourceChanged();
    void pageCountChanged();
    void currentPageChanged();
    void loadedChanged();
    void statusTextChanged();
    void currentPageSizeChanged();

private:
    friend class PDFEditController;

    void closeDocumentLocked();
    void setStatusText(const QString &statusText);
    void emitDocumentStateChanged();

    FPDF_DOCUMENT m_document = nullptr;
    QUrl m_source;
    QString m_filePath;
    QVector<QSizeF> m_pageSizes;
    qreal m_maximumPageWidth = 0.0;
    int m_pageCount = 0;
    int m_currentPage = 0;
    int m_revision = 0;
    bool m_allowsContentEditing = false;
    QString m_statusText;
};
