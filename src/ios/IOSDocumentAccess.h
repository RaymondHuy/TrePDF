#pragma once

#include <QList>
#include <QString>
#include <QUrl>

#include <functional>

// Only linked on iOS. All callbacks run on the application's main thread.
namespace IOSDocumentAccess {
struct Result {
    QList<QUrl> files;
    QString error;
    bool cancelled = false;
};
using Completion = std::function<void(Result)>;

QString managedDirectory();
bool isManagedFile(const QUrl &url);
bool openFiles(bool images, Completion completion);
void importPdfFiles(const QList<QUrl> &urls, Completion completion);
bool exportFile(const QUrl &url, Completion completion);
}
