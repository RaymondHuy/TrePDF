pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import TrePDF

Rectangle {
    id: root

    required property AppTheme theme
    required property bool collapsed
    required property var availableFontFamilies
    required property var fontFamiliesForFilter
    required property string selectedFontFamily
    required property real selectedLetterSpacing
    required property real selectedWordSpacing
    property real selectedLineSpacing: 1
    property real selectedParagraphSpaceBefore: 0
    property real selectedParagraphSpaceAfter: 0
    required property real selectedFontSize
    required property var formattingSession
    required property string formattingContextKey
    required property bool selectedFontBold
    required property bool selectedFontItalic
    required property bool selectedFontUnderline
    required property color selectedColor
    required property int selectedParagraphAlignment
    property bool formattingEnabled: false
    readonly property bool fontSizeEditPending: fontSizeEditState.pending
        || spacingEditState.letterPending || spacingEditState.wordPending
        || spacingEditState.linePending || spacingEditState.beforePending
        || spacingEditState.afterPending
    //% "Character spacing"
    readonly property string letterSpacingLabel: qsTrId("LetterSpacing")
    //% "Word spacing"
    readonly property string wordSpacingLabel: qsTrId("WordSpacing")
    //% "Line spacing"
    readonly property string lineSpacingLabel: qsTrId("LineSpacing")
    //% "Before paragraph (pt)"
    readonly property string paragraphSpaceBeforeLabel: qsTrId("ParagraphSpaceBeforePoints")
    //% "After paragraph (pt)"
    readonly property string paragraphSpaceAfterLabel: qsTrId("ParagraphSpaceAfterPoints")

    property var displayedFontFamilies: availableFontFamilies
    property int fontFamilyFilter: PDFEditController.VietnameseFonts
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
    //% "Align left"
    readonly property string alignLeftLabel: qsTrId("AlignLeft")
    //% "Align right"
    readonly property string alignRightLabel: qsTrId("AlignRight")
    //% "Align center"
    readonly property string alignCenterLabel: qsTrId("AlignCenter")
    //% "Justify"
    readonly property string justifyLabel: qsTrId("Justify")
    readonly property var fontFilterOptions: [
        { value: PDFEditController.VietnameseFonts,
          label: vietnameseFontsLabel },
        { value: PDFEditController.EnglishFonts,
          label: englishFontsLabel },
        { value: PDFEditController.ChineseFonts,
          label: chineseFontsLabel },
        { value: PDFEditController.JapaneseFonts,
          label: japaneseFontsLabel },
        { value: PDFEditController.KoreanFonts,
          label: koreanFontsLabel }
    ]
    readonly property var paragraphAlignmentOptions: [
        { value: PDFEditController.LeftAlignment,
          label: alignLeftLabel,
          iconSource: "../assets/icons/align-left.svg" },
        { value: PDFEditController.RightAlignment,
          label: alignRightLabel,
          iconSource: "../assets/icons/align-right.svg" },
        { value: PDFEditController.CenterAlignment,
          label: alignCenterLabel,
          iconSource: "../assets/icons/align-center.svg" },
        { value: PDFEditController.JustifyAlignment,
          label: justifyLabel,
          iconSource: "../assets/icons/align-justify.svg" }
    ]
    readonly property int selectedFontFamilyIndex: {
        if (selectedFontFamily.length === 0)
            return -1

        const selectedKey = selectedFontFamily.toLowerCase()
        for (let index = 0; index < displayedFontFamilies.length; ++index) {
            if (displayedFontFamilies[index].toLowerCase() === selectedKey)
                return index
        }
        return -1
    }

    function refreshDisplayedFontFamilies() {
        if (typeof root.fontFamiliesForFilter !== "function") {
            root.displayedFontFamilies = root.availableFontFamilies
            return
        }
        const families = root.fontFamiliesForFilter(root.fontFamilyFilter)
        root.displayedFontFamilies = families === undefined || families === null
            ? []
            : families
    }

    function commitPendingEdit() {
        fontSizeEditState.commitRequested()
        spacingEditState.commitRequested()
        return true
    }

    onAvailableFontFamiliesChanged:
        root.refreshDisplayedFontFamilies()

    signal collapsedChangeRequested(bool collapsed)
    signal fontFamilyChangeRequested(string fontFamily)
    signal fontSizeChangeRequested(real fontSize)
    signal letterSpacingChangeRequested(real points)
    signal wordSpacingChangeRequested(real points)
    signal lineSpacingChangeRequested(real factor)
    signal paragraphSpaceBeforeChangeRequested(real points)
    signal paragraphSpaceAfterChangeRequested(real points)
    signal fontBoldChangeRequested(bool enabled)
    signal fontItalicChangeRequested(bool enabled)
    signal fontUnderlineChangeRequested(bool enabled)
    signal fontColorChangeRequested(color color)
    signal paragraphAlignmentChangeRequested(int alignment)

    implicitWidth: collapsed ? 42 : 264
    color: theme.ribbonSurface

    QtObject {
        id: fontSizeEditState

        property bool pending: false
        signal commitRequested()
    }

    QtObject {
        id: spacingEditState

        property bool letterPending: false
        property bool wordPending: false
        property bool linePending: false
        property bool beforePending: false
        property bool afterPending: false
        signal commitRequested()
    }

    component SpacingField: TextField {
        id: spacingField

        required property string spacingKind
        required property string label
        required property real currentSpacing
        property real minimumValue: -10
        property real maximumValue: 50
        property real stepSize: 0.1
        property real defaultValue: 0
        property bool pending: false
        property var pendingSession: null
        property string pendingContextKey: ""

        function syncValue() {
            pending = false
            text = Number.isFinite(currentSpacing)
                ? Number(currentSpacing.toFixed(3)).toString() : ""
        }

        function beginEdit() {
            if (pending)
                return
            pending = true
            pendingSession = root.formattingSession
            pendingContextKey = root.formattingContextKey
        }

        function commitValue() {
            const entered = text.trim().replace(",", ".")
            const points = Number(entered)
            const apply = pending && root.formattingEnabled
                && pendingSession === root.formattingSession
                && pendingContextKey === root.formattingContextKey
                && /^-?[0-9]+(?:\.[0-9]+)?$/.test(entered)
                && Number.isFinite(points)
                && points >= minimumValue && points <= maximumValue
                && (!Number.isFinite(currentSpacing)
                    || Math.abs(points - currentSpacing) > 0.000001)
            pending = false
            if (apply) {
                if (spacingKind === "word")
                    root.wordSpacingChangeRequested(points)
                else if (spacingKind === "line")
                    root.lineSpacingChangeRequested(points)
                else if (spacingKind === "before")
                    root.paragraphSpaceBeforeChangeRequested(points)
                else if (spacingKind === "after")
                    root.paragraphSpaceAfterChangeRequested(points)
                else
                    root.letterSpacingChangeRequested(points)
            }
            syncValue()
        }

        function updatePendingState(value) {
            if (spacingKind === "word")
                spacingEditState.wordPending = value
            else if (spacingKind === "line")
                spacingEditState.linePending = value
            else if (spacingKind === "before")
                spacingEditState.beforePending = value
            else if (spacingKind === "after")
                spacingEditState.afterPending = value
            else
                spacingEditState.letterPending = value
        }

        selectByMouse: true
        inputMethodHints: Qt.ImhFormattedNumbersOnly
        Accessible.name: label
        onCurrentSpacingChanged: syncValue()
        onTextEdited: beginEdit()
        onAccepted: commitValue()
        onActiveFocusChanged: {
            if (!activeFocus)
                commitValue()
        }
        onPendingChanged: updatePendingState(pending)
        Keys.onPressed: event => {
            if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
                const entered = text.trim().replace(",", ".")
                const initial = entered.length > 0 && Number.isFinite(Number(entered))
                    ? Number(entered) : defaultValue
                beginEdit()
                text = Math.max(minimumValue, Math.min(maximumValue,
                    initial + (event.key === Qt.Key_Up ? stepSize : -stepSize))).toFixed(1)
                commitValue()
                event.accepted = true
            } else if (event.key === Qt.Key_Escape) {
                syncValue()
                event.accepted = true
            }
        }
        Component.onCompleted: syncValue()
        Component.onDestruction: updatePendingState(false)

        Connections {
            target: spacingEditState

            function onCommitRequested() { spacingField.commitValue() }
        }
        Connections {
            target: root

            function onFormattingSessionChanged() { spacingField.syncValue() }
            function onFormattingContextKeyChanged() { spacingField.syncValue() }
            function onFormattingEnabledChanged() { spacingField.syncValue() }
        }
        background: Rectangle {
            radius: 6
            color: spacingField.palette.base
            border.width: spacingField.activeFocus ? 2 : 1
            border.color: spacingField.activeFocus
                ? root.theme.foxitBorderOrange : root.theme.ribbonBorder
        }
    }

    ButtonGroup {
        id: paragraphAlignmentGroup
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
        //% "Expand text formatting"
        ToolTip.text: qsTrId("ExpandTextFormatting")
        ToolTip.visible: hovered
        ToolTip.delay: 500
        onClicked: root.collapsedChangeRequested(false)

        background: Rectangle {
            radius: 4
            color: expandButton.down
                ? "#f5dfcf"
                : expandButton.hovered
                    ? root.theme.foxitPaleOrange
                    : "transparent"
        }
    }

    Loader {
        anchors.fill: parent
        anchors.leftMargin: 1
        active: !root.collapsed

        sourceComponent: Component {
            ScrollView {
                id: formatScroll

                clip: true
                contentWidth: availableWidth
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                ColumnLayout {
                    width: formatScroll.availableWidth
                    spacing: 12

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        Layout.rightMargin: 8
                        Layout.topMargin: 10
                        spacing: 8

                        Label {
                            Layout.fillWidth: true
                            //% "Text Formatting"
                            text: qsTrId("TextFormatting")
                            color: root.theme.ribbonTextColor
                            font.pixelSize: 17
                            font.weight: Font.DemiBold
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
                            //% "Collapse text formatting"
                            ToolTip.text: qsTrId("CollapseTextFormatting")
                            ToolTip.visible: hovered
                            ToolTip.delay: 500
                            onClicked: root.collapsedChangeRequested(true)

                            background: Rectangle {
                                radius: 4
                                color: collapseButton.down
                                    ? "#f5dfcf"
                                    : collapseButton.hovered
                                        ? root.theme.foxitPaleOrange
                                        : "transparent"
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        Layout.rightMargin: 16
                        visible: !root.formattingEnabled
                        //% "Select editable text to enable formatting."
                        text: qsTrId("SelectEditableTextForFormatting")
                        color: "#747176"
                        font.pixelSize: 11
                        wrapMode: Text.Wrap
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: root.theme.ribbonBorder
                    }

                    ColumnLayout {
                        id: formatControls

                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        Layout.rightMargin: 16
                        Layout.bottomMargin: 16
                        spacing: 8
                        enabled: root.formattingEnabled

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

                            Layout.fillWidth: true
                            model: root.displayedFontFamilies
                            currentIndex: root.selectedFontFamilyIndex
                            displayText: root.selectedFontFamily
                            clip: true
                            onActivated: root.fontFamilyChangeRequested(
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
                            Layout.topMargin: 4
                            //% "Font size"
                            text: qsTrId("FontSize")
                            color: root.theme.ribbonTextColor
                            font.pixelSize: 12
                            font.weight: Font.Medium
                        }

                        ComboBox {
                            id: fontSizeBox

                            property var pendingSession: null
                            property string pendingContextKey: ""

                            function syncCurrentSize() {
                                fontSizeEditState.pending = false
                                const sizeText = root.selectedFontSize > 0
                                    ? String(root.selectedFontSize)
                                    : ""
                                currentIndex = model.indexOf(sizeText)
                                editText = sizeText
                            }

                            function beginSizeEdit() {
                                if (fontSizeEditState.pending)
                                    return

                                fontSizeEditState.pending = true
                                pendingSession = root.formattingSession
                                pendingContextKey = root.formattingContextKey
                            }

                            function commitSizeText() {
                                const enteredText = editText.trim()
                                const points = Number(enteredText)
                                const apply = fontSizeEditState.pending
                                    && root.formattingEnabled
                                    && pendingSession === root.formattingSession
                                    && pendingContextKey === root.formattingContextKey
                                    && /^[0-9]+$/.test(enteredText)
                                    && Number.isInteger(points)
                                    && points >= 1 && points <= 512
                                    && Math.abs(points - root.selectedFontSize) > 0.0001
                                fontSizeEditState.pending = false
                                if (apply)
                                    root.fontSizeChangeRequested(points)
                                syncCurrentSize()
                            }

                            Layout.fillWidth: true
                            editable: true
                            model: ["8", "9", "10", "11", "12", "14", "16", "18", "24", "36", "48", "72"]
                            currentIndex: -1
                            inputMethodHints: Qt.ImhDigitsOnly
                            clip: true
                            //% "Font size"
                            Accessible.name: qsTrId("FontSize")
                            Component.onCompleted: syncCurrentSize()
                            Component.onDestruction: fontSizeEditState.pending = false
                            onActivated: index => {
                                beginSizeEdit()
                                editText = textAt(index)
                                commitSizeText()
                            }
                            onAccepted: commitSizeText()

                            Connections {
                                target: fontSizeEditState

                                function onCommitRequested() {
                                    fontSizeBox.commitSizeText()
                                }
                            }

                            Connections {
                                target: root

                                function onSelectedFontSizeChanged() {
                                    fontSizeBox.syncCurrentSize()
                                }

                                function onFormattingSessionChanged() {
                                    fontSizeBox.syncCurrentSize()
                                }

                                function onFormattingContextKeyChanged() {
                                    fontSizeBox.syncCurrentSize()
                                }

                                function onFormattingEnabledChanged() {
                                    fontSizeBox.syncCurrentSize()
                                }
                            }

                            background: Rectangle {
                                radius: 6
                                color: fontSizeBox.palette.button
                                border.width: 1
                                border.color: fontSizeBox.activeFocus
                                    ? root.theme.foxitBorderOrange
                                    : root.theme.ribbonBorder
                            }

                            contentItem: TextField {
                                id: fontSizeInput

                                leftPadding: 12
                                rightPadding: fontSizeBox.editable && activeFocus ? 3 : 1
                                topPadding: 6 - fontSizeBox.padding
                                bottomPadding: 6 - fontSizeBox.padding
                                text: fontSizeBox.editText
                                font: fontSizeBox.font
                                enabled: fontSizeBox.editable
                                autoScroll: fontSizeBox.editable
                                readOnly: fontSizeBox.down
                                inputMethodHints: fontSizeBox.inputMethodHints
                                validator: fontSizeBox.validator
                                selectByMouse: fontSizeBox.selectTextByMouse
                                color: fontSizeBox.palette.text
                                selectionColor: fontSizeBox.palette.highlight
                                selectedTextColor: fontSizeBox.palette.highlightedText
                                verticalAlignment: Text.AlignVCenter
                                //% "Font size"
                                Accessible.name: qsTrId("FontSize")
                                onTextEdited: fontSizeBox.beginSizeEdit()
                                onActiveFocusChanged: {
                                    if (!activeFocus && !fontSizeBox.popup.visible)
                                        fontSizeBox.commitSizeText()
                                }
                                Keys.onPressed: event => {
                                    if ((event.key === Qt.Key_Return
                                            || event.key === Qt.Key_Enter)
                                            && !fontSizeBox.popup.visible) {
                                        fontSizeBox.commitSizeText()
                                        event.accepted = true
                                    }
                                }

                                background: Rectangle {
                                    visible: fontSizeBox.enabled
                                        && fontSizeBox.editable
                                        && !fontSizeBox.flat
                                    radius: 6
                                    color: fontSizeBox.palette.base
                                    border.width: fontSizeInput.activeFocus ? 2 : 1
                                    border.color: fontSizeInput.activeFocus
                                        ? root.theme.foxitBorderOrange
                                        : root.theme.ribbonBorder
                                }
                            }

                            validator: IntValidator {
                                bottom: 1
                                top: 512
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            Layout.topMargin: 4
                            //% "Spacing (pt)"
                            text: qsTrId("TextSpacingPoints")
                            color: root.theme.ribbonTextColor
                            font.pixelSize: 12
                            font.weight: Font.Medium
                        }

                        GridLayout {
                            Layout.fillWidth: true
                            columns: 2
                            columnSpacing: 8
                            rowSpacing: 4

                            Label {
                                Layout.fillWidth: true
                                text: root.letterSpacingLabel
                                color: root.theme.ribbonTextColor
                                font.pixelSize: 11
                            }
                            Label {
                                Layout.fillWidth: true
                                text: root.wordSpacingLabel
                                color: root.theme.ribbonTextColor
                                font.pixelSize: 11
                            }
                            SpacingField {
                                objectName: "letterSpacingInput"
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                Layout.preferredWidth: 1
                                spacingKind: "letter"
                                label: root.letterSpacingLabel
                                currentSpacing: root.selectedLetterSpacing
                            }
                            SpacingField {
                                objectName: "wordSpacingInput"
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                Layout.preferredWidth: 1
                                spacingKind: "word"
                                label: root.wordSpacingLabel
                                currentSpacing: root.selectedWordSpacing
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

                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                Layout.preferredWidth: 1
                                Layout.preferredHeight: 58
                                checkable: true
                                focusPolicy: Qt.TabFocus
                                checked: root.selectedFontBold
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
                                onClicked: root.fontBoldChangeRequested(
                                    !root.selectedFontBold)

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

                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                Layout.preferredWidth: 1
                                Layout.preferredHeight: 58
                                checkable: true
                                focusPolicy: Qt.TabFocus
                                checked: root.selectedFontItalic
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
                                onClicked: root.fontItalicChangeRequested(
                                    !root.selectedFontItalic)

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

                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                Layout.preferredWidth: 1
                                Layout.preferredHeight: 58
                                checkable: true
                                focusPolicy: Qt.TabFocus
                                checked: root.selectedFontUnderline
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
                                onClicked: root.fontUnderlineChangeRequested(
                                    !root.selectedFontUnderline)

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

                            Layout.fillWidth: true
                            Layout.preferredHeight: 38
                            onClicked: {
                                colorDialog.selectedColor = root.selectedColor
                                colorDialog.open()
                            }

                            contentItem: RowLayout {
                                spacing: 10

                                Rectangle {
                                    Layout.preferredWidth: 22
                                    Layout.preferredHeight: 22
                                    radius: 3
                                    color: root.selectedColor
                                    border.width: 1
                                    border.color: "#8a8785"
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: root.selectedColor.toString().toUpperCase()
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
                            Layout.topMargin: 4
                            //% "Paragraph"
                            text: qsTrId("ParagraphFormatting")
                            color: root.theme.ribbonTextColor
                            font.pixelSize: 12
                            font.weight: Font.Medium
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Label {
                                Layout.fillWidth: true
                                text: root.lineSpacingLabel
                                color: root.theme.ribbonTextColor
                                font.pixelSize: 11
                            }
                            SpacingField {
                                objectName: "lineSpacingInput"
                                Layout.preferredWidth: 88
                                spacingKind: "line"
                                label: root.lineSpacingLabel
                                currentSpacing: root.selectedLineSpacing
                                minimumValue: 0.5
                                maximumValue: 3
                                defaultValue: 1
                            }
                        }

                        GridLayout {
                            Layout.fillWidth: true
                            columns: 2
                            columnSpacing: 8
                            rowSpacing: 4

                            Label {
                                Layout.fillWidth: true
                                text: root.paragraphSpaceBeforeLabel
                                color: root.theme.ribbonTextColor
                                font.pixelSize: 11
                            }
                            Label {
                                Layout.fillWidth: true
                                text: root.paragraphSpaceAfterLabel
                                color: root.theme.ribbonTextColor
                                font.pixelSize: 11
                            }
                            SpacingField {
                                objectName: "paragraphSpaceBeforeInput"
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                Layout.preferredWidth: 1
                                spacingKind: "before"
                                label: root.paragraphSpaceBeforeLabel
                                currentSpacing: root.selectedParagraphSpaceBefore
                                minimumValue: 0
                                maximumValue: 144
                                stepSize: 1
                            }
                            SpacingField {
                                objectName: "paragraphSpaceAfterInput"
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                Layout.preferredWidth: 1
                                spacingKind: "after"
                                label: root.paragraphSpaceAfterLabel
                                currentSpacing: root.selectedParagraphSpaceAfter
                                minimumValue: 0
                                maximumValue: 144
                                stepSize: 1
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6

                            Repeater {
                                model: root.paragraphAlignmentOptions

                                delegate: ToolButton {
                                    id: alignmentButton

                                    required property var modelData

                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    Layout.preferredWidth: 1
                                    Layout.preferredHeight: 38
                                    checkable: true
                                    focusPolicy: Qt.TabFocus
                                    checked: root.selectedParagraphAlignment
                                        === modelData.value
                                    ButtonGroup.group: paragraphAlignmentGroup
                                    display: AbstractButton.IconOnly
                                    icon.source: modelData.iconSource
                                    icon.width: 20
                                    icon.height: 20
                                    icon.color: !enabled
                                        ? "#aaa8a6"
                                        : checked
                                            ? root.theme.foxitDarkOrange
                                            : "transparent"
                                    Accessible.name: modelData.label
                                    ToolTip.text: modelData.label
                                    ToolTip.visible: hovered
                                    ToolTip.delay: 500
                                    onClicked:
                                        root.paragraphAlignmentChangeRequested(
                                            modelData.value)

                                    background: Rectangle {
                                        radius: 4
                                        color: !alignmentButton.enabled
                                            ? "#f4f1ef"
                                            : alignmentButton.down
                                                ? "#f5dfcf"
                                                : alignmentButton.checked
                                                    || alignmentButton.hovered
                                                    ? root.theme.foxitPaleOrange
                                                    : "#ffffff"
                                        border.width: 1
                                        border.color: alignmentButton.checked
                                            ? root.theme.foxitBorderOrange
                                            : root.theme.ribbonBorder
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    ColorDialog {
        id: colorDialog

        //% "Font color"
        title: qsTrId("FontColor")
        onAccepted: root.fontColorChangeRequested(selectedColor)
    }
}
