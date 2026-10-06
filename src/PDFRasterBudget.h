#pragma once

#include <QtTypes>

#include <functional>
#include <memory>

namespace PDFRasterBudget {

inline constexpr qint64 limitBytes = 256LL * 1024 * 1024;

namespace detail {
struct State;
}

class Budget;

struct Statistics {
    qint64 currentLeaseBytes = 0;
    qint64 peakLeaseBytes = 0;
    qint64 leaseCount = 0;
    qint64 peakLeaseCount = 0;
};

// One lease follows a raster through worker allocation, queued delivery and
// scene-graph texture ownership. Copying a Lease does not charge it again.
class Reservation final
{
public:
    ~Reservation() noexcept;

    Reservation(const Reservation &) = delete;
    Reservation &operator=(const Reservation &) = delete;
    Reservation(Reservation &&) = delete;
    Reservation &operator=(Reservation &&) = delete;

    qint64 bytes() const noexcept;

private:
    friend class Budget;
    explicit Reservation(std::shared_ptr<detail::State> state);

    std::shared_ptr<detail::State> m_state;
    qint64 m_bytes = 0;
};

using Lease = std::shared_ptr<Reservation>;

class Budget final
{
public:
    explicit Budget(qint64 maximumBytes = limitBytes);
    ~Budget();

    Budget(const Budget &) = delete;
    Budget &operator=(const Budget &) = delete;
    Budget(Budget &&) = delete;
    Budget &operator=(Budget &&) = delete;

    // Charges twice the image size to cover CPU pixels and their GPU texture.
    // Invalid sizes, overflow and insufficient capacity return an empty lease.
    // This is an accounting limit, not a limit on PDFium/driver allocations.
    Lease tryReserve(qint64 imageBytes);
    bool owns(const Lease &lease) const noexcept;
    qint64 usedBytes() const;
    qint64 peakBytes() const;
    qint64 currentLeaseBytes() const { return usedBytes(); }
    qint64 peakLeaseBytes() const { return peakBytes(); }
    qint64 leaseCount() const;
    Statistics statistics() const;
    // Debug reset rebases peaks to live reservations; it never releases capacity.
    void resetPeaks();

    // Runs after capacity is released, without the budget mutex held, on the
    // releasing thread. GUI work must be queued by the caller. Passing an empty
    // callback disables notification; already captured callbacks may still run.
    void setReleaseCallback(std::function<void()> callback);

private:
    std::shared_ptr<detail::State> m_state;
};

Budget &globalBudget();
Lease tryReserve(qint64 imageBytes);
qint64 usedBytes();
qint64 peakBytes();
qint64 currentLeaseBytes();
qint64 peakLeaseBytes();
qint64 leaseCount();
void setReleaseCallback(std::function<void()> callback);

} // namespace PDFRasterBudget
