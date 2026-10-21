pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import TrePDF
import "ios"

Dialog {
    id: root
    objectName: "splitPdfDialog"

    required property AppTheme theme
    required property PDFSplitController controller
    property string errorText: ""
    property bool cancellationRequested: false
    property url completedFolder: ""
    property int completedCount: 0
    readonly property bool completed: completedFolder.toString().length > 0
    readonly property string message: errorText.length > 0 ? errorText : controller.statusText
    readonly property bool awaitingPassword: controller.passwordRequired
    readonly property url selectedSource: controller.source
    signal sourceRequested(url source)
    signal splitRequested(url folder)

    parent: Overlay.overlay
    anchors.centerIn: dialogMetrics.active ? undefined : parent
    x: dialogMetrics.active
        ? dialogMetrics.leftInset + (dialogMetrics.availableWidth - width) / 2 : 0
    y: dialogMetrics.active
        ? dialogMetrics.topInset + Math.max(8, (dialogMetrics.availableHeight - height) / 2) : 0
    width: Math.min(600, parent ? dialogMetrics.availableWidth - 32 : 600)
    height: Math.min(620, parent ? dialogMetrics.availableHeight - 32 : 620)
    padding: 20
    modal: true
    focus: true
    popupType: Popup.Item
    closePolicy: controller.busy ? Popup.NoAutoClose : Popup.CloseOnEscape
    //% "Split PDF"
    title: qsTrId("SplitPdf")

    DialogMetrics {
        id: dialogMetrics
        overlay: root.parent
    }

    function clearPasswordInput() {
        if (passwordInput)
            passwordInput.clear()
        if (showPassword)
            showPassword.checked = false
    }

    function unlockSource() {
        if (!awaitingPassword || controller.busy || passwordInput.text.length === 0)
            return
        errorText = ""
        controller.unlockSource(passwordInput.text)
        clearPasswordInput()
    }

    function focusPasswordInput() {
        if (visible && awaitingPassword && !controller.busy)
            passwordInput.forceActiveFocus()
    }

    onOpened: focusPasswordInput()
    onSelectedSourceChanged: clearPasswordInput()
    onAwaitingPasswordChanged: {
        if (!awaitingPassword)
            clearPasswordInput()
        else
            Qt.callLater(root.focusPasswordInput)
    }

    onAboutToHide: {
        clearPasswordInput()
        if (inputPickerLoader.status === Loader.Ready && inputPickerLoader.item)
            (inputPickerLoader.item as PlatformFileDialog).close()
        if (folderPickerLoader.status === Loader.Ready && folderPickerLoader.item)
            (folderPickerLoader.item as FolderDialog).close()
        inputPickerLoader.active = false
        folderPickerLoader.active = false
    }

    Connections {
        target: root.controller

        function onChanged() {
            if (!root.controller.busy) {
                root.cancellationRequested = false
                if (root.awaitingPassword)
                    Qt.callLater(root.focusPasswordInput)
            }
        }

        function onCompleted(folder: url, outputs: list<url>) {
            root.errorText = ""
            root.completedCount = outputs.length
            root.completedFolder = folder
        }
    }

    background: Rectangle {
        color: root.theme.ribbonSurface
        border.color: root.theme.ribbonBorder
        radius: 6
    }

    header: Label {
        text: root.title
        renderType: Text.NativeRendering
        padding: 20
        bottomPadding: 0
        font.pixelSize: 18
        font.weight: Font.DemiBold
        color: root.theme.ribbonTextColor
    }

    contentItem: ScrollView {
        id: splitScroll
        contentWidth: availableWidth
        clip: true
        contentData: [splitFields]
    }

    readonly property ColumnLayout _splitFields: ColumnLayout {
        id: splitFields
        width: splitScroll.availableWidth
        spacing: 12

        Label {
            Layout.fillWidth: true
            visible: !root.completed
            //% "Turn one PDF into smaller files. Choose how to group the pages; your original PDF stays unchanged."
            text: qsTrId("SplitPdfDescription")
            color: root.theme.ribbonTextColor
            wrapMode: Text.Wrap
        }

        RowLayout {
            Layout.fillWidth: true
            visible: !root.completed
            spacing: 12

            Button {
                objectName: "splitPdfChooseFileButton"
                //% "Choose PDF…"
                text: qsTrId("SplitPdfChooseFile")
                enabled: !root.controller.busy
                onClicked: {
                    root.errorText = ""
                    root.clearPasswordInput()
                    inputPickerLoader.active = true
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 3

                Label {
                    objectName: "splitPdfFileName"
                    Layout.fillWidth: true
                    text: root.controller.fileName.length > 0
                        ? root.controller.fileName
                        //% "No PDF selected"
                        : qsTrId("SplitPdfNoFile")
                    textFormat: Text.PlainText
                    elide: Text.ElideMiddle
                    font.weight: Font.DemiBold
                    color: root.theme.ribbonTextColor
                }
                Label {
                    Layout.fillWidth: true
                    visible: root.controller.pageCount > 0
                    //% "%1 pages"
                    text: qsTrId("SplitPdfPageCount").arg(root.controller.pageCount)
                    color: "#706b67"
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: !root.completed && root.awaitingPassword
            enabled: !root.controller.busy

            Label {
                Layout.fillWidth: true
                //% "Unlock this PDF to continue"
                text: qsTrId("SplitPdfUnlockTitle")
                font.weight: Font.DemiBold
                color: root.theme.ribbonTextColor
                wrapMode: Text.Wrap
            }

            Label {
                Layout.fillWidth: true
                //% "Enter the PDF password. TrePDF uses it only for this split and keeps the original password protection on every new file."
                text: qsTrId("SplitPdfPasswordHint")
                color: root.theme.ribbonTextColor
                wrapMode: Text.Wrap
            }

            TextField {
                id: passwordInput
                objectName: "splitPdfPasswordInput"
                Layout.fillWidth: true
                echoMode: showPassword.checked ? TextInput.Normal : TextInput.Password
                inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                    | Qt.ImhNoAutoUppercase
                // Keep one extra character so oversized input reaches validation.
                maximumLength: 1025
                selectByMouse: true
                //% "PDF password"
                placeholderText: qsTrId("SplitPdfPassword")
                Accessible.name: placeholderText
                onTextEdited: root.errorText = ""
                onAccepted: root.unlockSource()
            }

            RowLayout {
                Layout.fillWidth: true

                CheckBox {
                    id: showPassword
                    objectName: "splitPdfShowPasswordCheckBox"
                    //% "Show password"
                    text: qsTrId("ShowPdfPassword")
                }
                Item { Layout.fillWidth: true }
                Button {
                    objectName: "splitPdfUnlockButton"
                    //% "Unlock PDF"
                    text: qsTrId("SplitPdfUnlock")
                    enabled: passwordInput.text.length > 0 && !root.controller.busy
                    onClicked: root.unlockSource()
                }
            }

            Label {
                objectName: "splitPdfPasswordStatus"
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

        Label {
            objectName: "splitPdfEncryptionNotice"
            Layout.fillWidth: true
            visible: root.controller.encryptedSource && !root.awaitingPassword
            //% "Split files keep the original PDF’s password protection."
            text: qsTrId("SplitPdfEncryptionNotice")
            color: root.theme.ribbonTextColor
            wrapMode: Text.Wrap
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 4
            visible: !root.completed && !root.awaitingPassword
            enabled: !root.controller.busy

            Label {
                Layout.fillWidth: true
                //% "How would you like to split it?"
                text: qsTrId("SplitPdfMethod")
                font.weight: Font.DemiBold
                color: root.theme.ribbonTextColor
                wrapMode: Text.Wrap
            }

            RadioButton {
                objectName: "splitPdfEachPageOption"
                Layout.fillWidth: true
                //% "One file per page"
                text: qsTrId("SplitPdfEachPage")
                checked: root.controller.mode === 0
                onClicked: {
                    root.errorText = ""
                    root.controller.mode = 0
                }
            }

            RadioButton {
                objectName: "splitPdfEveryNOption"
                Layout.fillWidth: true
                //% "Split into groups of pages"
                text: qsTrId("SplitPdfEveryN")
                checked: root.controller.mode === 1
                onClicked: {
                    root.errorText = ""
                    root.controller.mode = 1
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 28
                visible: root.controller.mode === 1
                spacing: 10

                Label {
                    //% "Pages per file"
                    text: qsTrId("SplitPdfPagesPerFile")
                    color: root.theme.ribbonTextColor
                }
                SpinBox {
                    objectName: "splitPdfPagesPerFileInput"
                    from: 1
                    to: Math.max(1, root.controller.pageCount)
                    value: root.controller.pagesPerFile
                    editable: true
                    //% "Pages per file"
                    Accessible.name: qsTrId("SplitPdfPagesPerFile")
                    onValueModified: {
                        root.errorText = ""
                        root.controller.pagesPerFile = value
                    }
                }
                Item { Layout.fillWidth: true }
            }

            RadioButton {
                objectName: "splitPdfCustomOption"
                Layout.fillWidth: true
                //% "Choose page ranges"
                text: qsTrId("SplitPdfCustom")
                checked: root.controller.mode === 2
                onClicked: {
                    root.errorText = ""
                    root.controller.mode = 2
                }
            }

            TextField {
                objectName: "splitPdfRangesInput"
                Layout.fillWidth: true
                Layout.leftMargin: 28
                visible: root.controller.mode === 2
                text: root.controller.pageRanges
                maximumLength: 4096
                selectByMouse: true
                //% "Example: 1-3; 4-6; 7,9"
                placeholderText: qsTrId("SplitPdfRangesExample")
                //% "Choose page ranges"
                Accessible.name: qsTrId("SplitPdfCustom")
                onTextEdited: {
                    root.errorText = ""
                    root.controller.pageRanges = text
                }
            }

            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 28
                visible: root.controller.mode === 2
                //% "Separate files with a semicolon (;). Use a dash for a range and commas for individual pages. For example, 1-3; 4-6; 7,9 makes three files. Only the pages you enter are included."
                text: qsTrId("SplitPdfRangesHint")
                font.pixelSize: 12
                color: "#706b67"
                wrapMode: Text.Wrap
            }
        }

        Label {
            objectName: "splitPdfOutputSummary"
            Layout.fillWidth: true
            visible: !root.completed && root.controller.outputCount > 0
            //% "%1 PDF files will be created."
            text: qsTrId("SplitPdfOutputSummary").arg(root.controller.outputCount)
            font.weight: Font.DemiBold
            color: root.theme.ribbonTextColor
            wrapMode: Text.Wrap
        }

        Label {
            Layout.fillWidth: true
            visible: !root.completed && !root.awaitingPassword
            //% "Choose a destination next. TrePDF creates a new folder there for the split files, without replacing existing files."
            text: qsTrId("SplitPdfDestinationHint")
            color: "#706b67"
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }

        Label {
            Layout.fillWidth: true
            visible: !root.completed
            //% "Standard fillable forms are kept. Digital signatures, XFA forms and forms with scripts or calculations are not supported. Bookmarks, document-level attachments and accessibility tags are not included. Links to omitted pages are disabled."
            text: qsTrId("SplitPdfSupportNotice")
            color: "#706b67"
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }

        Label {
            objectName: "splitPdfCompletionSummary"
            Layout.fillWidth: true
            visible: root.completed
            //% "Created %1 PDF files in a new folder."
            text: qsTrId("SplitPdfCompletedSummary").arg(root.completedCount)
            font.pixelSize: 16
            font.weight: Font.DemiBold
            color: root.theme.ribbonTextColor
            wrapMode: Text.Wrap
        }

        TextEdit {
            objectName: "splitPdfCompletedFolder"
            Layout.fillWidth: true
            visible: root.completed
            text: root.completedFolder.toString()
            textFormat: TextEdit.PlainText
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.WrapAnywhere
            color: root.theme.ribbonTextColor
            Accessible.role: Accessible.StaticText
            //% "Output folder"
            Accessible.name: qsTrId("SplitPdfOutputFolder")
        }

        ProgressBar {
            objectName: "splitPdfProgress"
            Layout.fillWidth: true
            visible: root.controller.busy
            value: root.controller.progress
            indeterminate: visible && value <= 0
        }

        Label {
            objectName: "splitPdfStatus"
            Layout.fillWidth: true
            visible: !root.completed && !root.awaitingPassword && root.message.length > 0
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
        alignment: Qt.AlignRight

        Button {
            objectName: "cancelSplitPdfButton"
            text: root.completed
                //% "Close"
                ? qsTrId("Close")
                //% "Cancel"
                : qsTrId("Cancel")
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
            objectName: "saveSplitPdfButton"
            theme: root.theme
            visible: !root.completed
            //% "Choose folder and split…"
            text: qsTrId("SplitPdfChooseFolderAndSplit")
            enabled: root.controller.canSplit && !root.controller.busy
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: {
                root.errorText = ""
                folderPickerLoader.active = true
            }
        }
        OrangeButton {
            objectName: "openSplitPdfFolderButton"
            theme: root.theme
            visible: root.completed
            //% "Open folder"
            text: qsTrId("SplitPdfOpenFolder")
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: Qt.openUrlExternally(root.completedFolder)
        }
    }

    Loader {
        id: inputPickerLoader
        objectName: "splitPdfInputPickerLoader"
        active: false
        onLoaded: {
            if (status === Loader.Ready && item)
                (item as PlatformFileDialog).open()
        }
        sourceComponent: PlatformFileDialog {
            //% "Choose a PDF to split"
            title: qsTrId("SplitPdfChooseTitle")
            fileMode: FileDialog.OpenFile
            //% "PDF files (*.pdf)"
            nameFilters: [qsTrId("MergePdfFilesFilter")]
            onAccepted: {
                root.clearPasswordInput()
                root.sourceRequested(selectedFile)
                inputPickerLoader.active = false
            }
            onRejected: inputPickerLoader.active = false
        }
    }

    Loader {
        id: folderPickerLoader
        objectName: "splitPdfFolderPickerLoader"
        active: false
        onLoaded: {
            if (status === Loader.Ready && item)
                (item as FolderDialog).open()
        }
        sourceComponent: FolderDialog {
            objectName: "splitPdfFolderPicker"
            //% "Choose where to save the split PDF files"
            title: qsTrId("SplitPdfChooseFolderTitle")
            onAccepted: {
                root.splitRequested(selectedFolder)
                folderPickerLoader.active = false
            }
            onRejected: folderPickerLoader.active = false
        }
    }
}
