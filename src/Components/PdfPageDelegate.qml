import QtQuick
import QtQuick.Controls.Basic
import TrePDF

Item {
    id: pageCard

    required property int index
    required property AppTheme theme
    required property PDFDocument document
    required property PDFEditController editController
    required property PDFFormController formController
    required property PDFFormDesignerController formDesignerController
    property bool formsTabActive: false
    property bool editTabActive: false
    property bool formFieldsVisible: false
    property bool formFieldsInteractive: false
    property bool textFieldToolActive: false
    property bool checkboxToolActive: false
    property bool radioButtonToolActive: false
    property bool dropdownToolActive: false
    property bool listBoxToolActive: false
    property bool combFieldToolActive: false
    property real zoom: 1.0
    property int viewRotation: 0
    property bool pooled: false
    property int sessionChangeGeneration: 0
    readonly property real pageMargin: 32
    readonly property Item viewportItem: ListView.view
    readonly property real viewWidth: ListView.view ? ListView.view.width : 0
    readonly property real viewContentX:
        ListView.view ? ListView.view.contentX : 0
    readonly property real viewContentY:
        ListView.view ? ListView.view.contentY : 0
    readonly property int normalizedViewRotation:
        ((viewRotation % 360) + 360) % 360
    readonly property bool quarterTurn:
        normalizedViewRotation === 90 || normalizedViewRotation === 270
    readonly property real maximumViewPageWidth: quarterTurn
        ? document.maximumPageHeight
        : document.maximumPageWidth

    signal deleteCommentRequested(int page, string objectId)

    width: Math.max(viewWidth, maximumViewPageWidth * zoom + pageMargin * 2)
    height: pageFrame.height + pageLabel.implicitHeight + 12

    function prepareForSessionChange() {
        pageInteractionOverlay.prepareForSessionChange()
    }

    function beginSelectedEditing() {
        pageInteractionOverlay.beginSelectedEditing()
    }

    onSessionChangeGenerationChanged: prepareForSessionChange()

    ListView.onPooled: pooled = true
    ListView.onReused: pooled = false

    Item {
        id: pageFrame

        x: Math.max(pageCard.pageMargin, (pageCard.width - width) / 2)
        width: pageCard.quarterTurn
            ? pageView.implicitHeight
            : pageView.implicitWidth
        height: pageCard.quarterTurn
            ? pageView.implicitWidth
            : pageView.implicitHeight
    }

    Rectangle {
        x: pageFrame.x + 4
        y: pageFrame.y + 6
        width: pageFrame.width
        height: pageFrame.height
        color: "#24000000"
        radius: 2
        visible: pageFrame.width > 0 && pageFrame.height > 0 && !pageCard.pooled
    }

    Item {
        id: pageSurface

        anchors.centerIn: pageFrame
        width: pageView.implicitWidth
        height: pageView.implicitHeight
        rotation: pageCard.normalizedViewRotation
        visible: !pageCard.pooled

        PDFPageView {
            id: pageView

            document: pageCard.pooled ? null : pageCard.document
            editController: pageCard.pooled ? null : pageCard.editController
            formController: pageCard.pooled ? null : pageCard.formController
            formDesignerController: pageCard.pooled
                ? null
                : pageCard.formDesignerController
            formDesignMode: pageInteractionOverlay.formDesignMode
            page: pageCard.index
            zoom: pageCard.zoom
            pageColor: "white"
            width: implicitWidth
            height: implicitHeight
        }

        Rectangle {
            anchors.fill: pageView
            color: "transparent"
            border.color: pageCard.index === pageCard.document.currentPage
                ? pageCard.theme.foxitOrange
                : "#c6cad1"
            border.width: pageCard.index === pageCard.document.currentPage ? 2 : 1
            visible: pageView.width > 0 && pageView.height > 0
        }

        PageInteractionOverlay {
            id: pageInteractionOverlay

            anchors.fill: pageView
            theme: pageCard.theme
            editController: pageCard.editController
            formController: pageCard.formController
            formDesignerController: pageCard.formDesignerController
            page: pageCard.index
            pagePointScale: pageCard.zoom
            pooled: pageCard.pooled
            formsTabActive: pageCard.formsTabActive
            editTabActive: pageCard.editTabActive
            formFieldsVisible: pageCard.formFieldsVisible
            formFieldsInteractive: pageCard.formFieldsInteractive
            textFieldToolActive: pageCard.textFieldToolActive
            checkboxToolActive: pageCard.checkboxToolActive
            radioButtonToolActive: pageCard.radioButtonToolActive
            dropdownToolActive: pageCard.dropdownToolActive
            listBoxToolActive: pageCard.listBoxToolActive
            combFieldToolActive: pageCard.combFieldToolActive
            draftTextureCurrent: pageView.draftTextureCurrent
            draftTextureReady: pageView.draftTextureReady
            viewportItem: pageCard.viewportItem
            viewportContentX: pageCard.viewContentX
            viewportContentY: pageCard.viewContentY
            viewportTransformState: [
                pageCard.normalizedViewRotation,
                pageCard.zoom,
                pageCard.width,
                pageCard.height,
                pageFrame.x,
                pageFrame.y,
                pageSurface.x,
                pageSurface.y,
                pageSurface.width,
                pageSurface.height
            ]
            z: 1
            onDeleteCommentRequested: (page, objectId) =>
                pageCard.deleteCommentRequested(page, objectId)
        }
    }

    Label {
        id: pageLabel

        //% "Page %1"
        text: qsTrId("PageNumber").arg(pageCard.index + 1)
        color: pageCard.index === pageCard.document.currentPage
            ? pageCard.theme.foxitDarkOrange
            : "#6b625d"
        font.pixelSize: 12
        x: Math.max(pageCard.pageMargin, (pageCard.width - width) / 2)
        y: pageFrame.height + 8
        visible: !pageCard.pooled
    }
}
