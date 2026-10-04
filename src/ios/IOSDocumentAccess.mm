#include "IOSDocumentAccess.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <QWindow>

#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <memory>
#include <utility>

namespace {
NSString *nativeString(const QString &value)
{
    return [NSString stringWithCharacters:reinterpret_cast<const unichar *>(value.utf16())
                                  length:static_cast<NSUInteger>(value.size())];
}

QString qtString(NSString *value)
{
    return value ? QString::fromUtf8(value.UTF8String) : QString();
}

NSURL *nativeUrl(const QUrl &url)
{
    return [NSURL fileURLWithPath:nativeString(url.toLocalFile())];
}

dispatch_queue_t documentQueue()
{
    static dispatch_queue_t queue = dispatch_queue_create("com.treoffice.trepdf.documents", DISPATCH_QUEUE_SERIAL);
    return queue;
}

UIViewController *presenter()
{
    if (UIApplication.sharedApplication.applicationState != UIApplicationStateActive)
        return nil;
    // Use the window that owns Qt input first; this also works with older Qt
    // versions whose iOS integration predates UIWindowScene adoption.
    if (QWindow *window = QGuiApplication::focusWindow()) {
        UIView *view = (__bridge UIView *)reinterpret_cast<void *>(window->winId());
        UIViewController *controller = view.window.rootViewController;
        if (controller && !controller.presentedViewController && controller.view.window)
            return controller;
    }
    // Resolve the foreground scene each time: an iPad can have several windows.
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (scene.activationState != UISceneActivationStateForegroundActive
            || ![scene isKindOfClass:UIWindowScene.class])
            continue;
        for (UIWindow *window in static_cast<UIWindowScene *>(scene).windows) {
            if (!window.isKeyWindow)
                continue;
            UIViewController *controller = window.rootViewController;
            // Never put a Files picker on top of another native modal operation.
            if (controller && !controller.presentedViewController && controller.view.window)
                return controller;
        }
    }
    return nil;
}

QString copyError(NSError *error)
{
    //% "Could not copy the document. Check its availability and the free storage. %1"
    return qtTrId("IOSFilesCopyFailed").arg(qtString(error.localizedDescription));
}

// NSURLs are retained by the asynchronous operation. Security scope and file
// coordination both cover the complete streaming filesystem copy, never a later
// PDFium read. Cloud providers can block here without blocking the GUI thread.
bool coordinatedCopy(NSURL *source, const QString &destination, QString *error)
{
    const BOOL scoped = [source startAccessingSecurityScopedResource];
    // A Qt FileOpen/drop event carries a QUrl rather than the original NSURL.
    // Qt registers its security-scoped bookmark in its QFile engine. Hold that
    // engine alive during coordination when the reconstructed NSURL has no
    // scope of its own, without depending on Qt's private bookmark APIs.
    QFile qtAccess(qtString(source.path));
    if (!scoped)
        (void)qtAccess.open(QIODevice::ReadOnly);
    NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
    __block NSError *copyFailure = nil;
    __block BOOL copied = NO;
    NSError *coordinationFailure = nil;
    [coordinator coordinateReadingItemAtURL:source options:0 error:&coordinationFailure
                                byAccessor:^(NSURL *readableUrl) {
        NSNumber *regular = nil;
        NSNumber *symlink = nil;
        if (![readableUrl getResourceValue:&regular forKey:NSURLIsRegularFileKey error:&copyFailure]
            || ![readableUrl getResourceValue:&symlink forKey:NSURLIsSymbolicLinkKey error:&copyFailure]
            || !regular.boolValue || symlink.boolValue)
            return;
        copied = [NSFileManager.defaultManager copyItemAtURL:readableUrl
                                                     toURL:[NSURL fileURLWithPath:nativeString(destination)]
                                                     error:&copyFailure];
    }];
    if (scoped)
        [source stopAccessingSecurityScopedResource];
    if (!copied) {
        *error = copyError(coordinationFailure ?: copyFailure);
        return false;
    }
    return true;
}
}

@interface TPDocumentOperation : NSObject <UIDocumentPickerDelegate, UIAdaptivePresentationControllerDelegate> {
@public
    IOSDocumentAccess::Completion completion;
    std::unique_ptr<QTemporaryDir> exportSnapshot;
    bool images;
    bool exporting;
    bool finished;
}
@property(nonatomic, strong) UIDocumentPickerViewController *picker;
- (void)finish:(IOSDocumentAccess::Result)result;
- (void)importURLs:(NSArray<NSURL *> *)urls;
@end

// One picker/copy operation at a time bounds the queue and keeps the weak UIKit
// delegate alive until cancellation, dismissal, or the coordinated copy finishes.
static TPDocumentOperation *activeOperation;

@implementation TPDocumentOperation
- (void)finish:(IOSDocumentAccess::Result)result
{
    if (finished)
        return;
    finished = true;
    TPDocumentOperation *keepAlive = self;
    self.picker.delegate = nil;
    self.picker.presentationController.delegate = nil;
    self.picker = nil;
    exportSnapshot.reset();
    auto callback = std::move(completion);
    if (activeOperation == self)
        activeOperation = nil;
    if (callback)
        callback(std::move(result));
    (void)keepAlive;
}

- (void)importURLs:(NSArray<NSURL *> *)urls
{
    const bool importImages = images;
    dispatch_async(documentQueue(), ^{
        @autoreleasepool {
            IOSDocumentAccess::Result result;
            const NSUInteger limit = importImages ? 1 : 32;
            if (urls.count > limit) {
                //% "Select at most 32 PDF documents at a time."
                result.error = qtTrId("IOSFilesTooManyDocuments");
            }
            for (NSUInteger index = 0; index < MIN(urls.count, limit); ++index) {
                NSURL *url = urls[index];
                if (!url.isFileURL)
                    continue;
                const QString name = qtString(url.lastPathComponent);
                if (!importImages && QFileInfo(name).suffix().compare(QStringLiteral("pdf"), Qt::CaseInsensitive) != 0)
                    continue;
                const QUrl local = QUrl::fromLocalFile(qtString(url.path));
                if (IOSDocumentAccess::isManagedFile(local)) {
                    result.files.append(local);
                    continue;
                }
                const QString root = importImages
                    ? QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/ImportedImages")
                    : IOSDocumentAccess::managedDirectory() + QStringLiteral("/Imported");
                const QString directory = root + QLatin1Char('/')
                    + QUuid::createUuid().toString(QUuid::WithoutBraces);
                if (!QDir().mkpath(directory)) {
                    result.error = copyError(nil);
                    continue;
                }
                const QString destination = QDir(directory).filePath(name);
                if (coordinatedCopy(url, destination, &result.error))
                    result.files.append(QUrl::fromLocalFile(destination));
                else
                    QDir(directory).removeRecursively();
            }
            dispatch_async(dispatch_get_main_queue(), ^{ [self finish:result]; });
        }
    });
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls
{
    (void)controller;
    if (finished)
        return;
    self.picker.delegate = nil;
    self.picker.presentationController.delegate = nil;
    if (exporting) {
        IOSDocumentAccess::Result result;
        result.cancelled = urls.count == 0;
        [self finish:std::move(result)];
    } else {
        [self importURLs:urls];
    }
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller
{
    (void)controller;
    IOSDocumentAccess::Result result;
    result.cancelled = true;
    [self finish:std::move(result)];
}

- (void)presentationControllerDidDismiss:(UIPresentationController *)presentationController
{
    (void)presentationController;
    IOSDocumentAccess::Result result;
    result.cancelled = true;
    [self finish:std::move(result)];
}
@end

namespace IOSDocumentAccess {
QString managedDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/TrePDF");
}

bool isManagedFile(const QUrl &url)
{
    if (!url.isLocalFile() || url.hasQuery() || url.hasFragment()
        || !url.userInfo().isEmpty() || url.port() != -1
        || (!url.host().isEmpty() && url.host().compare(QStringLiteral("localhost"), Qt::CaseInsensitive) != 0))
        return false;
    const QFileInfo file(url.toLocalFile());
    const QString directory = QFileInfo(managedDirectory()).canonicalFilePath();
    return !directory.isEmpty() && file.isFile() && !file.isSymbolicLink()
        && file.canonicalFilePath().startsWith(directory + QLatin1Char('/'));
}

bool openFiles(bool images, Completion completion)
{
    UIViewController *controller = presenter();
    if (activeOperation || !controller)
        return false;
    auto *operation = [TPDocumentOperation new];
    operation->images = images;
    operation->completion = std::move(completion);
    operation.picker = [[UIDocumentPickerViewController alloc]
        initForOpeningContentTypes:images ? @[UTTypeImage] : @[UTTypePDF] asCopy:NO];
    operation.picker.allowsMultipleSelection = !images;
    operation.picker.delegate = operation;
    operation.picker.modalPresentationStyle = UIModalPresentationFormSheet;
    operation.picker.presentationController.delegate = operation;
    activeOperation = operation;
    [controller presentViewController:operation.picker animated:YES completion:nil];
    operation.picker.presentationController.delegate = operation;
    return true;
}

void importPdfFiles(const QList<QUrl> &urls, Completion completion)
{
    if (activeOperation) {
        Result result;
        //% "The Files picker is not available. Close any open dialog and try again."
        result.error = qtTrId("IOSFilesPickerUnavailable");
        completion(std::move(result));
        return;
    }
    auto *operation = [TPDocumentOperation new];
    operation->completion = std::move(completion);
    NSMutableArray<NSURL *> *sources = [NSMutableArray array];
    for (const QUrl &url : urls) {
        if (url.isLocalFile() && !url.hasQuery() && !url.hasFragment()
            && url.userInfo().isEmpty() && url.port() == -1
            && (url.host().isEmpty() || url.host().compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0))
            [sources addObject:nativeUrl(url)];
    }
    activeOperation = operation;
    [operation importURLs:sources];
}

bool exportFile(const QUrl &url, Completion completion)
{
    if (activeOperation || !presenter())
        return false;
    auto *operation = [TPDocumentOperation new];
    operation->exporting = true;
    operation->completion = std::move(completion);
    activeOperation = operation;
    dispatch_async(documentQueue(), ^{
        @autoreleasepool {
            Result result;
            operation->exportSnapshot = std::make_unique<QTemporaryDir>(
                QDir::tempPath() + QStringLiteral("/TrePDF-export-XXXXXX"));
            const QString destination = operation->exportSnapshot->filePath(QFileInfo(url.toLocalFile()).fileName());
            if (!operation->exportSnapshot->isValid())
                result.error = copyError(nil);
            else
                coordinatedCopy(nativeUrl(url), destination, &result.error);
            dispatch_async(dispatch_get_main_queue(), ^{
                Result finalResult = result;
                UIViewController *controller = presenter();
                if (!controller && finalResult.error.isEmpty()) {
                    //% "The Files picker is not available. Close any open dialog and try again."
                    finalResult.error = qtTrId("IOSFilesPickerUnavailable");
                }
                if (!finalResult.error.isEmpty()) {
                    [operation finish:finalResult];
                    return;
                }
                operation.picker = [[UIDocumentPickerViewController alloc]
                    initForExportingURLs:@[[NSURL fileURLWithPath:nativeString(destination)]] asCopy:YES];
                operation.picker.delegate = operation;
                operation.picker.modalPresentationStyle = UIModalPresentationFormSheet;
                operation.picker.presentationController.delegate = operation;
                [controller presentViewController:operation.picker animated:YES completion:nil];
                operation.picker.presentationController.delegate = operation;
            });
        }
    });
    return true;
}
}
