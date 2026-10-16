#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>

// Worker-only OCR implementation. The caller initializes the application's
// PDFium runtime, owns the immutable source snapshot and commits the returned
// staging file only after checking that its session/revision is still current.
namespace PDFOCRPipeline {

inline constexpr int maximumPages = 1000;
inline constexpr int renderDpi = 300;
inline constexpr qint64 maximumRenderPixels = 20 * 1024 * 1024;
inline constexpr int maximumRenderDimension = 10000;
inline constexpr int maximumCharactersPerPage = 50000;
inline constexpr int maximumCharactersPerDocument = 2000000;

struct Request {
    QString sourcePath;
    QByteArray password;
    // Zero-based source indices, unique and in document order. Unselected pages
    // remain present and unchanged in the output document.
    QVector<int> sourcePages;
    QString language;
    QString tessdataPath;
    std::shared_ptr<std::atomic_bool> cancellation;
};

struct Result {
    enum Outcome { Succeeded, Cancelled, Failed, NoText };
    Outcome outcome = Failed;
    int recognizedPages = 0;
    int skippedPages = 0;
    int emptyPages = 0;
    QString error;
};

// Creates a searchable copy at an exclusively owned temporary destination.
// Source content streams are never regenerated: qpdf imports Tesseract's
// invisible Unicode text as an isolated Form XObject on each recognized page.
// Existing text causes the entire page to be skipped. Cancel/failure/NoText
// removes any partial staging output. No PDFium lock is held during OCR or
// progress callbacks; the callback receives the number of completed pages.
Result run(const Request &request, const QString &stagingOutputPath,
           const std::function<void(int)> &progress = {});

} // namespace PDFOCRPipeline
