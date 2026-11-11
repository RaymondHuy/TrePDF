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
    property var prepareEditTransition: function() { return true }

    property bool colorDialogEditsInterior: false
    property int colorDialogPage: -1
    property string colorDialogObjectId: ""
    property color colorDialogInitialColor: "white"
    property var tableDraft: ({})
    property int tableSelectionPage: -1
    property string tableSelectionId: ""
    property PDFEditController tablePopupController: null
    property PDFDocument tablePopupDocument: null
    property int tablePopupPage: -1
    property string tablePopupObjectId: ""
    property bool tablePopupRowAxis: true
    property bool tablePopupRemoving: false
    property int tablePopupDimension: 0
    property Item tablePopupAnchor: null
    property bool tablePopupSubmitting: false
    readonly property bool tableSelected: editShapeSelected
        && tableDraft.shapeType === PDFEditController.TableShape
    readonly property int tableRows: Number(tableDraft.tableRows || 0)
    readonly property int tableColumns: Number(tableDraft.tableColumns || 0)
    readonly property bool tableInsertionAvailable: tableSelected
        && editController.selectedDrawingPropertiesEditable && !editController.saving

    readonly property bool editShapeSelected:
        root.editController.selectedKind === "addedShape"
            || root.editController.selectedKind === "sourceShape"
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

    function refreshTableDraft() {
        if (!editController)
            return
        const changed = tableSelectionPage !== editController.selectedPage
            || tableSelectionId !== editController.selectedObjectId
        tableSelectionPage = editController.selectedPage
        tableSelectionId = editController.selectedObjectId
        tableDraft = editShapeSelected ? editController.selectedDraftPreview() : ({})
        if (changed && !tablePopupSubmitting)
            closeTableStructurePopup()
    }

    function tablePopupMatchesSelection() {
        return tablePopupController === editController
            && tablePopupDocument === document
            && tablePopupPage === editController.selectedPage
            && tablePopupObjectId.length > 0
            && tablePopupObjectId === editController.selectedObjectId
            && tableInsertionAvailable
    }

    function closeTableStructurePopup() {
        if (tablePopupLoader.status === Loader.Ready && tablePopupLoader.item)
            (tablePopupLoader.item as Popup).close()
        tablePopupLoader.active = false
        tablePopupController = null
        tablePopupDocument = null
        tablePopupAnchor = null
        tablePopupPage = -1
        tablePopupObjectId = ""
    }

    function openTableStructurePopup(rowAxis, removing, anchor) {
        if (!tableInsertionAvailable)
            return
        const targetPage = editController.selectedPage
        const targetId = editController.selectedObjectId
        if (!prepareEditTransition()
                || targetPage !== editController.selectedPage
                || targetId !== editController.selectedObjectId)
            return
        closeTableStructurePopup()
        refreshTableDraft()
        const dimension = rowAxis ? tableRows : tableColumns
        if (!tableInsertionAvailable || (removing ? dimension <= 1 : dimension >= 20))
            return
        tablePopupController = editController
        tablePopupDocument = document
        tablePopupPage = targetPage
        tablePopupObjectId = targetId
        tablePopupRowAxis = rowAxis
        tablePopupRemoving = removing
        tablePopupDimension = dimension
        tablePopupAnchor = anchor
        tablePopupLoader.active = true
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
        if (!prepareEditTransition())
            return
        if ((!root.drawingCommentSelected && !root.editShapeSelected)
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
                && !root.editController.saving
                && root.prepareEditTransition()) {
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
        if (collapsed) {
            closeColorDialog()
            closeTableStructurePopup()
        }
    }
    onVisibleChanged: {
        if (!visible)
            closeTableStructurePopup()
    }
    onDocumentChanged: {
        closeColorDialog()
        closeTableStructurePopup()
    }
    onEditControllerChanged: {
        closeColorDialog()
        closeTableStructurePopup()
        refreshTableDraft()
    }
    Component.onCompleted: refreshTableDraft()

    Connections {
        target: root.editController

        function onSelectionChanged() {
            root.refreshTableDraft()
            if (colorDialogLoader.active
                    && !root.colorDialogMatchesSelection()) {
                root.closeColorDialog()
            }
        }
        function onPageDataChanged() {
            root.refreshTableDraft()
            if (tablePopupLoader.active && !root.tablePopupSubmitting
                    && (!root.tablePopupMatchesSelection()
                        || root.tablePopupDimension !== (root.tablePopupRowAxis ? root.tableRows : root.tableColumns)))
                root.closeTableStructurePopup()
        }
        function onHistoryStateChanged() {
            if (!root.tablePopupSubmitting)
                root.closeTableStructurePopup()
        }
        function onSavingChanged() {
            if (root.editController.saving)
                root.closeTableStructurePopup()
        }
    }

    Connections {
        target: root.document
        function onSourceChanged() { root.closeTableStructurePopup() }
        function onPageStructureChanged() { root.closeTableStructurePopup() }
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
        ToolTip.text: root.editShapeSelected
            //% "Expand shape properties"
            ? qsTrId("ExpandShapeProperties")
            //% "Expand drawing comment properties"
            : qsTrId("ExpandDrawingCommentProperties")
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
                            text: root.editShapeSelected
                                //% "Shape properties"
                                ? qsTrId("ShapeProperties")
                                //% "Drawing comment properties"
                                : qsTrId("ShapeCommentProperties")
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
                            ToolTip.text: root.editShapeSelected
                                //% "Collapse shape properties"
                                ? qsTrId("CollapseShapeProperties")
                                //% "Collapse drawing comment properties"
                                : qsTrId("CollapseDrawingCommentProperties")
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

                        ColumnLayout {
                            Layout.fillWidth: true
                            visible: root.tableSelected
                            spacing: 8

                            Label {
                                Layout.fillWidth: true
                                //% "Table rows and columns"
                                text: qsTrId("TableStructure")
                                color: root.theme.ribbonTextColor
                                font.pixelSize: 12
                                font.weight: Font.DemiBold
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Button {
                                    id: tableAppendRowButton
                                    objectName: "tableAppendRowButton"
                                    Layout.fillWidth: true
                                    enabled: root.tableInsertionAvailable && root.tableRows < 20
                                    //% "Add row"
                                    text: qsTrId("TableInsertRow")
                                    onClicked: root.openTableStructurePopup(true, false, tableAppendRowButton)
                                }
                                Button {
                                    id: tableAppendColumnButton
                                    objectName: "tableAppendColumnButton"
                                    Layout.fillWidth: true
                                    enabled: root.tableInsertionAvailable && root.tableColumns < 20
                                    //% "Add column"
                                    text: qsTrId("TableInsertColumn")
                                    onClicked: root.openTableStructurePopup(false, false, tableAppendColumnButton)
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Button {
                                    id: tableRemoveRowButton
                                    objectName: "tableRemoveRowButton"
                                    Layout.fillWidth: true
                                    enabled: root.tableInsertionAvailable && root.tableRows > 1
                                    //% "Delete row"
                                    text: qsTrId("TableRemoveRow")
                                    onClicked: root.openTableStructurePopup(true, true, tableRemoveRowButton)
                                }
                                Button {
                                    id: tableRemoveColumnButton
                                    objectName: "tableRemoveColumnButton"
                                    Layout.fillWidth: true
                                    enabled: root.tableInsertionAvailable && root.tableColumns > 1
                                    //% "Delete column"
                                    text: qsTrId("TableRemoveColumn")
                                    onClicked: root.openTableStructurePopup(false, true, tableRemoveColumnButton)
                                }
                            }

                            Label {
                                Layout.fillWidth: true
                                //% "Existing cell sizes stay the same. The table expands down or to the right."
                                text: qsTrId("TableInsertSizeHint")
                                color: "#747176"
                                font.pixelSize: 11
                                wrapMode: Text.Wrap
                            }

                            Label {
                                Layout.fillWidth: true
                                //% "Deletes the specified rows or columns, including their cell content."
                                text: qsTrId("TableRemoveHint")
                                color: "#747176"
                                font.pixelSize: 11
                                wrapMode: Text.Wrap
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 1
                                color: root.theme.ribbonBorder
                                Accessible.ignored: true
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: root.editController.selectedTableFrameOnly
                            //% "Drag the borders to resize the table frame. Text stays in place."
                            text: qsTrId("TableFrameOnlyHint")
                            color: root.theme.ribbonTextColor
                            font.pixelSize: 11
                            wrapMode: Text.Wrap
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: !root.editController
                                .selectedDrawingPropertiesEditable
                            text: root.editShapeSelected
                                //% "This shape's appearance is read-only."
                                ? qsTrId("ShapePropertiesReadOnly")
                                //% "This drawing comment's appearance is read-only."
                                : qsTrId("DrawingPropertiesReadOnly")
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
                                onClicked: {
                                    if (root.prepareEditTransition())
                                        root.editController.clearSelectedDrawingInteriorColor()
                                }
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
                                const requestedOpacity = value / 100
                                if (pointerValueChanged
                                        && sameSelection
                                        && root.editController
                                            .selectedDrawingPropertiesEditable
                                        && !root.editController.saving
                                        && root.prepareEditTransition()) {
                                    root.editController
                                        .setSelectedDrawingOpacity(requestedOpacity)
                                }
                                pointerValueChanged = false
                                pointerPage = -1
                                pointerObjectId = ""
                                value = Qt.binding(() => root.editController.selectedDrawingOpacity * 100)
                            }
                            onMoved: {
                                const requestedOpacity = value / 100
                                if (pressed) {
                                    pointerValueChanged = true
                                } else if (root.editController
                                               .selectedDrawingPropertiesEditable
                                           && !root.editController.saving
                                           && root.prepareEditTransition()) {
                                    root.editController
                                        .setSelectedDrawingOpacity(requestedOpacity)
                                }
                                if (!pressed)
                                    value = Qt.binding(() => root.editController.selectedDrawingOpacity * 100)
                            }
                        }
                    }
                }
            }
        }
    }

    Loader {
        id: tablePopupLoader

        active: false
        onLoaded: {
            if (status === Loader.Ready && item)
                (item as Popup).open()
        }
        sourceComponent: Popup {
            id: tablePopup
            objectName: "tableStructurePopup"

            property string errorText: ""
            readonly property bool removing: root.tablePopupRemoving
            readonly property bool rowAxis: root.tablePopupRowAxis
            readonly property int dimension: root.tablePopupDimension
            readonly property point anchorPoint: root.tablePopupAnchor && parent
                ? root.tablePopupAnchor.mapToItem(parent, root.tablePopupAnchor.width, root.tablePopupAnchor.height)
                : Qt.point(0, 0)

            parent: Overlay.overlay
            width: Math.min(340, parent ? parent.width - 24 : 340)
            height: Math.min(implicitHeight, parent ? parent.height - 24 : implicitHeight)
            x: Math.max(12, Math.min(anchorPoint.x - width, parent ? parent.width - width - 12 : 12))
            y: Math.max(12, Math.min(anchorPoint.y + 4, parent ? parent.height - height - 12 : 12))
            padding: 16
            modal: true
            focus: true
            popupType: Popup.Item
            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
            onClosed: root.closeTableStructurePopup()
            onOpened: {
                if (removing) {
                    indicesInput.forceActiveFocus()
                    indicesInput.selectAll()
                } else {
                    countBox.forceActiveFocus()
                }
            }

            function submit() {
                if (!root.tablePopupMatchesSelection()) {
                    root.closeTableStructurePopup()
                    return
                }
                let count = countBox.value
                let position = positionBox.value
                if (!removing) {
                    const countInput = countBox.contentItem as TextInput
                    const positionInput = positionBox.contentItem as TextInput
                    count = countInput ? countInput.acceptableInput
                        ? Number.fromLocaleString(countBox.locale, countInput.text) : NaN : count
                    position = positionInput ? positionInput.acceptableInput
                        ? Number.fromLocaleString(positionBox.locale, positionInput.text) : NaN : position
                    if (!Number.isInteger(count) || count < 1 || count > 20 - dimension
                            || !Number.isInteger(position) || position < 1 || position > dimension + 1) {
                        //% "Enter a valid quantity and insertion position."
                        errorText = qsTrId("TableStructureInvalidNumbers")
                        return
                    }
                }
                root.tablePopupSubmitting = true
                const error = removing
                    ? rowAxis ? root.editController.removeSelectedTableRows(indicesInput.text)
                        : root.editController.removeSelectedTableColumns(indicesInput.text)
                    : rowAxis ? root.editController.insertSelectedTableRows(position - 1, count)
                        : root.editController.insertSelectedTableColumns(position - 1, count)
                root.tablePopupSubmitting = false
                if (error.length > 0) {
                    errorText = error
                    return
                }
                root.refreshTableDraft()
                root.closeTableStructurePopup()
            }

            contentItem: ScrollView {
                id: structureScroll
                implicitHeight: structureContent.implicitHeight
                contentWidth: availableWidth

                ColumnLayout {
                    id: structureContent
                    width: structureScroll.availableWidth
                    spacing: 10

                    Label {
                        Layout.fillWidth: true
                        text: tablePopup.removing
                            ? tablePopup.rowAxis ? qsTrId("TableRemoveRow") : qsTrId("TableRemoveColumn")
                            : tablePopup.rowAxis ? qsTrId("TableInsertRow") : qsTrId("TableInsertColumn")
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                        color: root.theme.ribbonTextColor
                        wrapMode: Text.Wrap
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: !tablePopup.removing
                        //% "Quantity"
                        text: qsTrId("TableStructureQuantity")
                        color: root.theme.ribbonTextColor
                    }
                    SpinBox {
                        id: countBox
                        objectName: "tableStructureCountBox"
                        Layout.fillWidth: true
                        visible: !tablePopup.removing
                        from: 1
                        to: Math.max(1, 20 - tablePopup.dimension)
                        value: 1
                        editable: true
                        Accessible.name: qsTrId("TableStructureQuantity")
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: !tablePopup.removing
                        text: tablePopup.rowAxis
                            //% "Insert at row"
                            ? qsTrId("TableStructureRowPosition")
                            //% "Insert at column"
                            : qsTrId("TableStructureColumnPosition")
                        color: root.theme.ribbonTextColor
                    }
                    SpinBox {
                        id: positionBox
                        objectName: "tableStructurePositionBox"
                        Layout.fillWidth: true
                        visible: !tablePopup.removing
                        from: 1
                        to: tablePopup.dimension + 1
                        value: tablePopup.dimension + 1
                        editable: true
                        Accessible.name: tablePopup.rowAxis
                            ? qsTrId("TableStructureRowPosition") : qsTrId("TableStructureColumnPosition")
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: !tablePopup.removing
                        //% "New rows or columns are inserted before this position. Choose %1 to add at the end."
                        text: qsTrId("TableStructurePositionHint").arg(tablePopup.dimension + 1)
                        color: "#747176"
                        font.pixelSize: 11
                        wrapMode: Text.Wrap
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: tablePopup.removing
                        text: tablePopup.rowAxis
                            //% "Rows to delete"
                            ? qsTrId("TableStructureRowIndices")
                            //% "Columns to delete"
                            : qsTrId("TableStructureColumnIndices")
                        color: root.theme.ribbonTextColor
                    }
                    TextField {
                        id: indicesInput
                        objectName: "tableStructureIndicesInput"
                        Layout.fillWidth: true
                        visible: tablePopup.removing
                        text: String(tablePopup.dimension)
                        maximumLength: 128
                        //% "Example: 1,3,5"
                        placeholderText: qsTrId("TableStructureIndicesPlaceholder")
                        Accessible.name: tablePopup.rowAxis
                            ? qsTrId("TableStructureRowIndices") : qsTrId("TableStructureColumnIndices")
                        onAccepted: tablePopup.submit()
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: tablePopup.removing
                        //% "Enter row or column numbers separated by commas, for example 1,3,5."
                        text: qsTrId("TableStructureIndicesHint")
                        color: "#747176"
                        font.pixelSize: 11
                        wrapMode: Text.Wrap
                    }
                    Label {
                        objectName: "tableStructureError"
                        Layout.fillWidth: true
                        visible: tablePopup.errorText.length > 0
                        text: tablePopup.errorText
                        color: "#b3261e"
                        font.pixelSize: 11
                        wrapMode: Text.Wrap
                        Accessible.role: Accessible.AlertMessage
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Button {
                            objectName: "tableStructureCancelButton"
                            Layout.fillWidth: true
                            //% "Cancel"
                            text: qsTrId("Cancel")
                            onClicked: root.closeTableStructurePopup()
                        }
                        Button {
                            objectName: "tableStructureApplyButton"
                            Layout.fillWidth: true
                            //% "Apply"
                            text: qsTrId("TableStructureApply")
                            onClicked: tablePopup.submit()
                        }
                    }
                }
            }

            background: Rectangle {
                radius: 6
                color: root.theme.ribbonSurface
                border.color: root.theme.ribbonBorder
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
