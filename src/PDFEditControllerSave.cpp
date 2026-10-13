#include "PDFEditController.h"
#include "PDFEditController_p.h"
#include "ApplicationFonts.h"
#include "FontDownloadService.h"
#include "PDFFontAliases.h"
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

QString editFontKey(const QString &family, bool bold, bool italic)
{
    return family.trimmed().toCaseFolded() + QLatin1Char('|')
        + QString::number(bold) + QLatin1Char('|') + QString::number(italic);
}

bool sourceFaceSupportsEdit(const QString &family, bool bold, bool italic,
                            const QString &text)
{
    if (PDFFontAliases::sourceFingerprint(family).isEmpty()
        || !PDFFontAliases::matchesStyle(family, bold, italic))
        return false;
    QFont font(family);
    font.setBold(bold);
    font.setItalic(italic);
    font.setPixelSize(16);
    font.setStyleStrategy(QFont::NoFontMerging);
    const QRawFont raw = QRawFont::fromFont(font);
    if (!raw.isValid() || raw.familyName().compare(family, Qt::CaseInsensitive) != 0)
        return false;
    const auto characters = text.toUcs4();
    return std::all_of(characters.cbegin(), characters.cend(), [&raw](char32_t character) {
        // PDFium may infer separators that are not encoded in the subset.
        return QChar::isSpace(character) || raw.supportsCharacter(character);
    });
}

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
    cancelTextEditPreparation();
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
    cancelTextEditPreparation();
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
    cancelTextEditPreparation();
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
    cancelTextEditPreparation();
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
    if (m_pendingFontEdit)
        return m_pendingFontEdit->missingFamilies.value(0);
    return m_pendingFontSave ? m_pendingFontSave->failure.detail : QString();
}

QStringList PDFEditController::missingFontFamilies() const
{
    if (m_pendingFontEdit)
        return m_pendingFontEdit->missingFamilies;
    return m_pendingFontSave ? QStringList{m_pendingFontSave->failure.detail} : QStringList{};
}

QString PDFEditController::textEditFallbackFamily() const
{
    return m_pendingFontEdit ? m_pendingFontEdit->fallbackFamily : QString();
}

bool PDFEditController::requestSelectedTextEdit(const QString &styleAction)
{
    const QString requestedFamily = styleAction.startsWith(QLatin1String("fontFamily:"))
        ? styleAction.mid(11).trimmed() : QString();
    const bool familyAction = !requestedFamily.isEmpty() && QFontDatabase::hasFamily(requestedFamily);
    if (!m_document || !m_document->isLoaded() || m_saving || m_pendingFontSave
        || !m_hasSelection || !m_selectedCandidate.editable
        || !isTextCandidateKind(m_selectedCandidate.kind)
        || (styleAction != QString() && styleAction != QLatin1String("bold")
            && styleAction != QLatin1String("italic") && styleAction != QLatin1String("underline")
            && !familyAction))
        return false;
    if (m_textEditActive)
        return true;
    if (m_pendingFontEdit && textEditPreparationIsCurrent()
        && m_pendingFontEdit->styleAction == styleAction)
        return false;
    const QPointer<PDFDocument> selectedDocument = m_document;
    const quint64 selectedRevision = m_document->renderRevision();
    const QString selectedPath = m_document->renderFilePath();
    const int selectedPage = m_selectedCandidate.page;
    const QString selectedId = m_selectedCandidate.objectId;
    const Tool selectedTool = m_tool;
    const auto selectedDraftRevision = m_pageDraftRevisions.value(selectedPage);
    const quint64 expectedGeneration = m_textEditPreparationGeneration + (m_pendingFontEdit ? 1 : 0);
    cancelTextEditPreparation();
    // Closing the previous prompt can synchronously change the active session.
    if (m_textEditPreparationGeneration != expectedGeneration || m_pendingFontEdit
        || m_pendingFontSave || m_saving || m_textEditActive || !m_hasSelection
        || !m_document || m_document != selectedDocument || !m_document->isLoaded()
        || m_document->renderRevision() != selectedRevision
        || m_document->renderFilePath() != selectedPath
        || m_selectedCandidate.page != selectedPage || m_selectedCandidate.objectId != selectedId
        || m_tool != selectedTool || m_pageDraftRevisions.value(selectedPage) != selectedDraftRevision)
        return false;

    const Draft draft = selectedDraftForDisplay();
    // Bound both the scan and the number of network attempts, independently
    // from PDF source grouping limits and malformed span ranges.
    constexpr qsizetype maximumTextLength = 32768;
    constexpr qsizetype maximumFaces = 64;
    if (draft.text.size() > maximumTextLength || draft.characterSpans.size() > maximumTextLength) {
        //% "This text contains too many font runs to prepare for editing."
        emit statusMessage(qtTrId("TextEditFontPreparationTooLarge"));
        return false;
    }
    QVector<TextEditFontRequirement> displayRuns;
    QHash<QString, qsizetype> displayIndices;
    const auto addRun = [&](const CharacterFormat &format, const QString &text) {
        const QString key = editFontKey(format.fontFamily, format.fontBold, format.fontItalic);
        auto found = displayIndices.constFind(key);
        if (found == displayIndices.cend()) {
            if (displayRuns.size() >= maximumFaces)
                return false;
            const qsizetype index = displayRuns.size();
            displayRuns.append({format.fontFamily, text, format.fontBold, format.fontItalic});
            displayIndices.insert(key, index);
        } else {
            displayRuns[*found].text += text;
        }
        return true;
    };
    const CharacterFormat base = primaryCharacterFormat(draft);
    qsizetype position = 0;
    bool valid = true;
    for (const CharacterSpan &span : draft.characterSpans) {
        if (span.start < position || span.length < 0 || span.start > draft.text.size()
            || span.length > draft.text.size() - span.start) {
            valid = false;
            break;
        }
        if (span.start > position)
            valid = addRun(base, draft.text.mid(position, span.start - position));
        if (valid && span.length > 0)
            valid = addRun(span.format, draft.text.mid(span.start, span.length));
        if (!valid)
            break;
        position = span.start + span.length;
    }
    if (valid && (position < draft.text.size() || draft.text.isEmpty()))
        valid = addRun(base, draft.text.mid(position));
    if (valid && (styleAction == QLatin1String("bold") || styleAction == QLatin1String("italic")
                  || familyAction)) {
        CharacterFormat typing = characterFormatAtPosition(draft, draft.text.size(), true);
        if (styleAction == QLatin1String("bold"))
            typing.fontBold = !typing.fontBold;
        else if (styleAction == QLatin1String("italic"))
            typing.fontItalic = !typing.fontItalic;
        else
            typing.fontFamily = requestedFamily;
        valid = addRun(typing, {});
    }
    if (!valid) {
        //% "This text contains too many font runs to prepare for editing."
        emit statusMessage(qtTrId("TextEditFontPreparationTooLarge"));
        return false;
    }

    auto pending = std::make_unique<PendingFontEdit>();
    QHash<QString, qsizetype> semanticIndices;
    QHash<QString, QSet<QString>> displayFacesBySemanticKey;
    for (const auto &run : std::as_const(displayRuns)) {
        if (sourceFaceSupportsEdit(run.family, run.bold, run.italic, run.text))
            continue;
        const QString family = PDFFontAliases::semanticFamily(run.family).trimmed();
        const QString key = editFontKey(family, run.bold, run.italic);
        displayFacesBySemanticKey[key].insert(editFontKey(run.family, run.bold, run.italic));
        const auto found = semanticIndices.constFind(key);
        if (found == semanticIndices.cend()) {
            semanticIndices.insert(key, pending->requirements.size());
            pending->requirements.append({family, run.text, run.bold, run.italic});
        } else {
            pending->requirements[*found].text += run.text;
        }
    }
    for (const auto &required : std::as_const(pending->requirements)) {
        if (!ApplicationFonts::fontFaceSupportsText(required.family, required.bold,
                                                    required.italic, required.text)) {
            pending->fallbackRequirements.append(required);
            pending->missingDisplayFaces.unite(displayFacesBySemanticKey.value(
                editFontKey(required.family, required.bold, required.italic)));
            if (!pending->missingFamilies.contains(required.family, Qt::CaseInsensitive))
                pending->missingFamilies.append(required.family);
        }
    }
    if (pending->missingFamilies.isEmpty())
        return true;
    // Resolve once before showing the prompt, so consent names the same exact
    // local family that will be applied and embedded. Never rely on Qt's silent
    // per-glyph fallback or replace a usable source face of the same family.
    const auto supportsFallback = [&pending](const QString &family) {
        return !family.isEmpty() && std::all_of(
            pending->fallbackRequirements.cbegin(), pending->fallbackRequirements.cend(),
            [&family](const TextEditFontRequirement &required) {
                return ApplicationFonts::fontFaceSupportsText(
                    family, required.bold, required.italic, required.text);
            });
    };
    const QString defaultFamily = headerTextFontFamily();
    if (supportsFallback(defaultFamily)) {
        pending->fallbackFamily = defaultFamily;
    } else {
        // Font files and character work already have bounded validation. Cap
        // this local search as well, even on hosts with very large font lists.
        const auto &first = pending->fallbackRequirements.constFirst();
        const QStringList families = ApplicationFonts::availableFontFamilies(first.bold, first.italic);
        const qsizetype limit = qMin<qsizetype>(families.size(), 128);
        for (qsizetype index = 0; index < limit; ++index) {
            if (families.at(index) != defaultFamily && supportsFallback(families.at(index))) {
                pending->fallbackFamily = families.at(index);
                break;
            }
        }
    }
    pending->document = m_document;
    pending->revision = m_document->renderRevision();
    pending->sourceFilePath = m_document->renderFilePath();
    pending->page = m_selectedCandidate.page;
    pending->objectId = m_selectedCandidate.objectId;
    pending->tool = m_tool;
    pending->draftRevision = m_pageDraftRevisions.value(pending->page);
    pending->styleAction = styleAction;
    m_pendingFontEdit = std::move(pending);
    const quint64 generation = ++m_textEditPreparationGeneration;
    emit fontDownloadStateChanged();
    if (generation == m_textEditPreparationGeneration && textEditPreparationIsCurrent())
        emit fontDownloadRequested();
    return false;
}

bool PDFEditController::continueTextEditWithFallback()
{
    if (m_downloadingFont || !textEditPreparationIsCurrent()
        || m_pendingFontEdit->fallbackFamily.isEmpty())
        return false;
    // Revalidate the disclosed face without silently choosing a different one.
    for (const auto &required : std::as_const(m_pendingFontEdit->fallbackRequirements)) {
        if (!ApplicationFonts::fontFaceSupportsText(m_pendingFontEdit->fallbackFamily,
                                                    required.bold, required.italic, required.text))
            return false;
    }
    const auto pending = std::move(m_pendingFontEdit);
    const quint64 generation = ++m_textEditPreparationGeneration;
    const auto sameSelection = [&]() {
        return generation == m_textEditPreparationGeneration && !m_pendingFontEdit
            && !m_pendingFontSave && !m_saving && m_hasSelection && m_selectedCandidate.editable
            && m_document && m_document == pending->document && m_document->isLoaded()
            && m_document->renderRevision() == pending->revision
            && m_document->renderFilePath() == pending->sourceFilePath
            && m_selectedCandidate.page == pending->page
            && m_selectedCandidate.objectId == pending->objectId && m_tool == pending->tool;
    };
    emit fontDownloadStateChanged();
    if (!sameSelection() || m_textEditActive
        || m_pageDraftRevisions.value(pending->page) != pending->draftRevision)
        return false;

    // Capture the baseline before changing any fonts, so Escape and the single
    // edit Undo also restore the original families and geometry.
    beginSelectedTextEdit();
    if (!sameSelection() || !m_textEditActive
        || m_pageDraftRevisions.value(pending->page) != pending->draftRevision)
        return false;
    Draft draft = selectedDraftForDisplay();
    CharacterFormat typing = characterFormatAtPosition(draft, draft.text.size(), true);
    const auto replaceMissing = [&pending](CharacterFormat &format) {
        if (!pending->missingDisplayFaces.contains(
                editFontKey(format.fontFamily, format.fontBold, format.fontItalic)))
            return;
        format.fontFamily = pending->fallbackFamily;
        format.fontStretch = QFont::Unstretched;
        format.sourceAdvanceRatio = 0.0;
    };
    if (pending->styleAction == QLatin1String("bold")) {
        typing.fontBold = !typing.fontBold;
        typing.sourceAdvanceRatio = 0.0;
    } else if (pending->styleAction == QLatin1String("italic")) {
        typing.fontItalic = !typing.fontItalic;
        typing.sourceAdvanceRatio = 0.0;
    } else if (pending->styleAction == QLatin1String("underline")) {
        typing.fontUnderline = !typing.fontUnderline;
    } else if (pending->styleAction.startsWith(QLatin1String("fontFamily:"))) {
        typing.fontFamily = pending->styleAction.mid(11).trimmed();
        typing.fontStretch = QFont::Unstretched;
        typing.sourceAdvanceRatio = 0.0;
    }
    replaceMissing(typing);
    const bool changed = updateCharacterFormats(draft, 0, draft.text.size(), replaceMissing);
    if (changed)
        updateSelectedDraft(draft);
    if (!sameSelection() || !m_textEditActive
        || !sameDraftContent(selectedDraftForDisplay(), draft))
        return false;
    // Apply the pending action only to future typing, just like entering the
    // editor from the formatting panel. QML must not toggle the style again.
    m_selectedTextRangeStart = draft.text.size();
    m_selectedTextRangeLength = 0;
    m_typingCharacterFormat = typing;
    m_typingCharacterFormatPosition = draft.text.size();
    m_typingCharacterFormatActive = !sameCharacterFormat(
        typing, characterFormatAtPosition(draft, draft.text.size(), true));
    emit textEditPreparationReady(pending->page, pending->objectId, QString());
    if (sameSelection() && m_textEditActive)
        emit textFormatStateChanged();
    return true;
}

bool PDFEditController::textEditPreparationIsCurrent() const
{
    return m_pendingFontEdit && m_document && m_document == m_pendingFontEdit->document
        && m_document->renderRevision() == m_pendingFontEdit->revision
        && m_document->renderFilePath() == m_pendingFontEdit->sourceFilePath
        && m_hasSelection && m_selectedCandidate.editable && !m_textEditActive && !m_saving
        && m_selectedCandidate.page == m_pendingFontEdit->page
        && m_selectedCandidate.objectId == m_pendingFontEdit->objectId
        && m_tool == m_pendingFontEdit->tool
        && m_pageDraftRevisions.value(m_pendingFontEdit->page) == m_pendingFontEdit->draftRevision;
}

void PDFEditController::cancelTextEditPreparation()
{
    if (!m_pendingFontEdit)
        return;
    ++m_textEditPreparationGeneration;
    if (m_fontDownloadService) {
        disconnect(m_fontDownloadService, nullptr, this, nullptr);
        m_fontDownloadService->cancel();
        m_fontDownloadService->deleteLater();
        m_fontDownloadService = nullptr;
    }
    m_pendingFontEdit.reset();
    m_downloadingFont = false;
    emit fontDownloadStateChanged();
}

void PDFEditController::continueTextEditPreparation()
{
    if (!textEditPreparationIsCurrent()) {
        cancelTextEditPreparation();
        return;
    }
    if (!m_pendingFontEdit->accepted)
        return;
    // Recheck every exact face and its complete run after each installation.
    // A provider can offer only some styles, or a cached face can lack a glyph.
    const TextEditFontRequirement *next = nullptr;
    QStringList missing;
    for (const auto &required : std::as_const(m_pendingFontEdit->requirements)) {
        if (ApplicationFonts::fontFaceSupportsText(required.family, required.bold,
                                                   required.italic, required.text))
            continue;
        if (!next)
            next = &required;
        if (!missing.contains(required.family, Qt::CaseInsensitive))
            missing.append(required.family);
    }
    m_pendingFontEdit->missingFamilies = missing;
    if (!next) {
        const auto pending = std::move(m_pendingFontEdit);
        m_downloadingFont = false;
        const quint64 generation = ++m_textEditPreparationGeneration;
        const auto stillCurrent = [&]() {
            return generation == m_textEditPreparationGeneration && !m_pendingFontEdit
                && !m_pendingFontSave && !m_saving && !m_textEditActive && m_hasSelection
                && m_document && m_document == pending->document
                && m_document->renderRevision() == pending->revision
                && m_document->renderFilePath() == pending->sourceFilePath
                && m_selectedCandidate.page == pending->page
                && m_selectedCandidate.objectId == pending->objectId && m_tool == pending->tool
                && m_pageDraftRevisions.value(pending->page) == pending->draftRevision;
        };
        emit fontDownloadStateChanged();
        if (!stillCurrent())
            return;
        emit textFormatStateChanged();
        if (stillCurrent())
            emit textEditPreparationReady(pending->page, pending->objectId, pending->styleAction);
        return;
    }
    const QString key = editFontKey(next->family, next->bold, next->italic);
    if (m_pendingFontEdit->attemptedFaces.contains(key)) {
        //% "The downloaded %1 (%2) font still cannot display all the characters in this text."
        finishTextEditFontDownload(false, qtTrId("TextEditDownloadedFontIncomplete")
            .arg(next->family, FontDownloadService::styleName(next->bold, next->italic)));
        return;
    }
    m_pendingFontEdit->attemptedFaces.insert(key);
    uint requiredCharacter = 0;
    const auto cached = ApplicationFonts::downloadedFontFace(next->family, next->bold, next->italic);
    const QRawFont raw(cached.data, 16, QFont::PreferNoHinting);
    for (char32_t character : next->text.normalized(QString::NormalizationForm_C).toUcs4()) {
        if (!QChar::isSpace(character) && (!raw.isValid() || !raw.supportsCharacter(character))) {
            requiredCharacter = character;
            break;
        }
    }
    const TextEditFontRequirement nextFace = *next;
    const quint64 generation = m_textEditPreparationGeneration;
    m_downloadingFont = true;
    emit fontDownloadStateChanged();
    if (generation != m_textEditPreparationGeneration || !textEditPreparationIsCurrent())
        return;
    m_fontDownloadService = new FontDownloadService(this);
    connect(m_fontDownloadService, &FontDownloadService::finished,
            this, &PDFEditController::finishTextEditFontDownload);
    m_fontDownloadService->start(nextFace.family, nextFace.bold, nextFace.italic, requiredCharacter);
}

void PDFEditController::finishTextEditFontDownload(bool succeeded, const QString &error)
{
    if (!m_pendingFontEdit)
        return;
    if (!textEditPreparationIsCurrent()) {
        cancelTextEditPreparation();
        return;
    }
    if (m_fontDownloadService) {
        disconnect(m_fontDownloadService, nullptr, this, nullptr);
        m_fontDownloadService->deleteLater();
        m_fontDownloadService = nullptr;
    }
    if (!succeeded) {
        cancelTextEditPreparation();
        //% "%1\n\nText editing has not started. Your text is unchanged."
        emit fontDownloadFailed(qtTrId("FontDownloadEditFailed").arg(error));
        return;
    }
    continueTextEditPreparation();
}

void PDFEditController::clearFontRecovery()
{
    cancelTextEditPreparation();
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
    if (m_pendingFontEdit) {
        cancelTextEditPreparation();
        return;
    }
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
    if (m_pendingFontEdit) {
        if (m_downloadingFont)
            return;
        if (!accepted) {
            cancelTextEditPreparation();
            return;
        }
        m_pendingFontEdit->accepted = true;
        continueTextEditPreparation();
        return;
    }
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
        //% "%1\n\nYour PDF was not saved. Your edits have been kept."
        emit fontDownloadFailed(qtTrId("FontDownloadSaveFailed").arg(error));
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
