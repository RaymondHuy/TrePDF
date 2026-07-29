pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TrePDF

Rectangle {
    id: root

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
    property real zoom: 1.0
    property int viewRotation: 0
    property string transientStatus
    property int pendingNavigationPage: -1
    property int navigationGeneration: 0
    property int sessionChangeGeneration: 0
    property bool formDesignModePrepared: false
    readonly property bool formDesignMode:
        formsTabActive || editTabActive
    readonly property real viewportWidth: viewer.width
    readonly property real contentX: viewer.contentX
    readonly property real contentY: viewer.contentY

    signal openRequested()
    signal beginSelectedEditingRequested()

    color: "#f2efec"

    function syncFormDesignMode() {
        if (!formDesignMode) {
            formDesignModePrepared = false
            return
        }
        if (formDesignModePrepared)
            return

        formDesignModePrepared = true
        formDesignerController.prepareForDesignMode()
    }

    function navigateToPage(page) {
        if (!document.loaded || page < 0 || page >= document.pageCount)
            return

        const generation = ++navigationGeneration
        pendingNavigationPage = page
        document.setCurrentPage(page)

        Qt.callLater(function() {
            if (root.navigationGeneration !== generation
                    || root.pendingNavigationPage !== page) {
                return
            }

            if (!root.document.loaded || page >= root.document.pageCount) {
                root.pendingNavigationPage = -1
                return
            }

            viewer.positionViewAtIndex(page, ListView.Beginning)
            viewer.contentY = Math.max(0, viewer.contentY - 16)

            Qt.callLater(function() {
                if (root.navigationGeneration !== generation
                        || root.pendingNavigationPage !== page) {
                    return
                }

                root.document.setCurrentPage(page)
                root.pendingNavigationPage = -1
            })
        })
    }

    function prepareForSessionChange() {
        ++sessionChangeGeneration
        editController.finishSelectedFrameResize()
        editController.finishSelectedTextEdit()
        ++navigationGeneration
        pendingNavigationPage = -1
    }

    function beginSelectedEditing() {
        if (editController.selectedPage < 0)
            return false

        beginSelectedEditingRequested()
        return editController.textEditActive
    }

    function restorePosition(savedContentX, savedContentY) {
        prepareForSessionChange()
        const generation = navigationGeneration
        Qt.callLater(function() {
            if (root.navigationGeneration !== generation || !root.document.loaded)
                return

            viewer.contentX = Math.max(
                0,
                Math.min(Number(savedContentX),
                         Math.max(0, viewer.contentWidth - viewer.width)))
            viewer.contentY = Math.max(
                0,
                Math.min(Number(savedContentY),
                         Math.max(0, viewer.contentHeight - viewer.height)))
            root.syncCurrentPageFromScroll()
        })
    }

    function syncCurrentPageFromScroll() {
        if (pendingNavigationPage >= 0 || !document.loaded || viewer.count <= 0)
            return

        const page = viewer.indexAt(viewer.contentX + viewer.width * 0.5,
                                    viewer.contentY + viewer.height * 0.35)
        if (page >= 0 && document.currentPage !== page)
            document.setCurrentPage(page)
    }

    onFormDesignModeChanged: syncFormDesignMode()
    onFormDesignerControllerChanged: {
        formDesignModePrepared = false
        syncFormDesignMode()
    }
    Component.onCompleted: syncFormDesignMode()

    Connections {
        target: root.formController

        function onPageRollbackRequested(page) {
            root.navigateToPage(page)
        }

        function onPageNavigationRequested(page) {
            root.navigateToPage(page)
        }
    }

    ListView {
        id: viewer

        anchors.fill: parent
        visible: root.document.loaded
        model: root.document.loaded ? root.document.pageCount : 0
        spacing: 28
        reuseItems: true
        cacheBuffer: Math.min(Math.max(height * 0.5, 256), 640)
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.AutoFlickIfNeeded
        currentIndex: -1

        header: Item {
            width: ListView.view ? ListView.view.width : 0
            height: 32
        }

        footer: Item {
            width: ListView.view ? ListView.view.width : 0
            height: 32
        }

        ScrollBar.vertical: ScrollBar {
            implicitWidth: 12
            policy: ScrollBar.AlwaysOn
            palette.mid: "#847e79"
            palette.dark: "#625c58"
        }

        ScrollBar.horizontal: ScrollBar {
            policy: ScrollBar.AsNeeded
        }

        onContentYChanged: root.syncCurrentPageFromScroll()

        delegate: PdfPageDelegate {
            id: pageDelegate

            theme: root.theme
            document: root.document
            editController: root.editController
            formController: root.formController
            formDesignerController: root.formDesignerController
            formsTabActive: root.formsTabActive
            editTabActive: root.editTabActive
            formFieldsVisible: root.formFieldsVisible
            formFieldsInteractive: root.formFieldsInteractive
            textFieldToolActive: root.textFieldToolActive
            zoom: root.zoom
            viewRotation: root.viewRotation
            sessionChangeGeneration: root.sessionChangeGeneration

            Connections {
                target: root

                function onBeginSelectedEditingRequested() {
                    if (!pageDelegate.pooled
                            && pageDelegate.editController.selectedPage
                                === pageDelegate.index) {
                        pageDelegate.beginSelectedEditing()
                    }
                }
            }
        }
    }

    ColumnLayout {
        anchors.centerIn: parent
        spacing: 14
        visible: !root.document.loaded

        Label {
            //% "No document open"
            text: qsTrId("NoDocumentOpen")
            font.pixelSize: 22
            color: "#33251d"
            Layout.alignment: Qt.AlignHCenter
        }

        OrangeButton {
            theme: root.theme
            //% "Open PDF"
            text: qsTrId("OpenPdf")
            onClicked: root.openRequested()
            Layout.alignment: Qt.AlignHCenter
        }
    }

    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 20
        width: Math.min(parent.width - 32, statusToastLabel.implicitWidth + 28)
        height: 36
        radius: 4
        color: "#e63a3b3f"
        visible: root.transientStatus.length > 0
        z: 10

        Label {
            id: statusToastLabel

            anchors.fill: parent
            anchors.leftMargin: 14
            anchors.rightMargin: 14
            text: root.transientStatus
            textFormat: Text.PlainText
            color: "white"
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }
}
