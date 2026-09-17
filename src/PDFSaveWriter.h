#pragma once

#include <QString>

#include <memory>

class PDFEditController;

class PDFSaveWriter
{
public:
    class Request
    {
    public:
        bool isValid() const
        {
            return static_cast<bool>(m_data);
        }

    private:
        friend class PDFSaveWriter;
        struct Data;
        std::shared_ptr<const Data> m_data;
    };

    enum class Error {
        None,
        SourceUnavailable,
        SourceOpenFailed,
        FormSnapshotFailed,
        PageLoadFailed,
        UnsupportedSourceObject,
        SourceObjectMissing,
        SourceObjectRemovalFailed,
        FontUnavailable,
        TextObjectCreationFailed,
        ImageObjectCreationFailed,
        ShapeObjectCreationFailed,
        TextMarkupAnnotationCreationFailed,
        CommentAnnotationCreationFailed,
        CommentAnnotationUpdateFailed,
        CommentAnnotationRemovalFailed,
        WebLinkCreationFailed,
        WebLinkRemovalFailed,
        PageCreationFailed,
        PageDeletionFailed,
        PageContentGenerationFailed,
        AcroFormCreationFailed,
        OutputOpenFailed,
        OutputWriteFailed,
        OutputCommitFailed
    };

    struct Result {
        Error error = Error::None;
        QString detail;
        int page = -1;
        uint unsupportedCodePoint = 0;
        bool fontBold = false;
        bool fontItalic = false;
        bool fontDownloadSuggested = false;

        bool succeeded() const
        {
            return error == Error::None;
        }
    };

    static Result save(const PDFEditController &controller,
                       const QString &targetFilePath);
    static Request prepare(const PDFEditController &controller,
                           const QString &targetFilePath,
                           const QString &sourceFilePathOverride = {});
    static Result save(const Request &request);
};
