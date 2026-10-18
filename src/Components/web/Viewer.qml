pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TrePDFWeb as Backend
import ".."
import "../platform"

AppWindow {
    id: root

    title: pdf.fileName.length > 0 ? pdf.fileName + " — TrePDF Web" : "TrePDF Web"
    color: "#f2efec"

    readonly property bool hasDocument: pdf.pageCount > 0
    readonly property bool canOpen: pdf.ready && !pdf.busy
    readonly property bool hasDrafts: pdf.dirty || browserEditor.hasPendingInput
    readonly property var supportedModes: [ReaderModes.SinglePageMode,
        ReaderModes.ContinuousMode]
    property var viewContext: ({})
    property string pendingAction: ""
    property bool exportAfterApply: false

    function requestOpen(): void {
        if (hasDrafts) {
            pendingAction = "open"
            discardDialog.open()
        } else {
            pdf.openFile()
        }
    }

    function requestClose(): void {
        if (hasDrafts) {
            pendingAction = "close"
            discardDialog.open()
        } else {
            pdf.closeFile()
        }
    }

    function savePdf(): void {
        if (pdf.busy || !hasDocument)
            return
        Qt.inputMethod.commit()
        if (browserEditor.hasPendingInput) {
            exportAfterApply = true
            if (!browserEditor.applyProperties()) {
                exportAfterApply = false
                ribbonTabs.setCurrentIndex(1)
            }
        } else {
            pdf.downloadFile()
        }
    }

    AppTheme { id: appTheme }
    Backend.BrowserDocument { id: pdf }
    BrowserEditController {
        id: browserEditor
        document: pdf
        active: ribbonTabs.currentIndex === 1
        onHasPendingInputChanged: pdf.setPendingInput(hasPendingInput)
    }
    ListModel { id: tabs }
    QtObject {
        id: tabSession
        readonly property string title: pdf.fileName
        readonly property string filePath: pdf.fileName
        readonly property bool hasDrafts: root.hasDrafts
        readonly property bool saving: pdf.busy
    }

    Connections {
        target: pdf
        function onPagesChanged(): void {
            tabs.clear()
            if (root.hasDocument)
                tabs.append({ "session": tabSession })
            root.viewContext = ({})
            root.exportAfterApply = false
        }
        function onEdited(selectedId: int): void {
            if (root.exportAfterApply) {
                root.exportAfterApply = false
                Qt.callLater(pdf.downloadFile)
            }
        }
        function onStateChanged(): void {
            if (!pdf.busy && pdf.error.length > 0)
                root.exportAfterApply = false
        }
    }

    header: Column {
        width: root.width

        RibbonTabStrip {
            id: ribbonTabs
            width: parent.width
            theme: appTheme
            enabledTabs: [0, 1]
            enabled: !pdf.busy
            hasUnsavedChanges: root.hasDrafts
        }
        RibbonSurface {
            width: parent.width
            height: implicitHeight
            theme: appTheme
            Loader {
                anchors.fill: parent
                sourceComponent: browserEditor.active ? editTools : viewTools
            }
        }
        DocumentTabs {
            width: parent.width
            theme: appTheme
            model: tabs
            currentIndex: root.hasDocument ? 0 : -1
            enabled: root.canOpen
            onOpenRequested: root.requestOpen()
            onCloseRequested: root.requestClose()
        }
    }

    Component {
        id: viewTools
        ViewRibbonTools {
            theme: appTheme
            documentLoaded: root.hasDocument
            openAvailable: root.canOpen
            saveAvailable: pdf.canEdit
            hasUnsavedChanges: root.hasDrafts
            saving: pdf.busy
            saveAsAvailable: !pdf.busy
            searchAvailable: false
            splitViewSupported: false
            allowedPageDisplayModes: root.supportedModes
            pageDisplayMode: viewport.pageDisplayMode
            canNavigateBackward: root.hasDocument && viewport.currentPage > 0
            canNavigateForward: root.hasDocument && viewport.currentPage + 1 < pdf.pageCount
            onOpenRequested: root.requestOpen()
            onSaveRequested: root.savePdf()
            onSaveAsRequested: root.savePdf()
            onPageRequested: page => viewport.goToPage(page)
            onPageStepRequested: direction => viewport.goToPage(viewport.currentPage + direction)
            onZoomOutRequested: viewport.setZoom(viewport.effectiveZoom - 0.1)
            onZoomInRequested: viewport.setZoom(viewport.effectiveZoom + 0.1)
            onFitWidthRequested: viewport.fitToWidth()
            onRotateRequested: turns => viewport.rotate(turns)
            onPageDisplayModeRequested: mode => viewport.setPageDisplayMode(mode)
        }
    }
    Component {
        id: editTools
        EditRibbonTools {
            theme: appTheme
            documentLoaded: root.hasDocument && pdf.canEdit
            documentContext: pdf
            pageCount: pdf.pageCount
            currentPage: viewport.currentPage
            currentTool: browserEditor.tool
            currentShape: browserEditor.shape
            saving: pdf.busy
            canUndo: pdf.canUndo && !browserEditor.hasPendingInput
            canRedo: pdf.canRedo && !browserEditor.hasPendingInput
            textEditingAvailable: true
            imageAddingAvailable: false
            linksAvailable: false
            addPageAvailable: false
            deletePageAvailable: false
            mergeAvailable: false
            onSelectToolRequested: tool => browserEditor.selectTool(tool)
            onShapeRequested: shape => browserEditor.selectShape(shape)
            onUndoRequested: pdf.undo()
            onRedoRequested: pdf.redo()
        }
    }

    footer: ReaderControlBar {
        theme: appTheme
        documentLoaded: root.hasDocument
        navigationPage: viewport.currentPage
        pageCount: pdf.pageCount
        zoom: viewport.effectiveZoom
        pageDisplayMode: viewport.pageDisplayMode
        allowedPageDisplayModes: root.supportedModes
        viewReady: root.hasDocument && !pdf.busy
        viewContextOwner: root.viewContext
        onPageRequested: page => viewport.goToPage(page)
        onPageStepRequested: direction => viewport.goToPage(viewport.currentPage + direction)
        onZoomRequested: value => viewport.setZoom(value)
        onPageDisplayModeRequested: mode => viewport.setPageDisplayMode(mode)
        onViewRestoreRequested: (page, zoom) => {
            viewport.setZoom(zoom)
            viewport.goToPage(page)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        Label {
            Layout.fillWidth: true
            visible: pdf.error.length > 0 || browserEditor.message.length > 0
            text: pdf.error || browserEditor.message
            textFormat: Text.PlainText
            padding: 10
            wrapMode: Text.Wrap
            color: "#8b2f24"
            background: Rectangle { color: "#fff0e9" }
        }
        RowLayout {
            Layout.fillWidth: true
            visible: browserEditor.active
            spacing: 8
            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 12
                text: root.hasDocument && !pdf.canEdit ? pdf.editReason
                    : qsTr("Sửa văn bản: bấm vào chữ trên trang để gõ. Dùng Chọn để di chuyển, đổi kích thước hoặc thay ảnh.")
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: "#817c78"
                font.pixelSize: 12
            }
            Button {
                text: qsTr("Bỏ thay đổi")
                enabled: root.hasDrafts && !pdf.busy
                onClicked: {
                    root.pendingAction = "discard"
                    discardDialog.open()
                }
            }
            OrangeButton {
                Layout.rightMargin: 10
                theme: appTheme
                text: qsTr("Lưu PDF")
                enabled: root.hasDocument && !pdf.busy
                onClicked: root.savePdf()
            }
        }
        Label {
            Layout.fillWidth: true
            visible: browserEditor.active && root.hasDocument && pdf.sourceNotice.length > 0
            text: pdf.sourceNotice
            textFormat: Text.PlainText
            padding: 8
            wrapMode: Text.Wrap
            color: "#8b5a2b"
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                DocumentViewport {
                    id: viewport
                    anchors.fill: parent
                    document: pdf
                    editor: browserEditor
                    visible: root.hasDocument
                }
                DocumentEmptyState {
                    anchors.centerIn: parent
                    theme: appTheme
                    openEnabled: root.canOpen
                    visible: !root.hasDocument && !pdf.busy && pdf.ready
                    onOpenRequested: root.requestOpen()
                }
                BusyIndicator {
                    anchors.centerIn: parent
                    running: pdf.busy || (!pdf.ready && pdf.error.length === 0)
                    visible: running
                }
                Label {
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: 10
                    padding: 6
                    font.pixelSize: 11
                    text: qsTr("Web preview")
                    color: "#817c78"
                    background: Rectangle { color: "#f2efec"; radius: 4 }
                }
            }
            Loader {
                Layout.preferredWidth: Math.min(280, root.width * 0.4)
                Layout.fillHeight: true
                active: browserEditor.active && browserEditor.workingObject !== null
                visible: active
                sourceComponent: Component {
                    EditPropertiesPanel { controller: browserEditor; theme: appTheme }
                }
            }
        }
    }

    Dialog {
        id: discardDialog
        title: qsTr("Thay đổi chưa lưu")
        anchors.centerIn: parent
        width: Math.min(420, root.width - 32)
        modal: true
        popupType: Popup.Item
        standardButtons: Dialog.Discard | Dialog.Cancel
        Label {
            width: parent.width
            text: qsTr("Bỏ phần chỉnh sửa chưa lưu? Chọn Hủy để quay lại lưu PDF trước.")
            wrapMode: Text.Wrap
        }
        onDiscarded: {
            browserEditor.cancelPending()
            if (root.pendingAction === "discard") {
                pdf.discard()
            } else {
                pdf.closeFile()
                if (root.pendingAction === "open")
                    pdf.openFile()
            }
            root.pendingAction = ""
        }
        onRejected: root.pendingAction = ""
    }

    Shortcut {
        sequences: [StandardKey.Open]
        enabled: root.canOpen
        onActivated: root.requestOpen()
    }
    Shortcut {
        sequences: [StandardKey.Save, StandardKey.SaveAs]
        enabled: root.hasDocument && !pdf.busy
        onActivated: root.savePdf()
    }
    Shortcut {
        sequences: [StandardKey.Undo]
        enabled: browserEditor.active && pdf.canUndo && !pdf.busy
            && !browserEditor.hasPendingInput && !browserEditor.textEditing
        onActivated: pdf.undo()
    }
    Shortcut {
        sequences: [StandardKey.Redo]
        enabled: browserEditor.active && pdf.canRedo && !pdf.busy
            && !browserEditor.hasPendingInput && !browserEditor.textEditing
        onActivated: pdf.redo()
    }
}
