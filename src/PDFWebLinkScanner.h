#pragma once

#include <QByteArray>
#include <QRectF>
#include <QString>
#include <QVector>

#include <atomic>
#include <memory>

#include <fpdfview.h>

namespace PDFWebLinks {

inline constexpr int MaximumUrlLength = 4096;
inline constexpr int MaximumLinks = 5000;
inline constexpr int MaximumRectanglesPerLink = 32;
inline constexpr int MaximumRectanglesPerPage = 512;
inline constexpr int MaximumRectangles = 20000;
inline constexpr int MaximumAnnotationsPerPage = 4096;
inline constexpr int MaximumExistingRectanglesPerPage = 8192;

struct Link {
    // Source indexes remain stable while the session inserts/deletes pages.
    int sourcePage = -1;
    QString url;
    // Top-left normalized visible-page coordinates, including intrinsic PDF
    // rotation and CropBox, but excluding the viewer's transient rotation.
    QVector<QRectF> rects;

    bool operator==(const Link &other) const
    {
        return sourcePage == other.sourcePage
            && url == other.url && rects == other.rects;
    }
};

struct ScanResult {
    QVector<Link> links;
    bool limited = false;
    bool failed = false;
};

struct SourceLink {
    int sourcePage = -1;
    int annotationIndex = -1;
    QByteArray fingerprint;

    bool operator==(const SourceLink &other) const
    {
        return sourcePage == other.sourcePage
            && annotationIndex == other.annotationIndex
            && fingerprint == other.fingerprint;
    }
};

struct SourceScanResult {
    QVector<SourceLink> links;
    bool limited = false;
    bool failed = false;
};

// Returns an absolute, fully encoded HTTP(S) URL without credentials, or empty.
// Both input UTF-16 length and encoded output length are bounded.
QString validWebUrl(const QString &value);

// Reads saved source text using a separate document; no networking or mutation.
// PDFium must already be initialized by PDFDocument. The caller must not hold
// the process lock. Failure/cancellation returns no links; limited results may
// contain complete, validated links from the bounded part of the scan.
ScanResult scan(const QString &filePath,
                const std::shared_ptr<std::atomic_bool> &cancellation);

// Enumerates removable HTTP(S) URI annotations without loading page text.
// Protected, malformed, or relationship-bearing annotations are retained.
SourceScanResult scanSourceLinks(
    const QString &filePath,
    const std::shared_ptr<std::atomic_bool> &cancellation);

// Shared with saving to revalidate source identity before and after unrelated
// annotation edits. The caller owns the document/page and process lock.
SourceScanResult sourceLinksForPage(
    FPDF_DOCUMENT document, FPDF_PAGE page, int sourcePage,
    const std::shared_ptr<std::atomic_bool> &cancellation = {});

} // namespace PDFWebLinks
