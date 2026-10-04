pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import TrePDF
import "ios"

Dialog {
    id: root

    required property AppTheme theme
    required property SignatureLibrary library
    property string mode: "signature"
    property bool initialized: false
    property var strokes: []
    property var currentStroke: []
    readonly property var displayedStrokes: strokes.concat(
        currentStroke.length > 0 ? [currentStroke] : [])
    property url imageUrl: ""
    property string selectedEntryId: ""
    property color inkColor: "#17212b"
    property bool crossMark: false
    readonly property bool signatureMode: mode === "signature" || mode === "initials"
    readonly property real placementWidth: mode === "initials" ? 65
        : mode === "date" ? 90 : mode === "check" ? 16 : 140
    readonly property int pointCount: {
        let count = 0
        for (const stroke of strokes)
            count += stroke.length
        return count
    }

    signal placementRequested(real widthPoints)

    objectName: "signatureDialog"
    parent: Overlay.overlay
    anchors.centerIn: dialogMetrics.active ? undefined : parent
    x: dialogMetrics.active
        ? dialogMetrics.leftInset + (dialogMetrics.availableWidth - width) / 2 : 0
    y: dialogMetrics.active
        ? dialogMetrics.topInset + Math.max(8, (dialogMetrics.availableHeight - height) / 2) : 0
    width: Math.min(640, parent ? dialogMetrics.availableWidth - 32 : 640)
    height: Math.min(signatureMode ? 500 : 360,
        parent ? dialogMetrics.availableHeight - 32 : 500)
    padding: 18
    modal: true
    focus: true
    popupType: Popup.Item
    closePolicy: Popup.CloseOnEscape
    title: {
        switch (mode) {
        case "initials":
            //% "Add initials"
            return qsTrId("ESignAddInitials")
        case "date":
            //% "Add date"
            return qsTrId("ESignAddDate")
        case "check":
            //% "Add a check or cross"
            return qsTrId("ESignAddCheck")
        default:
            //% "Create a signature"
            return qsTrId("ESignCreateSignature")
        }
    }

    DialogMetrics {
        id: dialogMetrics
        overlay: root.parent
    }

    function schedulePreview() {
        if (!initialized)
            return
        library.clearPreview()
        previewTimer.restart()
    }

    function refreshPreview() {
        if (!initialized)
            return
        if (mode === "date") {
            library.prepareDate(dateField.text.trim(), dateFormat.currentIndex)
        } else if (mode === "check") {
            library.prepareCheck(crossMark)
        } else if (modeTabs.currentIndex === 0) {
            if (strokes.length > 0)
                library.prepareDrawing(strokes.map(stroke => stroke.map(
                    point => ({x: point.x, y: point.y}))), inkColor, 2.5)
            else
                library.clearPreview()
        } else if (modeTabs.currentIndex === 1) {
            if (signatureText.text.trim().length > 0)
                library.prepareText(signatureText.text, fontStyle.currentIndex, inkColor)
            else
                library.clearPreview()
        } else if (modeTabs.currentIndex === 2) {
            if (imageUrl.toString().length > 0)
                library.prepareImage(imageUrl, removeWhite.checked)
            else
                library.clearPreview()
        } else if (selectedEntryId.length > 0) {
            library.selectEntry(selectedEntryId)
        } else {
            library.clearPreview()
        }
    }

    function submit() {
        if (previewTimer.running) {
            previewTimer.stop()
            refreshPreview()
        }
        if (!library.hasPreview || currentStroke.length > 0)
            return
        if (signatureMode && modeTabs.currentIndex !== 3 && saveCheck.checked) {
            if (savedName.text.trim().length === 0) {
                savedName.forceActiveFocus()
                return
            }
            if (!library.savePreview(savedName.text.trim(), mode))
                return
        }
        placementRequested(placementWidth)
        accept()
    }

    function appendPoint(x, y) {
        const point = Qt.point(Math.max(0, Math.min(1, x / drawArea.width)),
            Math.max(0, Math.min(1, y / drawArea.height)))
        const points = currentStroke.slice()
        if (points.length > 0) {
            const last = points[points.length - 1]
            const dx = (point.x - last.x) * drawArea.width
            const dy = (point.y - last.y) * drawArea.height
            if (dx * dx + dy * dy < 1)
                return
        }
        if (points.length < 2048 && pointCount + points.length < 8192)
            points.push(point)
        else if (points.length > 1)
            points[points.length - 1] = point
        currentStroke = points
    }

    onOpened: {
        library.clearPreview()
        initialized = true
        refreshPreview()
        if (mode === "date")
            dateField.forceActiveFocus()
    }
    onAboutToHide: {
        initialized = false
        previewTimer.stop()
        if (imageDialogLoader.status === Loader.Ready) {
            const picker = imageDialogLoader.item as PlatformFileDialog
            if (picker)
                picker.close()
        }
        imageDialogLoader.active = false
    }

    background: Rectangle {
        color: root.theme.ribbonSurface
        border.color: root.theme.ribbonBorder
        radius: 10
    }

    header: Label {
        text: root.title
        padding: 18
        bottomPadding: 2
        font.pixelSize: 18
        font.bold: true
        color: root.theme.ribbonTextColor
    }

    component Preview: Rectangle {
        color: "#ffffff"
        border.color: root.theme.ribbonBorder
        radius: 6
        implicitHeight: 118
        Accessible.ignored: true

        Image {
            id: previewImage

            anchors.fill: parent
            anchors.margins: 14
            source: root.library.previewUrl
            sourceSize: Qt.size(1200, 400)
            fillMode: Image.PreserveAspectFit
            cache: false
            visible: root.library.hasPreview && status !== Image.Error
        }
        Label {
            anchors.centerIn: parent
            visible: !root.library.hasPreview || previewImage.status === Image.Error
            text: previewImage.status === Image.Error
                //% "The preview could not be displayed."
                ? qsTrId("ESignPreviewFailed")
                //% "Preview"
                : qsTrId("ESignPreview")
            color: "#817c78"
        }
    }

    contentItem: ScrollView {
        id: scrollView

        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: scrollView.availableWidth
            spacing: 10

            TabBar {
                id: modeTabs

                objectName: "signatureModeTabs"
                Layout.fillWidth: true
                visible: root.signatureMode
                onCurrentIndexChanged: root.schedulePreview()

                TabButton {
                    //% "Draw"
                    text: qsTrId("ESignDraw")
                }
                TabButton {
                    //% "Type"
                    text: qsTrId("ESignType")
                }
                TabButton {
                    //% "Image"
                    text: qsTrId("ESignImage")
                }
                TabButton {
                    //% "Saved"
                    text: qsTrId("ESignSaved")
                }
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 218
                visible: root.signatureMode
                currentIndex: modeTabs.currentIndex

                ColumnLayout {
                    spacing: 8

                    Rectangle {
                        id: drawArea

                        objectName: "signatureDrawArea"
                        Layout.alignment: Qt.AlignHCenter
                        Layout.preferredWidth: Math.min(504, scrollView.availableWidth)
                        Layout.preferredHeight: Layout.preferredWidth / 3
                        color: "#ffffff"
                        border.color: root.theme.ribbonBorder
                        radius: 6
                        clip: true
                        Accessible.role: Accessible.Pane
                        //% "Draw your signature with the mouse or trackpad"
                        Accessible.name: qsTrId("ESignDrawInstruction")

                        Rectangle {
                            x: 20
                            y: parent.height * 0.77
                            width: parent.width - 40
                            height: 1
                            color: "#eee8e4"
                            Accessible.ignored: true
                        }
                        Label {
                            anchors.centerIn: parent
                            visible: root.strokes.length === 0
                                && root.currentStroke.length === 0
                            //% "Draw your signature here"
                            text: qsTrId("ESignDrawPlaceholder")
                            color: "#817c78"
                            Accessible.ignored: true
                        }
                        InkDrawingView {
                            anchors.fill: parent
                            strokes: root.displayedStrokes
                            strokeColor: root.inkColor
                            strokeWidth: 2.5 * drawArea.width / 900
                        }
                        Repeater {
                            model: root.displayedStrokes.filter(stroke => stroke.length === 1)

                            delegate: Rectangle {
                                required property var modelData

                                x: modelData[0].x * drawArea.width - width / 2
                                y: modelData[0].y * drawArea.height - height / 2
                                width: 2.5 * drawArea.width / 900
                                height: width
                                radius: width / 2
                                color: root.inkColor
                                Accessible.ignored: true
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            acceptedButtons: Qt.LeftButton
                            preventStealing: true
                            cursorShape: Qt.CrossCursor
                            onPressed: mouse => {
                                if (root.strokes.length >= 64 || root.pointCount >= 8190) {
                                    mouse.accepted = false
                                    return
                                }
                                previewTimer.stop()
                                root.library.clearPreview()
                                root.currentStroke = []
                                root.appendPoint(mouse.x, mouse.y)
                            }
                            onPositionChanged: mouse => {
                                if (pressed)
                                    root.appendPoint(mouse.x, mouse.y)
                            }
                            onReleased: mouse => {
                                root.appendPoint(mouse.x, mouse.y)
                                if (root.currentStroke.length > 0)
                                    root.strokes = root.strokes.concat([root.currentStroke])
                                root.currentStroke = []
                                root.refreshPreview()
                            }
                            onCanceled: {
                                root.currentStroke = []
                                root.refreshPreview()
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true

                        ComboBox {
                            id: drawingColor

                            Layout.preferredWidth: 130
                            model: [
                                //% "Black ink"
                                qsTrId("ESignBlackInk"),
                                //% "Blue ink"
                                qsTrId("ESignBlueInk")
                            ]
                            //% "Ink color"
                            Accessible.name: qsTrId("ESignInkColor")
                            currentIndex: root.inkColor.toString() === "#164fa3" ? 1 : 0
                            onActivated: index => {
                                root.inkColor = index === 1 ? "#164fa3" : "#17212b"
                                root.schedulePreview()
                            }
                        }
                        Item { Layout.fillWidth: true }
                        Button {
                            //% "Undo stroke"
                            text: qsTrId("ESignUndoStroke")
                            enabled: root.strokes.length > 0
                            onClicked: {
                                root.strokes = root.strokes.slice(0, -1)
                                root.schedulePreview()
                            }
                        }
                        Button {
                            //% "Clear"
                            text: qsTrId("ESignClear")
                            enabled: root.strokes.length > 0
                            onClicked: {
                                root.strokes = []
                                root.schedulePreview()
                            }
                        }
                    }
                }

                ColumnLayout {
                    spacing: 8

                    TextField {
                        id: signatureText

                        objectName: "signatureTextField"
                        Layout.fillWidth: true
                        maximumLength: 100
                        selectByMouse: true
                        //% "Enter your name or initials"
                        placeholderText: qsTrId("ESignNamePlaceholder")
                        //% "Signature text"
                        Accessible.name: qsTrId("ESignSignatureText")
                        onTextEdited: root.schedulePreview()
                        onAccepted: root.submit()
                    }
                    RowLayout {
                        Layout.fillWidth: true

                        ComboBox {
                            id: fontStyle

                            Layout.fillWidth: true
                            model: [
                                //% "Handwriting"
                                qsTrId("ESignHandwriting"),
                                //% "Serif italic"
                                qsTrId("ESignSerifItalic"),
                                //% "Plain"
                                qsTrId("ESignPlain")
                            ]
                            //% "Signature style"
                            Accessible.name: qsTrId("ESignStyle")
                            onActivated: root.schedulePreview()
                        }
                        ComboBox {
                            Layout.preferredWidth: 130
                            model: drawingColor.model
                            currentIndex: root.inkColor.toString() === "#164fa3" ? 1 : 0
                            //% "Ink color"
                            Accessible.name: qsTrId("ESignInkColor")
                            onActivated: index => {
                                root.inkColor = index === 1 ? "#164fa3" : "#17212b"
                                root.schedulePreview()
                            }
                        }
                    }
                    Preview {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                    }
                }

                ColumnLayout {
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true

                        Button {
                            objectName: "signatureChooseImageButton"
                            //% "Choose image…"
                            text: qsTrId("ESignChooseImage")
                            onClicked: imageDialogLoader.active = true
                        }
                        CheckBox {
                            id: removeWhite

                            Layout.fillWidth: true
                            //% "Remove white background"
                            text: qsTrId("ESignRemoveWhite")
                            onToggled: root.schedulePreview()
                        }
                    }
                    Preview {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                    }
                    Label {
                        Layout.fillWidth: true
                        //% "Use a PNG, JPEG or WebP image of your signature."
                        text: qsTrId("ESignImageHint")
                        color: "#817c78"
                        wrapMode: Text.Wrap
                    }
                }

                ColumnLayout {
                    spacing: 8

                    ListView {
                        id: libraryList

                        objectName: "signatureLibraryList"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.minimumHeight: 126
                        model: root.library.entries.filter(entry => entry.kind === root.mode)
                        clip: true
                        spacing: 4
                        ScrollBar.vertical: ScrollBar { }

                        delegate: ItemDelegate {
                            id: entryDelegate

                            required property var modelData
                            width: libraryList.width
                            height: 54
                            highlighted: root.selectedEntryId === String(modelData.id)
                            onClicked: {
                                root.selectedEntryId = String(modelData.id)
                                root.refreshPreview()
                            }
                            Accessible.name: String(modelData.name)

                            contentItem: RowLayout {
                                spacing: 10

                                Image {
                                    Layout.preferredWidth: 90
                                    Layout.preferredHeight: 32
                                    source: entryDelegate.modelData.previewUrl
                                    sourceSize: Qt.size(270, 96)
                                    asynchronous: true
                                    fillMode: Image.PreserveAspectFit
                                    visible: status !== Image.Error
                                    Accessible.ignored: true
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: String(entryDelegate.modelData.name)
                                    elide: Text.ElideRight
                                    color: root.theme.ribbonTextColor
                                }
                                ToolButton {
                                    //% "Delete"
                                    text: qsTrId("Delete")
                                    //% "Delete saved signature %1"
                                    Accessible.name: qsTrId("ESignDeleteSaved").arg(
                                        String(entryDelegate.modelData.name))
                                    onClicked: {
                                        const entryId = String(entryDelegate.modelData.id)
                                        if (root.library.removeEntry(entryId)
                                            && root.selectedEntryId === entryId) {
                                            root.selectedEntryId = ""
                                            root.library.clearPreview()
                                        }
                                    }
                                }
                            }
                        }

                        Label {
                            anchors.centerIn: parent
                            width: parent.width - 20
                            visible: libraryList.count === 0
                            //% "No saved signatures. Create one and choose to save it on this Mac."
                            text: qsTrId("ESignLibraryEmpty")
                            color: "#817c78"
                            wrapMode: Text.Wrap
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }
                    Preview {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 76
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: root.mode === "date"
                spacing: 10

                RowLayout {
                    Layout.fillWidth: true

                    TextField {
                        id: dateField

                        objectName: "signatureDateField"
                        Layout.fillWidth: true
                        text: Qt.formatDate(new Date(), "yyyy-MM-dd")
                        maximumLength: 10
                        selectByMouse: true
                        //% "Date (YYYY-MM-DD)"
                        placeholderText: qsTrId("ESignDatePlaceholder")
                        //% "Date (YYYY-MM-DD)"
                        Accessible.name: qsTrId("ESignDatePlaceholder")
                        onTextEdited: root.schedulePreview()
                        onAccepted: root.submit()
                    }
                    ComboBox {
                        id: dateFormat

                        objectName: "signatureDateFormat"
                        Layout.preferredWidth: 160
                        model: [
                            //% "dd/MM/yyyy"
                            qsTrId("ESignDayFirstFormat"),
                            //% "MM/dd/yyyy"
                            qsTrId("ESignMonthFirstFormat"),
                            //% "yyyy-MM-dd"
                            qsTrId("ESignIsoFormat")
                        ]
                        //% "Date format"
                        Accessible.name: qsTrId("ESignDateFormat")
                        onActivated: root.schedulePreview()
                    }
                }
                Preview {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 120
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: root.mode === "check"
                spacing: 10

                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 16

                    Button {
                        objectName: "signatureCheckButton"
                        Layout.preferredWidth: 100
                        text: "\u2713"
                        font.pixelSize: 24
                        checkable: true
                        autoExclusive: true
                        checked: !root.crossMark
                        //% "Check mark"
                        Accessible.name: qsTrId("ESignCheckMark")
                        onClicked: {
                            root.crossMark = false
                            root.refreshPreview()
                        }
                    }
                    Button {
                        objectName: "signatureCrossButton"
                        Layout.preferredWidth: 100
                        text: "\u2715"
                        font.pixelSize: 24
                        checkable: true
                        autoExclusive: true
                        checked: root.crossMark
                        //% "Cross mark"
                        Accessible.name: qsTrId("ESignCrossMark")
                        onClicked: {
                            root.crossMark = true
                            root.refreshPreview()
                        }
                    }
                }
                Preview {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 100
                }
            }

            RowLayout {
                Layout.fillWidth: true
                visible: root.signatureMode && modeTabs.currentIndex !== 3
                spacing: 8

                CheckBox {
                    id: saveCheck

                    objectName: "signatureSaveCheck"
                    //% "Save on this Mac"
                    text: qsTrId("ESignSaveOnMac")
                    checked: false
                }
                TextField {
                    id: savedName

                    objectName: "signatureSavedName"
                    Layout.fillWidth: true
                    visible: saveCheck.checked
                    maximumLength: 80
                    selectByMouse: true
                    //% "Name for the saved signature"
                    placeholderText: qsTrId("ESignSavedName")
                    //% "Name for the saved signature"
                    Accessible.name: qsTrId("ESignSavedName")
                    onAccepted: root.submit()
                }
                Item {
                    Layout.fillWidth: true
                    visible: !saveCheck.checked
                }
            }

            Label {
                Layout.fillWidth: true
                //% "Choose Place, then click a page. You can move or resize it before saving the PDF."
                text: qsTrId("ESignPlacementHint")
                color: "#817c78"
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: root.library.error.length > 0
                text: root.library.error
                color: "#b3261e"
                wrapMode: Text.Wrap
                Accessible.role: Accessible.AlertMessage
            }
        }
    }

    footer: DialogButtonBox {
        padding: 14
        spacing: 8

        Button {
            //% "Cancel"
            text: qsTrId("Cancel")
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: root.reject()
        }
        Button {
            objectName: "signatureAcceptButton"
            //% "Place"
            text: qsTrId("ESignPlace")
            enabled: root.library.hasPreview && root.currentStroke.length === 0
                && (!root.signatureMode || modeTabs.currentIndex === 3
                    || !saveCheck.checked || savedName.text.trim().length > 0)
            highlighted: true
            DialogButtonBox.buttonRole: DialogButtonBox.ApplyRole
            onClicked: root.submit()
        }
    }

    Timer {
        id: previewTimer

        interval: 180
        onTriggered: root.refreshPreview()
    }

    Loader {
        id: imageDialogLoader

        active: false
        sourceComponent: PlatformFileDialog {
            //% "Choose a signature image"
            title: qsTrId("ESignChooseImageTitle")
            nameFilters: [
                //% "Signature images (*.png *.jpg *.jpeg *.webp)"
                qsTrId("ESignImageFilter")
            ]
            fileMode: FileDialog.OpenFile
            onAccepted: {
                root.imageUrl = selectedFile
                root.refreshPreview()
                imageDialogLoader.active = false
            }
            onRejected: imageDialogLoader.active = false
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
