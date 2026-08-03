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

    property string errorText: ""
    property string groupNameDraft: ""
    property bool groupNameEditPending: false
    property string pendingEditFieldId: ""
    property string pendingEditValue: ""

    signal collapsedChangeRequested(bool collapsed)

    implicitWidth: collapsed ? 42 : 264
    color: theme.ribbonSurface

    function refreshField() {
        const field = controller.fieldData(fieldId)
        if (fieldId.length === 0
                || Number(field.type)
                    !== PDFFormDesignerController.RadioButton) {
            groupNameDraft = ""
            return
        }

        groupNameDraft = String(field.name)
    }

    function commitGroupName(editFieldId, value) {
        const groupName = String(value).trim()
        if (groupName.length === 0) {
            //% "Enter a group ID."
            errorText = qsTrId("EnterRadioGroupId")
            return false
        }

        if (!controller.setFieldName(editFieldId, groupName)) {
            //% "Could not update the radio group."
            errorText = qsTrId("UpdateRadioGroupFailed")
            return false
        }

        if (editFieldId === fieldId)
            groupNameDraft = groupName
        errorText = ""
        return true
    }

    function commitPendingEdit() {
        if (!groupNameEditPending)
            return true

        const editFieldId = pendingEditFieldId
        const editValue = pendingEditValue
        if (!commitGroupName(editFieldId, editValue))
            return false

        if (editFieldId === pendingEditFieldId
                && editValue === pendingEditValue) {
            clearPendingEdit()
        }
        return true
    }

    function clearPendingEdit() {
        groupNameEditPending = false
        pendingEditFieldId = ""
        pendingEditValue = ""
    }

    function deleteRadioButton() {
        const deletingFieldId = fieldId
        if (!controller.removeField(fieldId)) {
            //% "Could not delete the radio button."
            errorText = qsTrId("DeleteRadioButtonFailed")
        } else if (pendingEditFieldId === deletingFieldId) {
            clearPendingEdit()
        }
    }

    onFieldIdChanged: {
        const pendingFieldChanged = groupNameEditPending
            && pendingEditFieldId !== fieldId
        const committed = commitPendingEdit()
        if (!committed && pendingFieldChanged)
            clearPendingEdit()
        if (committed || pendingFieldChanged)
            errorText = ""
        refreshField()
    }
    Component.onCompleted: refreshField()

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
        //% "Expand radio group options"
        ToolTip.text: qsTrId("ExpandRadioGroupOptions")
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
                        //% "Radio group"
                        text: qsTrId("RadioGroup")
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
                        //% "Collapse radio group options"
                        ToolTip.text: qsTrId("CollapseRadioGroupOptions")
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
                    //% "Radio buttons with the same group ID belong to one group."
                    text: qsTrId("RadioGroupIdHint")
                    color: "#747176"
                    font.pixelSize: 11
                    wrapMode: Text.Wrap
                }

                Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    Layout.topMargin: 16
                    //% "Group ID"
                    text: qsTrId("RadioGroupId")
                    color: root.theme.ribbonTextColor
                    font.pixelSize: 12
                    font.weight: Font.Medium
                }

                TextField {
                    id: groupNameField

                    property string editFieldId: ""
                    property string editValue: ""

                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    Layout.topMargin: 6
                    text: root.groupNameDraft
                    selectByMouse: true
                    //% "Enter group ID"
                    placeholderText: qsTrId("EnterRadioGroupIdPlaceholder")

                    function captureEditIdentity() {
                        editFieldId = root.fieldId
                        editValue = text
                    }

                    Component.onCompleted: captureEditIdentity()
                    onActiveFocusChanged: {
                        if (activeFocus)
                            captureEditIdentity()
                    }
                    onTextEdited: {
                        editValue = text
                        root.groupNameDraft = text
                        root.pendingEditFieldId = editFieldId
                        root.pendingEditValue = editValue
                        root.groupNameEditPending = true
                    }
                    onTextChanged: {
                        if (!root.groupNameEditPending
                                && editFieldId !== root.fieldId) {
                            captureEditIdentity()
                        }
                    }
                    onEditingFinished: root.commitPendingEdit()
                }

                Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    Layout.topMargin: 6
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
                    id: deleteRadioButton

                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    Layout.topMargin: 12
                    Layout.bottomMargin: 16
                    Layout.preferredHeight: 34
                    //% "Delete radio button"
                    text: qsTrId("DeleteRadioButton")
                    ToolTip.text: text
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    onClicked: root.deleteRadioButton()

                    contentItem: Label {
                        text: deleteRadioButton.text
                        color: deleteRadioButton.enabled
                            ? "#a62e1f"
                            : "#8c8783"
                        font.pixelSize: 12
                        font.weight: Font.Medium
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }

                    background: Rectangle {
                        radius: 4
                        color: deleteRadioButton.down
                            ? "#f8ded8"
                            : deleteRadioButton.hovered
                                ? "#fff0ed"
                                : "white"
                        border.color: deleteRadioButton.enabled
                            ? "#cf6a59"
                            : root.theme.ribbonBorder
                        border.width: deleteRadioButton.visualFocus ? 2 : 1
                    }
                }
            }
        }
    }
}
