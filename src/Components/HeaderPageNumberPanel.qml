pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TrePDF

Rectangle {
    id: root
    objectName: "headerPageNumberPanel"

    required property AppTheme theme
    required property HeaderPageNumberPlacement placement
    readonly property bool inputFocused: firstPageInput.contentItem.activeFocus
        || lastPageInput.contentItem.activeFocus || romanStartInput.contentItem.activeFocus
        || decimalStartInput.contentItem.activeFocus

    implicitWidth: 280
    color: theme.ribbonSurface
    Keys.onEscapePressed: event => {
        if (!root.placement.submitting) {
            root.placement.cancel()
            event.accepted = true
        }
    }

    component PageInput: SpinBox {
        required property string accessibleLabel
        property bool initialized: false
        Component.onCompleted: initialized = true
        Layout.fillWidth: true
        from: 1
        to: Math.max(1, root.placement.pageCount)
        editable: true
        live: true
        enabled: !root.placement.submitting
        Accessible.name: accessibleLabel
        palette.highlight: root.theme.foxitOrange
    }

    component FieldLabel: Label {
        Layout.fillWidth: true
        color: root.theme.ribbonTextColor
        wrapMode: Text.Wrap
    }

    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 1
        color: root.theme.ribbonBorder
        Accessible.ignored: true
    }

    Connections {
        target: root.placement
        function onFocusRequested() { firstPageInput.forceActiveFocus() }
    }

    ScrollView {
        id: panelScroll
        anchors.fill: parent
        anchors.margins: 16
        contentWidth: availableWidth
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ColumnLayout {
            width: panelScroll.availableWidth
            spacing: 10

            FieldLabel {
                //% "Page numbers"
                text: qsTrId("HeaderPageNumbers")
                font.pixelSize: 17
                font.weight: Font.DemiBold
            }
            FieldLabel {
                //% "All page positions below refer to pages in the PDF, starting at 1."
                text: qsTrId("HeaderPageNumbersPdfPagesHint")
                font.pixelSize: 11
            }
            FieldLabel {
                id: firstPageLabel
                //% "Start at PDF page"
                text: qsTrId("HeaderPageNumbersFirstPage")
            }
            PageInput {
                id: firstPageInput
                objectName: "headerPageNumberFirstPage"
                accessibleLabel: firstPageLabel.text
                value: root.placement.firstPage
                onValueChanged: {
                    if (firstPageInput.initialized)
                        root.placement.firstPage = value
                }
            }
            FieldLabel {
                id: lastPageLabel
                //% "Stop at PDF page (inclusive)"
                text: qsTrId("HeaderPageNumbersLastPage")
            }
            PageInput {
                id: lastPageInput
                objectName: "headerPageNumberLastPage"
                accessibleLabel: lastPageLabel.text
                value: root.placement.lastPage
                onValueChanged: {
                    if (lastPageInput.initialized)
                        root.placement.lastPage = value
                }
            }
            CheckBox {
                id: romanSelector
                objectName: "headerPageNumberRomanEnabled"
                Layout.fillWidth: true
                //% "Roman numerals (i, ii, iii)"
                text: qsTrId("HeaderPageNumbersRoman")
                checked: root.placement.romanEnabled
                palette.highlight: root.theme.foxitOrange
                onCheckedChanged: root.placement.romanEnabled = checked
            }
            FieldLabel {
                id: romanStartLabel
                //% "Begin i at PDF page"
                text: qsTrId("HeaderPageNumbersRomanStart")
                enabled: root.placement.romanEnabled
            }
            PageInput {
                id: romanStartInput
                objectName: "headerPageNumberRomanStart"
                accessibleLabel: romanStartLabel.text
                enabled: root.placement.romanEnabled && !root.placement.submitting
                value: root.placement.romanStartPage
                onValueChanged: {
                    if (romanStartInput.initialized)
                        root.placement.romanStartPage = value
                }
            }
            CheckBox {
                id: decimalSelector
                objectName: "headerPageNumberDecimalEnabled"
                Layout.fillWidth: true
                //% "Arabic numbers (1, 2, 3)"
                text: qsTrId("HeaderPageNumbersDecimal")
                checked: root.placement.decimalEnabled
                palette.highlight: root.theme.foxitOrange
                onCheckedChanged: root.placement.decimalEnabled = checked
            }
            FieldLabel {
                id: decimalStartLabel
                //% "Begin 1 at PDF page"
                text: qsTrId("HeaderPageNumbersDecimalStart")
                enabled: root.placement.decimalEnabled
            }
            PageInput {
                id: decimalStartInput
                objectName: "headerPageNumberDecimalStart"
                accessibleLabel: decimalStartLabel.text
                enabled: root.placement.decimalEnabled && !root.placement.submitting
                value: root.placement.decimalStartPage
                onValueChanged: {
                    if (decimalStartInput.initialized)
                        root.placement.decimalStartPage = value
                }
            }
            FieldLabel {
                //% "Arabic numbers restart at 1. Roman numerals stop before that page. Pages before the first numbering start stay blank."
                text: qsTrId("HeaderPageNumbersSequenceHint")
                font.pixelSize: 11
            }
            FieldLabel {
                //% "Apply creates a draft. Use Save or Save As to write it into the PDF."
                text: qsTrId("HeaderDraftSaveHint")
                font.pixelSize: 11
            }
            FieldLabel {
                objectName: "headerPageNumberError"
                text: root.placement.validationError || root.placement.errorText
                visible: text.length > 0
                color: "#b3261e"
                font.pixelSize: 11
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Button {
                    objectName: "headerPageNumberCancelButton"
                    Layout.fillWidth: true
                    enabled: !root.placement.submitting
                    //% "Cancel"
                    text: qsTrId("Cancel")
                    onClicked: root.placement.cancel()
                }
                Button {
                    objectName: "headerPageNumberApplyButton"
                    Layout.fillWidth: true
                    enabled: root.placement.canSubmit
                    highlighted: true
                    //% "Apply"
                    text: qsTrId("HeaderApply")
                    onClicked: root.placement.submit()
                }
            }
        }
    }
}
