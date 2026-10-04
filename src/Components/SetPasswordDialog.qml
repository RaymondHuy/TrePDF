pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TrePDF
import "ios"

Dialog {
    id: root
    objectName: "setPasswordDialog"

    required property AppTheme theme
    required property PDFEditController editController
    property string errorText: ""
    readonly property bool changingPassword: editController && editController.document
        && editController.document.passwordProtected
    readonly property bool canSubmit: editController
        && editController.canSetOpenPassword && !editController.saving
        && passwordInput.text.length > 0 && confirmationInput.text.length > 0

    parent: Overlay.overlay
    anchors.centerIn: dialogMetrics.active ? undefined : parent
    x: dialogMetrics.active
        ? dialogMetrics.leftInset + (dialogMetrics.availableWidth - width) / 2 : 0
    y: dialogMetrics.active
        ? dialogMetrics.topInset + Math.max(8, (dialogMetrics.availableHeight - height) / 2) : 0
    width: Math.min(440, parent ? dialogMetrics.availableWidth - 32 : 440)
    height: dialogMetrics.active
        ? Math.min(implicitHeight, dialogMetrics.availableHeight - 16) : implicitHeight
    padding: 20
    modal: true
    focus: true
    popupType: Popup.Item
    closePolicy: Popup.CloseOnEscape
    title: changingPassword
        //% "Change PDF password"
        ? qsTrId("ChangePdfPasswordTitle")
        //% "Set PDF password"
        : qsTrId("SetPdfPasswordTitle")

    DialogMetrics {
        id: dialogMetrics
        overlay: root.parent
    }

    function submit() {
        if (!canSubmit)
            return
        if (passwordInput.text !== confirmationInput.text) {
            //% "The passwords do not match. Please try again."
            errorText = qsTrId("PdfPasswordsDoNotMatch")
            confirmationInput.forceActiveFocus()
            confirmationInput.selectAll()
            return
        }
        errorText = ""
        if (editController.setOpenPassword(passwordInput.text))
            accept()
    }

    onOpened: passwordInput.forceActiveFocus()
    onAboutToHide: {
        passwordInput.clear()
        confirmationInput.clear()
        showPassword.checked = false
    }

    Connections {
        target: root.editController

        function onStatusMessage(message) {
            root.errorText = message
        }
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
        id: passwordScroll
        contentWidth: availableWidth
        implicitHeight: passwordFields.implicitHeight
        clip: true
        contentData: [passwordFields]
    }

    readonly property ColumnLayout _passwordFields: ColumnLayout {
        id: passwordFields
        width: passwordScroll.availableWidth
        spacing: 12

        Label {
            Layout.fillWidth: true
            text: root.changingPassword
                //% "The new password will replace the current opening password after you Save or Save As."
                ? qsTrId("ChangePdfPasswordSaveHint")
                //% "A password will be required to open this PDF after you Save or Save As."
                : qsTrId("SetPdfPasswordSaveHint")
            color: root.theme.ribbonTextColor
            wrapMode: Text.Wrap
        }
        TextField {
            id: passwordInput
            objectName: "setPdfPasswordInput"
            Layout.fillWidth: true
            echoMode: showPassword.checked ? TextInput.Normal : TextInput.Password
            inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                | Qt.ImhNoAutoUppercase
            // Keep an extra character so oversized input reaches validation
            // instead of being silently truncated into a valid password.
            maximumLength: 128
            selectByMouse: true
            //% "New password"
            placeholderText: qsTrId("NewPdfPassword")
            Accessible.name: placeholderText
            onTextEdited: root.errorText = ""
            onAccepted: confirmationInput.forceActiveFocus()
        }
        TextField {
            id: confirmationInput
            objectName: "confirmPdfPasswordInput"
            Layout.fillWidth: true
            echoMode: showPassword.checked ? TextInput.Normal : TextInput.Password
            inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                | Qt.ImhNoAutoUppercase
            maximumLength: passwordInput.maximumLength
            selectByMouse: true
            //% "Confirm password"
            placeholderText: qsTrId("ConfirmPdfPassword")
            Accessible.name: placeholderText
            onTextEdited: root.errorText = ""
            onAccepted: root.submit()
        }
        CheckBox {
            id: showPassword
            objectName: "showPdfPasswordCheckBox"
            //% "Show password"
            text: qsTrId("ShowPdfPassword")
        }
        Label {
            objectName: "setPdfPasswordError"
            Layout.fillWidth: true
            visible: root.errorText.length > 0
            text: root.errorText
            textFormat: Text.PlainText
            color: "#b3261e"
            wrapMode: Text.Wrap
            Accessible.role: Accessible.AlertMessage
        }
    }

    footer: DialogButtonBox {
        padding: 16
        spacing: 8

        Button {
            objectName: "cancelSetPdfPasswordButton"
            //% "Cancel"
            text: qsTrId("Cancel")
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: root.reject()
        }
        OrangeButton {
            objectName: "applyPdfPasswordButton"
            theme: root.theme
            //% "Apply"
            text: qsTrId("ApplyPdfPassword")
            enabled: root.canSubmit
            DialogButtonBox.buttonRole: DialogButtonBox.ApplyRole
            onClicked: root.submit()
        }
    }
}
