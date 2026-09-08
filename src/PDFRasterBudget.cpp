#include "PDFRasterBudget.h"

#include <QtAssert>

#include <algorithm>
#include <limits>
#include <mutex>
#include <new>
#include <utility>

namespace PDFRasterBudget {

namespace detail {

struct State {
    explicit State(qint64 maximum)
        : maximumBytes((std::max)(qint64{0}, maximum))
    {
    }

    std::mutex mutex;
    const qint64 maximumBytes;
    qint64 usedBytes = 0;
    qint64 peakBytes = 0;
    std::shared_ptr<const std::function<void()>> releaseCallback;
};

} // namespace detail

Reservation::Reservation(std::shared_ptr<detail::State> state)
    : m_state(std::move(state))
{
}

Reservation::~Reservation() noexcept
{
    // A not-yet-registered reservation can be destroyed if shared_ptr control
    // block allocation fails while tryReserve() still holds the budget mutex.
    if (m_bytes == 0) {
        return;
    }

    std::shared_ptr<const std::function<void()>> callback;
    {
        const std::lock_guard<std::mutex> locker(m_state->mutex);
        Q_ASSERT(m_state->usedBytes >= m_bytes);
        m_state->usedBytes -= m_bytes;
        callback = m_state->releaseCallback;
    }
    if (callback) {
        // A notification must never unwind through a worker or SG destructor.
        try {
            (*callback)();
        } catch (...) {
        }
    }
}

qint64 Reservation::bytes() const noexcept
{
    return m_bytes;
}

Budget::Budget(qint64 maximumBytes)
    : m_state(std::make_shared<detail::State>(maximumBytes))
{
}

Budget::~Budget() = default;

Lease Budget::tryReserve(qint64 imageBytes)
{
    if (imageBytes <= 0
        || imageBytes > (std::numeric_limits<qint64>::max)() / 2) {
        return {};
    }
    const qint64 chargedBytes = imageBytes * 2;
    const std::lock_guard<std::mutex> locker(m_state->mutex);
    if (chargedBytes > m_state->maximumBytes - m_state->usedBytes) {
        return {};
    }

    Lease lease;
    try {
        // Keep m_bytes zero until both object and control block exist, so an
        // allocation failure cannot attempt a recursive lock during cleanup.
        lease = Lease(new Reservation(m_state));
    } catch (const std::bad_alloc &) {
        return {};
    }
    m_state->usedBytes += chargedBytes;
    m_state->peakBytes = (std::max)(m_state->peakBytes, m_state->usedBytes);
    lease->m_bytes = chargedBytes;
    return lease;
}

qint64 Budget::usedBytes() const
{
    const std::lock_guard<std::mutex> locker(m_state->mutex);
    return m_state->usedBytes;
}

qint64 Budget::peakBytes() const
{
    const std::lock_guard<std::mutex> locker(m_state->mutex);
    return m_state->peakBytes;
}

void Budget::setReleaseCallback(std::function<void()> callback)
{
    std::shared_ptr<const std::function<void()>> replacement;
    if (callback) {
        replacement = std::make_shared<const std::function<void()>>(
            std::move(callback));
    }
    {
        const std::lock_guard<std::mutex> locker(m_state->mutex);
        // Destroy the previous callback outside the mutex as its captures may
        // themselves own leases from this budget.
        replacement.swap(m_state->releaseCallback);
    }
}

Budget &globalBudget()
{
    static Budget budget;
    return budget;
}

Lease tryReserve(qint64 imageBytes)
{
    return globalBudget().tryReserve(imageBytes);
}

qint64 usedBytes()
{
    return globalBudget().usedBytes();
}

qint64 peakBytes()
{
    return globalBudget().peakBytes();
}

void setReleaseCallback(std::function<void()> callback)
{
    globalBudget().setReleaseCallback(std::move(callback));
}

} // namespace PDFRasterBudget
