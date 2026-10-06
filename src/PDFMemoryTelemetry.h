#pragma once

#ifndef TREPDF_ENABLE_MEMORY_TELEMETRY
#define TREPDF_ENABLE_MEMORY_TELEMETRY 0
#endif

#include <QObject>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#if TREPDF_ENABLE_MEMORY_TELEMETRY
#include "PDFRasterBudget.h"
#include <array>
#include <memory>
class QImage;
class PDFEditController;
class PDFSearchController;
class PDFDocumentSession;
class PDFPageItem;
#endif

// Estimated retained application resources plus sampled macOS process/heap memory.
// Process, malloc heap and resource estimates are independent snapshot groups.
// RasterBudget/LeaseBytes is a conservative estimate, not process-resident bytes.
class PDFMemoryTelemetry : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(bool enabled READ enabled CONSTANT FINAL)
    Q_PROPERTY(QVariantMap snapshot READ snapshot NOTIFY snapshotChanged FINAL)

public:
    explicit PDFMemoryTelemetry(QObject *parent = nullptr);
    static constexpr bool enabled() { return TREPDF_ENABLE_MEMORY_TELEMETRY != 0; }
    static QVariantMap snapshot();
    Q_INVOKABLE void resetPeaks();
    static void startLogging(QObject *owner);

#if TREPDF_ENABLE_MEMORY_TELEMETRY
    enum Metric {
        PendingImageBytes, TextureCount, TileCount, VisiblePageItemCount,
        CandidatePageCount, CandidateBytes, DraftCount, DraftBytes, DraftImageBytes,
        UndoCommandCount, UndoBytes, UndoImageBytes, RedoCommandCount, RedoBytes,
        RedoImageBytes, SearchResultCount, SearchRectangleCount, SearchBytes,
        SessionCount, WorkerDocumentCount, MetricCount
    };
    using Values = std::array<qint64, MetricCount>;

    // Thread safe; these never emit signals, log, or acquire the PDFium lock.
    static void add(Metric metric, qint64 delta);
    static void setSource(const void *owner, const Values &values);
    static void removeSource(const void *owner);
    static void resetAllPeaks();

    class ImageTicket final {
    public:
        explicit ImageTicket(qint64 bytes, const PDFRasterBudget::Lease &lease = {});
        ~ImageTicket();
        ImageTicket(const ImageTicket &) = delete;
        ImageTicket &operator=(const ImageTicket &) = delete;
    private:
        qint64 m_bytes;
    };
    using PendingImage = std::shared_ptr<ImageTicket>;
    // A weak association observes coverage without retaining the image/lease.
    static PendingImage trackImage(const QImage &image, const PDFRasterBudget::Lease &lease = {});

    // GUI-thread observers; they only inspect already-materialized containers.
    static void track(PDFEditController *controller);
    static void track(PDFSearchController *controller);
    static void track(PDFDocumentSession *session);
    static void track(PDFPageItem *item);

private:
    friend class PDFEditController;
    static void updateEditController(PDFEditController *controller);
    static void updateSearchController(PDFSearchController *controller);
#endif

signals:
    void snapshotChanged();
};
