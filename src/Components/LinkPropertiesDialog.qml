pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import TrePDF

Dialog {
    id: root

    required property AppTheme theme
    required property int pageCount
    required property var initialProperties
    required property bool creating

    property color borderColor: initialProperties.color ?? "#000000"
    property string errorText: ""
    readonly property real initialLineWidth: Number(initialProperties.width ?? 1)
    readonly property var lineWidths: {
        const values = [1, 2, 3]
        if (!values.includes(initialLineWidth))
            values.push(initialLineWidth)
        return values.map(value => ({
            value: value,
            //% "%1 pt"
            text: qsTrId("LinkLineWidthPoints").arg(value)
        }))
    }

    signal submitRequested(var properties)

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(480, parent ? parent.width - 32 : 480)
    height: Math.min(implicitHeight, parent ? parent.height - 32 : implicitHeight)
    padding: 20
    modal: true
    focus: true
    popupType: Popup.Item
    closePolicy: Popup.CloseOnEscape
    title: creating
        //% "Create link"
        ? qsTrId("CreateManualLink")
        //% "Link properties"
        : qsTrId("ManualLinkProperties")

    function submit() {
        const isWeb = actionSelector.currentIndex === 0
        if (isWeb && urlField.text.trim().length === 0) {
            //% "Enter a website URL."
            errorText = qsTrId("ManualLinkUrlRequired")
            urlField.forceActiveFocus()
            return
        }
        errorText = ""
        submitRequested({
            url: isWeb ? urlField.text.trim() : "",
            targetPage: isWeb ? -1 : pageSelector.value - 1,
            visible: typeSelector.currentIndex === 1,
            color: borderColor,
            width: widthSelector.currentValue,
            lineStyle: styleSelector.currentIndex,
            highlight: highlightSelector.currentIndex
        })
    }

    function showSaveError() {
        //% "The link could not be applied. Check the URL or destination page and try again. Save newly inserted pages first."
        errorText = qsTrId("ManualLinkPropertiesInvalid")
    }

    onOpened: {
        if (actionSelector.currentIndex === 0)
            urlField.forceActiveFocus()
        else
            pageSelector.forceActiveFocus()
    }
    onAboutToHide: {
        if (colorDialogLoader.status === Loader.Ready) {
            const dialog = colorDialogLoader.item as ColorDialog
            if (dialog)
                dialog.close()
        }
        colorDialogLoader.active = false
    }

    background: Rectangle {
        color: root.theme.ribbonSurface
        border.color: root.theme.ribbonBorder
        radius: 10
    }

    header: Label {
        text: root.title
        padding: 20
        bottomPadding: 0
        font.pixelSize: 18
        font.bold: true
        color: root.theme.ribbonTextColor
    }

    contentItem: ScrollView {
        id: scrollView

        implicitHeight: fields.implicitHeight
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            id: fields

            width: scrollView.availableWidth
            spacing: 16

            GroupBox {
                Layout.fillWidth: true
                //% "Link appearance"
                title: qsTrId("ManualLinkAppearance")

                GridLayout {
                    anchors.fill: parent
                    columns: 2
                    columnSpacing: 14
                    rowSpacing: 10

                    Label {
                        //% "Link type"
                        text: qsTrId("ManualLinkType")
                    }
                    ComboBox {
                        id: typeSelector

                        Layout.fillWidth: true
                        model: [
                            //% "Invisible rectangle"
                            qsTrId("ManualLinkInvisibleRectangle"),
                            //% "Visible rectangle"
                            qsTrId("ManualLinkVisibleRectangle")
                        ]
                        currentIndex: root.initialProperties.visible ? 1 : 0
                        //% "Link type"
                        Accessible.name: qsTrId("ManualLinkType")
                    }
                    Label {
                        //% "Highlight style"
                        text: qsTrId("ManualLinkHighlightStyle")
                    }
                    ComboBox {
                        id: highlightSelector

                        Layout.fillWidth: true
                        model: [
                            //% "None"
                            qsTrId("ManualLinkHighlightNone"),
                            //% "Invert"
                            qsTrId("ManualLinkHighlightInvert"),
                            //% "Outline"
                            qsTrId("ManualLinkHighlightOutline"),
                            //% "Inset"
                            qsTrId("ManualLinkHighlightInset")
                        ]
                        currentIndex: Number(root.initialProperties.highlight ?? 1)
                        //% "Highlight style"
                        Accessible.name: qsTrId("ManualLinkHighlightStyle")
                    }
                    Label {
                        //% "Line thickness"
                        text: qsTrId("ManualLinkLineThickness")
                        enabled: typeSelector.currentIndex === 1
                    }
                    ComboBox {
                        id: widthSelector

                        Layout.fillWidth: true
                        enabled: typeSelector.currentIndex === 1
                        model: root.lineWidths
                        textRole: "text"
                        valueRole: "value"
                        currentIndex: root.lineWidths.findIndex(
                            option => option.value === root.initialLineWidth)
                        //% "Line thickness"
                        Accessible.name: qsTrId("ManualLinkLineThickness")
                    }
                    Label {
                        //% "Line style"
                        text: qsTrId("ManualLinkLineStyle")
                        enabled: typeSelector.currentIndex === 1
                    }
                    ComboBox {
                        id: styleSelector

                        Layout.fillWidth: true
                        enabled: typeSelector.currentIndex === 1
                        model: [
                            //% "Solid"
                            qsTrId("ManualLinkSolid"),
                            //% "Dashed"
                            qsTrId("ManualLinkDashed"),
                            //% "Underline"
                            qsTrId("ManualLinkUnderline")
                        ]
                        currentIndex: Number(root.initialProperties.lineStyle ?? 0)
                        //% "Line style"
                        Accessible.name: qsTrId("ManualLinkLineStyle")
                    }
                    Label {
                        //% "Color"
                        text: qsTrId("ManualLinkColor")
                        enabled: typeSelector.currentIndex === 1
                    }
                    Button {
                        id: colorButton

                        Layout.fillWidth: true
                        enabled: typeSelector.currentIndex === 1
                        onClicked: colorDialogLoader.active = true
                        //% "Link border color"
                        Accessible.name: qsTrId("ManualLinkBorderColor")

                        contentItem: RowLayout {
                            spacing: 8
                            Rectangle {
                                Layout.preferredWidth: 20
                                Layout.preferredHeight: 16
                                color: root.borderColor
                                border.color: root.theme.ribbonBorder
                                Accessible.ignored: true
                            }
                            Label {
                                Layout.fillWidth: true
                                text: root.borderColor.toString().toUpperCase()
                                color: colorButton.enabled
                                    ? root.theme.ribbonTextColor : "#8b8b8b"
                            }
                        }
                    }
                }
            }

            GroupBox {
                Layout.fillWidth: true
                //% "Link action"
                title: qsTrId("ManualLinkAction")

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 10

                    ComboBox {
                        id: actionSelector

                        Layout.fillWidth: true
                        model: [
                            //% "Open a web page"
                            qsTrId("ManualLinkOpenWebPage"),
                            //% "Go to a page in this document"
                            qsTrId("ManualLinkGoToPage")
                        ]
                        currentIndex: Number(root.initialProperties.targetPage ?? -1) >= 0
                            ? 1 : 0
                        //% "Link action"
                        Accessible.name: qsTrId("ManualLinkAction")
                    }
                    TextField {
                        id: urlField

                        Layout.fillWidth: true
                        visible: actionSelector.currentIndex === 0
                        text: root.initialProperties.url ?? ""
                        //% "https://example.com"
                        placeholderText: qsTrId("ManualLinkUrlPlaceholder")
                        //% "Website URL"
                        Accessible.name: qsTrId("ManualLinkWebsiteUrl")
                        selectByMouse: true
                        inputMethodHints: Qt.ImhUrlCharactersOnly
                        onAccepted: root.submit()
                        onTextEdited: root.errorText = ""
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        visible: actionSelector.currentIndex === 1
                        spacing: 10

                        Label {
                            //% "Page"
                            text: qsTrId("ManualLinkPage")
                        }
                        SpinBox {
                            id: pageSelector

                            Layout.fillWidth: true
                            from: 1
                            to: Math.max(1, root.pageCount)
                            value: Math.max(1,
                                Number(root.initialProperties.targetPage ?? 0) + 1)
                            editable: true
                            //% "Destination page"
                            Accessible.name: qsTrId("ManualLinkDestinationPage")
                        }
                        Label {
                            //% "of %1"
                            text: qsTrId("ManualLinkPageCount").arg(root.pageCount)
                        }
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                visible: root.errorText.length > 0
                text: root.errorText
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
            //% "Cancel"
            text: qsTrId("Cancel")
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: root.reject()
        }
        Button {
            text: root.creating
                //% "Create link"
                ? qsTrId("CreateManualLink")
                //% "Apply"
                : qsTrId("ApplyManualLink")
            highlighted: true
            DialogButtonBox.buttonRole: DialogButtonBox.ApplyRole
            onClicked: root.submit()
        }
    }

    Loader {
        id: colorDialogLoader

        active: false
        sourceComponent: ColorDialog {
            //% "Link border color"
            title: qsTrId("ManualLinkBorderColor")
            onAccepted: {
                root.borderColor = Qt.rgba(selectedColor.r,
                    selectedColor.g, selectedColor.b, 1)
                colorDialogLoader.active = false
            }
            onRejected: colorDialogLoader.active = false
        }
        onLoaded: {
            if (status === Loader.Ready) {
                const dialog = item as ColorDialog
                if (dialog) {
                    dialog.selectedColor = root.borderColor
                    dialog.open()
                }
            }
        }
    }
}
