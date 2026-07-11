#pragma once

#include <QMutex>

// PDFium lazily initializes process-wide font data while loading a page. The
// application otherwise keeps GUI and render-thread document handles separate,
// but those first page loads still need serialization.
inline QMutex &pdfiumPageLoadMutex()
{
    static QMutex mutex;
    return mutex;
}
