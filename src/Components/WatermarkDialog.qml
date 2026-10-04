pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import TrePDF
import "ios"

Dialog {
    id: root
    objectName: "watermarkDialog"

    required property AppTheme theme
    required property PDFEditController editController
    required property string mode
    property bool operationAllowed: false
    property string selectedWatermarkId: ""
    property string imageToken: ""
    property bool existingImage: false
    property color textColor: "#808080"
    property real alignmentX: 0.5
    property real alignmentY: 0.5
    property string previewSource: ""
    property string errorText: ""
    property bool initialized: false
    property bool restoring: false
    property bool submitting: false
    readonly property bool removing: mode === "remove"
    readonly property bool ready: editController && editController.watermarksReady
        && !editController.loadingWatermarks
    readonly property var watermarks: editController ? editController.watermarks : []
    readonly property bool hasSelection: mode === "add" || selectedWatermarkId.length > 0
    readonly property bool canEdit: operationAllowed && ready && hasSelection
    readonly property bool hasImage: imageToken.length > 0 || existingImage
    readonly property bool canSubmit: canEdit && !submitting && (removing
        || ((allPagesSelector.checked || pageRangeInput.text.trim().length > 0)
            && (typeSelector.currentIndex === 1 ? hasImage
                : textInput.text.trim().length > 0)))
    readonly property var fontFamilies: Qt.fontFamilies()

    parent: Overlay.overlay
    anchors.centerIn: dialogMetrics.active ? undefined : parent
    x: dialogMetrics.active
        ? dialogMetrics.leftInset + (dialogMetrics.availableWidth - width) / 2 : 0
    y: dialogMetrics.active
        ? dialogMetrics.topInset + Math.max(8, (dialogMetrics.availableHeight - height) / 2) : 0
    width: Math.min(820, parent ? dialogMetrics.availableWidth - 32 : 820)
    height: Math.min(implicitHeight, parent ? dialogMetrics.availableHeight - 32 : implicitHeight)
    padding: 20
    modal: true
    focus: true
    popupType: Popup.Item
    closePolicy: Popup.CloseOnEscape
    title: removing
        //% "Remove watermark"
        ? qsTrId("RemoveWatermark")
        : mode === "update"
            //% "Update watermark"
            ? qsTrId("UpdateWatermark")
            //% "Add watermark"
            : qsTrId("AddWatermark")

    DialogMetrics {
        id: dialogMetrics
        overlay: root.parent
    }

    function settings() {
        return {
            imageMode: typeSelector.currentIndex === 1,
            text: textInput.text,
            fontFamily: fontSelector.editText,
            fontSize: sizeSelector.value,
            color: textColor.toString(),
            opacity: opacitySlider.value,
            rotation: rotationSelector.value,
            scale: scaleSlider.value,
            x: alignmentX,
            y: alignmentY,
            behind: layerSelector.currentIndex === 1,
            pages: allPagesSelector.checked ? "all" : pageRangeInput.text.trim(),
            imageToken: imageToken
        }
    }

    function schedulePreview() {
        if (restoring || !initialized || !visible)
            return
        errorText = ""
        previewTimer.restart()
    }

    function refreshPreview() {
        if (!canEdit || (typeSelector.currentIndex === 1 && !hasImage)) {
            previewSource = ""
            return
        }
        previewSource = editController.watermarkPreview(settings(), selectedWatermarkId)
    }

    function chooseWatermark(index) {
        if (index < 0 || index >= watermarks.length) {
            selectedWatermarkId = ""
            previewSource = ""
            return
        }
        restoring = true
        const watermark = watermarks[index]
        selectedWatermarkId = watermark.id
        imageToken = ""
        existingImage = Boolean(watermark.imageMode)
        typeSelector.currentIndex = watermark.imageMode ? 1 : 0
        textInput.text = watermark.text ?? ""
        fontSelector.currentIndex = fontFamilies.indexOf(watermark.fontFamily ?? "Arial")
        fontSelector.editText = watermark.fontFamily ?? "Arial"
        sizeSelector.value = Number(watermark.fontSize ?? 48)
        textColor = watermark.color ?? "#808080"
        opacitySlider.value = Number(watermark.opacity ?? 0.25)
        rotationSelector.value = Number(watermark.rotation ?? -45)
        scaleSlider.value = Number(watermark.scale ?? 0.5)
        alignmentX = Number(watermark.x ?? 0.5)
        alignmentY = Number(watermark.y ?? 0.5)
        positionSelector.currentIndex = Math.round(alignmentY * 2) * 3
            + Math.round(alignmentX * 2)
        layerSelector.currentIndex = watermark.behind ? 1 : 0
        const pages = String(watermark.pages ?? "all")
        allPagesSelector.checked = pages.toLowerCase() === "all"
        pageRangeInput.text = allPagesSelector.checked ? "" : pages
        errorText = ""
        restoring = false
        previewTimer.restart()
    }

    function initialize() {
        if (!visible || !ready || initialized || submitting)
            return
        if (mode !== "add") {
            if (watermarks.length === 0) {
                groupSelector.currentIndex = -1
                return
            }
            groupSelector.currentIndex = 0
            chooseWatermark(0)
        } else {
            previewTimer.restart()
        }
        initialized = true
    }

    function submit() {
        if (!canSubmit)
            return
        previewTimer.stop()
        errorText = ""
        submitting = true
        const succeeded = removing
            ? editController.removeWatermark(selectedWatermarkId)
            : editController.applyWatermark(settings(), selectedWatermarkId)
        submitting = false
        if (succeeded)
            accept()
    }

    onOpened: {
        initializationTimer.restart()
        if (mode === "add")
            textInput.forceActiveFocus()
    }
    onReadyChanged: {
        if (visible)
            initializationTimer.restart()
    }
    onWatermarksChanged: {
        if (visible && !submitting)
            initializationTimer.restart()
    }
    onTextColorChanged: schedulePreview()
    onAboutToHide: {
        initializationTimer.stop()
        previewTimer.stop()
        if (imageDialogLoader.status === Loader.Ready) {
            const dialog = imageDialogLoader.item as PlatformFileDialog
            if (dialog)
                dialog.close()
        }
        if (colorDialogLoader.status === Loader.Ready) {
            const dialog = colorDialogLoader.item as ColorDialog
            if (dialog)
                dialog.close()
        }
        imageDialogLoader.active = false
        colorDialogLoader.active = false
    }

    Connections {
        target: root.editController
        function onStatusMessage(message) { root.errorText = message }
    }

    Timer {
        id: initializationTimer
        interval: 0
        onTriggered: root.initialize()
    }

    Timer {
        id: previewTimer
        interval: 180
        onTriggered: root.refreshPreview()
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
        id: scrollView
        implicitHeight: contentColumn.implicitHeight
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            id: contentColumn
            width: scrollView.availableWidth
            spacing: 12

            RowLayout {
                Layout.fillWidth: true
                visible: root.editController && root.editController.loadingWatermarks
                BusyIndicator { running: parent.visible }
                Label {
                    Layout.fillWidth: true
                    //% "Loading watermarks…"
                    text: qsTrId("LoadingWatermarks")
                    color: root.theme.ribbonTextColor
                }
            }

            Label {
                Layout.fillWidth: true
                visible: root.ready && root.mode !== "add" && root.watermarks.length === 0
                //% "No editable TrePDF watermarks were found in this PDF."
                text: qsTrId("NoEditableWatermarks")
                color: root.theme.ribbonTextColor
                wrapMode: Text.Wrap
            }

            RowLayout {
                Layout.fillWidth: true
                visible: root.mode !== "add" && root.watermarks.length > 0
                Label {
                    //% "Watermark"
                    text: qsTrId("Watermark")
                    color: root.theme.ribbonTextColor
                }
                ComboBox {
                    id: groupSelector
                    objectName: "watermarkGroupSelector"
                    Layout.fillWidth: true
                    enabled: root.ready && root.operationAllowed
                    model: root.watermarks
                    textRole: "label"
                    valueRole: "id"
                    //% "Watermark"
                    Accessible.name: qsTrId("Watermark")
                    onActivated: index => root.chooseWatermark(index)
                }
            }

            Label {
                Layout.fillWidth: true
                visible: root.removing && root.hasSelection
                //% "Remove this watermark from all pages where it appears? The change can be undone and is written to the PDF when you Save or Save As."
                text: qsTrId("RemoveWatermarkConfirmation")
                color: root.theme.ribbonTextColor
                wrapMode: Text.Wrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 20
                visible: root.mode === "add" || root.hasSelection

                GridLayout {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 400
                    Layout.alignment: Qt.AlignTop
                    columns: 2
                    columnSpacing: 12
                    rowSpacing: 9
                    visible: !root.removing
                    enabled: root.canEdit

                    Label {
                        //% "Type"
                        text: qsTrId("WatermarkType")
                    }
                    ComboBox {
                        id: typeSelector
                        objectName: "watermarkTypeSelector"
                        Layout.fillWidth: true
                        model: [
                            //% "Text"
                            qsTrId("WatermarkText"),
                            //% "Image"
                            qsTrId("Image")
                        ]
                        //% "Type"
                        Accessible.name: qsTrId("WatermarkType")
                        onCurrentIndexChanged: root.schedulePreview()
                    }
                    Label {
                        visible: typeSelector.currentIndex === 0
                        //% "Text"
                        text: qsTrId("WatermarkText")
                    }
                    TextField {
                        id: textInput
                        objectName: "watermarkTextInput"
                        Layout.fillWidth: true
                        visible: typeSelector.currentIndex === 0
                        maximumLength: 200
                        selectByMouse: true
                        //% "CONFIDENTIAL"
                        text: qsTrId("WatermarkDefaultText")
                        //% "Watermark text"
                        Accessible.name: qsTrId("WatermarkTextInput")
                        onTextChanged: root.schedulePreview()
                    }
                    Label {
                        visible: typeSelector.currentIndex === 1
                        //% "Image"
                        text: qsTrId("Image")
                    }
                    Button {
                        objectName: "watermarkImageButton"
                        Layout.fillWidth: true
                        visible: typeSelector.currentIndex === 1
                        text: root.hasImage
                            //% "Change image…"
                            ? qsTrId("WatermarkChangeImage")
                            //% "Choose image…"
                            : qsTrId("WatermarkChooseImage")
                        onClicked: imageDialogLoader.active = true
                    }
                    Label {
                        visible: typeSelector.currentIndex === 0
                        //% "Font"
                        text: qsTrId("WatermarkFont")
                    }
                    ComboBox {
                        id: fontSelector
                        objectName: "watermarkFontSelector"
                        Layout.fillWidth: true
                        visible: typeSelector.currentIndex === 0
                        editable: true
                        model: root.fontFamilies
                        currentIndex: root.fontFamilies.indexOf("Arial")
                        editText: "Arial"
                        //% "Font"
                        Accessible.name: qsTrId("WatermarkFont")
                        onEditTextChanged: root.schedulePreview()
                    }
                    Label {
                        visible: typeSelector.currentIndex === 0
                        //% "Font size (pt)"
                        text: qsTrId("WatermarkFontSize")
                    }
                    SpinBox {
                        id: sizeSelector
                        objectName: "watermarkSizeSelector"
                        Layout.fillWidth: true
                        visible: typeSelector.currentIndex === 0
                        from: 1
                        to: 200
                        value: 48
                        editable: true
                        //% "Font size (pt)"
                        Accessible.name: qsTrId("WatermarkFontSize")
                        onValueModified: root.schedulePreview()
                    }
                    Label {
                        visible: typeSelector.currentIndex === 0
                        //% "Color"
                        text: qsTrId("WatermarkColor")
                    }
                    Button {
                        id: colorButton
                        objectName: "watermarkColorButton"
                        Layout.fillWidth: true
                        visible: typeSelector.currentIndex === 0
                        //% "Watermark color"
                        Accessible.name: qsTrId("WatermarkColorInput")
                        onClicked: colorDialogLoader.active = true
                        contentItem: RowLayout {
                            spacing: 8
                            Rectangle {
                                Layout.preferredWidth: 20
                                Layout.preferredHeight: 16
                                color: root.textColor
                                border.color: root.theme.ribbonBorder
                                Accessible.ignored: true
                            }
                            Label {
                                Layout.fillWidth: true
                                text: root.textColor.toString().toUpperCase()
                                color: root.theme.ribbonTextColor
                            }
                        }
                    }
                    Label {
                        visible: typeSelector.currentIndex === 1
                        //% "Image width"
                        text: qsTrId("WatermarkImageWidth")
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        visible: typeSelector.currentIndex === 1
                        Slider {
                            id: scaleSlider
                            objectName: "watermarkScaleSlider"
                            Layout.fillWidth: true
                            from: 0.05
                            to: 1
                            stepSize: 0.01
                            value: 0.5
                            //% "Image width relative to the page"
                            Accessible.name: qsTrId("WatermarkImageScale")
                            onMoved: root.schedulePreview()
                        }
                        Label {
                            Layout.preferredWidth: 40
                            //% "%1%"
                            text: qsTrId("WatermarkPercent").arg(Math.round(scaleSlider.value * 100))
                        }
                    }
                    Label {
                        //% "Opacity"
                        text: qsTrId("WatermarkOpacity")
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Slider {
                            id: opacitySlider
                            objectName: "watermarkOpacitySlider"
                            Layout.fillWidth: true
                            from: 0
                            to: 1
                            stepSize: 0.01
                            value: 0.25
                            //% "Opacity"
                            Accessible.name: qsTrId("WatermarkOpacity")
                            onMoved: root.schedulePreview()
                        }
                        Label {
                            Layout.preferredWidth: 40
                            //% "%1%"
                            text: qsTrId("WatermarkPercent").arg(Math.round(opacitySlider.value * 100))
                        }
                    }
                    Label {
                        //% "Rotation (°)"
                        text: qsTrId("WatermarkRotation")
                    }
                    SpinBox {
                        id: rotationSelector
                        objectName: "watermarkRotationSelector"
                        Layout.fillWidth: true
                        from: -180
                        to: 180
                        value: -45
                        editable: true
                        //% "Rotation (°)"
                        Accessible.name: qsTrId("WatermarkRotation")
                        onValueModified: root.schedulePreview()
                    }
                    Label {
                        //% "Position"
                        text: qsTrId("WatermarkPosition")
                    }
                    ComboBox {
                        id: positionSelector
                        objectName: "watermarkPositionSelector"
                        Layout.fillWidth: true
                        currentIndex: 4
                        model: [
                            //% "Top left"
                            qsTrId("WatermarkTopLeft"),
                            //% "Top center"
                            qsTrId("WatermarkTopCenter"),
                            //% "Top right"
                            qsTrId("WatermarkTopRight"),
                            //% "Middle left"
                            qsTrId("WatermarkMiddleLeft"),
                            //% "Center"
                            qsTrId("WatermarkCenter"),
                            //% "Middle right"
                            qsTrId("WatermarkMiddleRight"),
                            //% "Bottom left"
                            qsTrId("WatermarkBottomLeft"),
                            //% "Bottom center"
                            qsTrId("WatermarkBottomCenter"),
                            //% "Bottom right"
                            qsTrId("WatermarkBottomRight")
                        ]
                        //% "Position"
                        Accessible.name: qsTrId("WatermarkPosition")
                        onActivated: index => {
                            root.alignmentX = (index % 3) / 2
                            root.alignmentY = Math.floor(index / 3) / 2
                            root.schedulePreview()
                        }
                    }
                    Label {
                        //% "Layer"
                        text: qsTrId("WatermarkLayer")
                    }
                    ComboBox {
                        id: layerSelector
                        objectName: "watermarkLayerSelector"
                        Layout.fillWidth: true
                        model: [
                            //% "In front of page content"
                            qsTrId("WatermarkForeground"),
                            //% "Behind page content"
                            qsTrId("WatermarkBackground")
                        ]
                        //% "Layer"
                        Accessible.name: qsTrId("WatermarkLayer")
                        onCurrentIndexChanged: root.schedulePreview()
                    }
                    Label {
                        //% "Pages"
                        text: qsTrId("WatermarkPages")
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        CheckBox {
                            id: allPagesSelector
                            objectName: "watermarkAllPagesSelector"
                            checked: true
                            //% "All pages"
                            text: qsTrId("WatermarkAllPages")
                            onToggled: root.schedulePreview()
                        }
                        TextField {
                            id: pageRangeInput
                            objectName: "watermarkPageRangeInput"
                            Layout.fillWidth: true
                            enabled: !allPagesSelector.checked
                            maximumLength: 2048
                            selectByMouse: true
                            //% "For example: 1-3, 5"
                            placeholderText: qsTrId("WatermarkPageRangePlaceholder")
                            //% "Pages"
                            Accessible.name: qsTrId("WatermarkPages")
                            onTextChanged: root.schedulePreview()
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.preferredWidth: root.removing ? 500 : 260
                    Layout.alignment: Qt.AlignTop
                    spacing: 8

                    Label {
                        //% "Preview"
                        text: qsTrId("WatermarkPreview")
                        color: root.theme.ribbonTextColor
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: root.removing ? 260 : 350
                        color: "#e9e5e1"
                        border.color: root.theme.ribbonBorder
                        Image {
                            id: previewImage
                            objectName: "watermarkPreviewImage"
                            anchors.fill: parent
                            anchors.margins: 8
                            source: root.previewSource
                            sourceSize: Qt.size(800, 1000)
                            fillMode: Image.PreserveAspectFit
                            smooth: true
                            Accessible.ignored: true
                        }
                        Label {
                            anchors.fill: parent
                            anchors.margins: 16
                            visible: root.previewSource.length === 0
                                || previewImage.status === Image.Error
                            //% "Preview is unavailable. Check the watermark settings."
                            text: qsTrId("WatermarkPreviewUnavailable")
                            color: root.theme.ribbonTextColor
                            wrapMode: Text.Wrap
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        //% "Sample page. The watermark is placed relative to each selected page."
                        text: qsTrId("WatermarkPreviewHint")
                        color: root.theme.ribbonTextColor
                        font.pixelSize: 11
                        wrapMode: Text.Wrap
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                visible: !root.removing
                //% "Apply creates an undoable draft. Save or Save As writes the watermark to the PDF."
                text: qsTrId("WatermarkSaveHint")
                color: root.theme.ribbonTextColor
                wrapMode: Text.Wrap
            }
            Label {
                objectName: "watermarkErrorLabel"
                Layout.fillWidth: true
                visible: root.errorText.length > 0
                text: root.errorText
                textFormat: Text.PlainText
                color: "#b3261e"
                wrapMode: Text.Wrap
                Accessible.role: Accessible.AlertMessage
            }
        }
    }

    footer: DialogButtonBox {
        padding: 16
        spacing: 8

        Button {
            objectName: "cancelWatermarkButton"
            //% "Cancel"
            text: qsTrId("Cancel")
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: root.reject()
        }
        OrangeButton {
            objectName: "applyWatermarkButton"
            theme: root.theme
            text: root.removing
                //% "Remove"
                ? qsTrId("WatermarkRemove")
                //% "Apply"
                : qsTrId("WatermarkApply")
            enabled: root.canSubmit
            DialogButtonBox.buttonRole: DialogButtonBox.ApplyRole
            onClicked: root.submit()
        }
    }

    Loader {
        id: imageDialogLoader
        active: false
        onLoaded: {
            if (status === Loader.Ready && item)
                (item as PlatformFileDialog).open()
        }
        sourceComponent: PlatformFileDialog {
            //% "Choose watermark image"
            title: qsTrId("WatermarkChooseImageTitle")
            fileMode: FileDialog.OpenFile
            nameFilters: [
                //% "Images (*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff)"
                qsTrId("WatermarkImageFiles")
            ]
            onAccepted: {
                if (root.operationAllowed && root.editController) {
                    const image = root.editController.loadWatermarkImage(selectedFile)
                    if (image.token) {
                        root.imageToken = image.token
                        root.schedulePreview()
                    }
                }
                imageDialogLoader.active = false
            }
            onRejected: imageDialogLoader.active = false
        }
    }

    Loader {
        id: colorDialogLoader
        active: false
        onLoaded: {
            if (status === Loader.Ready && item)
                (item as ColorDialog).open()
        }
        sourceComponent: ColorDialog {
            //% "Watermark color"
            title: qsTrId("WatermarkColorInput")
            selectedColor: root.textColor
            onAccepted: {
                if (root.operationAllowed)
                    root.textColor = selectedColor
                colorDialogLoader.active = false
            }
            onRejected: colorDialogLoader.active = false
        }
    }
}
