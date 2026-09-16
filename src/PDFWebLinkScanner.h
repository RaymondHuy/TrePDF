#pragma once

#include <QRectF>
#include <QString>
#include <QVector>

#include <atomic>
#include <memory>

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

// Returns an absolute, fully encoded HTTP(S) URL without credentials, or empty.
// Both input UTF-16 length and encoded output length are bounded.
QString validWebUrl(const QString &value);

// Reads saved source text using a separate document; no networking or mutation.
// PDFium must already be initialized by PDFDocument. The caller must not hold
// the process lock. Failure/cancellation returns no links; limited results may
// contain complete, validated links from the bounded part of the scan.
ScanResult scan(const QString &filePath,
                const std::shared_ptr<std::atomic_bool> &cancellation);

} // namespace PDFWebLinks
