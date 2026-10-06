#include "PDFMemoryTelemetry.h"

#if TREPDF_ENABLE_MEMORY_TELEMETRY
#include "PDFRasterBudget.h"
#include <QCoreApplication>
#include <QDebug>
#include <QHash>
#include <QImage>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <mutex>
#include <optional>
#if defined(Q_OS_MACOS)
#include <mach/mach.h>
#include <malloc/malloc.h>
#endif

namespace {
// Process peaks are sampled independently of heap and resource-budget estimates.
using ProcessValues = std::array<qint64, 3>;
constexpr const char *processNames[] = {
    "phys_footprint", "resident_size", "virtual_size"
};

std::optional<ProcessValues> processMemory()
{
#if defined(Q_OS_MACOS)
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_REV1_COUNT;
    const kern_return_t status = task_info(mach_task_self(), TASK_VM_INFO,
        reinterpret_cast<task_info_t>(&info), &count);
    if (status == KERN_SUCCESS && count >= TASK_VM_INFO_REV1_COUNT)
        return ProcessValues{qint64(info.phys_footprint), qint64(info.resident_size),
                             qint64(info.virtual_size)};
#endif
    // An unavailable reading must not look like a zero-byte process.
    return std::nullopt;
}

using MallocValues = std::array<qint64, 2>;
constexpr const char *mallocNames[] = {"mallocInUseBytes", "mallocReservedBytes"};

std::optional<MallocValues> mallocMemory()
{
#if defined(Q_OS_MACOS)
    malloc_statistics_t stats{};
    // nullptr asks libmalloc to sum every registered zone, not just the default
    // zone. Read counters only: never enumerate blocks or request pressure relief.
    malloc_zone_statistics(nullptr, &stats);
    return MallocValues{qint64(stats.size_in_use), qint64(stats.size_allocated)};
#else
    return std::nullopt;
#endif
}

struct State {
    struct PendingImage {
        qint64 bytes;
        std::weak_ptr<PDFRasterBudget::Reservation> lease;
    };
    std::mutex mutex;
    PDFMemoryTelemetry::Values current{};
    PDFMemoryTelemetry::Values peak{};
    QHash<const void *, PDFMemoryTelemetry::Values> sources;
    QHash<const void *, PendingImage> pendingImages;
    qint64 pendingUnleasedPeak = 0;
    qint64 pendingRasterCoveredPeak = 0;
    ProcessValues processPeak{};
    bool hasProcessPeak = false;
    MallocValues mallocPeak{};
    bool hasMallocPeak = false;
};

struct PendingCoverage {
    qint64 pendingUnleased;
    qint64 pendingRasterCovered;
};

PendingCoverage pendingCoverage(const State &s)
{
    qint64 covered = 0;
    for (const auto &image : s.pendingImages) {
        if (!image.lease.expired())
            covered += image.bytes;
    }
    using M = PDFMemoryTelemetry;
    const auto &v = s.current;
    const qint64 unleased = v[M::PendingImageBytes] - covered;
    return {unleased, covered};
}

PendingCoverage recordPendingCoverage(State &s)
{
    const auto result = pendingCoverage(s);
    s.pendingUnleasedPeak = (std::max)(s.pendingUnleasedPeak, result.pendingUnleased);
    s.pendingRasterCoveredPeak = (std::max)(s.pendingRasterCoveredPeak, result.pendingRasterCovered);
    return result;
}

State &state()
{
    // Worker thread-local PDFium handles and SG textures can outlive the app.
    // A small Debug-only process-lifetime registry keeps their teardown safe.
    static auto *value = new State;
    return *value;
}

QPointer<QTimer> &loggingTimer()
{
    static QPointer<QTimer> timer;
    return timer;
}

constexpr const char *names[] = {
    "pendingImageBytes", "textureCount", "tileCount", "visiblePageItemCount",
    "candidatePageCount", "candidateBytes", "draftCount", "draftBytes", "draftImageBytes",
    "undoCommandCount", "undoBytes", "undoImageBytes", "redoCommandCount", "redoBytes",
    "redoImageBytes", "searchResultCount", "searchRectangleCount", "searchBytes",
    "sessionCount", "workerDocumentCount"
};
static_assert(std::size(names) == PDFMemoryTelemetry::MetricCount);

QVariantMap reading(qint64 current, qint64 peak)
{
    return {{QStringLiteral("current"), current}, {QStringLiteral("peak"), peak}};
}

void logSnapshot()
{
    const auto data = PDFMemoryTelemetry::snapshot();
    const auto process = data.value(QStringLiteral("processMemory")).toMap();
    const auto heap = data.value(QStringLiteral("mallocHeap")).toMap();
    const auto resources = data.value(QStringLiteral("resourceEstimates")).toMap();
    const auto raster = resources.value(QStringLiteral("rasterBudget")).toMap();
    auto value = [](const QVariantMap &group, const char *name, const char *field = "current") {
        return group.value(QLatin1String(name)).toMap().value(QLatin1String(field)).toLongLong();
    };
    auto bytes = [&](const QVariantMap &group, const char *name) {
        if (!group.value(QLatin1String(name)).toMap().value(QStringLiteral("current")).isValid())
            return QStringLiteral("unavailable");
        return QStringLiteral("%1 MiB / peak %2 MiB")
            .arg(value(group, name) / (1024.0 * 1024.0), 0, 'f', 1)
            .arg(value(group, name, "peak") / (1024.0 * 1024.0), 0, 'f', 1);
    };
    auto count = [&](const QVariantMap &group, const char *name) {
        return QStringLiteral("%1 / peak %2").arg(value(group, name)).arg(value(group, name, "peak"));
    };
    qInfo().noquote().nospace()
        << "[Memory] Separate views; do not add or subtract groups\n"
        << "[Process memory]\n"
        << "phys_footprint: " << bytes(process, "phys_footprint") << " (primary process-memory KPI)\n"
        << "resident_size:  " << bytes(process, "resident_size") << '\n'
        << "virtual_size:   " << bytes(process, "virtual_size") << '\n'
        << "[Malloc heap - all zones]\n"
        << "MallocInUse:    " << bytes(heap, "mallocInUseBytes") << " (actual malloc heap usage)\n"
        << "MallocReserved: " << bytes(heap, "mallocReservedBytes") << " (allocator-reserved memory)\n"
        << "[TrePDF resources - estimates and counts, not process-resident bytes]\n"
        << "RasterBudget/LeaseBytes: " << bytes(raster, "leaseBytes") << " (conservative CPU + GPU estimate; may outlive CPU QImage)\n"
        << "RasterBudget/LeaseCount: " << count(raster, "leaseCount") << '\n'
        << "PendingImg:  " << bytes(resources, "pendingImageBytes") << '\n'
        << "PendingUnleased: " << bytes(resources, "pendingImageUnleasedBytes") << '\n'
        << "PendingRasterCovered: " << bytes(resources, "pendingImageRasterCoveredBytes") << " (overlap diagnostic)\n"
        << "Tiles:       " << count(resources, "tileCount") << '\n'
        << "Textures:    " << count(resources, "textureCount") << '\n'
        << "Visible:     " << count(resources, "visiblePageItemCount") << '\n'
        << "Candidates:  " << bytes(resources, "candidateBytes") << " / pages " << count(resources, "candidatePageCount") << '\n'
        << "Drafts:      " << bytes(resources, "draftBytes") << " / count " << count(resources, "draftCount") << '\n'
        << "DraftImages: " << bytes(resources, "draftImageBytes") << '\n'
        << "Undo:        " << bytes(resources, "undoBytes") << " / commands " << count(resources, "undoCommandCount") << '\n'
        << "UndoImages:  " << bytes(resources, "undoImageBytes") << '\n'
        << "Redo:        " << bytes(resources, "redoBytes") << " / commands " << count(resources, "redoCommandCount") << '\n'
        << "RedoImages:  " << bytes(resources, "redoImageBytes") << '\n'
        << "Search:      " << bytes(resources, "searchBytes") << " / results " << count(resources, "searchResultCount")
        << " / rectangles " << count(resources, "searchRectangleCount") << '\n'
        << "Sessions:    " << count(resources, "sessionCount") << '\n'
        << "WorkerDocs:  " << count(resources, "workerDocumentCount") << '\n'
        << "Thumbnails:  shared raster/image/texture resources (no separate cache)";
}
} // namespace

void PDFMemoryTelemetry::add(Metric metric, qint64 delta)
{
    auto &s = state();
    const std::lock_guard<std::mutex> lock(s.mutex);
    Q_ASSERT(s.current[metric] + delta >= 0);
    s.current[metric] += delta;
    s.peak[metric] = (std::max)(s.peak[metric], s.current[metric]);
}

void PDFMemoryTelemetry::setSource(const void *owner, const Values &values)
{
    auto &s = state();
    const std::lock_guard<std::mutex> lock(s.mutex);
    auto &previous = s.sources[owner];
    for (size_t i = 0; i < MetricCount; ++i) {
        Q_ASSERT(values[i] >= 0);
        s.current[i] += values[i] - previous[i];
        s.peak[i] = (std::max)(s.peak[i], s.current[i]);
    }
    previous = values;
}

void PDFMemoryTelemetry::removeSource(const void *owner)
{
    auto &s = state();
    const std::lock_guard<std::mutex> lock(s.mutex);
    const auto found = s.sources.constFind(owner);
    if (found == s.sources.cend())
        return;
    for (size_t i = 0; i < MetricCount; ++i)
        s.current[i] -= found.value()[i];
    s.sources.remove(owner);
}

void PDFMemoryTelemetry::resetAllPeaks()
{
    auto process = processMemory();
    const auto heap = mallocMemory();
    // Never hold telemetry and raster mutexes simultaneously.
    PDFRasterBudget::globalBudget().resetPeaks();
    auto &s = state();
    const std::lock_guard<std::mutex> lock(s.mutex);
    s.peak = s.current;
    const auto coverage = pendingCoverage(s);
    s.pendingUnleasedPeak = coverage.pendingUnleased;
    s.pendingRasterCoveredPeak = coverage.pendingRasterCovered;
    s.processPeak = process.value_or(ProcessValues{});
    s.hasProcessPeak = process.has_value();
    s.mallocPeak = heap.value_or(MallocValues{});
    s.hasMallocPeak = heap.has_value();
}

PDFMemoryTelemetry::ImageTicket::ImageTicket(qint64 bytes, const PDFRasterBudget::Lease &lease)
    : m_bytes(bytes)
{
    auto &s = state();
    const std::lock_guard<std::mutex> lock(s.mutex);
    // A local/test budget is not part of the process-wide raster charge. Nor
    // can a too-small lease stand in for this image's full pixel buffer.
    const auto coveringLease = PDFRasterBudget::globalBudget().owns(lease)
            && lease->bytes() / 2 >= bytes
        ? std::weak_ptr<PDFRasterBudget::Reservation>(lease)
        : std::weak_ptr<PDFRasterBudget::Reservation>();
    s.pendingImages.insert(this, {bytes, coveringLease});
    s.current[PendingImageBytes] += bytes;
    s.peak[PendingImageBytes] = (std::max)(s.peak[PendingImageBytes], s.current[PendingImageBytes]);
}

PDFMemoryTelemetry::ImageTicket::~ImageTicket()
{
    auto &s = state();
    const std::lock_guard<std::mutex> lock(s.mutex);
    s.current[PendingImageBytes] -= m_bytes;
    s.pendingImages.remove(this);
}

PDFMemoryTelemetry::PendingImage PDFMemoryTelemetry::trackImage(
    const QImage &image, const PDFRasterBudget::Lease &lease)
{
    return image.isNull() ? PendingImage{} : std::make_shared<ImageTicket>(
        qint64(image.bytesPerLine()) * image.height(), lease);
}
#endif

PDFMemoryTelemetry::PDFMemoryTelemetry(QObject *parent) : QObject(parent)
{
#if TREPDF_ENABLE_MEMORY_TELEMETRY
    startLogging(QCoreApplication::instance());
    if (loggingTimer())
        connect(loggingTimer(), &QTimer::timeout, this, &PDFMemoryTelemetry::snapshotChanged);
#endif
}

void PDFMemoryTelemetry::startLogging(QObject *owner)
{
#if TREPDF_ENABLE_MEMORY_TELEMETRY
    Q_ASSERT(!owner || owner->thread() == QThread::currentThread());
    if (!owner || loggingTimer())
        return;
    auto *timer = new QTimer(owner);
    timer->setObjectName(QStringLiteral("trepdfMemoryTelemetryTimer"));
    timer->setTimerType(Qt::VeryCoarseTimer);
    timer->setInterval(5000);
    connect(timer, &QTimer::timeout, timer, &logSnapshot);
    loggingTimer() = timer;
    timer->start();
#else
    Q_UNUSED(owner);
#endif
}

QVariantMap PDFMemoryTelemetry::snapshot()
{
#if TREPDF_ENABLE_MEMORY_TELEMETRY
    auto process = processMemory();
    // Query the allocator before locking our registry (which itself allocates).
    const auto heap = mallocMemory();
    const auto raster = PDFRasterBudget::globalBudget().statistics();
    Values current, peak;
    qint64 pendingUnleased, pendingUnleasedPeak, pendingCovered, pendingCoveredPeak;
    ProcessValues processPeak;
    bool hasProcessPeak;
    MallocValues mallocPeak;
    bool hasMallocPeak;
    {
        auto &s = state();
        const std::lock_guard<std::mutex> lock(s.mutex);
        current = s.current;
        peak = s.peak;
        const auto coverage = recordPendingCoverage(s);
        pendingUnleased = coverage.pendingUnleased;
        pendingUnleasedPeak = s.pendingUnleasedPeak;
        pendingCovered = coverage.pendingRasterCovered;
        pendingCoveredPeak = s.pendingRasterCoveredPeak;
        if (process) {
            for (size_t i = 0; i < process->size(); ++i)
                s.processPeak[i] = (std::max)(s.processPeak[i], (*process)[i]);
            s.hasProcessPeak = true;
        }
        processPeak = s.processPeak;
        hasProcessPeak = s.hasProcessPeak;
        if (heap) {
            for (size_t i = 0; i < heap->size(); ++i)
                s.mallocPeak[i] = (std::max)(s.mallocPeak[i], (*heap)[i]);
            s.hasMallocPeak = true;
        }
        mallocPeak = s.mallocPeak;
        hasMallocPeak = s.hasMallocPeak;
    }
    QVariantMap resources;
    for (size_t i = 0; i < MetricCount; ++i)
        resources.insert(QLatin1String(names[i]), reading(current[i], peak[i]));
    resources.insert(QStringLiteral("pendingImageUnleasedBytes"), reading(pendingUnleased, pendingUnleasedPeak));
    resources.insert(QStringLiteral("pendingImageRasterCoveredBytes"), reading(pendingCovered, pendingCoveredPeak));
    resources.insert(QStringLiteral("rasterBudget"), QVariantMap{
        {QStringLiteral("leaseBytes"), reading(raster.currentLeaseBytes, raster.peakLeaseBytes)},
        {QStringLiteral("leaseCount"), reading(raster.leaseCount, raster.peakLeaseCount)}});
    resources.insert(QStringLiteral("estimatedOnly"), true);
    resources.insert(QStringLiteral("thumbnailSeparateCache"), false);
    QVariantMap processGroup{
        {QStringLiteral("available"), process.has_value()},
        {QStringLiteral("primaryMetric"), QStringLiteral("phys_footprint")}};
    for (size_t i = 0; i < std::size(processNames); ++i) {
        processGroup.insert(QLatin1String(processNames[i]), QVariantMap{
            {QStringLiteral("current"), process ? QVariant((*process)[i]) : QVariant()},
            {QStringLiteral("peak"), hasProcessPeak ? QVariant(processPeak[i]) : QVariant()}});
    }
    QVariantMap heapGroup{{QStringLiteral("available"), heap.has_value()}};
    for (size_t i = 0; i < std::size(mallocNames); ++i) {
        heapGroup.insert(QLatin1String(mallocNames[i]), QVariantMap{
            {QStringLiteral("current"), heap ? QVariant((*heap)[i]) : QVariant()},
            {QStringLiteral("peak"), hasMallocPeak ? QVariant(mallocPeak[i]) : QVariant()}});
    }
    return {{QStringLiteral("processMemory"), processGroup},
            {QStringLiteral("mallocHeap"), heapGroup},
            {QStringLiteral("resourceEstimates"), resources}};
#else
    return {};
#endif
}

void PDFMemoryTelemetry::resetPeaks()
{
#if TREPDF_ENABLE_MEMORY_TELEMETRY
    resetAllPeaks();
    emit snapshotChanged();
#endif
}
