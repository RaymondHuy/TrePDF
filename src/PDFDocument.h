#pragma once

#include <QByteArray>
#include <QObject>
#include <QSet>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QUrl>
#include <QVector>
#include <QtQml/qqmlregistration.h>

#include <fpdfview.h>

class PDFEditController;

struct PDFPageInsertion
{
    int page = -1;
    QSizeF size;

    bool operator==(const PDFPageInsertion &other) const
    {
        return page == other.page && size == other.size;
    }
};

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
    Q_PROPERTY(qreal maximumPageHeight READ maximumPageHeight NOTIFY pageCountChanged FINAL)

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
    qreal maximumPageHeight() const;

    Q_INVOKABLE bool open(const QUrl &source, const QString &password = QString());
    Q_INVOKABLE void close();
    Q_INVOKABLE void setCurrentPage(int page);
    Q_INVOKABLE void nextPage();
    Q_INVOKABLE void previousPage();
    Q_INVOKABLE QSizeF pageSize(int page) const;

    QString renderFilePath() const;
    QByteArray documentPassword() const;
    bool passwordRequired() const;
    int renderRevision() const;
    int sourcePageCount() const;
    int sourcePageIndex(int page) const;
    int displayPageIndexForSourcePage(int sourcePage) const;
    bool isTransientBlankPage(int page) const;
    bool allowsFormFilling() const;

signals:
    void sourceChanged();
    void pageCountChanged();
    void currentPageChanged();
    void loadedChanged();
    void statusTextChanged();
    void currentPageSizeChanged();
    void pageStructureChanged(bool currentPageMappingChanged);

private:
    friend class PDFEditController;

    void closeDocumentLocked();
    void setStatusText(const QString &statusText);
    void emitDocumentStateChanged();
    void setTransientPageStructure(const QSet<int> &deletedSourcePages,
                                   const QVector<PDFPageInsertion> &pageInsertions,
                                   int currentPage = -1);
    void updatePageMetrics();

    FPDF_DOCUMENT m_document = nullptr;
    QUrl m_source;
    QString m_filePath;
    QByteArray m_documentPassword;
    bool m_passwordRequired = false;
    QVector<QSizeF> m_sourcePageSizes;
    QVector<QSizeF> m_pageSizes;
    QVector<int> m_displaySourcePages;
    QVector<PDFPageInsertion> m_pageInsertions;
    qreal m_maximumPageWidth = 0.0;
    qreal m_maximumPageHeight = 0.0;
    int m_sourcePageCount = 0;
    int m_pageCount = 0;
    int m_currentPage = 0;
    int m_revision = 0;
    bool m_allowsContentEditing = false;
    bool m_allowsAnnotations = false;
    bool m_allowsPageAssembly = false;
    bool m_allowsFormFilling = false;
    QString m_statusText;
};
