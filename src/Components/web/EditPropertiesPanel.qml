pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import ".."

Rectangle {
    id: root

    required property BrowserEditController controller
    required property AppTheme theme
    readonly property var displayTarget: controller.pendingObject || controller.selectedObject
    readonly property bool textObject: displayTarget !== null && displayTarget.type === "text"
    readonly property bool imageObject: displayTarget !== null && displayTarget.type === "image"
    readonly property bool editable: displayTarget !== null && displayTarget.editable !== false
    property bool resetting: false
    property string inputTargetKey: ""
    property var inputPendingTarget: null

    implicitWidth: 272
    color: theme.ribbonSurface
    border.color: theme.ribbonBorder
    visible: displayTarget !== null

    function targetKey(): string {
        return displayTarget ? displayTarget.type + ":" + displayTarget.page + ":" + displayTarget.id : ""
    }

    function refreshTargetInputs(): void {
        if (controller.hasPendingInput && inputTargetKey === targetKey()
                && inputPendingTarget === controller.pendingObject)
            return
        resetInputs()
    }

    function resetInputs(): void {
        inputTargetKey = targetKey()
        inputPendingTarget = controller.pendingObject
        resetting = true
        const object = controller.workingObject
        colorInput.text = object && object.color ? object.color : "#202124"
        for (const field of [fontInput, strokeInput, xInput, yInput, widthInput, heightInput])
            field.resetValue()
        resetting = false
    }

    onDisplayTargetChanged: inputReset.restart()
    Component.onCompleted: resetInputs()

    Timer {
        id: inputReset
        interval: 0
        onTriggered: root.refreshTargetInputs()
    }

    component NumberInput: TextField {
        id: field
        required property string fieldName
        property real factor: 1
        Layout.fillWidth: true
        selectByMouse: true
        inputMethodHints: Qt.ImhFormattedNumbersOnly
        objectName: "webEdit" + fieldName + "Input"
        function resetValue(): void {
            const object = root.controller.workingObject
            text = object && object[fieldName] !== undefined
                ? String(Math.round(Number(object[fieldName]) * factor * 1000) / 1000) : ""
        }
        onTextEdited: {
            if (!root.resetting) {
                root.controller.setProperty(fieldName, text.trim().length ? Number(text) / factor : NaN)
                if (root.imageObject && root.controller.imageAspectLocked) {
                    if (fieldName === "width")
                        heightInput.resetValue()
                    else if (fieldName === "height")
                        widthInput.resetValue()
                }
            }
        }
    }

    Flickable {
        anchors.fill: parent
        anchors.margins: 14
        contentHeight: fields.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: fields
            width: parent.width
            spacing: 10
            enabled: root.controller.editingAllowed && root.editable

            Label {
                Layout.fillWidth: true
                text: root.controller.pendingObject ? qsTr("Thêm chữ")
                    : root.imageObject ? qsTr("Thuộc tính ảnh") : qsTr("Thuộc tính đối tượng")
                font.pixelSize: 16
                font.bold: true
                color: root.theme.ribbonTextColor
            }
            Label {
                Layout.fillWidth: true
                text: !root.displayTarget ? "" : !root.editable ? root.displayTarget.readOnlyReason
                    || qsTr("Đối tượng này chưa hỗ trợ chỉnh sửa trên web.")
                    : root.textObject
                    ? qsTr("Gõ trực tiếp trong ô trên trang. Dùng Sửa văn bản hoặc nhấp đúp chữ để sửa. Áp dụng hoặc Lưu PDF khi hoàn tất.")
                    : root.imageObject ? qsTr("Kéo ảnh để di chuyển, kéo các chấm ở viền để đổi kích thước. Lưu PDF khi hoàn tất.")
                    : qsTr("Áp dụng thay đổi, rồi Lưu để tải PDF đã sửa.")
                wrapMode: Text.Wrap
                font.pixelSize: 11
                color: "#77716b"
            }
            Label {
                Layout.fillWidth: true
                visible: root.textObject && root.displayTarget.source === true
                    && root.displayTarget.fontSubstitutionRequired === true && root.editable
                text: qsTr("Sửa nội dung, cỡ chữ hoặc màu sẽ dùng phông Liberation Sans. Chỉ di chuyển giữ nguyên phông gốc.")
                wrapMode: Text.Wrap
                font.pixelSize: 11
                color: "#8b5a2b"
            }
            Label {
                visible: root.textObject
                text: qsTr("Cỡ chữ (pt)")
                color: root.theme.ribbonTextColor
            }
            NumberInput {
                id: fontInput
                visible: root.textObject
                fieldName: "fontSize"
                Accessible.name: qsTr("Cỡ chữ")
            }
            Label {
                visible: !root.imageObject
                text: qsTr("Màu")
                color: root.theme.ribbonTextColor
            }
            TextField {
                id: colorInput
                visible: !root.imageObject
                objectName: "webEditColorInput"
                Layout.fillWidth: true
                selectByMouse: true
                maximumLength: 7
                placeholderText: "#202124"
                Accessible.name: qsTr("Màu")
                onTextEdited: {
                    if (!root.resetting)
                        root.controller.setProperty("color", text)
                }
            }
            Label {
                visible: !root.textObject && !root.imageObject
                text: qsTr("Độ dày nét (pt)")
                color: root.theme.ribbonTextColor
            }
            NumberInput {
                id: strokeInput
                visible: !root.textObject && !root.imageObject
                fieldName: "strokeWidth"
                Accessible.name: qsTr("Độ dày nét")
            }
            CheckBox {
                objectName: "webImageAspectLock"
                Layout.fillWidth: true
                visible: root.imageObject
                text: qsTr("Giữ tỉ lệ ảnh")
                checked: root.controller.imageAspectLocked
                onToggled: root.controller.imageAspectLocked = checked
            }
            RowLayout {
                Layout.fillWidth: true
                visible: root.imageObject
                Button {
                    objectName: "webImageRotateLeft"
                    Layout.fillWidth: true
                    text: qsTr("Xoay trái")
                    enabled: !root.controller.hasPendingInput
                    onClicked: root.controller.rotateImage(-1)
                }
                Button {
                    objectName: "webImageRotateRight"
                    Layout.fillWidth: true
                    text: qsTr("Xoay phải")
                    enabled: !root.controller.hasPendingInput
                    onClicked: root.controller.rotateImage(1)
                }
            }
            Button {
                objectName: "webImageReplaceButton"
                Layout.fillWidth: true
                visible: root.imageObject
                text: qsTr("Thay ảnh…")
                enabled: !root.controller.hasPendingInput
                onClicked: root.controller.replaceImage()
            }
            Label {
                text: qsTr("Vị trí và kích thước (% trang)")
                color: root.theme.ribbonTextColor
                font.pixelSize: 12
            }
            GridLayout {
                Layout.fillWidth: true
                columns: 2
                Label { text: qsTr("Trái"); color: root.theme.ribbonTextColor }
                NumberInput { id: xInput; fieldName: "x"; factor: 100; Accessible.name: qsTr("Trái") }
                Label { text: qsTr("Trên"); color: root.theme.ribbonTextColor }
                NumberInput { id: yInput; fieldName: "y"; factor: 100; Accessible.name: qsTr("Trên") }
                Label { text: qsTr("Rộng"); color: root.theme.ribbonTextColor }
                NumberInput { id: widthInput; fieldName: "width"; factor: 100; Accessible.name: qsTr("Rộng") }
                Label { text: qsTr("Cao"); color: root.theme.ribbonTextColor }
                NumberInput { id: heightInput; fieldName: "height"; factor: 100; Accessible.name: qsTr("Cao") }
            }
            Label {
                Layout.fillWidth: true
                visible: root.controller.message.length > 0
                text: root.controller.message
                wrapMode: Text.Wrap
                color: "#8b2f24"
                font.pixelSize: 12
            }
            OrangeButton {
                objectName: "webEditApplyButton"
                Layout.fillWidth: true
                theme: root.theme
                text: qsTr("Áp dụng")
                enabled: root.controller.hasPendingInput
                onClicked: root.controller.applyProperties()
            }
            RowLayout {
                Layout.fillWidth: true
                Button {
                    objectName: "webEditCancelButton"
                    Layout.fillWidth: true
                    text: qsTr("Hủy")
                    enabled: root.controller.hasPendingInput
                    onClicked: {
                        root.controller.cancelPending()
                        root.resetInputs()
                    }
                }
                Button {
                    objectName: "webEditDeleteButton"
                    Layout.fillWidth: true
                    text: qsTr("Xóa")
                    enabled: root.controller.selectedObject !== null && !root.controller.hasPendingInput
                    onClicked: root.controller.deleteSelection()
                }
            }
        }
    }
}
