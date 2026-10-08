pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import TrePDF

Rectangle {
    id: root
    objectName: "headerTextPanel"

    required property AppTheme theme
    required property HeaderTextPlacement placement
    readonly property bool inputFocused: pageRangeInput.activeFocus
        || fontFamilyBox.activeFocus || fontFamilyBox.popup.visible
        || fontSizeInput.activeFocus || fontSizeBox.popup.visible
    property int fontFamilyFilter: PDFEditController.VietnameseFonts
    property var displayedFontFamilies: []
    //% "Vietnamese"
    readonly property string vietnameseFontsLabel: qsTrId("VietnameseFonts")
    //% "English"
    readonly property string englishFontsLabel: qsTrId("EnglishFonts")
    //% "Chinese"
    readonly property string chineseFontsLabel: qsTrId("ChineseFonts")
    //% "Japanese"
    readonly property string japaneseFontsLabel: qsTrId("JapaneseFonts")
    //% "Korean"
    readonly property string koreanFontsLabel: qsTrId("KoreanFonts")
    readonly property var fontFilterOptions: [
        { value: PDFEditController.VietnameseFonts, label: vietnameseFontsLabel },
        { value: PDFEditController.EnglishFonts, label: englishFontsLabel },
        { value: PDFEditController.ChineseFonts, label: chineseFontsLabel },
        { value: PDFEditController.JapaneseFonts, label: japaneseFontsLabel },
        { value: PDFEditController.KoreanFonts, label: koreanFontsLabel }
    ]
    readonly property int selectedFontFamilyIndex: {
        const selectedKey = placement.fontFamily.toLowerCase()
        for (let index = 0; index < displayedFontFamilies.length; ++index) {
            if (displayedFontFamilies[index].toLowerCase() === selectedKey)
                return index
        }
        return -1
    }
    readonly property bool canSubmit: placement.canSubmit
        && (allPagesSelector.checked || pageRangeInput.text.trim().length > 0)

    implicitWidth: 280
    color: theme.ribbonSurface
    Keys.onEscapePressed: event => {
        if (!root.placement.submitting) {
            root.placement.cancel()
            event.accepted = true
        }
    }

    function refreshDisplayedFontFamilies() {
        displayedFontFamilies = placement.editController.headerTextFontFamilies(
            fontFamilyFilter, placement.fontBold, placement.fontItalic)
    }

    Component.onCompleted: refreshDisplayedFontFamilies()

    Connections {
        target: root.placement

        function onFontFamilyChanged() { root.refreshDisplayedFontFamilies() }
        function onFontBoldChanged() { root.refreshDisplayedFontFamilies() }
        function onFontItalicChanged() { root.refreshDisplayedFontFamilies() }
        function onFontSizeChanged() { fontSizeBox.syncCurrentSize() }
    }

    function submit() {
        fontSizeBox.commitSizeText()
        if (!canSubmit)
            return
        placement.submit(allPagesSelector.checked ? "all" : pageRangeInput.text.trim())
    }

    function openColorDialog() {
        if (!placement.active || placement.submitting)
            return
        if (colorDialogLoader.status === Loader.Ready) {
            const dialog = colorDialogLoader.item as ColorDialog
            if (dialog) {
                dialog.selectedColor = placement.textColor
                dialog.open()
            }
        } else {
            colorDialogLoader.active = true
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

    ScrollView {
        id: panelScroll
        anchors.fill: parent
        anchors.margins: 16
        contentWidth: availableWidth
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ColumnLayout {
            width: panelScroll.availableWidth
            spacing: 12

            Label {
                Layout.fillWidth: true
                //% "Header text"
                text: qsTrId("HeaderText")
                color: root.theme.ribbonTextColor
                font.pixelSize: 17
                font.weight: Font.DemiBold
                wrapMode: Text.Wrap
            }

            Label {
                Layout.fillWidth: true
                //% "Font family"
                text: qsTrId("FontFamily")
                color: root.theme.ribbonTextColor
                font.pixelSize: 12
                font.weight: Font.Medium
            }

            ComboBox {
                id: fontFamilyBox
                objectName: "headerFontFamilyComboBox"

                Layout.fillWidth: true
                model: root.displayedFontFamilies
                currentIndex: root.selectedFontFamilyIndex
                displayText: root.placement.fontFamily
                clip: true
                //% "Font family"
                Accessible.name: qsTrId("FontFamily")
                onActivated: root.placement.setFontFamily(
                    fontFamilyBox.currentText)

                popup: Popup {
                    id: fontFamilyPopup

                    x: Math.min(0, fontFamilyBox.width - width)
                    y: fontFamilyBox.height
                    width: Math.max(fontFamilyBox.width, 300)
                    implicitHeight: contentItem.implicitHeight
                        + topPadding + bottomPadding
                    height: Math.min(
                        implicitHeight,
                        420,
                        Math.max(
                            80,
                            root.height
                                - topMargin - bottomMargin))
                    topMargin: 6
                    bottomMargin: 6
                    padding: 1
                    palette: fontFamilyBox.palette
                    closePolicy: Popup.CloseOnEscape
                        | Popup.CloseOnPressOutside
                    onAboutToShow:
                        root.refreshDisplayedFontFamilies()

                    contentItem: ColumnLayout {
                        spacing: 0

                        GridLayout {
                            Layout.fillWidth: true
                            Layout.margins: 6
                            columns: 3
                            columnSpacing: 4
                            rowSpacing: 4

                            Repeater {
                                model: root.fontFilterOptions.length

                                delegate: Button {
                                    id: fontFilterButton

                                    required property int index
                                    readonly property var option:
                                        root.fontFilterOptions[index]

                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    Layout.preferredWidth: 1
                                    Layout.preferredHeight: 28
                                    checkable: true
                                    autoExclusive: true
                                    checked: root.fontFamilyFilter
                                        === option.value
                                    text: option.label
                                    leftPadding: 6
                                    rightPadding: 6
                                    topPadding: 3
                                    bottomPadding: 3
                                    hoverEnabled: true
                                    onClicked: {
                                        root.fontFamilyFilter =
                                            option.value
                                        root.refreshDisplayedFontFamilies()
                                        fontList.positionViewAtBeginning()
                                        fontFamilyBox.forceActiveFocus()
                                    }

                                    contentItem: Label {
                                        text: fontFilterButton.text
                                        color: fontFilterButton.checked
                                            ? root.theme.foxitDarkOrange
                                            : root.theme.ribbonTextColor
                                        font.pixelSize: 10
                                        horizontalAlignment:
                                            Text.AlignHCenter
                                        verticalAlignment:
                                            Text.AlignVCenter
                                        elide: Text.ElideRight
                                    }

                                    background: Rectangle {
                                        radius: 5
                                        color: fontFilterButton.down
                                            ? "#f5dfcf"
                                            : fontFilterButton.checked
                                                || fontFilterButton.hovered
                                                ? root.theme.foxitPaleOrange
                                                : "transparent"
                                        border.width:
                                            fontFilterButton.checked
                                                ? 1 : 0
                                        border.color:
                                            root.theme.foxitBorderOrange
                                    }
                                }
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 1
                            color: root.theme.ribbonBorder
                        }

                        Item {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.minimumHeight: 80
                            Layout.preferredHeight: Math.min(
                                300,
                                Math.max(80, fontList.contentHeight))

                            ListView {
                                id: fontList

                                anchors.fill: parent
                                clip: true
                                reuseItems: true
                                model: fontFamilyBox.delegateModel
                                currentIndex:
                                    fontFamilyBox.highlightedIndex
                                highlightMoveDuration: 0
                                ScrollIndicator.vertical:
                                    ScrollIndicator {}
                            }

                            Label {
                                anchors.fill: parent
                                anchors.margins: 12
                                visible: fontFamilyBox.count === 0
                                //% "No supported fonts match this filter."
                                text: qsTrId("NoFontsMatchFilter")
                                color: "#747176"
                                font.pixelSize: 11
                                horizontalAlignment:
                                    Text.AlignHCenter
                                verticalAlignment:
                                    Text.AlignVCenter
                                wrapMode: Text.Wrap
                            }
                        }
                    }

                    background: Rectangle {
                        color: fontFamilyBox.palette.window
                        border.width: 1
                        border.color: root.theme.ribbonBorder
                    }
                }

                background: Rectangle {
                    radius: 6
                    color: fontFamilyBox.palette.button
                    border.width: 1
                    border.color: fontFamilyBox.activeFocus
                        ? root.theme.foxitBorderOrange
                        : root.theme.ribbonBorder
                }

            }

            Label {
                Layout.fillWidth: true
                //% "Font size"
                text: qsTrId("FontSize")
                color: root.theme.ribbonTextColor
                font.pixelSize: 12
                font.weight: Font.Medium
            }

            ComboBox {
                id: fontSizeBox
                objectName: "headerFontSizeBox"

                property bool pending: false

                function syncCurrentSize() {
                    pending = false
                    const sizeText = String(root.placement.fontSize)
                    currentIndex = model.indexOf(sizeText)
                    editText = sizeText
                }

                function commitSizeText() {
                    const entered = editText.trim()
                    const points = Number(entered)
                    const apply = pending && root.placement.active
                        && /^[0-9]+$/.test(entered) && Number.isInteger(points)
                        && points >= 1 && points <= 512
                    pending = false
                    if (apply)
                        root.placement.setFontSize(points)
                    syncCurrentSize()
                }

                Layout.fillWidth: true
                editable: true
                model: ["8", "9", "10", "11", "12", "14", "16", "18", "24", "36", "48", "72"]
                currentIndex: -1
                inputMethodHints: Qt.ImhDigitsOnly
                //% "Font size"
                Accessible.name: qsTrId("FontSize")
                Component.onCompleted: syncCurrentSize()
                onActivated: index => {
                    pending = true
                    editText = textAt(index)
                    commitSizeText()
                }
                onAccepted: commitSizeText()

                contentItem: TextField {
                    id: fontSizeInput
                    objectName: "headerFontSizeInput"
                    leftPadding: 12
                    rightPadding: activeFocus ? 3 : 1
                    topPadding: 6 - fontSizeBox.padding
                    bottomPadding: 6 - fontSizeBox.padding
                    text: fontSizeBox.editText
                    font: fontSizeBox.font
                    enabled: fontSizeBox.editable
                    autoScroll: true
                    readOnly: fontSizeBox.down
                    inputMethodHints: fontSizeBox.inputMethodHints
                    validator: fontSizeBox.validator
                    selectByMouse: true
                    color: fontSizeBox.palette.text
                    selectionColor: fontSizeBox.palette.highlight
                    selectedTextColor: fontSizeBox.palette.highlightedText
                    verticalAlignment: Text.AlignVCenter
                    //% "Font size"
                    Accessible.name: qsTrId("FontSize")
                    onTextEdited: fontSizeBox.pending = true
                    onActiveFocusChanged: {
                        if (!activeFocus && !fontSizeBox.popup.visible)
                            fontSizeBox.commitSizeText()
                    }
                    Keys.onPressed: event => {
                        if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                                && !fontSizeBox.popup.visible) {
                            fontSizeBox.commitSizeText()
                            event.accepted = true
                        } else if (event.key === Qt.Key_Escape) {
                            fontSizeBox.syncCurrentSize()
                            event.accepted = true
                        }
                    }
                    background: Rectangle {
                        radius: 6
                        color: fontSizeBox.palette.base
                        border.width: fontSizeInput.activeFocus ? 2 : 1
                        border.color: fontSizeInput.activeFocus
                            ? root.theme.foxitBorderOrange : root.theme.ribbonBorder
                    }
                }

                validator: IntValidator {
                    bottom: 1
                    top: 512
                }

                background: Rectangle {
                    radius: 6
                    color: fontSizeBox.palette.button
                    border.width: 1
                    border.color: fontSizeBox.activeFocus
                        ? root.theme.foxitBorderOrange : root.theme.ribbonBorder
                }
            }

            Label {
                Layout.fillWidth: true
                Layout.topMargin: 4
                //% "Style"
                text: qsTrId("FontStyle")
                color: root.theme.ribbonTextColor
                font.pixelSize: 12
                font.weight: Font.Medium
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 6

                ToolButton {
                    id: boldButton
                    objectName: "headerFontBoldButton"

                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: 58
                    checkable: true
                    focusPolicy: Qt.TabFocus
                    checked: root.placement.fontBold
                    display: AbstractButton.TextUnderIcon
                    spacing: 3
                    //% "Bold"
                    text: qsTrId("Bold")
                    icon.source: "../assets/icons/bold-text.svg"
                    icon.width: 20
                    icon.height: 20
                    icon.color: !enabled
                        ? "#aaa8a6"
                        : checked
                            ? root.theme.foxitDarkOrange
                            : "transparent"
                    font.pixelSize: 10
                    onClicked: {
                        root.placement.setFontBold(!root.placement.fontBold)
                        boldButton.checked = Qt.binding(() => root.placement.fontBold)
                    }

                    background: Rectangle {
                        radius: 4
                        color: !boldButton.enabled
                            ? "transparent"
                            : boldButton.down
                                ? "#f5dfcf"
                                : boldButton.checked || boldButton.hovered
                                    ? root.theme.foxitPaleOrange
                                    : "transparent"
                        border.width: boldButton.checked ? 1 : 0
                        border.color: root.theme.foxitBorderOrange
                    }
                }

                ToolButton {
                    id: italicButton
                    objectName: "headerFontItalicButton"

                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: 58
                    checkable: true
                    focusPolicy: Qt.TabFocus
                    checked: root.placement.fontItalic
                    display: AbstractButton.TextUnderIcon
                    spacing: 3
                    //% "Italic"
                    text: qsTrId("Italic")
                    icon.source: "../assets/icons/italic-font.svg"
                    icon.width: 20
                    icon.height: 20
                    icon.color: !enabled
                        ? "#aaa8a6"
                        : checked
                            ? root.theme.foxitDarkOrange
                            : "transparent"
                    font.pixelSize: 10
                    onClicked: {
                        root.placement.setFontItalic(!root.placement.fontItalic)
                        italicButton.checked = Qt.binding(() => root.placement.fontItalic)
                    }

                    background: Rectangle {
                        radius: 4
                        color: !italicButton.enabled
                            ? "transparent"
                            : italicButton.down
                                ? "#f5dfcf"
                                : italicButton.checked || italicButton.hovered
                                    ? root.theme.foxitPaleOrange
                                    : "transparent"
                        border.width: italicButton.checked ? 1 : 0
                        border.color: root.theme.foxitBorderOrange
                    }
                }

                ToolButton {
                    id: underlineButton
                    objectName: "headerFontUnderlineButton"

                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: 58
                    checkable: true
                    focusPolicy: Qt.TabFocus
                    checked: root.placement.fontUnderline
                    display: AbstractButton.TextUnderIcon
                    spacing: 3
                    //% "Underline"
                    text: qsTrId("Underline")
                    icon.source: "../assets/icons/underline-text.svg"
                    icon.width: 20
                    icon.height: 20
                    icon.color: !enabled
                        ? "#aaa8a6"
                        : checked
                            ? root.theme.foxitDarkOrange
                            : "transparent"
                    font.pixelSize: 10
                    onClicked: {
                        root.placement.setFontUnderline(!root.placement.fontUnderline)
                        underlineButton.checked = Qt.binding(() => root.placement.fontUnderline)
                    }

                    background: Rectangle {
                        radius: 4
                        color: !underlineButton.enabled
                            ? "transparent"
                            : underlineButton.down
                                ? "#f5dfcf"
                                : underlineButton.checked || underlineButton.hovered
                                    ? root.theme.foxitPaleOrange
                                    : "transparent"
                        border.width: underlineButton.checked ? 1 : 0
                        border.color: root.theme.foxitBorderOrange
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                Layout.topMargin: 4
                //% "Font color"
                text: qsTrId("FontColor")
                color: root.theme.ribbonTextColor
                font.pixelSize: 12
                font.weight: Font.Medium
            }

            Button {
                id: colorButton
                objectName: "headerFontColorButton"
                //% "Font color"
                Accessible.name: qsTrId("FontColor")

                Layout.fillWidth: true
                Layout.preferredHeight: 38
                onClicked: root.openColorDialog()

                contentItem: RowLayout {
                    spacing: 10

                    Rectangle {
                        Layout.preferredWidth: 22
                        Layout.preferredHeight: 22
                        radius: 3
                        color: root.placement.textColor
                        border.width: 1
                        border.color: "#8a8785"
                    }

                    Label {
                        Layout.fillWidth: true
                        text: root.placement.textColor.toString().toUpperCase()
                        color: colorButton.enabled
                            ? root.theme.ribbonTextColor
                            : "#aaa8a6"
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                background: Rectangle {
                    radius: 4
                    color: !colorButton.enabled
                        ? "#f4f1ef"
                        : colorButton.down
                            ? "#f5dfcf"
                            : colorButton.hovered
                                ? root.theme.foxitPaleOrange
                                : "#ffffff"
                    border.width: 1
                    border.color: colorButton.enabled
                        ? root.theme.ribbonBorder
                        : "#ddd9d6"
                }
            }

            Label {
                Layout.fillWidth: true
                //% "Apply to"
                text: qsTrId("HeaderApplyTo")
                color: root.theme.ribbonTextColor
                font.weight: Font.DemiBold
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                ButtonGroup { id: pageSelectionGroup }

                RadioButton {
                    id: allPagesSelector
                    objectName: "headerAllPagesSelector"
                    Layout.fillWidth: true
                    ButtonGroup.group: pageSelectionGroup
                    checked: true
                    //% "All pages"
                    text: qsTrId("HeaderAllPages")
                    palette.highlight: root.theme.foxitDarkOrange
                    onToggled: root.placement.errorText = ""
                }

                RadioButton {
                    id: customPagesSelector
                    objectName: "headerCustomPagesSelector"
                    Layout.fillWidth: true
                    ButtonGroup.group: pageSelectionGroup
                    //% "Selected pages"
                    text: qsTrId("HeaderSelectedPages")
                    palette.highlight: root.theme.foxitDarkOrange
                    onToggled: {
                        root.placement.errorText = ""
                        if (checked)
                            pageRangeInput.forceActiveFocus()
                    }
                }

                TextField {
                    id: pageRangeInput
                    objectName: "headerPageRangeInput"
                    Layout.fillWidth: true
                    enabled: customPagesSelector.checked
                    maximumLength: 2048
                    selectByMouse: true
                    //% "For example: 1-3, 5"
                    placeholderText: qsTrId("HeaderPageRangePlaceholder")
                    //% "Selected pages"
                    Accessible.name: qsTrId("HeaderSelectedPages")
                    onTextChanged: root.placement.errorText = ""
                    onAccepted: root.submit()
                }
            }

            Label {
                Layout.fillWidth: true
                //% "Apply creates a draft. Save or Save As writes the text to the PDF."
                text: qsTrId("HeaderTextDraftHint")
                color: root.theme.ribbonTextColor
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }

            Label {
                objectName: "headerTextError"
                Layout.fillWidth: true
                visible: root.placement.errorText.length > 0
                text: root.placement.errorText
                color: "#b3261e"
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Button {
                    objectName: "headerTextCancelButton"
                    Layout.fillWidth: true
                    enabled: !root.placement.submitting
                    //% "Cancel"
                    text: qsTrId("Cancel")
                    onClicked: root.placement.cancel()
                }

                Button {
                    objectName: "headerTextApplyButton"
                    Layout.fillWidth: true
                    enabled: root.canSubmit
                    highlighted: true
                    //% "Apply"
                    text: qsTrId("HeaderApply")
                    onClicked: root.submit()
                }
            }
        }
    }

    Loader {
        id: colorDialogLoader
        active: false
        onLoaded: {
            const dialog = item as ColorDialog
            if (dialog && root.placement.active) {
                dialog.selectedColor = root.placement.textColor
                dialog.open()
            }
        }
        sourceComponent: ColorDialog {
            objectName: "headerFontColorDialog"
            //% "Font color"
            title: qsTrId("FontColor")
            onAccepted: {
                if (root.placement.active && !root.placement.submitting)
                    root.placement.setTextColor(selectedColor)
                colorDialogLoader.active = false
            }
            onRejected: colorDialogLoader.active = false
        }
    }
}
