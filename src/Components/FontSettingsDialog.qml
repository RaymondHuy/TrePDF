pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import TrePDF
import "ios"

Dialog {
    id: root
    objectName: "fontSettingsDialog"

    required property AppTheme theme
    required property UserFontManager manager
    property url regularFile: ""
    property url boldFile: ""
    property url italicFile: ""
    property url boldItalicFile: ""
    property int choosingStyle: 0
    property string importedFamily: ""
    readonly property bool imported: importedFamily.length > 0
    readonly property bool hasFiles: regularFile.toString().length > 0
        || boldFile.toString().length > 0 || italicFile.toString().length > 0
        || boldItalicFile.toString().length > 0
    readonly property bool canSubmit: !imported && familyInput.text.trim().length > 0
        && hasFiles && !fontFileDialogLoader.active
    readonly property int previewStyle: (previewBold.checked ? 1 : 0)
        + (previewItalic.checked ? 2 : 0)
    readonly property bool previewReady: manager.previewFamily.length > 0

    parent: Overlay.overlay
    anchors.centerIn: dialogMetrics.active ? undefined : parent
    x: dialogMetrics.active
        ? dialogMetrics.leftInset + (dialogMetrics.availableWidth - width) / 2 : 0
    y: dialogMetrics.active
        ? dialogMetrics.topInset + Math.max(8, (dialogMetrics.availableHeight - height) / 2) : 0
    width: Math.min(720, parent ? dialogMetrics.availableWidth - 32 : 720)
    height: Math.min(implicitHeight, parent ? dialogMetrics.availableHeight - 32 : 620)
    padding: 20
    modal: true
    focus: true
    popupType: Popup.Item
    closePolicy: fontFileDialogLoader.active ? Popup.NoAutoClose : Popup.CloseOnEscape
    //% "Add fonts"
    title: qsTrId("UserFontDialogTitle")

    DialogMetrics {
        id: dialogMetrics
        overlay: root.parent
    }

    function styleName(index: int): string {
        switch (index) {
        case 1:
            //% "Bold"
            return qsTrId("FontDownloadStyleBold")
        case 2:
            //% "Italic"
            return qsTrId("FontDownloadStyleItalic")
        case 3:
            //% "Bold Italic"
            return qsTrId("FontDownloadStyleBoldItalic")
        default:
            //% "Regular"
            return qsTrId("FontDownloadStyleRegular")
        }
    }

    function selectedFile(index: int): url {
        switch (index) {
        case 1: return boldFile
        case 2: return italicFile
        case 3: return boldItalicFile
        default: return regularFile
        }
    }

    function setSelectedFile(index: int, file: url) {
        switch (index) {
        case 1: boldFile = file; break
        case 2: italicFile = file; break
        case 3: boldItalicFile = file; break
        default: regularFile = file; break
        }
        manager.clearError()
    }

    function submit() {
        if (canSubmit && manager.importFonts(familyInput.text.trim(), regularFile,
                boldFile, italicFile, boldItalicFile)) {
            importedFamily = familyInput.text.trim()
            closeButton.forceActiveFocus()
        }
    }

    function refreshPreview() {
        if (manager)
            manager.preparePreview(selectedFile(previewStyle), previewBold.checked,
                previewItalic.checked)
    }

    onPreviewStyleChanged: refreshPreview()
    onRegularFileChanged: { if (previewStyle === 0) refreshPreview() }
    onBoldFileChanged: { if (previewStyle === 1) refreshPreview() }
    onItalicFileChanged: { if (previewStyle === 2) refreshPreview() }
    onBoldItalicFileChanged: { if (previewStyle === 3) refreshPreview() }
    onOpened: {
        manager.clearError()
        refreshPreview()
        familyInput.forceActiveFocus()
    }
    onAboutToHide: {
        if (fontFileDialogLoader.status === Loader.Ready) {
            const picker = fontFileDialogLoader.item as PlatformFileDialog
            if (picker)
                picker.close()
        }
        fontFileDialogLoader.active = false
        manager.clearPreview()
    }
    Component.onDestruction: { if (manager) manager.clearPreview() }

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
        id: formScroll
        contentWidth: availableWidth
        implicitHeight: fontFields.implicitHeight
        clip: true
        contentData: [fontFields]
    }

    readonly property ColumnLayout _fontFields: ColumnLayout {
        id: fontFields
        width: formScroll.availableWidth
        spacing: 12

        Label {
            Layout.fillWidth: true
            //% "Enter the font's family name, for example Noto Sans, then choose the files for the styles you have. You do not need all four styles."
            text: qsTrId("UserFontDialogHint")
            color: root.theme.ribbonTextColor
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            //% "Font family name"
            text: qsTrId("UserFontFamilyLabel")
            color: root.theme.ribbonTextColor
            font.weight: Font.DemiBold
        }
        TextField {
            id: familyInput
            objectName: "userFontFamilyInput"
            Layout.fillWidth: true
            enabled: !root.imported
            maximumLength: 256
            selectByMouse: true
            //% "For example: Noto Sans"
            placeholderText: qsTrId("UserFontFamilyPlaceholder")
            //% "Font family name"
            Accessible.name: qsTrId("UserFontFamilyLabel")
            onTextEdited: root.manager.clearError()
            onAccepted: root.submit()
        }
        Repeater {
            model: 4
            delegate: GridLayout {
                id: styleRow
                required property int index
                readonly property string styleTitle: root.styleName(index)
                readonly property url fileUrl: root.selectedFile(index)
                readonly property bool hasFile: fileUrl.toString().length > 0
                Layout.fillWidth: true
                columns: width >= 560 ? 2 : 1
                columnSpacing: 12
                rowSpacing: 4
                enabled: !root.imported

                Label {
                    Layout.fillWidth: true
                    Layout.preferredWidth: styleRow.columns === 2 ? 160 : -1
                    Layout.maximumWidth: styleRow.columns === 2 ? 160 : Infinity
                    text: styleRow.styleTitle
                    color: root.theme.ribbonTextColor
                    wrapMode: Text.Wrap
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        text: styleRow.hasFile ? root.manager.fileName(styleRow.fileUrl)
                            //% "No file selected"
                            : qsTrId("UserFontNoFile")
                        textFormat: Text.PlainText
                        color: styleRow.hasFile ? root.theme.ribbonTextColor : "#777078"
                        elide: Text.ElideMiddle
                        ToolTip.visible: fileHover.hovered && styleRow.hasFile
                        ToolTip.text: styleRow.fileUrl.toString()
                        HoverHandler { id: fileHover }
                    }
                    Button {
                        objectName: "userFontChooseFile" + styleRow.index
                        //% "Choose file"
                        text: qsTrId("UserFontChooseFile")
                        //% "Choose a %1 font file"
                        Accessible.name: qsTrId("UserFontChooseStyleFile").arg(styleRow.styleTitle)
                        onClicked: {
                            root.choosingStyle = styleRow.index
                            fontFileDialogLoader.active = true
                        }
                    }
                    ToolButton {
                        objectName: "userFontClearFile" + styleRow.index
                        enabled: styleRow.hasFile
                        text: "×"
                        //% "Remove the %1 file"
                        Accessible.name: qsTrId("UserFontRemoveStyleFile").arg(styleRow.styleTitle)
                        ToolTip.text: Accessible.name
                        ToolTip.visible: hovered
                        onClicked: root.setSelectedFile(styleRow.index, "")
                    }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label {
                Layout.fillWidth: true
                //% "Preview"
                text: qsTrId("UserFontPreviewTitle")
                font.weight: Font.DemiBold
                color: root.theme.ribbonTextColor
            }
            Button {
                id: previewBold
                objectName: "userFontPreviewBoldButton"
                //% "Bold"
                text: qsTrId("UserFontPreviewBold")
                font.bold: true
                checkable: true
            }
            Button {
                id: previewItalic
                objectName: "userFontPreviewItalicButton"
                //% "Italic"
                text: qsTrId("UserFontPreviewItalic")
                font.italic: true
                checkable: true
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 128
            color: "#ffffff"
            border.color: root.theme.ribbonBorder
            radius: 4

            ScrollView {
                anchors.fill: parent
                anchors.margins: 1
                visible: root.previewReady
                contentWidth: availableWidth
                clip: true

                TextArea {
                    objectName: "userFontPreviewText"
                    //% "The quick brown fox jumps over the lazy dog.\nTiếng Việt: Nguyễn Ánh — 0123456789"
                    text: qsTrId("UserFontPreviewSample")
                    //% "Type here to try the font"
                    Accessible.name: qsTrId("UserFontPreviewInput")
                    //% "Type here to try the font"
                    placeholderText: qsTrId("UserFontPreviewInput")
                    font: root.manager.previewFont
                    textFormat: TextEdit.PlainText
                    wrapMode: TextEdit.Wrap
                    selectByMouse: true
                    padding: 12
                    color: root.theme.ribbonTextColor
                    background: Item {}
                }
            }
            Label {
                objectName: "userFontPreviewMessage"
                anchors.fill: parent
                anchors.margins: 12
                visible: !root.previewReady
                text: root.manager.previewError
                textFormat: Text.PlainText
                color: "#656268"
                wrapMode: Text.Wrap
                verticalAlignment: Text.AlignVCenter
            }
        }
        Label {
            Layout.fillWidth: true
            //% "Use TTF or OTF files from the same font family. Added fonts are saved in TrePDF and remain available the next time you open the app."
            text: qsTrId("UserFontStorageHint")
            color: "#656268"
            wrapMode: Text.Wrap
        }
        Label {
            objectName: "userFontImportError"
            Layout.fillWidth: true
            visible: root.manager.error.length > 0
            text: root.manager.error
            textFormat: Text.PlainText
            color: "#b3261e"
            wrapMode: Text.Wrap
            Accessible.role: Accessible.AlertMessage
        }
        Label {
            objectName: "userFontImportSuccess"
            Layout.fillWidth: true
            visible: root.imported
            //% "%1 has been added. You can now choose it when editing text."
            text: qsTrId("UserFontImportSuccess").arg(root.importedFamily)
            textFormat: Text.PlainText
            color: "#28753d"
            wrapMode: Text.Wrap
            Accessible.role: Accessible.AlertMessage
        }
    }

    footer: DialogButtonBox {
        padding: 16
        spacing: 8

        Button {
            id: closeButton
            objectName: "userFontCloseButton"
            text: root.imported
                //% "Close"
                ? qsTrId("UserFontClose")
                //% "Cancel"
                : qsTrId("Cancel")
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: root.reject()
        }
        OrangeButton {
            objectName: "userFontAddButton"
            theme: root.theme
            //% "Add font"
            text: qsTrId("UserFontAdd")
            visible: !root.imported
            enabled: root.canSubmit
            DialogButtonBox.buttonRole: DialogButtonBox.ApplyRole
            onClicked: root.submit()
        }
    }

    Loader {
        id: fontFileDialogLoader
        objectName: "userFontFileDialogLoader"
        active: false
        sourceComponent: PlatformFileDialog {
            //% "Choose a %1 font file"
            title: qsTrId("UserFontChooseStyleFile").arg(root.styleName(root.choosingStyle))
            nameFilters: [
                //% "Font files (*.ttf *.otf)"
                qsTrId("UserFontFileFilter")
            ]
            fileMode: FileDialog.OpenFile
            onAccepted: {
                root.setSelectedFile(root.choosingStyle, selectedFile)
                fontFileDialogLoader.active = false
            }
            onRejected: fontFileDialogLoader.active = false
        }
        onLoaded: {
            if (status === Loader.Ready) {
                const picker = item as PlatformFileDialog
                if (picker)
                    picker.open()
            }
        }
    }
}
