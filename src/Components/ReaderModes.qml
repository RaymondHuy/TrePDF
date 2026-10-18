import QtQuick

QtObject {
    // Values match PDFDocumentSession so desktop bindings retain their contract.
    enum PageDisplayMode {
        SinglePageMode = 0,
        ContinuousMode = 1,
        FacingMode = 2,
        ContinuousFacingMode = 3
    }

    enum SplitViewMode {
        NoSplit = 0,
        VerticalSplit = 1,
        HorizontalSplit = 2
    }
}
