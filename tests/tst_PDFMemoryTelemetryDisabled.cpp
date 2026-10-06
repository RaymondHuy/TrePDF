#include "PDFMemoryTelemetry.h"
#include <QCoreApplication>
#include <QTest>
#include <QTimer>

class PDFMemoryTelemetryDisabledTest : public QObject
{
    Q_OBJECT
private slots:
    void hasNoTimerOrSnapshot()
    {
        static_assert(!PDFMemoryTelemetry::enabled());
        QObject owner;
        PDFMemoryTelemetry telemetry(&owner);
        PDFMemoryTelemetry::startLogging(&owner);
        telemetry.resetPeaks();
        QVERIFY(telemetry.snapshot().isEmpty());
        QVERIFY(owner.findChildren<QTimer *>().isEmpty());
        QVERIFY(!QCoreApplication::instance()->findChild<QTimer *>(
            QStringLiteral("trepdfMemoryTelemetryTimer")));
    }
};
QTEST_GUILESS_MAIN(PDFMemoryTelemetryDisabledTest)
#include "tst_PDFMemoryTelemetryDisabled.moc"
