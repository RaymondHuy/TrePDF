#pragma once

#include "SpreadsheetWorkbook.h"

#include <QString>
#include <QVector>

#include <fpdfview.h>

#include <atomic>
#include <memory>

class PDFTableExtractor final
{
public:
    // The caller owns a separate worker document and holds PDFiumProcessLock
    // for this whole call. No PDFium handle or UI object escapes this boundary.
    // Returns zero tables for prose/scans; false for cancellation, safety caps,
    // or a detected grid that cannot be extracted without losing information.
    static bool extractPage(
        FPDF_DOCUMENT document,
        int sourcePage,
        QVector<SpreadsheetSheet> &tables,
        const std::shared_ptr<std::atomic_bool> &cancellation,
        QString &error);
};
