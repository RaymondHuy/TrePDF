pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TrePDF

Rectangle {
    id: root

    required property AppTheme theme
    required property PDFFormDesignerController controller
    required property string fieldId
    required property bool collapsed

    property int maxLength: 1
    property string errorText: ""
    property bool maxLengthEditPending: false
    property string pendingEditFieldId: ""
    property int pendingEditValue: 0

    signal collapsedChangeRequested(bool collapsed)

    implicitWidth: collapsed ? 42 : 264
    color: theme.ribbonSurface

    function isCombField(field) {
        return fieldId.length > 0
            && Number(field.type) === PDFFormDesignerController.Text
            && (Number(field.flags)
                & PDFFormDesignerController.Comb) !== 0
    }

    function refreshField() {
        const field = controller.fieldData(fieldId)
        if (!isCombField(field)) {
            maxLength = 1
            return
        }

        maxLength = Math.max(
            1,
            Math.min(
                controller.maximumTextFieldInputLength,
                Number(field.maxLength) || 1))
    }

    function showUpdateError() {
        //% "Could not update the comb field options."
        errorText = qsTrId("UpdateCombFieldOptionsFailed")
    }

    function clearPendingEdit() {
        maxLengthEditPending = false
        pendingEditFieldId = ""
        pendingEditValue = 0
    }

    function stageMaxLength(value) {
        const updatedMaxLength = Math.max(
            1,
            Math.min(
                controller.maximumTextFieldInputLength,
                Math.round(Number(value))))
        maxLength = updatedMaxLength

        const field = controller.fieldData(fieldId)
        if (!isCombField(field)) {
            clearPendingEdit()
            return
        }
        if (updatedMaxLength === Number(field.maxLength)) {
            clearPendingEdit()
            errorText = ""
            return
        }

        pendingEditFieldId = fieldId
        pendingEditValue = updatedMaxLength
        maxLengthEditPending = true
        errorText = ""
    }

    function commitPendingEdit() {
        if (!maxLengthEditPending)
            return true

        const editFieldId = pendingEditFieldId
        const editValue = pendingEditValue
        if (!controller.setFieldMaxLength(editFieldId, editValue)) {
            showUpdateError()
            return false
        }

        if (editFieldId === fieldId)
            maxLength = editValue
        if (editFieldId === pendingEditFieldId
                && editValue === pendingEditValue) {
            clearPendingEdit()
        }
        errorText = ""
        return true
    }

    function deleteCombField() {
        const deletingFieldId = fieldId
        if (!controller.removeField(deletingFieldId)) {
            //% "Could not delete the comb field."
            errorText = qsTrId("DeleteCombFieldFailed")
        } else if (pendingEditFieldId === deletingFieldId) {
            clearPendingEdit()
        }
    }

    onFieldIdChanged: {
        const pendingFieldChanged = maxLengthEditPending
            && pendingEditFieldId !== fieldId
        const committed = commitPendingEdit()
        if (!committed && pendingFieldChanged)
            clearPendingEdit()
        if (committed || pendingFieldChanged)
            errorText = ""
        refreshField()
    }
    Component.onCompleted: refreshField()
    Component.onDestruction: commitPendingEdit()

    Connections {
        target: root.controller

        function onModelReset() {
            root.clearPendingEdit()
            root.refreshField()
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 1
        color: root.theme.ribbonBorder
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
        //% "Expand comb field options"
        ToolTip.text: qsTrId("ExpandCombFieldOptions")
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
            ColumnLayout {
                spacing: 0

                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    Layout.rightMargin: 8
                    Layout.topMargin: 10
                    spacing: 8

                    Label {
                        Layout.fillWidth: true
                        //% "Comb field options"
                        text: qsTrId("CombFieldOptions")
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
                        //% "Collapse comb field options"
                        ToolTip.text: qsTrId("CollapseCombFieldOptions")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        Accessible.name: ToolTip.text
                        onClicked: {
                            if (root.commitPendingEdit())
                                root.collapsedChangeRequested(true)
                        }

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
                            border.width: collapseButton.visualFocus ? 1 : 0
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    Layout.topMargin: 8
                    //% "The character limit also determines the number of cells."
                    text: qsTrId("CombFieldOptionsHint")
                    color: "#747176"
                    font.pixelSize: 11
                    wrapMode: Text.Wrap
                }

                Label {
                    id: characterLimitLabel

                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    Layout.topMargin: 18
                    //% "Character limit"
                    text: qsTrId("CombFieldCharacterLimit")
                    color: root.theme.ribbonTextColor
                    font.pixelSize: 12
                    font.weight: Font.Medium
                }

                SpinBox {
                    id: characterLimitSpinBox

                    readonly property bool editorActiveFocus:
                        contentItem && contentItem.activeFocus

                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    Layout.topMargin: 6
                    from: 1
                    to: root.controller.maximumTextFieldInputLength
                    value: root.maxLength
                    editable: true
                    live: true
                    palette.highlight: root.theme.foxitDarkOrange
                    Accessible.name: characterLimitLabel.text
                    onValueChanged: {
                        if (editorActiveFocus
                                || activeFocus
                                || up.pressed
                                || down.pressed) {
                            root.stageMaxLength(value)
                        }
                    }
                    onEditorActiveFocusChanged: {
                        if (!editorActiveFocus)
                            root.commitPendingEdit()
                    }
                    onActiveFocusChanged: {
                        if (!activeFocus)
                            root.commitPendingEdit()
                    }
                }

                Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    Layout.topMargin: 8
                    visible: root.errorText.length > 0
                    text: root.errorText
                    color: "#a62e1f"
                    font.pixelSize: 11
                    wrapMode: Text.Wrap
                }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: root.theme.ribbonBorder
                }

                Button {
                    id: deleteCombFieldButton

                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    Layout.topMargin: 12
                    Layout.bottomMargin: 16
                    Layout.preferredHeight: 34
                    //% "Delete comb field"
                    text: qsTrId("DeleteCombField")
                    ToolTip.text: text
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    onClicked: root.deleteCombField()

                    contentItem: Label {
                        text: deleteCombFieldButton.text
                        color: deleteCombFieldButton.enabled
                            ? "#a62e1f"
                            : "#8c8783"
                        font.pixelSize: 12
                        font.weight: Font.Medium
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }

                    background: Rectangle {
                        radius: 4
                        color: deleteCombFieldButton.down
                            ? "#f8ded8"
                            : deleteCombFieldButton.hovered
                                ? "#fff0ed"
                                : "white"
                        border.color: deleteCombFieldButton.enabled
                            ? "#cf6a59"
                            : root.theme.ribbonBorder
                        border.width: deleteCombFieldButton.visualFocus ? 2 : 1
                    }
                }
            }
        }
    }
}
