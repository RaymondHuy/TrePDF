#pragma once

#include <QMutex>

// PDFium's public API is not thread-safe, even when callers use independent
// FPDF_DOCUMENT handles. Acquire exactly one PDFiumProcessLock at the outermost
// boundary of every PDFium transaction. Helpers called by that transaction must
// assume the lock is already held and must never acquire it again. Never keep the
// lock while emitting signals, posting queued callbacks, or waiting for another
// thread.
//
// The mutex intentionally lives until process exit. Render workers keep a
// thread-local PDFium document whose destructor can run while the thread pool is
// being torn down, after ordinary function-local statics might have been
// destroyed.
inline QMutex &pdfiumProcessMutex()
{
    static QMutex *const mutex = new QMutex;
    return *mutex;
}

inline bool &pdfiumProcessLockOwnedByCurrentThread()
{
    static thread_local bool owned = false;
    return owned;
}

class PDFiumProcessLock final
{
public:
    enum class Mode {
        Blocking,
        TryLock,
    };

    explicit PDFiumProcessLock(Mode mode = Mode::Blocking)
    {
        bool &ownedByCurrentThread = pdfiumProcessLockOwnedByCurrentThread();
        if (ownedByCurrentThread) {
            qFatal("Nested PDFiumProcessLock acquisition would deadlock; PDFium transactions must not be re-entrant.");
        }
        if (mode == Mode::Blocking) {
            pdfiumProcessMutex().lock();
            m_locked = true;
        } else {
            m_locked = pdfiumProcessMutex().tryLock();
        }
        if (m_locked) {
            ownedByCurrentThread = true;
        }
    }

    ~PDFiumProcessLock()
    {
        if (!m_locked) {
            return;
        }
        bool &ownedByCurrentThread = pdfiumProcessLockOwnedByCurrentThread();
        Q_ASSERT(ownedByCurrentThread);
        ownedByCurrentThread = false;
        pdfiumProcessMutex().unlock();
    }

    bool isLocked() const
    {
        return m_locked;
    }

    // Lock ownership is thread-affine, so moving the guard is intentionally
    // forbidden together with copying it.
    Q_DISABLE_COPY_MOVE(PDFiumProcessLock)

private:
    bool m_locked = false;
};

inline void assertPDFiumProcessLockHeld()
{
    const bool ownedByCurrentThread = pdfiumProcessLockOwnedByCurrentThread();
    Q_ASSERT(ownedByCurrentThread);
}

inline void assertPDFiumProcessLockNotHeld()
{
    const bool ownedByCurrentThread = pdfiumProcessLockOwnedByCurrentThread();
    Q_ASSERT(!ownedByCurrentThread);
}
