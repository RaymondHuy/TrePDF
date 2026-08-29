pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import TrePDF

Rectangle {
    id: root

    required property AppTheme theme
    required property PDFDocument document
    required property PDFEditController editController
    required property bool collapsed

    property bool colorDialogEditsInterior: false
    property int colorDialogPage: -1
    property string colorDialogObjectId: ""
    property color colorDialogInitialColor: "white"

    readonly property bool drawingCommentSelected: {
        const kind = root.editController.selectedKind
        return kind === "square"
            || kind === "circle"
            || kind === "line"
            || kind === "polygon"
            || kind === "polyLine"
            || kind === "ink"
    }
    //% "Stroke color"
    readonly property string strokeColorLabel: qsTrId("StrokeColor")
    //% "Fill color"
    readonly property string fillColorLabel: qsTrId("FillColor")
    //% "Stroke color: %1"
    readonly property string strokeColorAccessibleName:
        qsTrId("StrokeColorValue").arg(root.colorText(
            root.editController.selectedDrawingStrokeColor))
    readonly property string fillColorAccessibleName:
        root.editController.selectedDrawingHasInterior
            //% "Fill color: %1"
            ? qsTrId("FillColorValue").arg(root.colorText(
                root.editController.selectedDrawingInteriorColor))
            //% "Fill color: No fill"
            : qsTrId("FillColorNoFill")

    signal collapsedChangeRequested(bool collapsed)

    implicitWidth: collapsed ? 42 : 264
    color: theme.ribbonSurface

    function opaqueColor(color) {
        return Qt.rgba(color.r, color.g, color.b, 1)
    }

    function colorText(color) {
        return root.opaqueColor(color).toString().toUpperCase()
    }

    function closeColorDialog() {
        root.colorDialogPage = -1
        root.colorDialogObjectId = ""
        if (colorDialogLoader.status === Loader.Ready) {
            const dialog = colorDialogLoader.item as ColorDialog
            if (dialog && dialog.visible)
                dialog.close()
        }
        colorDialogLoader.active = false
    }

    function colorDialogMatchesSelection() {
        return root.colorDialogPage === root.editController.selectedPage
            && root.colorDialogObjectId.length > 0
            && root.colorDialogObjectId
                === root.editController.selectedObjectId
    }

    function openColorDialog(interior) {
        if (!root.drawingCommentSelected
                || !root.editController.selectedDrawingPropertiesEditable
                || root.editController.saving) {
            return
        }

        root.colorDialogEditsInterior = interior
        root.colorDialogPage = root.editController.selectedPage
        root.colorDialogObjectId = root.editController.selectedObjectId
        const currentColor = interior
            && root.editController.selectedDrawingHasInterior
            ? root.editController.selectedDrawingInteriorColor
            : interior
                ? Qt.rgba(1, 1, 1, 1)
                : root.editController.selectedDrawingStrokeColor
        root.colorDialogInitialColor = root.opaqueColor(currentColor)
        colorDialogLoader.active = true
    }

    function applyColor() {
        if (colorDialogLoader.status === Loader.Ready
                && root.colorDialogMatchesSelection()
                && root.editController.selectedDrawingPropertiesEditable
                && !root.editController.saving) {
            const dialog = colorDialogLoader.item as ColorDialog
            if (dialog) {
                const color = root.opaqueColor(dialog.selectedColor)
                if (root.colorDialogEditsInterior) {
                    root.editController.setSelectedDrawingInteriorColor(color)
                } else {
                    root.editController.setSelectedDrawingStrokeColor(color)
                }
            }
        }
        root.colorDialogPage = -1
        root.colorDialogObjectId = ""
        colorDialogLoader.active = false
    }

    onCollapsedChanged: {
        if (collapsed)
            closeColorDialog()
    }
    onDocumentChanged: closeColorDialog()
    onEditControllerChanged: closeColorDialog()

    Connections {
        target: root.editController

        function onSelectionChanged() {
            if (colorDialogLoader.active
                    && !root.colorDialogMatchesSelection()) {
                root.closeColorDialog()
            }
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 1
        color: root.theme.ribbonBorder
        Accessible.ignored: true
    }

    ToolButton {
        id: expandButton

        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 8
        width: 32
        height: 32
        visible: root.collapsed
        icon.source: "../assets/icons/chevron-left.svg"
        icon.width: 18
        icon.height: 18
        icon.color: root.theme.foxitDarkOrange
        display: AbstractButton.IconOnly
        focusPolicy: Qt.TabFocus
        //% "Expand drawing comment properties"
        ToolTip.text: qsTrId("ExpandDrawingCommentProperties")
        ToolTip.visible: hovered
        ToolTip.delay: 500
        Accessible.name: ToolTip.text
        onClicked: root.collapsedChangeRequested(false)

        background: Rectangle {
            radius: 4
            color: expandButton.down
                ? "#f5dfcf"
                : expandButton.hovered
                    ? root.theme.foxitPaleOrange
                    : "transparent"
            border.color: expandButton.visualFocus
                ? root.theme.foxitDarkOrange
                : "transparent"
            border.width: expandButton.visualFocus ? 1 : 0
        }
    }

    Loader {
        anchors.fill: parent
        anchors.leftMargin: 1
        active: !root.collapsed

        sourceComponent: Component {
            ScrollView {
                id: propertiesScroll

                clip: true
                contentWidth: availableWidth
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                ColumnLayout {
                    width: propertiesScroll.availableWidth
                    spacing: 12

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        Layout.rightMargin: 8
                        Layout.topMargin: 10
                        spacing: 8

                        Label {
                            Layout.fillWidth: true
                            //% "Drawing comment properties"
                            text: qsTrId("ShapeCommentProperties")
                            color: root.theme.ribbonTextColor
                            font.pixelSize: 17
                            font.weight: Font.DemiBold
                            wrapMode: Text.Wrap
                        }

                        ToolButton {
                            id: collapseButton

                            Layout.preferredWidth: 32
                            Layout.preferredHeight: 32
                            icon.source: "../assets/icons/chevron-right.svg"
                            icon.width: 18
                            icon.height: 18
                            icon.color: root.theme.foxitDarkOrange
                            display: AbstractButton.IconOnly
                            focusPolicy: Qt.TabFocus
                            //% "Collapse drawing comment properties"
                            ToolTip.text:
                                qsTrId("CollapseDrawingCommentProperties")
                            ToolTip.visible: hovered
                            ToolTip.delay: 500
                            Accessible.name: ToolTip.text
                            onClicked: root.collapsedChangeRequested(true)

                            background: Rectangle {
                                radius: 4
                                color: collapseButton.down
                                    ? "#f5dfcf"
                                    : collapseButton.hovered
                                        ? root.theme.foxitPaleOrange
                                        : "transparent"
                                border.color: collapseButton.visualFocus
                                    ? root.theme.foxitDarkOrange
                                    : "transparent"
                                border.width:
                                    collapseButton.visualFocus ? 1 : 0
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: root.theme.ribbonBorder
                        Accessible.ignored: true
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        Layout.rightMargin: 16
                        Layout.bottomMargin: 16
                        spacing: 8

                        Label {
                            Layout.fillWidth: true
                            visible: !root.editController
                                .selectedDrawingPropertiesEditable
                            //% "This drawing comment's appearance is read-only."
                            text: qsTrId("DrawingPropertiesReadOnly")
                            color: "#747176"
                            font.pixelSize: 10
                            wrapMode: Text.Wrap
                        }

                        Label {
                            Layout.fillWidth: true
                            text: root.strokeColorLabel
                            color: root.theme.ribbonTextColor
                            font.pixelSize: 11
                            font.weight: Font.Medium
                        }

                        Button {
                            id: strokeColorButton

                            objectName: "drawingStrokeColorButton"
                            Layout.fillWidth: true
                            Layout.preferredHeight: 36
                            enabled: root.editController
                                    .selectedDrawingPropertiesEditable
                                && !root.editController.saving
                            Accessible.name: root.strokeColorAccessibleName
                            onClicked: root.openColorDialog(false)

                            contentItem: RowLayout {
                                spacing: 10

                                Rectangle {
                                    Layout.preferredWidth: 20
                                    Layout.preferredHeight: 20
                                    radius: 3
                                    color: root.editController
                                        .selectedDrawingStrokeColor
                                    border.width: 1
                                    border.color: "#8a8785"
                                    Accessible.ignored: true
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: root.colorText(root.editController
                                        .selectedDrawingStrokeColor)
                                    color: strokeColorButton.enabled
                                        ? root.theme.ribbonTextColor
                                        : "#aaa8a6"
                                    verticalAlignment: Text.AlignVCenter
                                }
                            }

                            background: Rectangle {
                                radius: 4
                                color: !strokeColorButton.enabled
                                    ? "#f4f1ef"
                                    : strokeColorButton.down
                                        ? "#f5dfcf"
                                        : strokeColorButton.hovered
                                            ? root.theme.foxitPaleOrange
                                            : "#ffffff"
                                border.width: 1
                                border.color: strokeColorButton.enabled
                                    ? root.theme.ribbonBorder
                                    : "#ddd9d6"
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: root.editController
                                .selectedDrawingSupportsFill
                            text: root.fillColorLabel
                            color: root.theme.ribbonTextColor
                            font.pixelSize: 11
                            font.weight: Font.Medium
                        }

                        RowLayout {
                            objectName: "drawingFillColorRow"
                            Layout.fillWidth: true
                            spacing: 6
                            visible: root.editController
                                .selectedDrawingSupportsFill

                            Button {
                                id: fillColorButton

                                objectName: "drawingFillColorButton"
                                Layout.fillWidth: true
                                Layout.preferredHeight: 36
                                enabled: root.editController
                                        .selectedDrawingPropertiesEditable
                                    && !root.editController.saving
                                Accessible.name: root.fillColorAccessibleName
                                onClicked: root.openColorDialog(true)

                                contentItem: RowLayout {
                                    spacing: 10

                                    Rectangle {
                                        Layout.preferredWidth: 20
                                        Layout.preferredHeight: 20
                                        radius: 3
                                        color: root.editController
                                                .selectedDrawingHasInterior
                                            ? root.editController
                                                .selectedDrawingInteriorColor
                                            : "#ffffff"
                                        border.width: 1
                                        border.color: "#8a8785"
                                        Accessible.ignored: true

                                        Label {
                                            anchors.centerIn: parent
                                            visible: !root.editController
                                                .selectedDrawingHasInterior
                                            text: "\u2571"
                                            color: "#b3261e"
                                            font.pixelSize: 18
                                            Accessible.ignored: true
                                        }
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: root.editController
                                                .selectedDrawingHasInterior
                                            ? root.colorText(root.editController
                                                .selectedDrawingInteriorColor)
                                            : noFillButton.text
                                        color: fillColorButton.enabled
                                            ? root.theme.ribbonTextColor
                                            : "#aaa8a6"
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                }

                                background: Rectangle {
                                    radius: 4
                                    color: !fillColorButton.enabled
                                        ? "#f4f1ef"
                                        : fillColorButton.down
                                            ? "#f5dfcf"
                                            : fillColorButton.hovered
                                                ? root.theme.foxitPaleOrange
                                                : "#ffffff"
                                    border.width: 1
                                    border.color: fillColorButton.enabled
                                        ? root.theme.ribbonBorder
                                        : "#ddd9d6"
                                }
                            }

                            Button {
                                id: noFillButton

                                objectName: "drawingNoFillButton"
                                Layout.preferredHeight: 36
                                Layout.preferredWidth: 72
                                enabled: root.editController
                                        .selectedDrawingPropertiesEditable
                                    && root.editController
                                        .selectedDrawingHasInterior
                                    && !root.editController.saving
                                //% "No fill"
                                text: qsTrId("NoFill")
                                onClicked: root.editController
                                    .clearSelectedDrawingInteriorColor()
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: 2
                            spacing: 8

                            Label {
                                Layout.fillWidth: true
                                //% "Opacity"
                                text: qsTrId("Opacity")
                                color: root.theme.ribbonTextColor
                                font.pixelSize: 11
                                font.weight: Font.Medium
                            }

                            Label {
                                //% "%1%"
                                text: qsTrId("PercentValue").arg(
                                    Math.round(opacitySlider.value))
                                color: "#747176"
                                font.pixelSize: 10
                                horizontalAlignment: Text.AlignRight
                            }
                        }

                        Slider {
                            id: opacitySlider

                            objectName: "drawingOpacitySlider"
                            property bool pointerValueChanged: false
                            property int pointerPage: -1
                            property string pointerObjectId: ""

                            Layout.fillWidth: true
                            from: 0
                            to: 100
                            stepSize: 1
                            snapMode: Slider.SnapAlways
                            value: root.editController
                                .selectedDrawingOpacity * 100
                            enabled: root.editController
                                    .selectedDrawingPropertiesEditable
                                && !root.editController.saving
                            //% "Opacity"
                            Accessible.name: qsTrId("Opacity")
                            onPressedChanged: {
                                if (pressed) {
                                    pointerValueChanged = false
                                    pointerPage
                                        = root.editController.selectedPage
                                    pointerObjectId = root.editController
                                        .selectedObjectId
                                    return
                                }

                                const sameSelection = pointerPage
                                        === root.editController.selectedPage
                                    && pointerObjectId.length > 0
                                    && pointerObjectId
                                        === root.editController
                                            .selectedObjectId
                                if (pointerValueChanged
                                        && sameSelection
                                        && root.editController
                                            .selectedDrawingPropertiesEditable
                                        && !root.editController.saving) {
                                    root.editController
                                        .setSelectedDrawingOpacity(value / 100)
                                }
                                pointerValueChanged = false
                                pointerPage = -1
                                pointerObjectId = ""
                            }
                            onMoved: {
                                if (pressed) {
                                    pointerValueChanged = true
                                } else if (root.editController
                                               .selectedDrawingPropertiesEditable
                                           && !root.editController.saving) {
                                    root.editController
                                        .setSelectedDrawingOpacity(value / 100)
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Loader {
        id: colorDialogLoader

        active: false
        sourceComponent: Component {
            ColorDialog {
                title: root.colorDialogEditsInterior
                    ? root.fillColorLabel
                    : root.strokeColorLabel
                onAccepted: root.applyColor()
                onRejected: {
                    root.colorDialogPage = -1
                    root.colorDialogObjectId = ""
                    colorDialogLoader.active = false
                }
            }
        }
        onLoaded: {
            const dialog = item as ColorDialog
            if (dialog) {
                dialog.selectedColor = root.colorDialogInitialColor
                dialog.open()
            }
        }
    }
}
