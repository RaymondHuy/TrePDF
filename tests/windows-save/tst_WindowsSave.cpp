#include "PDFiumDocument.h"
#include "PDFiumSynchronization.h"
#include "PDFAtomicSaveFile.h"

#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

#include <fpdf_edit.h>
#include <fpdf_save.h>
#include <fpdf_text.h>

#include <windows.h>

#include <utility>

namespace {

struct MemoryWriter : FPDF_FILEWRITE {
    QByteArray bytes;
    MemoryWriter()
    {
        version = 1;
        WriteBlock = [](FPDF_FILEWRITE *writer, const void *data, unsigned long size) {
            static_cast<MemoryWriter *>(writer)->bytes.append(
                static_cast<const char *>(data), qsizetype(size));
            return 1;
        };
    }
};

QByteArray pdfBytes(const QString &text, double width)
{
    assertPDFiumProcessLockHeld();
    FPDF_DOCUMENT document = FPDF_CreateNewDocument();
    if (!document)
        return {};
    const auto closeDocument = qScopeGuard([&] { FPDF_CloseDocument(document); });
    FPDF_PAGE page = FPDFPage_New(document, 0, width, 400);
    if (!page)
        return {};
    const auto closePage = qScopeGuard([&] { FPDF_ClosePage(page); });
    FPDF_PAGEOBJECT object = FPDFPageObj_NewTextObj(document, "Helvetica", 18);
    if (!object)
        return {};
    if (!FPDFText_SetText(object, reinterpret_cast<FPDF_WIDESTRING>(text.utf16()))) {
        FPDFPageObj_Destroy(object);
        return {};
    }
    FPDFPageObj_Transform(object, 1, 0, 0, 1, 20, 200);
    FPDFPage_InsertObject(page, object);
    if (!FPDFPage_GenerateContent(page))
        return {};
    MemoryWriter writer;
    return FPDF_SaveAsCopy(document, &writer, FPDF_NO_INCREMENTAL) ? writer.bytes : QByteArray();
}

QString pageText(FPDF_DOCUMENT document)
{
    assertPDFiumProcessLockHeld();
    FPDF_PAGE page = FPDF_LoadPage(document, 0);
    if (!page)
        return {};
    const auto closePage = qScopeGuard([&] { FPDF_ClosePage(page); });
    FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
    if (!textPage)
        return {};
    const auto closeText = qScopeGuard([&] { FPDFText_ClosePage(textPage); });
    const int count = FPDFText_CountChars(textPage);
    if (count < 0 || count > 1000)
        return {};
    QString text(count + 1, QChar::Null);
    const int written = FPDFText_GetText(textPage, 0, count,
                                        reinterpret_cast<unsigned short *>(text.data()));
    text.resize(qMax(0, written - 1));
    return text;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile output(path);
    return output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size();
}

bool replaceFile(const QString &path, const QByteArray &bytes)
{
    PDFAtomicSaveFile output(path);
    const bool opened = output.open(QIODevice::WriteOnly);
    const bool written = opened && output.write(bytes) == bytes.size();
    const bool saved = written && output.commit();
    if (!saved)
        qWarning() << "Atomic save failed:" << output.errorString();
    return saved;
}

QByteArray readFile(const QString &path)
{
    QFile input(path);
    return input.open(QIODevice::ReadOnly) ? input.readAll() : QByteArray();
}

} // namespace

class WindowsSaveTests : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        PDFiumProcessLock lock;
        FPDF_InitLibrary();
    }

    void cleanupTestCase()
    {
        PDFiumProcessLock lock;
        FPDF_DestroyLibrary();
    }

    void atomicReplacementRetainsOldReaders()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("bản gốc.pdf"));
        PDFiumProcessLock lock;
        const QByteArray original = pdfBytes(QStringLiteral("original snapshot"), 320);
        const QByteArray replacement = pdfBytes(QStringLiteral("new saved snapshot"), 640);
        QVERIFY(!original.isEmpty());
        QVERIFY(!replacement.isEmpty());
        QVERIFY(writeFile(path, original));

        auto mainDocument = PDFiumDocument::open(path);
        auto formDocument = PDFiumDocument::open(path);
        auto scanDocument = PDFiumDocument::open(path);
        auto writerDocument = PDFiumDocument::open(path);
        QVERIFY(mainDocument.get());
        QVERIFY(formDocument.get());
        QVERIFY(scanDocument.get());
        QVERIFY(writerDocument.get());
        QVERIFY(replaceFile(path, replacement));
        QCOMPARE(readFile(path), replacement);

        // Read page/text only after replacement to exercise lazy reads through
        // the old file object, not merely already cached document metadata.
        for (FPDF_DOCUMENT document : {mainDocument.get(), formDocument.get(),
                                       scanDocument.get(), writerDocument.get()}) {
            QCOMPARE(pageText(document), QStringLiteral("original snapshot"));
            FS_SIZEF size = {};
            QVERIFY(FPDF_GetPageSizeByIndexF(document, 0, &size));
            QCOMPARE(size.width, 320.0f);
        }
        auto reopened = PDFiumDocument::open(path);
        QVERIFY(reopened.get());
        QCOMPARE(pageText(reopened), QStringLiteral("new saved snapshot"));
        FS_SIZEF size = {};
        QVERIFY(FPDF_GetPageSizeByIndexF(reopened, 0, &size));
        QCOMPARE(size.width, 640.0f);
    }

    void externalLockPreservesDestinationAndAllowsRetry()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("locked.pdf"));
        PDFiumProcessLock lock;
        const QByteArray original = pdfBytes(QStringLiteral("keep this original"), 320);
        const QByteArray replacement = pdfBytes(QStringLiteral("successful retry"), 640);
        QVERIFY(writeFile(path, original));
        auto document = PDFiumDocument::open(path);
        QVERIFY(document.get());
        {
            HANDLE external = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            QVERIFY(external != INVALID_HANDLE_VALUE);
            const auto closeExternal = qScopeGuard([&] { CloseHandle(external); });
            QVERIFY(!replaceFile(path, replacement));
            QCOMPARE(readFile(path), original);
            QCOMPARE(pageText(document), QStringLiteral("keep this original"));
        }
        QVERIFY(replaceFile(path, replacement));
        auto reopened = PDFiumDocument::open(path);
        QVERIFY(reopened.get());
        QCOMPARE(pageText(reopened), QStringLiteral("successful retry"));
        QCOMPARE(pageText(document), QStringLiteral("keep this original"));
    }

    void moveKeepsReaderAlive()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("move.pdf"));
        PDFiumProcessLock lock;
        QVERIFY(writeFile(path, pdfBytes(QStringLiteral("moved reader"), 320)));
        auto first = PDFiumDocument::open(path);
        QVERIFY(first.get());
        PDFiumDocument second(std::move(first));
        QVERIFY(!first.get());
        QCOMPARE(pageText(second), QStringLiteral("moved reader"));
        auto third = PDFiumDocument::open(path);
        third = std::move(second);
        QVERIFY(!second.get());
        QVERIFY(replaceFile(path, pdfBytes(QStringLiteral("new file"), 640)));
        QCOMPARE(pageText(third), QStringLiteral("moved reader"));
        third.reset();
        QVERIFY(!third.get());
        third.reset();
    }

    void newDestinationAndCancelledWrite()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("new.pdf"));
        {
            PDFAtomicSaveFile output(path);
            QVERIFY(output.open(QIODevice::WriteOnly));
            QCOMPARE(output.write("complete"), 8);
            QVERIFY(!QFile::exists(path));
            QVERIFY(output.commit());
        }
        QCOMPARE(readFile(path), QByteArray("complete"));
        {
            PDFAtomicSaveFile output(path);
            QVERIFY(output.open(QIODevice::WriteOnly));
            QCOMPARE(output.write("incomplete"), 10);
            output.cancelWriting();
            QVERIFY(!output.commit());
        }
        QCOMPARE(readFile(path), QByteArray("complete"));
        {
            PDFAtomicSaveFile output(path);
            QVERIFY(output.open(QIODevice::WriteOnly));
            QCOMPARE(output.write("abandoned"), 9);
        }
        QCOMPARE(readFile(path), QByteArray("complete"));
        QCOMPARE(QDir(directory.path()).entryList(QDir::Files), QStringList{"new.pdf"});
    }

    void readOnlyDestinationIsPreserved()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("read-only.pdf"));
        QVERIFY(writeFile(path, "original"));
        QVERIFY(SetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()), FILE_ATTRIBUTE_READONLY));
        const auto restore = qScopeGuard([&] {
            SetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()), FILE_ATTRIBUTE_NORMAL);
        });
        QVERIFY(!replaceFile(path, "replacement"));
        QCOMPARE(readFile(path), QByteArray("original"));
    }

    void errorsAndPassword()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString protectedPath = QFINDTESTDATA("../fixtures/password-1.pdf");
        QVERIFY(!protectedPath.isEmpty());
        PDFiumProcessLock lock;
        auto wrongPassword = PDFiumDocument::open(protectedPath, "incorrect");
        QVERIFY(!wrongPassword.get());
        QCOMPARE(wrongPassword.error(), static_cast<unsigned long>(FPDF_ERR_PASSWORD));
        auto missing = PDFiumDocument::open(directory.filePath(QStringLiteral("missing.pdf")));
        QVERIFY(!missing.get());
        QCOMPARE(missing.error(), static_cast<unsigned long>(FPDF_ERR_FILE));
        auto encrypted = PDFiumDocument::open(protectedPath, "fixture-open-1");
        QVERIFY(encrypted.get());
        QCOMPARE(encrypted.error(), static_cast<unsigned long>(FPDF_ERR_SUCCESS));
        QCOMPARE(FPDF_GetPageCount(encrypted), 1);

        auto nulPath = PDFiumDocument::open(protectedPath + QChar::Null + QStringLiteral(".pdf"),
                                            "fixture-open-1");
        QVERIFY(!nulPath.get());
        QCOMPARE(nulPath.error(), static_cast<unsigned long>(FPDF_ERR_FILE));
        const QString corruptPath = directory.filePath(QStringLiteral("corrupt.pdf"));
        QVERIFY(writeFile(corruptPath, "not a PDF"));
        auto corrupt = PDFiumDocument::open(corruptPath);
        QVERIFY(!corrupt.get());
        QCOMPARE(corrupt.error(), static_cast<unsigned long>(FPDF_ERR_FORMAT));
    }
};

QTEST_GUILESS_MAIN(WindowsSaveTests)
#include "tst_WindowsSave.moc"
