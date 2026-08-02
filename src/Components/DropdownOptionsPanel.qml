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

    property bool sourceBacked: false
    property string errorText: ""
    property string newOptionDraft: ""

    signal collapsedChangeRequested(bool collapsed)

    implicitWidth: collapsed ? 42 : 264
    color: theme.ribbonSurface

    function copyOptions(values) {
        const copiedOptions = []
        if (values === undefined || values === null)
            return copiedOptions

        for (let index = 0; index < values.length; ++index)
            copiedOptions.push(String(values[index]))
        return copiedOptions
    }

    function currentOptions() {
        const options = []
        for (let index = 0; index < optionsModel.count; ++index)
            options.push(String(optionsModel.get(index).value))
        return options
    }

    function replaceOptions(values) {
        const options = copyOptions(values)
        optionsModel.clear()
        for (let index = 0; index < options.length; ++index)
            optionsModel.append({ value: options[index] })
    }

    function refreshField() {
        const field = controller.fieldData(fieldId)
        if (fieldId.length === 0
                || Number(field.type)
                    !== PDFFormDesignerController.Dropdown) {
            optionsModel.clear()
            sourceBacked = false
            return
        }

        replaceOptions(field.options)
        sourceBacked = Boolean(field.sourceBacked)
    }

    function optionIndex(option, ignoredIndex) {
        for (let index = 0; index < optionsModel.count; ++index) {
            if (index !== ignoredIndex
                    && String(optionsModel.get(index).value) === option) {
                return index
            }
        }
        return -1
    }

    function commitOptions(options) {
        const success = controller.setFieldOptions(fieldId, options)
        if (!success) {
            //% "Could not update the dropdown options."
            errorText = qsTrId("UpdateDropdownOptionsFailed")
            return false
        }

        errorText = ""
        return true
    }

    function addOption(value) {
        const option = String(value).trim()
        if (option.length === 0) {
            //% "Enter an option value."
            errorText = qsTrId("EnterDropdownOptionValue")
            return false
        }
        if (optionIndex(option, -1) >= 0) {
            //% "Dropdown options must be unique."
            errorText = qsTrId("DropdownOptionsMustBeUnique")
            return false
        }

        const options = currentOptions()
        options.push(option)
        if (!commitOptions(options))
            return false

        optionsModel.append({ value: option })
        return true
    }

    function renameOption(index, value) {
        if (index < 0 || index >= optionsModel.count)
            return false

        const option = String(value).trim()
        if (option.length === 0) {
            //% "Enter an option value."
            errorText = qsTrId("EnterDropdownOptionValue")
            return false
        }
        if (optionIndex(option, index) >= 0) {
            //% "Dropdown options must be unique."
            errorText = qsTrId("DropdownOptionsMustBeUnique")
            return false
        }
        if (String(optionsModel.get(index).value) === option) {
            errorText = ""
            return true
        }

        const success = controller.renameFieldOption(fieldId, index, option)
        if (!success) {
            //% "Could not update the dropdown options."
            errorText = qsTrId("UpdateDropdownOptionsFailed")
            return false
        }

        optionsModel.setProperty(index, "value", option)
        errorText = ""
        return true
    }

    function finishOptionEdit(editFieldId, optionIndex, value) {
        if (editFieldId === fieldId)
            return renameOption(optionIndex, value)

        const option = String(value).trim()
        if (editFieldId.length === 0 || option.length === 0)
            return false
        return controller.renameFieldOption(
            editFieldId,
            optionIndex,
            option)
    }

    function removeOption(index) {
        if (index < 0 || index >= optionsModel.count)
            return
        if (optionsModel.count <= 1) {
            //% "A dropdown must contain at least one option."
            errorText = qsTrId("DropdownRequiresOneOption")
            return
        }

        const options = currentOptions()
        options.splice(index, 1)
        if (commitOptions(options))
            optionsModel.remove(index)
    }

    function deleteDropdown() {
        if (!controller.removeField(fieldId)) {
            //% "Could not delete the dropdown."
            errorText = qsTrId("DeleteDropdownFailed")
        }
    }

    onFieldIdChanged: {
        errorText = ""
        newOptionDraft = ""
        refreshField()
    }
    Component.onCompleted: refreshField()

    Connections {
        target: root.controller

        function onModelReset() {
            root.refreshField()
        }
    }

    ListModel {
        id: optionsModel
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
        //% "Expand dropdown options"
        ToolTip.text: qsTrId("ExpandDropdownOptions")
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
                        //% "Dropdown options"
                        text: qsTrId("DropdownOptions")
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
                        //% "Collapse dropdown options"
                        ToolTip.text: qsTrId("CollapseDropdownOptions")
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
                            border.width: collapseButton.visualFocus ? 1 : 0
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    Layout.topMargin: 8
                    //% "Add, edit, or remove values shown in this dropdown."
                    text: qsTrId("ManageDropdownOptionsHint")
                    color: "#747176"
                    font.pixelSize: 11
                    wrapMode: Text.Wrap
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    Layout.topMargin: 12
                    spacing: 6

                    TextField {
                        id: newOptionField

                        Layout.fillWidth: true
                        text: root.newOptionDraft
                        selectByMouse: true
                        //% "New option"
                        placeholderText: qsTrId("NewDropdownOption")
                        onTextEdited: root.newOptionDraft = text
                        onAccepted: {
                            if (root.addOption(text))
                                root.newOptionDraft = ""
                        }
                    }

                    ToolButton {
                        id: addOptionButton

                        Layout.preferredWidth: 32
                        Layout.preferredHeight: 32
                        enabled: root.newOptionDraft.trim().length > 0
                        text: "+"
                        font.pixelSize: 18
                        //% "Add option"
                        ToolTip.text: qsTrId("AddDropdownOption")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        Accessible.name: ToolTip.text
                        onClicked: {
                            if (root.addOption(root.newOptionDraft))
                                root.newOptionDraft = ""
                        }

                        background: Rectangle {
                            radius: 4
                            color: addOptionButton.down
                                ? "#f5dfcf"
                                : addOptionButton.hovered
                                    ? root.theme.foxitPaleOrange
                                    : "white"
                            border.color: addOptionButton.visualFocus
                                ? root.theme.foxitDarkOrange
                                : root.theme.ribbonBorder
                            border.width: addOptionButton.visualFocus ? 2 : 1
                        }
                    }
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

                Rectangle {
                    Layout.fillWidth: true
                    Layout.topMargin: 12
                    Layout.preferredHeight: 1
                    color: root.theme.ribbonBorder
                }

                ListView {
                    id: optionsList

                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    Layout.topMargin: 12
                    Layout.bottomMargin: 12
                    clip: true
                    spacing: 6
                    model: optionsModel
                    reuseItems: true

                    delegate: RowLayout {
                        id: optionDelegate

                        required property int index
                        required property string value

                        width: ListView.view.width
                        height: 34
                        spacing: 6

                        TextField {
                            id: optionEditor

                            property string editFieldId: ""
                            property int editOptionIndex: -1

                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            text: optionDelegate.value
                            selectByMouse: true

                            function captureEditIdentity() {
                                editFieldId = root.fieldId
                                editOptionIndex = optionDelegate.index
                            }

                            Component.onCompleted: captureEditIdentity()
                            onActiveFocusChanged: {
                                if (activeFocus)
                                    captureEditIdentity()
                            }
                            onTextEdited: {
                                root.finishOptionEdit(
                                    editFieldId,
                                    editOptionIndex,
                                    text)
                            }
                            onEditingFinished: {
                                root.finishOptionEdit(
                                    editFieldId,
                                    editOptionIndex,
                                    text)
                                text = Qt.binding(
                                    () => optionDelegate.value)
                            }
                        }

                        ListView.onReused: {
                            optionEditor.captureEditIdentity()
                            optionEditor.text = Qt.binding(
                                () => optionDelegate.value)
                        }

                        ToolButton {
                            id: removeOptionButton

                            Layout.preferredWidth: 30
                            Layout.fillHeight: true
                            enabled: optionsModel.count > 1
                            text: "\u2212"
                            font.pixelSize: 16
                            //% "Remove option"
                            ToolTip.text: qsTrId("RemoveDropdownOption")
                            ToolTip.visible: hovered
                            ToolTip.delay: 500
                            Accessible.name: ToolTip.text
                            onClicked: root.removeOption(optionDelegate.index)

                            background: Rectangle {
                                radius: 4
                                color: removeOptionButton.down
                                    ? "#f5dfcf"
                                    : removeOptionButton.hovered
                                        ? root.theme.foxitPaleOrange
                                        : "white"
                                border.color: removeOptionButton.visualFocus
                                    ? root.theme.foxitDarkOrange
                                    : root.theme.ribbonBorder
                                border.width: removeOptionButton.visualFocus
                                    ? 2
                                    : 1
                            }
                        }
                    }

                    ScrollBar.vertical: ScrollBar {
                        policy: ScrollBar.AsNeeded
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: root.theme.ribbonBorder
                }

                Button {
                    id: deleteDropdownButton

                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    Layout.topMargin: 12
                    Layout.bottomMargin: 16
                    Layout.preferredHeight: 34
                    //% "Delete dropdown"
                    text: qsTrId("DeleteDropdown")
                    ToolTip.text: text
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    onClicked: root.deleteDropdown()

                    contentItem: Label {
                        text: deleteDropdownButton.text
                        color: deleteDropdownButton.enabled
                            ? "#a62e1f"
                            : "#8c8783"
                        font.pixelSize: 12
                        font.weight: Font.Medium
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }

                    background: Rectangle {
                        radius: 4
                        color: deleteDropdownButton.down
                            ? "#f8ded8"
                            : deleteDropdownButton.hovered
                                ? "#fff0ed"
                                : "white"
                        border.color: deleteDropdownButton.enabled
                            ? "#cf6a59"
                            : root.theme.ribbonBorder
                        border.width: deleteDropdownButton.visualFocus ? 2 : 1
                    }
                }
            }
        }
    }
}
