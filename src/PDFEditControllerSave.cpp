#include "PDFEditController.h"
#include "PDFEditController_p.h"
#include "ApplicationFonts.h"
#include "FontDownloadService.h"
#include "PDFAnnotationUtils.h"
#include "PDFFormController.h"
#include "PDFFormDesignerController.h"
#include "PDFSaveWriter.h"
#include "PDFiumSynchronization.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMutexLocker>
#include <QPolygonF>
#include <QQuickTextDocument>
#include <QRawFont>
#include <QRunnable>
#include <QSet>
#include <QStringList>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QThreadPool>
#include <QTimeZone>
#include <QUuid>
#include <QVariantMap>
#include <QtTranslation>

#include <fpdf_annot.h>
#include <fpdf_edit.h>
#include <fpdf_text.h>

using namespace PDFEditControllerPrivate;

namespace {

QString saveFailureText(const PDFSaveWriter::Result &result)
{
    switch (result.error) {
    case PDFSaveWriter::Error::None:
        return {};
    case PDFSaveWriter::Error::SourceUnavailable:
        //% "No PDF is open."
        return qtTrId("NoPdfOpenForSaving");
    case PDFSaveWriter::Error::SourceOpenFailed:
        //% "The source PDF could not be opened for saving."
        return qtTrId("PdfSaveSourceOpenFailed");
    case PDFSaveWriter::Error::FormSnapshotFailed:
        //% "The PDF could not be saved."
        return qtTrId("PdfSaveFailed");
    case PDFSaveWriter::Error::PageLoadFailed:
        //% "Page %1 could not be loaded while saving."
        return qtTrId("PdfSavePageLoadFailed").arg(result.page + 1);
    case PDFSaveWriter::Error::UnsupportedSourceObject:
        //% "Page %1 contains edited text that cannot be saved safely yet."
        return qtTrId("PdfSaveUnsupportedSourceObject").arg(result.page + 1);
    case PDFSaveWriter::Error::SourceObjectMissing:
        //% "An edited object could not be found on page %1."
        return qtTrId("PdfSaveSourceObjectMissing").arg(result.page + 1);
    case PDFSaveWriter::Error::SourceObjectRemovalFailed:
        //% "An original object could not be removed or replaced on page %1."
        return qtTrId("PdfSaveObjectRemovalFailed").arg(result.page + 1);
    case PDFSaveWriter::Error::FontUnavailable: {
        if (result.unsupportedCodePoint != 0) {
            const char32_t codePoint = static_cast<char32_t>(
                result.unsupportedCodePoint);
            const QString character = QString::fromUcs4(&codePoint, 1);
            const QString unicodeName = QStringLiteral("U+%1")
                .arg(
                    result.unsupportedCodePoint,
                    4,
                    16,
                    QLatin1Char('0'))
                .toUpper();
            //% "The font \u201c%1\u201d does not support the character \u201c%2\u201d (%3). Choose a different font."
            return qtTrId("PdfSaveFontMissingCharacter")
                .arg(result.detail, character, unicodeName);
        }
        //% "The font %1 cannot be embedded in the saved PDF."
        return qtTrId("PdfSaveFontUnavailable").arg(result.detail);
    }
    case PDFSaveWriter::Error::TextObjectCreationFailed:
        //% "Saved text could not be created on page %1."
        return qtTrId("PdfSaveTextCreationFailed").arg(result.page + 1);
    case PDFSaveWriter::Error::ImageObjectCreationFailed:
        //% "The image could not be created on page %1."
        return qtTrId("PdfSaveImageCreationFailed").arg(result.page + 1);
    case PDFSaveWriter::Error::ShapeObjectCreationFailed:
        //% "The shape could not be created on page %1."
        return qtTrId("PdfSaveShapeCreationFailed").arg(result.page + 1);
    case PDFSaveWriter::Error::TextMarkupAnnotationCreationFailed:
        //% "The text markup could not be created on page %1."
        return qtTrId("PdfSaveTextMarkupCreationFailed").arg(result.page + 1);
    case PDFSaveWriter::Error::CommentAnnotationCreationFailed:
        //% "The comment could not be created on page %1."
        return qtTrId("PdfSaveCommentCreationFailed").arg(result.page + 1);
    case PDFSaveWriter::Error::WebLinkCreationFailed:
        if (result.page < 0) {
            //% "Web links could not be saved."
            return qtTrId("PdfSaveWebLinksFailed");
        }
        //% "Web links could not be created on page %1."
        return qtTrId("PdfSaveWebLinkCreationFailed").arg(result.page + 1);
    case PDFSaveWriter::Error::WebLinkRemovalFailed:
        //% "Web links could not be removed safely. The PDF was not saved."
        return qtTrId("PdfSaveWebLinkRemovalFailed");
    case PDFSaveWriter::Error::ManualLinkUpdateFailed:
        //% "The links could not be saved. Check their destination pages and try again."
        return qtTrId("PdfSaveManualLinkFailed");
    case PDFSaveWriter::Error::WatermarkUpdateFailed:
        //% "Watermarks could not be saved safely. The destination file was not changed."
        return qtTrId("PdfSaveWatermarkFailed");
    case PDFSaveWriter::Error::CommentAnnotationUpdateFailed:
        //% "The comment could not be updated on page %1."
        return qtTrId("PdfSaveCommentUpdateFailed").arg(result.page + 1);
    case PDFSaveWriter::Error::CommentAnnotationRemovalFailed:
        //% "The comment could not be deleted on page %1."
        return qtTrId("PdfSaveCommentRemovalFailed").arg(result.page + 1);
    case PDFSaveWriter::Error::PageCreationFailed:
        //% "Blank page %1 could not be created while saving."
        return qtTrId("PdfSavePageCreationFailed").arg(result.page + 1);
    case PDFSaveWriter::Error::PageDeletionFailed:
        //% "Page %1 could not be deleted while saving."
        return qtTrId("PdfSavePageDeletionFailed").arg(result.page + 1);
    case PDFSaveWriter::Error::PageContentGenerationFailed:
        //% "Page %1 could not be finalized for saving."
        return qtTrId("PdfSavePageGenerationFailed").arg(result.page + 1);
    case PDFSaveWriter::Error::AcroFormCreationFailed:
        //% "The AcroForm fields could not be created: %1"
        return qtTrId("PdfSaveAcroFormCreationFailed").arg(result.detail);
    case PDFSaveWriter::Error::PasswordProtectionFailed:
        //% "Password protection could not be applied. The PDF was not saved."
        return qtTrId("PdfSavePasswordProtectionFailed");
    case PDFSaveWriter::Error::OutputOpenFailed:
        //% "The destination file could not be opened: %1"
        return qtTrId("PdfSaveOutputOpenFailed").arg(result.detail);
    case PDFSaveWriter::Error::OutputWriteFailed:
        //% "The PDF could not be written: %1"
        return qtTrId("PdfSaveOutputWriteFailed").arg(result.detail);
    case PDFSaveWriter::Error::OutputCommitFailed:
        //% "The saved PDF could not replace the destination file: %1"
        return qtTrId("PdfSaveOutputCommitFailed").arg(result.detail);
    }
    //% "The PDF could not be saved."
    return qtTrId("PdfSaveFailed");
}

} // namespace

bool PDFEditController::save()
{
    cancelWebLinkCreation();
    if (!m_document || !m_document->isLoaded()) {
        //% "No PDF is open."
        emit statusMessage(qtTrId("NoPdfOpenForSaving"));
        return false;
    }
    if (!hasDrafts()) {
        //% "There are no draft changes to save."
        emit statusMessage(qtTrId("NoDraftChangesToSave"));
        return true;
    }
    return saveAs(m_document->source());
}

bool PDFEditController::saveAs(const QUrl &target)
{
    cancelWebLinkCreation();
    if (m_saving) {
        return false;
    }
    if (!m_document || !m_document->isLoaded()) {
        //% "No PDF is open."
        emit statusMessage(qtTrId("NoPdfOpenForSaving"));
        return false;
    }
    if (!m_pendingTextMarkupCreations.isEmpty()) {
        //% "Text information is still loading. Please try again."
        emit statusMessage(qtTrId("TextGeometryStillLoading"));
        return false;
    }
    if (!target.isLocalFile() || target.toLocalFile().isEmpty()) {
        //% "Only local PDF files are supported"
        emit statusMessage(qtTrId("LocalPdfFilesOnly"));
        return false;
    }

    finishSelectedTextEdit();
    PDFDocument *const expectedDocument = m_document.data();
    const int expectedRevision = expectedDocument->renderRevision();
    const QString expectedSourceFilePath = expectedDocument->renderFilePath();
    m_saving = true;
    emit savingChanged();
    const QString targetFilePath = QFileInfo(target.toLocalFile()).absoluteFilePath();
    QString temporarySourceFilePath;
    QString sourceFilePathOverride;
    QString snapshotError;
    if (m_formController && m_formController->hasDrafts()) {
        if (!m_formController->exportSnapshotBlocking(
                temporarySourceFilePath,
                snapshotError)) {
            return completeSave(
                PDFSaveWriter::Result{
                    PDFSaveWriter::Error::FormSnapshotFailed,
                    snapshotError},
                targetFilePath,
                expectedDocument,
                expectedRevision,
                expectedSourceFilePath);
        }
        sourceFilePathOverride = temporarySourceFilePath;
    }

    const PDFSaveWriter::Request request = PDFSaveWriter::prepare(
        *this,
        targetFilePath,
        sourceFilePathOverride);
    const PDFSaveWriter::Result result = PDFSaveWriter::save(request);
    return completeSave(
        result,
        targetFilePath,
        expectedDocument,
        expectedRevision,
        expectedSourceFilePath,
        temporarySourceFilePath);
}

bool PDFEditController::requestSave()
{
    cancelWebLinkCreation();
    if (!m_document || !m_document->isLoaded()) {
        //% "No PDF is open."
        emit statusMessage(qtTrId("NoPdfOpenForSaving"));
        return false;
    }
    if (!hasDrafts()) {
        //% "There are no draft changes to save."
        emit statusMessage(qtTrId("NoDraftChangesToSave"));
        emit saveFinished(true);
        return true;
    }
    return requestSaveAs(m_document->source());
}

bool PDFEditController::requestSaveAs(const QUrl &target)
{
    cancelWebLinkCreation();
    if (m_saving) {
        return false;
    }
    if (!m_document || !m_document->isLoaded()) {
        //% "No PDF is open."
        emit statusMessage(qtTrId("NoPdfOpenForSaving"));
        return false;
    }
    if (!m_pendingTextMarkupCreations.isEmpty()) {
        //% "Text information is still loading. Please try again."
        emit statusMessage(qtTrId("TextGeometryStillLoading"));
        return false;
    }
    if (!target.isLocalFile() || target.toLocalFile().isEmpty()) {
        //% "Only local PDF files are supported"
        emit statusMessage(qtTrId("LocalPdfFilesOnly"));
        return false;
    }

    finishSelectedTextEdit();
    const QString targetFilePath = QFileInfo(target.toLocalFile()).absoluteFilePath();
    m_allowFontRecovery = true;
    m_attemptedFontDownloads.clear();
    PDFDocument *const expectedDocument = m_document.data();
    const int expectedRevision = m_document->renderRevision();
    const QString expectedSourceFilePath = m_document->renderFilePath();
    m_saving = true;
    emit savingChanged();

    if (m_formController && m_formController->hasDrafts()) {
        QPointer<PDFEditController> controller(this);
        m_formController->exportSnapshot(
            [controller,
             targetFilePath,
             expectedDocument,
             expectedRevision,
             expectedSourceFilePath](
                bool succeeded,
                const QString &snapshotFilePath,
                const QString &error) {
                if (!controller) {
                    if (!snapshotFilePath.isEmpty()) {
                        QFile::remove(snapshotFilePath);
                    }
                    return;
                }
                if (!succeeded) {
                    controller->completeSave(
                        PDFSaveWriter::Result{
                            PDFSaveWriter::Error::FormSnapshotFailed,
                            error},
                        targetFilePath,
                        expectedDocument,
                        expectedRevision,
                        expectedSourceFilePath);
                    return;
                }
                controller->startAsyncSave(
                    targetFilePath,
                    expectedDocument,
                    expectedRevision,
                    expectedSourceFilePath,
                    snapshotFilePath,
                    snapshotFilePath);
            });
        return true;
    }

    startAsyncSave(
        targetFilePath,
        expectedDocument,
        expectedRevision,
        expectedSourceFilePath);
    return true;
}

void PDFEditController::startAsyncSave(
    const QString &targetFilePath,
    PDFDocument *expectedDocument,
    int expectedRevision,
    const QString &expectedSourceFilePath,
    const QString &sourceFilePathOverride,
    const QString &temporarySourceFilePath)
{
    const PDFSaveWriter::Request request = PDFSaveWriter::prepare(
        *this,
        targetFilePath,
        sourceFilePathOverride);
    if (!request.isValid()) {
        completeSave(
            PDFSaveWriter::Result{
                PDFSaveWriter::Error::SourceUnavailable},
            targetFilePath,
            expectedDocument,
            expectedRevision,
            expectedSourceFilePath,
            temporarySourceFilePath);
        return;
    }

    QPointer<PDFEditController> controller(this);
    QThreadPool::globalInstance()->start(QRunnable::create(
        [controller,
         expectedDocument,
         expectedRevision,
         expectedSourceFilePath,
         request,
         targetFilePath,
         temporarySourceFilePath]() {
            PDFSaveWriter::Result result = PDFSaveWriter::save(request);
            if (!controller) {
                if (!temporarySourceFilePath.isEmpty()) {
                    QFile::remove(temporarySourceFilePath);
                }
                return;
            }
            QObject *app = QCoreApplication::instance();
            if (!app) {
                if (!temporarySourceFilePath.isEmpty()) {
                    QFile::remove(temporarySourceFilePath);
                }
                return;
            }
            QMetaObject::invokeMethod(
                app,
                [controller,
                 expectedDocument,
                 expectedRevision,
                 expectedSourceFilePath,
                 result = std::move(result),
                 targetFilePath,
                 temporarySourceFilePath]() {
                    if (controller) {
                        controller->completeSave(
                            result,
                            targetFilePath,
                            expectedDocument,
                            expectedRevision,
                            expectedSourceFilePath,
                            temporarySourceFilePath);
                    } else if (!temporarySourceFilePath.isEmpty()) {
                        QFile::remove(temporarySourceFilePath);
                    }
                },
                Qt::QueuedConnection);
        }));
}

bool PDFEditController::completeSave(
    const PDFSaveWriter::Result &result,
    const QString &targetFilePath,
    PDFDocument *expectedDocument,
    int expectedRevision,
    const QString &expectedSourceFilePath,
    const QString &temporarySourceFilePath)
{
    const bool documentUnchanged = m_document
        && m_document == expectedDocument
        && m_document->renderRevision() == expectedRevision
        && m_document->renderFilePath() == expectedSourceFilePath;
    const QString fontKey = result.detail.trimmed().toCaseFolded()
        + QLatin1Char('|') + QString::number(result.fontBold)
        + QLatin1Char('|') + QString::number(result.fontItalic);
    if (m_allowFontRecovery && documentUnchanged
        && result.error == PDFSaveWriter::Error::FontUnavailable
        && result.fontDownloadSuggested && m_attemptedFontDownloads.size() < 8
        && !m_attemptedFontDownloads.contains(fontKey)) {
        m_attemptedFontDownloads.insert(fontKey);
        m_pendingFontSave = std::make_unique<PendingFontSave>(PendingFontSave{
            result, targetFilePath, expectedDocument, expectedRevision,
            expectedSourceFilePath, temporarySourceFilePath});
        emit fontDownloadStateChanged();
        emit fontDownloadRequested();
        return false;
    }
    m_allowFontRecovery = false;
    m_attemptedFontDownloads.clear();
    if (!temporarySourceFilePath.isEmpty()) {
        QFile::remove(temporarySourceFilePath);
    }
    if (!result.succeeded()) {
        m_saving = false;
        emit savingChanged();
        emit statusMessage(saveFailureText(result));
        emit saveFinished(false);
        return false;
    }

    if (!documentUnchanged) {
        m_saving = false;
        emit savingChanged();
        //% "Saved %1"
        emit statusMessage(qtTrId("PdfSavedFile").arg(
            QFileInfo(targetFilePath).fileName()));
        emit saveFinished(true);
        return true;
    }

    // Opening the saved document resets the session and clears the pending setting.
    const QString documentPassword = QString::fromUtf8(
        m_pendingOpenPassword.isEmpty()
            ? m_document->documentPassword() : m_pendingOpenPassword);
    const bool reopened = m_document->open(
        QUrl::fromLocalFile(targetFilePath), documentPassword);
    m_saving = false;
    emit savingChanged();
    if (!reopened) {
        m_document->close();
        //% "The PDF was saved, but TrePDF could not reopen it."
        emit statusMessage(qtTrId("PdfSavedButReopenFailed"));
        emit saveFinished(true);
        return true;
    }

    if (m_tool == LinkTool) {
        beginLinkEditing();
    }

    //% "Saved %1"
    emit statusMessage(qtTrId("PdfSavedFile").arg(QFileInfo(targetFilePath).fileName()));
    emit saveFinished(true);
    return true;
}

QString PDFEditController::missingFontFamily() const
{
    return m_pendingFontSave ? m_pendingFontSave->failure.detail : QString();
}

void PDFEditController::clearFontRecovery()
{
    if (m_fontDownloadService) {
        disconnect(m_fontDownloadService, nullptr, this, nullptr);
        m_fontDownloadService->cancel();
        m_fontDownloadService->deleteLater();
        m_fontDownloadService = nullptr;
    }
    if (m_pendingFontSave && !m_pendingFontSave->temporarySourceFilePath.isEmpty())
        QFile::remove(m_pendingFontSave->temporarySourceFilePath);
    m_pendingFontSave.reset();
    m_downloadingFont = false;
    m_allowFontRecovery = false;
    m_attemptedFontDownloads.clear();
    emit fontDownloadStateChanged();
}

void PDFEditController::cancelFontDownload()
{
    if (!m_pendingFontSave)
        return;
    clearFontRecovery();
    m_saving = false;
    emit savingChanged();
    //% "Font download cancelled. Your edits have been kept."
    emit statusMessage(qtTrId("FontDownloadCancelled"));
    emit saveFinished(false);
}

void PDFEditController::respondToFontDownload(bool accepted)
{
    if (!m_pendingFontSave || m_downloadingFont)
        return;
    if (!accepted) {
        cancelFontDownload();
        return;
    }
    if (!m_document || m_document != m_pendingFontSave->document
        || m_document->renderRevision() != m_pendingFontSave->revision
        || m_document->renderFilePath() != m_pendingFontSave->sourceFilePath) {
        cancelFontDownload();
        return;
    }
    m_downloadingFont = true;
    emit fontDownloadStateChanged();
    const auto &failure = m_pendingFontSave->failure;
    bool familyCached = true;
    for (int style = 0; style < 4; ++style) {
        const bool bold = (style & 2) != 0;
        const bool italic = (style & 1) != 0;
        const auto cached = ApplicationFonts::downloadedFontFace(failure.detail, bold, italic);
        if (!cached.isValid() || (bold == failure.fontBold && italic == failure.fontItalic
            && failure.unsupportedCodePoint
            && !QRawFont(cached.data, 16).supportsCharacter(failure.unsupportedCodePoint))) {
            familyCached = false;
            break;
        }
    }
    if (familyCached) {
        finishFontDownload(true, {});
        return;
    }
    m_fontDownloadService = new FontDownloadService(this);
    connect(m_fontDownloadService, &FontDownloadService::finished,
            this, &PDFEditController::finishFontDownload);
    m_fontDownloadService->start(failure.detail, failure.fontBold,
                                failure.fontItalic, failure.unsupportedCodePoint);
}

void PDFEditController::finishFontDownload(bool succeeded, const QString &error)
{
    if (!m_pendingFontSave)
        return;
    if (!succeeded) {
        clearFontRecovery();
        m_saving = false;
        emit savingChanged();
        //% "The font could not be downloaded: %1. Your edits have been kept."
        emit statusMessage(qtTrId("FontDownloadSaveFailed").arg(error));
        emit saveFinished(false);
        return;
    }
    if (!m_document || m_document != m_pendingFontSave->document
        || m_document->renderRevision() != m_pendingFontSave->revision
        || m_document->renderFilePath() != m_pendingFontSave->sourceFilePath) {
        cancelFontDownload();
        return;
    }
    if (m_fontDownloadService) {
        m_fontDownloadService->deleteLater();
        m_fontDownloadService = nullptr;
    }
    auto pending = std::move(m_pendingFontSave);
    m_downloadingFont = false;
    emit fontDownloadStateChanged();
    emit textFormatStateChanged();
    //% "Font downloaded. Continuing to save…"
    emit statusMessage(qtTrId("FontDownloadContinuingSave"));
    startAsyncSave(pending->targetFilePath, pending->document, pending->revision,
                   pending->sourceFilePath, pending->temporarySourceFilePath,
                   pending->temporarySourceFilePath);
}
