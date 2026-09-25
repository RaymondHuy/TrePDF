#pragma once

#include <QColor>
#include <QHash>
#include <QImage>
#include <QPainterPath>
#include <QSet>
#include <QString>
#include <QVector>
#include <atomic>
#include <memory>
#include <fpdfview.h>

class QPainter;

// Value-only snapshots shared by the GUI, scanner, renderer and save worker.
namespace PDFWatermarks {
inline constexpr char MarkName[] = "TrePDFWatermark";
inline constexpr int MaximumGroups = 32;
inline constexpr int MaximumTextLength = 200;
inline constexpr int MaximumPages = 10000;

struct Spec {
    QString id;
    bool imageMode = false;
    QString text;
    QString fontFamily;
    qreal fontSize = 48;
    QColor color = QColor("#808080");
    qreal opacity = 0.3;
    qreal rotation = -45;
    qreal scale = 0.4; // Image width as a fraction of the visible page width.
    qreal x = 0.5; // Alignment within the available space, with page margins.
    qreal y = 0.5;
    bool behind = false;
    QImage image;
    QPainterPath textPath; // Prepared on the GUI thread, in PDF points.
};

struct Group {
    Spec spec;
    QSet<int> pages; // Immutable source page identities, never display indexes.
    QHash<int, QByteArray> fingerprints;
};

struct Change {
    Spec spec;
    QSet<int> pages;
    QHash<int, QByteArray> originals;
    bool removed = false;
};

struct PageChange {
    Spec spec;
    QByteArray expectedFingerprint; // Empty for an addition on this page.
    bool add = false;
};

struct ScanResult {
    QVector<Group> groups;
    QString error;
    bool succeeded = false;
};

// prepareText and paint run on the GUI thread. PDFium functions below require
// the caller's process lock, except scan which owns bounded transactions.
bool prepareText(Spec &spec, QString &error);
bool validSpec(const Spec &spec, bool requireArtwork = true);
QTransform placement(const Spec &spec, const QSizeF &pageSize);
void paint(QPainter &painter, const Spec &spec, const QSizeF &pageSize);
bool hasMark(FPDF_PAGEOBJECT object);
ScanResult scan(const QString &path, const QByteArray &password,
                const std::shared_ptr<std::atomic_bool> &cancel);
// Validate the complete staging document against the same adoption/work limits
// before saving, so an accepted save remains discoverable after reopening.
bool validateDocument(FPDF_DOCUMENT document, QString &error);
// The preview document must be discarded after applying a nonempty batch.
// Existing source objects are deactivated in preview, removed on save.
bool apply(FPDF_DOCUMENT document, FPDF_PAGE page,
           const QVector<PageChange> &changes, bool preview, QString &error);
}
