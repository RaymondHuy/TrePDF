pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic

Item {
    id: root

    required property AppTheme theme
    required property var fields
    required property bool interactive
    required property int page
    required property int focusedAnnotationIndex
    property Item viewportItem: null
    property real viewportContentX: 0
    property real viewportContentY: 0
    property var viewportTransformState: []
    property int openAnnotationIndex: -1
    property int highlightedOptionIndex: -1
    property int pendingOpenSourcePage: -1
    property int pendingOpenAnnotationIndex: -1
    property string pendingOpenIdentityToken: ""
    property string typeAheadPrefix: ""
    property double lastTypeAheadTimestamp: 0
    property var listScrollPositions: ({})
    signal fieldFocusRequested(
        int sourcePage,
        int annotationIndex,
        string identityToken)
    signal optionSelected(
        int sourcePage,
        int annotationIndex,
        int optionIndex,
        bool selected,
        bool toggle,
        string identityToken)

    visible: interactive && fields.length > 0
    clip: true

    readonly property rect visibleViewportRect: {
        // Keep the mapping binding live while the surrounding ListView scrolls.
        const mappingState = viewportTransformState
        const mappingRevision = viewportContentX
            + viewportContentY
            + String(mappingState).length
        if (!viewportItem) {
            return Qt.rect(
                mappingRevision * 0,
                0,
                width,
                height)
        }
        const mapped = root.mapFromItem(
            viewportItem,
            0,
            0,
            viewportItem.width,
            viewportItem.height)
        return Qt.rect(
            mapped.x + mappingRevision * 0,
            mapped.y,
            mapped.width,
            mapped.height)
    }
    readonly property real visibleLeft: Math.max(
        0,
        visibleViewportRect.x)
    readonly property real visibleTop: Math.max(
        0,
        visibleViewportRect.y)
    readonly property real visibleRight: Math.min(
        width,
        visibleViewportRect.x + visibleViewportRect.width)
    readonly property real visibleBottom: Math.min(
        height,
        visibleViewportRect.y + visibleViewportRect.height)

    function isSelected(field, optionIndex) {
        return field.selectedIndices.indexOf(optionIndex) >= 0
    }

    function selectedLabel(field) {
        if (field.selectedIndices.length === 0)
            return ""
        const selectedIndex = field.selectedIndices[0]
        return selectedIndex >= 0 && selectedIndex < field.options.length
            ? field.options[selectedIndex]
            : ""
    }

    function closePopup() {
        openAnnotationIndex = -1
        highlightedOptionIndex = -1
        pendingOpenSourcePage = -1
        pendingOpenAnnotationIndex = -1
        pendingOpenIdentityToken = ""
        typeAheadPrefix = ""
        lastTypeAheadTimestamp = 0
    }

    function fieldForAnnotation(annotationIndex) {
        for (let index = 0; index < fields.length; ++index) {
            if (fields[index].annotationIndex === annotationIndex)
                return fields[index]
        }
        return null
    }

    function openCombo(field) {
        if (!field
                || !field.comboBox
                || field.readOnly
                || field.options.length === 0
                || field.width * root.width < 18
                || field.height * root.height < 13) {
            return false
        }
        openAnnotationIndex = field.annotationIndex
        highlightedOptionIndex = field.selectedIndices.length > 0
            ? field.selectedIndices[0]
            : 0
        typeAheadPrefix = ""
        lastTypeAheadTimestamp = 0
        return true
    }

    function openFocusedCombo() {
        return openCombo(fieldForAnnotation(focusedAnnotationIndex))
    }

    function requestComboOpen(field) {
        if (!field)
            return
        if (focusedAnnotationIndex === field.annotationIndex) {
            openCombo(field)
            return
        }
        pendingOpenSourcePage = field.sourcePage
        pendingOpenAnnotationIndex = field.annotationIndex
        pendingOpenIdentityToken = field.identityToken
        fieldFocusRequested(
            field.sourcePage,
            field.annotationIndex,
            field.identityToken)
    }

    function completeComboFocus(sourcePage,
                                annotationIndex,
                                identityToken,
                                focused) {
        if (pendingOpenSourcePage !== sourcePage
                || pendingOpenAnnotationIndex !== annotationIndex
                || pendingOpenIdentityToken !== identityToken) {
            return
        }
        pendingOpenSourcePage = -1
        pendingOpenAnnotationIndex = -1
        pendingOpenIdentityToken = ""
        const field = fieldForAnnotation(annotationIndex)
        if (focused
                && field
                && field.sourcePage === sourcePage
                && field.identityToken === identityToken) {
            openCombo(field)
        }
    }

    function handleTypeAhead(field, text) {
        if (!text || text.length === 0)
            return false
        const now = Date.now()
        if (now - lastTypeAheadTimestamp > 900)
            typeAheadPrefix = ""
        lastTypeAheadTimestamp = now
        typeAheadPrefix += text.toLocaleLowerCase()

        let match = -1
        for (let index = 0; index < field.options.length; ++index) {
            if (String(field.options[index])
                    .toLocaleLowerCase()
                    .startsWith(typeAheadPrefix)) {
                match = index
                break
            }
        }
        if (match < 0 && typeAheadPrefix.length > text.length) {
            typeAheadPrefix = text.toLocaleLowerCase()
            for (let index = 0; index < field.options.length; ++index) {
                if (String(field.options[index])
                        .toLocaleLowerCase()
                        .startsWith(typeAheadPrefix)) {
                    match = index
                    break
                }
            }
        }
        if (match < 0)
            return false
        highlightedOptionIndex = match
        return true
    }

    function handleKey(key, modifiers, text) {
        if (openAnnotationIndex < 0) {
            const openShortcut = key === Qt.Key_Return
                || key === Qt.Key_Enter
                || key === Qt.Key_Space
                || key === Qt.Key_F4
                || (key === Qt.Key_Down
                    && (modifiers & Qt.AltModifier))
            return openShortcut && openFocusedCombo()
        }

        const field = fieldForAnnotation(openAnnotationIndex)
        if (!field || field.options.length === 0) {
            closePopup()
            return false
        }

        if (key === Qt.Key_Escape) {
            closePopup()
            return true
        }

        let nextIndex = highlightedOptionIndex
        if (key === Qt.Key_Up)
            nextIndex = Math.max(0, nextIndex - 1)
        else if (key === Qt.Key_Down)
            nextIndex = Math.min(field.options.length - 1, nextIndex + 1)
        else if (key === Qt.Key_Home)
            nextIndex = 0
        else if (key === Qt.Key_End)
            nextIndex = field.options.length - 1
        else if (key === Qt.Key_PageUp)
            nextIndex = Math.max(0, nextIndex - 5)
        else if (key === Qt.Key_PageDown)
            nextIndex = Math.min(field.options.length - 1, nextIndex + 5)
        else if (key === Qt.Key_Return
                 || key === Qt.Key_Enter
                 || key === Qt.Key_Space) {
            if (highlightedOptionIndex >= 0) {
                optionSelected(
                    field.sourcePage,
                    openAnnotationIndex,
                    highlightedOptionIndex,
                    true,
                    false,
                    field.identityToken)
            }
            closePopup()
            return true
        } else if (key === Qt.Key_Tab
                   || key === Qt.Key_Backtab) {
            const selectedIndex = field.selectedIndices.length > 0
                ? field.selectedIndices[0]
                : -1
            if (highlightedOptionIndex >= 0
                    && highlightedOptionIndex !== selectedIndex) {
                optionSelected(
                    field.sourcePage,
                    openAnnotationIndex,
                    highlightedOptionIndex,
                    true,
                    false,
                    field.identityToken)
            }
            closePopup()
            return false
        } else if (!(modifiers
                     & (Qt.ControlModifier | Qt.MetaModifier))
                   && text
                   && text.length > 0) {
            handleTypeAhead(field, text)
            return true
        } else {
            return false
        }

        highlightedOptionIndex = nextIndex
        return true
    }

    function listScrollKey(field) {
        return String(field.sourcePage) + ":" + field.identityToken
    }

    function rememberListScrollPosition(field, contentY) {
        listScrollPositions[listScrollKey(field)] = contentY
    }

    function savedListScrollPosition(field) {
        const value = listScrollPositions[listScrollKey(field)]
        return typeof value === "number" && Number.isFinite(value)
            ? value
            : -1
    }

    function fieldStillExists(annotationIndex) {
        for (let index = 0; index < fields.length; ++index) {
            if (fields[index].annotationIndex === annotationIndex)
                return true
        }
        return false
    }

    onVisibleChanged: {
        if (!visible)
            closePopup()
    }

    onPageChanged: {
        closePopup()
        listScrollPositions = ({})
    }

    onFocusedAnnotationIndexChanged: {
        if (openAnnotationIndex >= 0
                && focusedAnnotationIndex !== openAnnotationIndex) {
            closePopup()
        }
    }

    onFieldsChanged: {
        if (pendingOpenAnnotationIndex >= 0) {
            const pendingField = fieldForAnnotation(
                pendingOpenAnnotationIndex)
            if (!pendingField
                    || pendingField.sourcePage
                        !== pendingOpenSourcePage
                    || pendingField.identityToken
                        !== pendingOpenIdentityToken) {
                closePopup()
                return
            }
        }
        if (openAnnotationIndex >= 0
                && !fieldStillExists(openAnnotationIndex)) {
            closePopup()
        } else if (openAnnotationIndex >= 0) {
            const openField = fieldForAnnotation(openAnnotationIndex)
            if (!openField
                    || highlightedOptionIndex
                        >= openField.options.length) {
                closePopup()
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        visible: root.openAnnotationIndex >= 0
        enabled: visible
        acceptedButtons: Qt.LeftButton
        preventStealing: true
        z: 50

        onClicked: root.closePopup()
    }

    Repeater {
        model: root.fields

        delegate: Item {
            id: fieldItem

            required property var modelData
            readonly property var field: modelData
            readonly property real optionHeight: Math.max(
                24,
                Math.min(34, height))
            readonly property bool popupOpen:
                root.openAnnotationIndex === field.annotationIndex
            readonly property bool fieldFocused:
                root.focusedAnnotationIndex === field.annotationIndex
            readonly property bool customUsable:
                width >= 18 && height >= 13

            x: field.x * root.width
            y: field.y * root.height
            width: Math.max(1, field.width * root.width)
            height: Math.max(1, field.height * root.height)
            z: popupOpen ? 100 : 1

            onCustomUsableChanged: {
                if (!customUsable && popupOpen)
                    root.closePopup()
            }

            Button {
                id: comboButton

                anchors.fill: parent
                visible: fieldItem.field.comboBox
                    && fieldItem.customUsable
                enabled: !fieldItem.field.readOnly
                    && fieldItem.field.options.length > 0
                hoverEnabled: true
                focusPolicy: Qt.NoFocus
                clip: true
                text: root.selectedLabel(fieldItem.field)
                Accessible.role: Accessible.ComboBox
                Accessible.name: fieldItem.field.accessibleName
                Accessible.readOnly: fieldItem.field.readOnly
                Accessible.focusable: true
                Accessible.focused: fieldItem.fieldFocused
                Accessible.description: fieldItem.popupOpen
                    && root.highlightedOptionIndex >= 0
                    && root.highlightedOptionIndex
                        < fieldItem.field.options.length
                    ? fieldItem.field.options[
                        root.highlightedOptionIndex]
                    : text

                onClicked: {
                    if (fieldItem.popupOpen)
                        root.closePopup()
                    else
                        root.requestComboOpen(fieldItem.field)
                }

                background: Rectangle {
                    radius: Math.min(6, comboButton.height / 4)
                    color: comboButton.enabled
                        ? root.theme.ribbonSurface
                        : "#f3f0ed"
                    border.width: comboButton.hovered
                        || fieldItem.popupOpen
                        || fieldItem.fieldFocused ? 2 : 1
                    border.color: comboButton.hovered
                        || fieldItem.popupOpen
                        || fieldItem.fieldFocused
                        ? root.theme.foxitBorderOrange
                        : root.theme.ribbonBorder
                }

                contentItem: Item {
                    Label {
                        anchors.left: parent.left
                        anchors.leftMargin: Math.max(
                            7,
                            comboButton.height * 0.22)
                        anchors.right: arrowSurface.left
                        anchors.rightMargin: 5
                        anchors.verticalCenter: parent.verticalCenter
                        text: comboButton.text
                        color: comboButton.enabled
                            ? root.theme.ribbonTextColor
                            : "#817a75"
                        font.pixelSize: Math.max(
                            10,
                            Math.min(24, comboButton.height * 0.44))
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                    }

                    Rectangle {
                        id: arrowSurface

                        anchors.top: parent.top
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        width: Math.min(comboButton.height, 32)
                        radius: Math.min(5, comboButton.height / 5)
                        color: comboButton.hovered
                            || fieldItem.popupOpen
                            || fieldItem.fieldFocused
                            ? root.theme.foxitPaleOrange
                            : "transparent"

                        Image {
                            anchors.centerIn: parent
                            width: Math.max(
                                9,
                                Math.min(15, arrowSurface.width * 0.42))
                            height: width
                            source: "../assets/icons/chevron-right.svg"
                            sourceSize.width: 18
                            sourceSize.height: 18
                            rotation: fieldItem.popupOpen ? 270 : 90
                            smooth: true
                        }
                    }
                }
            }

            Control {
                id: listBox

                anchors.fill: parent
                visible: !fieldItem.field.comboBox
                    && fieldItem.customUsable
                hoverEnabled: true
                focusPolicy: Qt.NoFocus
                padding: 2
                clip: true
                Accessible.role: Accessible.List
                Accessible.name: fieldItem.field.accessibleName
                Accessible.readOnly: fieldItem.field.readOnly
                Accessible.focusable: true
                Accessible.focused: fieldItem.fieldFocused

                background: Rectangle {
                    radius: Math.min(6, listBox.height / 8)
                    color: root.theme.ribbonSurface
                    border.width: listBox.hovered
                        || fieldItem.fieldFocused ? 2 : 1
                    border.color: listBox.hovered
                        || fieldItem.fieldFocused
                        ? root.theme.foxitBorderOrange
                        : root.theme.ribbonBorder
                }

                contentItem: ListView {
                    id: optionList

                    property bool scrollPositionInitialized: false
                    readonly property real rowHeight: Math.max(
                        22,
                        Math.min(
                            32,
                            fieldItem.height
                                / Math.min(
                                    4,
                                    Math.max(
                                        1,
                                        fieldItem.field.options.length))))

                    model: fieldItem.field.options
                    clip: true
                    reuseItems: true
                    boundsBehavior: Flickable.StopAtBounds
                    interactive: contentHeight > height
                    currentIndex: fieldItem.field.selectedIndices.length > 0
                        ? fieldItem.field.selectedIndices[0]
                        : -1

                    Component.onCompleted: {
                        const savedPosition
                            = root.savedListScrollPosition(
                                fieldItem.field)
                        if (savedPosition >= 0) {
                            contentY = savedPosition
                            returnToBounds()
                        } else if (currentIndex >= 0) {
                            positionViewAtIndex(
                                currentIndex,
                                ListView.Contain)
                        }
                        scrollPositionInitialized = true
                    }

                    Component.onDestruction:
                        root.rememberListScrollPosition(
                            fieldItem.field,
                            contentY)

                    onContentYChanged: {
                        if (scrollPositionInitialized) {
                            root.rememberListScrollPosition(
                                fieldItem.field,
                                contentY)
                        }
                    }

                    delegate: ItemDelegate {
                        id: listOption

                        required property int index
                        required property string modelData
                        readonly property bool optionSelected:
                            root.isSelected(
                                fieldItem.field,
                                index)

                        width: Math.max(
                            0,
                            ListView.view.width
                                - (listScrollBar.visible
                                    ? listScrollBar.width
                                    : 0))
                        height: optionList.rowHeight
                        hoverEnabled: true
                        focusPolicy: Qt.NoFocus
                        enabled: !fieldItem.field.readOnly
                        text: modelData
                        highlighted: optionSelected
                        Accessible.role: Accessible.ListItem
                        Accessible.name: text
                        Accessible.selectable: true
                        Accessible.selected: optionSelected

                        onClicked: {
                            root.optionSelected(
                                fieldItem.field.sourcePage,
                                fieldItem.field.annotationIndex,
                                index,
                                fieldItem.field.multiSelect
                                    ? !optionSelected
                                    : true,
                                fieldItem.field.multiSelect,
                                fieldItem.field.identityToken)
                        }

                        background: Rectangle {
                            color: listOption.down
                                ? "#f5dfcf"
                                : listOption.optionSelected
                                    ? root.theme.foxitPaleOrange
                                    : listOption.hovered
                                        ? "#faf5f1"
                                        : root.theme.ribbonSurface

                            Rectangle {
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                width: 3
                                color: root.theme.foxitOrange
                                visible: listOption.optionSelected
                            }
                        }

                        contentItem: Label {
                            leftPadding: 8
                            rightPadding: 6
                            text: listOption.text
                            color: listOption.enabled
                                ? root.theme.ribbonTextColor
                                : "#817a75"
                            font.pixelSize: Math.max(
                                10,
                                Math.min(
                                    18,
                                    optionList.rowHeight * 0.48))
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                    }

                    ScrollBar.vertical: ScrollBar {
                        id: listScrollBar

                        width: 11
                        policy: optionList.contentHeight > optionList.height
                            ? ScrollBar.AlwaysOn
                            : ScrollBar.AlwaysOff

                        background: Rectangle {
                            radius: width / 2
                            color: "#f1ece8"
                        }

                        contentItem: Rectangle {
                            implicitWidth: 7
                            implicitHeight: 28
                            radius: width / 2
                            color: listScrollBar.pressed
                                ? root.theme.foxitOrange
                                : listScrollBar.hovered
                                    ? root.theme.foxitHoverOrange
                                    : "#c98a5b"
                        }
                    }
                }
            }

            Rectangle {
                id: comboPopup

                readonly property real desiredWidth: Math.max(
                    fieldItem.width,
                    Math.min(320, fieldItem.width * 1.25))
                readonly property real desiredHeight: Math.min(
                    fieldItem.field.options.length
                        * fieldItem.optionHeight + 8,
                    root.height * 0.42)
                readonly property real availableWidth: Math.max(
                    1,
                    root.visibleRight - root.visibleLeft)
                readonly property real spaceBelow:
                    Math.max(
                        0,
                        root.visibleBottom
                        - fieldItem.y
                        - fieldItem.height
                        - 4)
                readonly property real spaceAbove:
                    Math.max(
                        0,
                        fieldItem.y - root.visibleTop - 4)
                readonly property bool opensUpward:
                    spaceBelow < desiredHeight
                        && spaceAbove > spaceBelow
                readonly property real absoluteX: Math.max(
                    root.visibleLeft,
                    Math.min(
                        fieldItem.x,
                        root.visibleRight - width))

                x: absoluteX - fieldItem.x
                y: opensUpward
                    ? -height - 4
                    : fieldItem.height + 4
                width: Math.max(
                    1,
                    Math.min(desiredWidth, availableWidth))
                height: Math.max(
                    1,
                    Math.min(
                        desiredHeight,
                        opensUpward ? spaceAbove : spaceBelow))
                visible: fieldItem.field.comboBox
                    && fieldItem.customUsable
                    && fieldItem.popupOpen
                    && fieldItem.field.options.length > 0
                radius: 8
                color: root.theme.ribbonSurface
                border.width: 1
                border.color: root.theme.foxitBorderOrange
                clip: true
                z: 10

                ListView {
                    id: comboOptions

                    anchors.fill: parent
                    anchors.margins: 4
                    model: fieldItem.field.options
                    clip: true
                    reuseItems: true
                    boundsBehavior: Flickable.StopAtBounds
                    currentIndex: fieldItem.popupOpen
                        ? root.highlightedOptionIndex
                        : fieldItem.field.selectedIndices.length > 0
                            ? fieldItem.field.selectedIndices[0]
                            : -1

                    Component.onCompleted: {
                        if (currentIndex >= 0)
                            positionViewAtIndex(
                                currentIndex,
                                ListView.Contain)
                    }

                    onCurrentIndexChanged: {
                        if (fieldItem.popupOpen && currentIndex >= 0) {
                            positionViewAtIndex(
                                currentIndex,
                                ListView.Contain)
                        }
                    }

                    delegate: ItemDelegate {
                        id: comboOption

                        required property int index
                        required property string modelData
                        readonly property bool optionSelected:
                            root.isSelected(
                                fieldItem.field,
                                index)
                        readonly property bool keyboardHighlighted:
                            fieldItem.popupOpen
                            && root.highlightedOptionIndex === index

                        width: Math.max(
                            0,
                            ListView.view.width
                                - (comboScrollBar.visible
                                    ? comboScrollBar.width
                                    : 0))
                        height: fieldItem.optionHeight
                        hoverEnabled: true
                        focusPolicy: Qt.NoFocus
                        text: modelData
                        highlighted: optionSelected
                        Accessible.role: Accessible.ListItem
                        Accessible.name: text
                        Accessible.selectable: true
                        Accessible.selected: optionSelected

                        onClicked: {
                            root.optionSelected(
                                fieldItem.field.sourcePage,
                                fieldItem.field.annotationIndex,
                                index,
                                true,
                                false,
                                fieldItem.field.identityToken)
                            root.closePopup()
                        }

                        background: Rectangle {
                            radius: 5
                            color: comboOption.down
                                ? "#f5dfcf"
                                : comboOption.optionSelected
                                    ? root.theme.foxitPaleOrange
                                    : comboOption.hovered
                                        ? "#faf5f1"
                                        : root.theme.ribbonSurface
                            border.width:
                                comboOption.keyboardHighlighted
                                && !comboOption.optionSelected ? 1 : 0
                            border.color: root.theme.foxitBorderOrange

                            Rectangle {
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                width: 3
                                radius: 2
                                color: root.theme.foxitOrange
                                visible: comboOption.optionSelected
                            }
                        }

                        contentItem: Label {
                            leftPadding: 9
                            rightPadding: 6
                            text: comboOption.text
                            color: root.theme.ribbonTextColor
                            font.pixelSize: Math.max(
                                10,
                                Math.min(
                                    18,
                                    fieldItem.optionHeight * 0.48))
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                    }

                    ScrollBar.vertical: ScrollBar {
                        id: comboScrollBar

                        width: 11
                        policy: comboOptions.contentHeight
                            > comboOptions.height
                            ? ScrollBar.AlwaysOn
                            : ScrollBar.AlwaysOff

                        background: Rectangle {
                            radius: width / 2
                            color: "#f1ece8"
                        }

                        contentItem: Rectangle {
                            implicitWidth: 7
                            implicitHeight: 28
                            radius: width / 2
                            color: comboScrollBar.pressed
                                ? root.theme.foxitOrange
                                : comboScrollBar.hovered
                                    ? root.theme.foxitHoverOrange
                                    : "#c98a5b"
                        }
                    }
                }
            }
        }
    }
}
