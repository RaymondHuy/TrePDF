pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import TrePDF
import "ios"

Dialog {
    id: root
    objectName: "mergePdfDialog"

    required property AppTheme theme
    required property PDFMergeController controller
    property string errorText: ""
    property bool cancellationRequested: false
    property int dragFrom: -1
    property int dragTo: -1
    property real dragY: 0
    readonly property bool hasSelection: fileList.currentIndex >= 0
        && fileList.currentIndex < fileList.count
    readonly property string message: errorText.length > 0 ? errorText : controller.statusText
    signal mergeRequested(url target)

    parent: Overlay.overlay
    anchors.centerIn: dialogMetrics.active ? undefined : parent
    x: dialogMetrics.active
        ? dialogMetrics.leftInset + (dialogMetrics.availableWidth - width) / 2 : 0
    y: dialogMetrics.active
        ? dialogMetrics.topInset + Math.max(8, (dialogMetrics.availableHeight - height) / 2) : 0
    width: Math.min(720, parent ? dialogMetrics.availableWidth - 32 : 720)
    height: Math.min(620, parent ? dialogMetrics.availableHeight - 32 : 620)
    padding: 20
    modal: true
    focus: true
    popupType: Popup.Item
    closePolicy: controller.busy ? Popup.NoAutoClose : Popup.CloseOnEscape
    //% "Merge PDF"
    title: qsTrId("MergePdf")

    DialogMetrics {
        id: dialogMetrics
        overlay: root.parent
    }

    function moveSelection(destination: int) {
        if (controller.busy || !hasSelection || destination < 0 || destination >= fileList.count)
            return
        controller.moveFile(fileList.currentIndex, destination)
        fileList.currentIndex = destination
        fileList.positionViewAtIndex(destination, ListView.Contain)
        errorText = ""
    }

    function updateDrag(position: real) {
        dragY = position
        dragTo = Math.max(0, Math.min(fileList.count - 1,
            Math.floor((fileList.contentY + position) / (64 + fileList.spacing))))
    }

    function stopDrag(commit: bool) {
        const destination = dragTo
        const source = dragFrom
        dragFrom = -1
        dragTo = -1
        if (commit && source >= 0 && destination >= 0 && source !== destination) {
            fileList.currentIndex = source
            moveSelection(destination)
        }
    }

    onAboutToHide: {
        stopDrag(false)
        if (inputPickerLoader.status === Loader.Ready && inputPickerLoader.item)
            (inputPickerLoader.item as PlatformFileDialog).close()
        if (outputPickerLoader.status === Loader.Ready && outputPickerLoader.item)
            (outputPickerLoader.item as PlatformFileDialog).close()
        inputPickerLoader.active = false
        outputPickerLoader.active = false
    }

    Connections {
        target: root.controller

        function onChanged() {
            if (!root.controller.busy)
                root.cancellationRequested = false
        }

        function onCompleted(target: url) {
            root.accept()
        }
    }

    background: Rectangle {
        color: root.theme.ribbonSurface
        border.color: root.theme.ribbonBorder
        radius: 6
    }

    header: Label {
        text: root.title
        padding: 20
        bottomPadding: 0
        font.pixelSize: 18
        font.weight: Font.DemiBold
        color: root.theme.ribbonTextColor
    }

    contentItem: ScrollView {
        id: mergeScroll
        contentWidth: availableWidth
        clip: true
        contentData: [mergeFields]
    }

    readonly property ColumnLayout _mergeFields: ColumnLayout {
        id: mergeFields
        width: mergeScroll.availableWidth
        spacing: 12

        Label {
            Layout.fillWidth: true
            //% "Add PDF files, arrange their order, then save them as one new PDF. Your original files stay unchanged."
            text: qsTrId("MergePdfDescription")
            color: root.theme.ribbonTextColor
            wrapMode: Text.Wrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Button {
                objectName: "mergePdfAddFilesButton"
                //% "Add PDF files…"
                text: qsTrId("MergePdfAddFiles")
                enabled: !root.controller.busy
                onClicked: {
                    root.errorText = ""
                    inputPickerLoader.active = true
                }
            }

            Label {
                Layout.fillWidth: true
                //% "%1 files · %2 pages"
                text: qsTrId("MergePdfSummary").arg(fileList.count).arg(root.controller.totalPages)
                horizontalAlignment: Text.AlignRight
                color: root.theme.ribbonTextColor
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 240
            color: "#ffffff"
            border.color: root.theme.ribbonBorder
            radius: 4

            ListView {
                id: fileList
                objectName: "mergePdfFileList"
                anchors.fill: parent
                anchors.margins: 5
                clip: true
                spacing: 4
                model: root.controller.files
                currentIndex: -1
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                keyNavigationEnabled: !root.controller.busy
                activeFocusOnTab: true
                Accessible.role: Accessible.List
                //% "PDF files in merge order"
                Accessible.name: qsTrId("MergePdfFileList")

                delegate: ItemDelegate {
                    id: fileRow
                    required property int index
                    required property var modelData
                    objectName: "mergePdfFileRow" + index
                    width: fileList.width
                    height: 64
                    enabled: !root.controller.busy
                    highlighted: fileList.currentIndex === index
                    Accessible.name: modelData.name
                    onClicked: {
                        fileList.currentIndex = index
                        fileList.forceActiveFocus()
                    }

                    background: Rectangle {
                        radius: 3
                        color: fileRow.highlighted ? root.theme.foxitPaleOrange
                            : fileRow.hovered ? "#f8f5f2" : "transparent"
                        border.width: fileRow.highlighted || root.dragTo === fileRow.index ? 1 : 0
                        border.color: root.theme.foxitOrange
                    }

                    contentItem: RowLayout {
                        spacing: 10

                        Label {
                            id: dragGrip
                            Layout.preferredWidth: 28
                            Layout.fillHeight: true
                            text: "⠿"
                            font.pixelSize: 22
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            color: root.theme.ribbonTextColor
                            Accessible.ignored: true

                            MouseArea {
                                anchors.fill: parent
                                enabled: !root.controller.busy
                                cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                                preventStealing: true
                                onPressed: function(mouse) {
                                    fileList.currentIndex = fileRow.index
                                    root.dragFrom = fileRow.index
                                    root.updateDrag(mapToItem(fileList, mouse.x, mouse.y).y)
                                }
                                onPositionChanged: function(mouse) {
                                    if (pressed)
                                        root.updateDrag(mapToItem(fileList, mouse.x, mouse.y).y)
                                }
                                onReleased: root.stopDrag(true)
                                onCanceled: root.stopDrag(false)
                            }
                        }

                        Label {
                            Layout.preferredWidth: 24
                            text: fileRow.index + 1
                            color: root.theme.ribbonTextColor
                            horizontalAlignment: Text.AlignHCenter
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 3

                            Label {
                                Layout.fillWidth: true
                                text: fileRow.modelData.name
                                textFormat: Text.PlainText
                                elide: Text.ElideMiddle
                                font.weight: Font.DemiBold
                                color: root.theme.ribbonTextColor
                            }
                            Label {
                                Layout.fillWidth: true
                                text: fileRow.modelData.path
                                textFormat: Text.PlainText
                                elide: Text.ElideMiddle
                                font.pixelSize: 11
                                color: "#706b67"
                            }
                        }

                        Label {
                            //% "%1 pages"
                            text: qsTrId("MergePdfPageCount").arg(fileRow.modelData.pageCount)
                            color: root.theme.ribbonTextColor
                            rightPadding: 8
                        }
                    }
                }

                Label {
                    anchors.fill: parent
                    anchors.margins: 20
                    visible: fileList.count === 0
                    //% "Choose at least two PDF files to get started."
                    text: qsTrId("MergePdfEmpty")
                    color: "#706b67"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    wrapMode: Text.Wrap
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Button {
                objectName: "mergePdfMoveUpButton"
                //% "Move up"
                text: qsTrId("MergePdfMoveUp")
                enabled: !root.controller.busy && root.hasSelection && fileList.currentIndex > 0
                onClicked: root.moveSelection(fileList.currentIndex - 1)
            }
            Button {
                objectName: "mergePdfMoveDownButton"
                //% "Move down"
                text: qsTrId("MergePdfMoveDown")
                enabled: !root.controller.busy && root.hasSelection && fileList.currentIndex < fileList.count - 1
                onClicked: root.moveSelection(fileList.currentIndex + 1)
            }
            Item { Layout.fillWidth: true }
            Button {
                objectName: "mergePdfRemoveFileButton"
                //% "Remove"
                text: qsTrId("MergePdfRemoveFile")
                enabled: !root.controller.busy && root.hasSelection
                onClicked: {
                    const previousIndex = fileList.currentIndex
                    root.controller.removeFile(previousIndex)
                    fileList.currentIndex = Math.min(previousIndex, fileList.count - 1)
                    root.errorText = ""
                }
            }
        }

        Label {
            Layout.fillWidth: true
            //% "Drag the handle to reorder, or select a file and use Move up / Move down."
            text: qsTrId("MergePdfReorderHint")
            font.pixelSize: 12
            color: "#706b67"
            wrapMode: Text.Wrap
        }

        Label {
            Layout.fillWidth: true
            //% "Page content is preserved. Bookmarks, attachments and accessibility tags are not merged yet."
            text: qsTrId("MergePdfMetadataNotice")
            font.pixelSize: 12
            color: "#706b67"
            wrapMode: Text.Wrap
        }

        ProgressBar {
            Layout.fillWidth: true
            visible: root.controller.busy
            indeterminate: visible
        }

        Label {
            objectName: "mergePdfStatus"
            Layout.fillWidth: true
            visible: root.message.length > 0
            text: root.message
            textFormat: Text.PlainText
            color: root.controller.busy && root.errorText.length === 0
                ? root.theme.ribbonTextColor : "#b3261e"
            wrapMode: Text.Wrap
            Accessible.role: Accessible.AlertMessage
        }
    }

    footer: DialogButtonBox {
        padding: 16
        spacing: 8

        Button {
            objectName: "cancelMergePdfButton"
            //% "Cancel"
            text: qsTrId("Cancel")
            enabled: !root.cancellationRequested
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: {
                if (root.controller.busy) {
                    root.cancellationRequested = true
                    root.controller.cancel()
                } else {
                    root.reject()
                }
            }
        }
        OrangeButton {
            objectName: "saveMergePdfButton"
            theme: root.theme
            //% "Merge and save as…"
            text: qsTrId("MergePdfSaveAs")
            enabled: root.controller.canMerge && !root.controller.busy
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: {
                root.errorText = ""
                outputPickerLoader.active = true
            }
        }
    }

    Timer {
        interval: 50
        repeat: true
        running: root.visible && root.dragFrom >= 0
            && (root.dragY < 28 || root.dragY > fileList.height - 28)
        onTriggered: {
            const delta = root.dragY < 28 ? -12 : 12
            fileList.contentY = Math.max(0, Math.min(
                Math.max(0, fileList.contentHeight - fileList.height), fileList.contentY + delta))
            root.updateDrag(root.dragY)
        }
    }

    Loader {
        id: inputPickerLoader
        active: false
        onLoaded: {
            if (status === Loader.Ready && item)
                (item as PlatformFileDialog).open()
        }
        sourceComponent: PlatformFileDialog {
            //% "Choose PDF files to merge"
            title: qsTrId("MergePdfChooseFiles")
            fileMode: FileDialog.OpenFiles
            //% "PDF files (*.pdf)"
            nameFilters: [qsTrId("MergePdfFilesFilter")]
            onAccepted: {
                root.controller.addFiles(selectedFiles)
                inputPickerLoader.active = false
            }
            onRejected: inputPickerLoader.active = false
        }
    }

    Loader {
        id: outputPickerLoader
        active: false
        onLoaded: {
            if (status === Loader.Ready && item)
                (item as PlatformFileDialog).open()
        }
        sourceComponent: PlatformFileDialog {
            //% "Save merged PDF"
            title: qsTrId("MergePdfSaveTitle")
            fileMode: FileDialog.SaveFile
            defaultSuffix: "pdf"
            //% "PDF files (*.pdf)"
            nameFilters: [qsTrId("MergePdfFilesFilter")]
            onAccepted: {
                root.mergeRequested(selectedFile)
                outputPickerLoader.active = false
            }
            onRejected: outputPickerLoader.active = false
        }
    }
}
