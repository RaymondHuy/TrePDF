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

    property bool multiline: false
    property int maxLength: 0
    property string errorText: ""
    property bool maxLengthEditPending: false
    property string pendingEditFieldId: ""
    property int pendingEditValue: 0

    signal collapsedChangeRequested(bool collapsed)

    implicitWidth: collapsed ? 42 : 264
    color: theme.ribbonSurface

    function refreshField() {
        const field = controller.fieldData(fieldId)
        if (fieldId.length === 0
                || Number(field.type)
                    !== PDFFormDesignerController.Text) {
            multiline = false
            maxLength = 0
            return
        }

        multiline = (Number(field.flags)
            & PDFFormDesignerController.Multiline) !== 0
        maxLength = Math.max(
            0,
            Math.min(
                controller.maximumTextFieldInputLength,
                Number(field.maxLength)))
    }

    function showUpdateError() {
        //% "Could not update the text field options."
        errorText = qsTrId("UpdateTextFieldOptionsFailed")
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
                Number(value)))
        maxLength = updatedMaxLength

        const field = controller.fieldData(fieldId)
        if (fieldId.length === 0
                || Number(field.type)
                    !== PDFFormDesignerController.Text) {
            clearPendingEdit()
            return
        }
        if (updatedMaxLength
                === Math.max(0, Number(field.maxLength))) {
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

    function setMultiline(enabled) {
        if (!commitPendingEdit())
            return

        const field = controller.fieldData(fieldId)
        const currentFlags = Number(field.flags)
        const updatedFlags = enabled
            ? currentFlags | PDFFormDesignerController.Multiline
            : currentFlags & ~PDFFormDesignerController.Multiline
        if (!controller.setFieldFlags(fieldId, updatedFlags)) {
            showUpdateError()
            return
        }

        multiline = enabled
        errorText = ""
    }

    function setCharacterLimitEnabled(enabled) {
        const updatedMaxLength = enabled
            ? maxLength > 0 ? maxLength : 100
            : 0
        if (!controller.setFieldMaxLength(
                fieldId,
                updatedMaxLength)) {
            showUpdateError()
            return
        }

        clearPendingEdit()
        maxLength = updatedMaxLength
        errorText = ""
    }

    function deleteTextField() {
        const deletingFieldId = fieldId
        if (!controller.removeField(deletingFieldId)) {
            //% "Could not delete the text field."
            errorText = qsTrId("DeleteTextFieldFailed")
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
        //% "Expand text field options"
        ToolTip.text: qsTrId("ExpandTextFieldOptions")
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
                        //% "Text field options"
                        text: qsTrId("TextFieldOptions")
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
                        //% "Collapse text field options"
                        ToolTip.text: qsTrId("CollapseTextFieldOptions")
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
                    //% "Control how users can enter text in this field."
                    text: qsTrId("TextFieldOptionsHint")
                    color: "#747176"
                    font.pixelSize: 11
                    wrapMode: Text.Wrap
                }

                CheckBox {
                    Layout.fillWidth: true
                    Layout.leftMargin: 12
                    Layout.rightMargin: 12
                    Layout.topMargin: 16
                    checked: root.multiline
                    //% "Multi-line"
                    text: qsTrId("MultilineTextField")
                    palette.highlight: root.theme.foxitDarkOrange
                    onClicked: root.setMultiline(!root.multiline)
                }

                Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: 40
                    Layout.rightMargin: 16
                    //% "Allow line breaks and wrapped text."
                    text: qsTrId("MultilineTextFieldHint")
                    color: "#747176"
                    font.pixelSize: 11
                    wrapMode: Text.Wrap
                }

                CheckBox {
                    Layout.fillWidth: true
                    Layout.leftMargin: 12
                    Layout.rightMargin: 12
                    Layout.topMargin: 16
                    checked: root.maxLength > 0
                    //% "Limit characters"
                    text: qsTrId("LimitTextFieldCharacters")
                    palette.highlight: root.theme.foxitDarkOrange
                    onClicked: root.setCharacterLimitEnabled(
                        root.maxLength <= 0)
                }

                Label {
                    id: maximumCharactersLabel

                    Layout.fillWidth: true
                    Layout.leftMargin: 40
                    Layout.rightMargin: 16
                    Layout.topMargin: 8
                    visible: root.maxLength > 0
                    //% "Maximum characters"
                    text: qsTrId("MaximumTextFieldCharacters")
                    color: root.theme.ribbonTextColor
                    font.pixelSize: 12
                    font.weight: Font.Medium
                }

                SpinBox {
                    id: maximumCharactersSpinBox

                    readonly property bool editorActiveFocus:
                        contentItem && contentItem.activeFocus

                    Layout.fillWidth: true
                    Layout.leftMargin: 40
                    Layout.rightMargin: 16
                    Layout.topMargin: 6
                    visible: root.maxLength > 0
                    from: 1
                    to: root.controller.maximumTextFieldInputLength
                    value: root.maxLength > 0 ? root.maxLength : 100
                    editable: true
                    live: true
                    palette.highlight: root.theme.foxitDarkOrange
                    Accessible.name: maximumCharactersLabel.text
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
                    id: deleteTextFieldButton

                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    Layout.topMargin: 12
                    Layout.bottomMargin: 16
                    Layout.preferredHeight: 34
                    //% "Delete text field"
                    text: qsTrId("DeleteTextField")
                    ToolTip.text: text
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    onClicked: root.deleteTextField()

                    contentItem: Label {
                        text: deleteTextFieldButton.text
                        color: deleteTextFieldButton.enabled
                            ? "#a62e1f"
                            : "#8c8783"
                        font.pixelSize: 12
                        font.weight: Font.Medium
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }

                    background: Rectangle {
                        radius: 4
                        color: deleteTextFieldButton.down
                            ? "#f8ded8"
                            : deleteTextFieldButton.hovered
                                ? "#fff0ed"
                                : "white"
                        border.color: deleteTextFieldButton.enabled
                            ? "#cf6a59"
                            : root.theme.ribbonBorder
                        border.width: deleteTextFieldButton.visualFocus ? 2 : 1
                    }
                }
            }
        }
    }
}
