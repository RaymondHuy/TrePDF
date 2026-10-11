pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import TrePDF

Item {
    id: root

    required property AppTheme theme
    required property PDFFormDesignerController controller
    required property int page
    required property real pagePointScale
    property bool pooled: false
    property bool formsActive: false
    property bool editActive: false
    property bool fieldsVisible: false
    property bool fieldsInteractive: false
    property bool textFieldToolActive: false
    property bool checkboxToolActive: false
    property bool radioButtonToolActive: false
    property bool dropdownToolActive: false
    property bool listBoxToolActive: false
    property bool combFieldToolActive: false
    property rect placementRect: Qt.rect(0, 0, 0, 0)
    property real placementStartX: 0
    property real placementStartY: 0
    property string pendingFocusFieldId: ""
    property string loadedEditorFieldId: ""
    property bool syncingTextEditor: false
    readonly property bool activeTextFieldMultiline:
        (formFieldLayer.activeFieldFlags
            & PDFFormDesignerController.Multiline) !== 0
    readonly property bool activeTextFieldComb:
        (formFieldLayer.activeFieldFlags
            & PDFFormDesignerController.Comb) !== 0
        && formFieldLayer.activeFieldMaxLength > 0
        && !activeTextFieldMultiline
    readonly property int activeTextFieldInputLengthLimit: Math.max(
        1,
        formFieldLayer.activeFieldMaxLength > 0
            ? Math.min(
                  controller.maximumTextFieldInputLength,
                  formFieldLayer.activeFieldMaxLength)
            : Math.max(
                  controller.maximumTextFieldInputLength,
                  formFieldLayer.activeFieldValue.length))
    readonly property real resizeHandleSize: 8
    readonly property real resizeHandleHitSize: 24
    readonly property real minimumResizeWidth: 60
    readonly property real minimumResizeHeight: 24
    readonly property real radioButtonDefaultSidePoints: 18
    readonly property real radioButtonMinimumSidePoints: 10
    readonly property bool placementCombField: combFieldToolActive
    readonly property int placementFieldType: textFieldToolActive
        || placementCombField
        ? PDFFormDesignerController.Text
        : checkboxToolActive
            ? PDFFormDesignerController.Checkbox
            : radioButtonToolActive
                ? PDFFormDesignerController.RadioButton
                : dropdownToolActive
                    ? PDFFormDesignerController.Dropdown
                    : listBoxToolActive
                        ? PDFFormDesignerController.ListBox
                        : -1

    function resetPlacement() {
        placementRect = Qt.rect(0, 0, 0, 0)
    }

    function updatePlacement(x, y) {
        const boundedX = Math.max(0, Math.min(width, x))
        const boundedY = Math.max(0, Math.min(height, y))
        if (placementFieldType === PDFFormDesignerController.Checkbox
                || placementFieldType
                    === PDFFormDesignerController.RadioButton) {
            const deltaX = boundedX - placementStartX
            const deltaY = boundedY - placementStartY
            const directionX = deltaX < 0 ? -1 : 1
            const directionY = deltaY < 0 ? -1 : 1
            const maximumHorizontalSide = directionX < 0
                ? placementStartX
                : width - placementStartX
            const maximumVerticalSide = directionY < 0
                ? placementStartY
                : height - placementStartY
            const side = Math.min(
                Math.max(Math.abs(deltaX), Math.abs(deltaY)),
                maximumHorizontalSide,
                maximumVerticalSide)
            const endX = placementStartX + directionX * side
            const endY = placementStartY + directionY * side
            placementRect = Qt.rect(
                Math.min(placementStartX, endX),
                Math.min(placementStartY, endY),
                side,
                side)
            return
        }
        placementRect = Qt.rect(
            Math.min(placementStartX, boundedX),
            Math.min(placementStartY, boundedY),
            Math.abs(boundedX - placementStartX),
            Math.abs(boundedY - placementStartY))
    }

    function resizedFieldRect(startRect, horizontal, vertical, deltaX, deltaY) {
        const pageWidth = Math.max(1, root.width)
        const pageHeight = Math.max(1, root.height)
        const minimumWidth = Math.min(
            Number(startRect.width),
            root.minimumResizeWidth / pageWidth)
        const minimumHeight = Math.min(
            Number(startRect.height),
            root.minimumResizeHeight / pageHeight)
        let left = Number(startRect.x)
        let top = Number(startRect.y)
        let right = left + Number(startRect.width)
        let bottom = top + Number(startRect.height)

        if (horizontal < 0) {
            left = Math.max(
                0,
                Math.min(
                    right - minimumWidth,
                    left + deltaX / pageWidth))
        } else if (horizontal > 0) {
            right = Math.min(
                1,
                Math.max(
                    left + minimumWidth,
                    right + deltaX / pageWidth))
        }

        if (vertical < 0) {
            top = Math.max(
                0,
                Math.min(
                    bottom - minimumHeight,
                    top + deltaY / pageHeight))
        } else if (vertical > 0) {
            bottom = Math.min(
                1,
                Math.max(
                    top + minimumHeight,
                    bottom + deltaY / pageHeight))
        }

        return Qt.rect(left, top, right - left, bottom - top)
    }

    function syncSingleLineTextEditor() {
        if (singleLineTextEditorLoader.status !== Loader.Ready)
            return

        const editor = singleLineTextEditorLoader.item as TextField
        if (!editor)
            return
        const fieldId = formFieldLayer.activeFieldId
        const fieldChanged = root.loadedEditorFieldId !== fieldId
        if (!fieldChanged
                && editor.text === formFieldLayer.activeFieldValue)
            return
        if (fieldChanged && editor.activeFocus
                && root.loadedEditorFieldId.length > 0) {
            root.controller.setTextFieldFocused(
                root.loadedEditorFieldId,
                false)
        }
        root.loadedEditorFieldId = fieldId
        if (editor.text !== formFieldLayer.activeFieldValue) {
            root.syncingTextEditor = true
            try {
                editor.text = formFieldLayer.activeFieldValue
            } finally {
                root.syncingTextEditor = false
            }
        }
        if (fieldChanged && editor.activeFocus && fieldId.length > 0)
            root.controller.setTextFieldFocused(fieldId, true)
    }

    function syncMultilineTextEditor() {
        if (multilineTextEditorLoader.status !== Loader.Ready)
            return

        const editor = multilineTextEditorLoader.item as TextArea
        if (!editor)
            return
        const fieldId = formFieldLayer.activeFieldId
        const fieldChanged = root.loadedEditorFieldId !== fieldId
        if (!fieldChanged
                && editor.text === formFieldLayer.activeFieldValue)
            return
        if (fieldChanged && editor.activeFocus
                && root.loadedEditorFieldId.length > 0) {
            root.controller.setTextFieldFocused(
                root.loadedEditorFieldId,
                false)
        }
        root.loadedEditorFieldId = fieldId
        if (editor.text !== formFieldLayer.activeFieldValue) {
            root.syncingTextEditor = true
            try {
                editor.text = formFieldLayer.activeFieldValue
            } finally {
                root.syncingTextEditor = false
            }
        }
        if (fieldChanged && editor.activeFocus && fieldId.length > 0)
            root.controller.setTextFieldFocused(fieldId, true)
    }

    function syncActiveTextEditor() {
        if (root.activeTextFieldMultiline)
            syncMultilineTextEditor()
        else
            syncSingleLineTextEditor()
    }

    function limitedTextPrefix(text, maxLength) {
        let prefixLength = Math.min(text.length, maxLength)
        if (prefixLength > 0 && prefixLength < text.length) {
            const previousCodeUnit = text.charCodeAt(prefixLength - 1)
            const nextCodeUnit = text.charCodeAt(prefixLength)
            if (previousCodeUnit >= 0xD800
                    && previousCodeUnit <= 0xDBFF
                    && nextCodeUnit >= 0xDC00
                    && nextCodeUnit <= 0xDFFF) {
                --prefixLength
            }
        }
        return text.slice(0, prefixLength)
    }

    function activeTextWithinLimit() {
        const activeText = formFieldLayer.activeFieldValue
        const maxLength = root.activeTextFieldInputLengthLimit
        return activeText.length > maxLength
            ? root.limitedTextPrefix(activeText, maxLength)
            : activeText
    }

    function restoreSingleLineTextLimit(fieldId) {
        Qt.callLater(function() {
            if (root.loadedEditorFieldId !== fieldId
                    || singleLineTextEditorLoader.status
                        !== Loader.Ready) {
                return
            }

            const editor = singleLineTextEditorLoader.item as TextField
            const maxLength = root.activeTextFieldInputLengthLimit
            if (!editor
                    || editor.text.length <= maxLength) {
                return
            }

            const acceptedText = root.activeTextWithinLimit()
            const restoredCursorPosition = Math.max(
                0,
                Math.min(
                    acceptedText.length,
                    editor.cursorPosition
                        - (editor.text.length - acceptedText.length)))
            editor.text = acceptedText
            editor.cursorPosition = restoredCursorPosition
        })
    }

    function restoreMultilineTextLimit(fieldId) {
        if (root.loadedEditorFieldId !== fieldId
                || multilineTextEditorLoader.status !== Loader.Ready) {
            return
        }

        const editor = multilineTextEditorLoader.item as TextArea
        const maxLength = root.activeTextFieldInputLengthLimit
        if (!editor || editor.text.length <= maxLength)
            return

        const acceptedText = root.activeTextWithinLimit()
        const restoredCursorPosition = Math.max(
            0,
            Math.min(
                acceptedText.length,
                editor.cursorPosition
                    - (editor.text.length - acceptedText.length)))
        editor.text = acceptedText
        editor.cursorPosition = restoredCursorPosition
    }

    function commitActiveTextEditor() {
        if (root.loadedEditorFieldId.length === 0)
            return

        if (singleLineTextEditorLoader.active
                && singleLineTextEditorLoader.status === Loader.Ready) {
            const singleLineEditor
                = singleLineTextEditorLoader.item as TextField
            if (singleLineEditor
                    && singleLineEditor.text
                        !== formFieldLayer.activeFieldValue) {
                root.controller.setTextFieldText(
                    root.loadedEditorFieldId,
                    singleLineEditor.text)
            }
            return
        }
        if (multilineTextEditorLoader.active
                && multilineTextEditorLoader.status === Loader.Ready) {
            const multilineEditor
                = multilineTextEditorLoader.item as TextArea
            if (!multilineEditor
                    || multilineEditor.text
                        === formFieldLayer.activeFieldValue) {
                return
            }
            root.controller.setTextFieldText(
                root.loadedEditorFieldId,
                multilineEditor.text)
        }
    }

    function focusPendingTextField() {
        if (pendingFocusFieldId.length === 0
                || formFieldLayer.activeFieldId !== pendingFocusFieldId)
            return

        if (root.activeTextFieldMultiline) {
            if (multilineTextEditorLoader.status !== Loader.Ready)
                return
            const multilineEditor
                = multilineTextEditorLoader.item as TextArea
            if (!multilineEditor)
                return
            pendingFocusFieldId = ""
            multilineEditor.forceActiveFocus()
        } else {
            if (singleLineTextEditorLoader.status !== Loader.Ready)
                return
            const singleLineEditor
                = singleLineTextEditorLoader.item as TextField
            if (!singleLineEditor)
                return
            pendingFocusFieldId = ""
            singleLineEditor.forceActiveFocus()
            if (root.activeTextFieldComb) {
                singleLineEditor.deselect()
                singleLineEditor.cursorPosition = singleLineEditor.length
            }
        }
    }

    function clearLoadedTextEditorWhenInactive() {
        Qt.callLater(function() {
            if (singleLineTextEditorLoader.active
                    || multilineTextEditorLoader.active
                    || root.loadedEditorFieldId.length === 0) {
                return
            }

            root.controller.setTextFieldFocused(
                root.loadedEditorFieldId,
                false)
            root.loadedEditorFieldId = ""
        })
    }

    function openActiveDropdown() {
        if (activeDropdownEditorLoader.status !== Loader.Ready)
            return

        const editor = activeDropdownEditorLoader.item as ComboBox
        if (!editor || !editor.enabled || editor.count <= 0)
            return
        editor.forceActiveFocus()
        editor.popup.open()
    }

    function focusActiveListBox() {
        if (activeListBoxEditorLoader.status !== Loader.Ready)
            return

        const editor = activeListBoxEditorLoader.item as Item
        if (editor && editor.enabled)
            editor.forceActiveFocus()
    }

    function finishPlacement() {
        if (width <= 0 || height <= 0) {
            resetPlacement()
            return
        }

        let fieldRect = placementRect
        const clickPlacement = fieldRect.width < 4 && fieldRect.height < 4
        if (clickPlacement) {
            if (placementFieldType
                    === PDFFormDesignerController.Checkbox
                    || placementFieldType
                        === PDFFormDesignerController.RadioButton) {
                const defaultSidePoints = placementFieldType
                    === PDFFormDesignerController.RadioButton
                    ? radioButtonDefaultSidePoints
                    : 24
                const defaultSide = Math.min(
                    defaultSidePoints * Math.max(0.1, pagePointScale),
                    width,
                    height)
                fieldRect = Qt.rect(
                    Math.min(placementStartX, width - defaultSide),
                    Math.min(placementStartY, height - defaultSide),
                    defaultSide,
                    defaultSide)
            } else {
                const defaultWidth = Math.min(160, width)
                const defaultHeight = placementFieldType
                        === PDFFormDesignerController.ListBox
                    ? Math.min(124, height)
                    : Math.min(28, height)
                fieldRect = Qt.rect(
                    Math.min(placementStartX, width - defaultWidth),
                    Math.min(placementStartY, height - defaultHeight),
                    defaultWidth,
                    defaultHeight)
            }
        } else if ((placementFieldType
                        === PDFFormDesignerController.Checkbox
                    || placementFieldType
                        === PDFFormDesignerController.RadioButton)
                   ? fieldRect.width
                       < (placementFieldType
                            === PDFFormDesignerController.RadioButton
                            ? radioButtonMinimumSidePoints
                            : 12) * Math.max(0.1, pagePointScale)
                   : fieldRect.width < 20 || fieldRect.height < 18) {
            resetPlacement()
            return
        }

        const fieldId = placementCombField
            ? controller.addCombField(
                  page,
                  fieldRect.x / width,
                  fieldRect.y / height,
                  fieldRect.width / width,
                  fieldRect.height / height)
            : controller.addField(
                  placementFieldType,
                  page,
                  fieldRect.x / width,
                  fieldRect.y / height,
                  fieldRect.width / width,
                  fieldRect.height / height)
        resetPlacement()
        if (fieldId.length === 0)
            return

        pendingFocusFieldId = placementFieldType
            === PDFFormDesignerController.Text
            ? fieldId
            : ""
        controller.selectTextField(fieldId)
        if (pendingFocusFieldId.length > 0) {
            Qt.callLater(function() {
                root.focusPendingTextField()
            })
        }
    }

    function prepareForSessionChange() {
        commitActiveTextEditor()
        pendingFocusFieldId = ""
        activeFieldFrame.cancelResize()
        formFieldLayer.cancelFieldMove()
        resetPlacement()
    }

    onPooledChanged: prepareForSessionChange()
    onFormsActiveChanged: {
        if (!formsActive) {
            prepareForSessionChange()
            controller.clearTextFieldSelection()
        }
    }
    onEditActiveChanged: {
        if (!editActive)
            formFieldLayer.cancelFieldMove()
    }
    onFieldsVisibleChanged: {
        if (!fieldsVisible) {
            activeFieldFrame.cancelResize()
            formFieldLayer.cancelFieldMove()
            controller.clearTextFieldFocus()
        }
    }
    onFieldsInteractiveChanged: {
        if (!fieldsInteractive) {
            activeFieldFrame.cancelResize()
            formFieldLayer.cancelFieldMove()
            controller.clearTextFieldFocus()
        }
    }
    onTextFieldToolActiveChanged: {
        if (textFieldToolActive)
            formFieldLayer.cancelFieldMove()
        else
            resetPlacement()
    }
    onCheckboxToolActiveChanged: {
        if (checkboxToolActive)
            formFieldLayer.cancelFieldMove()
        else
            resetPlacement()
    }
    onRadioButtonToolActiveChanged: {
        if (radioButtonToolActive)
            formFieldLayer.cancelFieldMove()
        else
            resetPlacement()
    }
    onDropdownToolActiveChanged: {
        if (dropdownToolActive)
            formFieldLayer.cancelFieldMove()
        else
            resetPlacement()
    }
    onListBoxToolActiveChanged: {
        if (listBoxToolActive)
            formFieldLayer.cancelFieldMove()
        else
            resetPlacement()
    }
    onCombFieldToolActiveChanged: {
        if (combFieldToolActive)
            formFieldLayer.cancelFieldMove()
        else
            resetPlacement()
    }

    MouseArea {
        id: placementArea

        anchors.fill: parent
        enabled: root.formsActive
            && root.placementFieldType >= 0
            && !root.pooled
        visible: enabled
        acceptedButtons: Qt.LeftButton
        cursorShape: enabled ? Qt.CrossCursor : Qt.ArrowCursor
        preventStealing: true
        z: 0

        onPressed: mouse => {
            root.placementStartX = Math.max(0, Math.min(width, mouse.x))
            root.placementStartY = Math.max(0, Math.min(height, mouse.y))
            root.placementRect = Qt.rect(
                root.placementStartX,
                root.placementStartY,
                0,
                0)
        }
        onPositionChanged: mouse => {
            if (pressed)
                root.updatePlacement(mouse.x, mouse.y)
        }
        onReleased: mouse => {
            root.updatePlacement(mouse.x, mouse.y)
            root.finishPlacement()
        }
        onCanceled: root.resetPlacement()
    }

    Rectangle {
        x: root.placementRect.x
        y: root.placementRect.y
        width: root.placementRect.width
        height: root.placementRect.height
        visible: placementArea.pressed
            && width > 0
            && height > 0
        color: "#22f58220"
        border.color: root.theme.foxitDarkOrange
        border.width: 1
        z: 1
    }

    MouseArea {
        anchors.fill: parent
        enabled: root.formsActive
            && root.fieldsInteractive
            && root.placementFieldType < 0
            && !root.pooled
        visible: enabled
        acceptedButtons: Qt.LeftButton
        z: 1

        onPressed: mouse => {
            root.controller.clearTextFieldFocus()
            root.controller.clearTextFieldSelection()
            mouse.accepted = false
        }
    }

    component EdgeResizeHandle: Item {
        id: resizeHandle

        required property int horizontal
        required property int vertical
        required property int resizeCursor
        property rect dragStartRect: Qt.rect(0, 0, 0, 0)
        property point dragStartPosition: Qt.point(0, 0)

        x: horizontal < 0
            ? -width / 2
            : horizontal > 0
                ? activeFieldFrame.width - width / 2
                : (activeFieldFrame.width - width) / 2
        y: vertical < 0
            ? -height / 2
            : vertical > 0
                ? activeFieldFrame.height - height / 2
                : (activeFieldFrame.height - height) / 2
        width: root.resizeHandleHitSize
        height: root.resizeHandleHitSize
        visible: (root.formsActive || root.editActive)
            && root.fieldsInteractive
            && !root.pooled
            && !formFieldLayer.moving
        z: 20

        Rectangle {
            anchors.centerIn: parent
            width: root.resizeHandleSize
            height: root.resizeHandleSize
            radius: width / 2
            color: "#f5a55a"
            border.width: 1
            border.color: "#fffdfb"
            Accessible.ignored: true
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            hoverEnabled: true
            preventStealing: true
            containmentMask: QtObject {
                function contains(point) {
                    if (point.x < 0 || point.x >= resizeHandle.width
                            || point.y < 0
                            || point.y >= resizeHandle.height)
                        return false

                    const edgeInset = 3
                    if (resizeHandle.horizontal < 0) {
                        return point.x
                            <= resizeHandle.width / 2 + edgeInset
                    }
                    if (resizeHandle.horizontal > 0) {
                        return point.x
                            >= resizeHandle.width / 2 - edgeInset
                    }
                    if (resizeHandle.vertical < 0) {
                        return point.y
                            <= resizeHandle.height / 2 + edgeInset
                    }
                    return point.y
                        >= resizeHandle.height / 2 - edgeInset
                }
            }
            cursorShape: resizeHandle.resizeCursor
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            //% "Resize form field"
            Accessible.name: qsTrId("ResizeFormField")

            onPressed: mouse => {
                const fieldId = formFieldLayer.activeFieldId
                if (fieldId.length === 0) {
                    mouse.accepted = false
                    return
                }

                resizeHandle.dragStartRect
                    = formFieldLayer.activeFieldRect
                resizeHandle.dragStartPosition = mapToItem(
                    root,
                    mouse.x,
                    mouse.y)
                formFieldLayer.cancelFieldMove()
                activeFieldFrame.beginResize(
                    fieldId,
                    resizeHandle.dragStartRect)
                mouse.accepted = true
            }
            onPositionChanged: mouse => {
                if (!pressed || !activeFieldFrame.resizing)
                    return

                const currentPosition = mapToItem(
                    root,
                    mouse.x,
                    mouse.y)
                activeFieldFrame.updateResize(
                    root.resizedFieldRect(
                        resizeHandle.dragStartRect,
                        resizeHandle.horizontal,
                        resizeHandle.vertical,
                        currentPosition.x
                            - resizeHandle.dragStartPosition.x,
                        currentPosition.y
                            - resizeHandle.dragStartPosition.y))
            }
            onReleased: activeFieldFrame.finishResize()
            onCanceled: activeFieldFrame.cancelResize()

            Keys.onPressed: event => {
                const step = (event.modifiers & Qt.ShiftModifier) ? 10 : 1
                let deltaX = 0
                let deltaY = 0
                if (resizeHandle.horizontal !== 0) {
                    if (event.key === Qt.Key_Left)
                        deltaX = -step
                    else if (event.key === Qt.Key_Right)
                        deltaX = step
                }
                if (resizeHandle.vertical !== 0) {
                    if (event.key === Qt.Key_Up)
                        deltaY = -step
                    else if (event.key === Qt.Key_Down)
                        deltaY = step
                }
                if (deltaX === 0 && deltaY === 0)
                    return

                const fieldId = formFieldLayer.activeFieldId
                const startRect = formFieldLayer.activeFieldRect
                activeFieldFrame.beginResize(fieldId, startRect)
                activeFieldFrame.updateResize(
                    root.resizedFieldRect(
                        startRect,
                        resizeHandle.horizontal,
                        resizeHandle.vertical,
                        deltaX,
                        deltaY))
                activeFieldFrame.finishResize()
                event.accepted = true
            }
        }
    }

    FormFieldLayer {
        id: formFieldLayer

        anchors.fill: parent
        controller: root.controller
        page: root.page
        pooled: root.pooled
        formsActive: root.formsActive
        sourceFieldsVisible: root.formsActive || root.editActive
        fieldsVisible: root.fieldsVisible
        fieldsInteractive: root.fieldsInteractive
        moveEnabled: (root.formsActive || root.editActive)
            && root.fieldsInteractive
            && (root.placementFieldType < 0
                || (formFieldLayer.activeFieldType
                        === root.placementFieldType
                    && (root.placementFieldType
                            === PDFFormDesignerController.Checkbox
                        || root.placementFieldType
                            === PDFFormDesignerController.RadioButton
                        || root.placementFieldType
                            === PDFFormDesignerController.Dropdown
                        || root.placementFieldType
                            === PDFFormDesignerController.ListBox
                        || root.placementFieldType
                            === PDFFormDesignerController.Text)))
            && !root.pooled
        accentColor: root.theme.foxitDarkOrange
        pointScale: root.pagePointScale
        z: 2

        onFieldActivated: fieldId => {
            const field = root.controller.fieldData(fieldId)
            const fieldType = Number(field.type)
            if (fieldType
                    === PDFFormDesignerController.Checkbox) {
                root.pendingFocusFieldId = ""
                const readOnly
                    = (Number(field.flags)
                        & PDFFormDesignerController.ReadOnly) !== 0
                const sourceBacked = Boolean(field.sourceBacked)
                if (sourceBacked && !readOnly) {
                    root.controller.activateSourceCheckbox(
                        fieldId,
                        root.width,
                        root.height)
                } else if (root.formsActive || !readOnly) {
                    const value = String(field.value).toLowerCase()
                    const checked = value === "yes"
                        || value === "true"
                        || value === "on"
                        || value === "1"
                    root.controller.setFieldValue(
                        fieldId,
                        checked ? "Off" : "Yes")
                }
                return
            }
            if (fieldType
                    === PDFFormDesignerController.RadioButton) {
                root.pendingFocusFieldId = ""
                const readOnly
                    = (Number(field.flags)
                        & PDFFormDesignerController.ReadOnly) !== 0
                const sourceBacked = Boolean(field.sourceBacked)
                if (sourceBacked && !readOnly && !root.formsActive) {
                    root.controller.activateSourceCheckbox(
                        fieldId,
                        root.width,
                        root.height)
                } else if (root.formsActive || !readOnly) {
                    const value = String(field.value).toLowerCase()
                    const checked = value === "yes"
                        || value === "true"
                        || value === "on"
                        || value === "1"
                    root.controller.setFieldValue(
                        fieldId,
                        checked ? "Off" : "Yes")
                }
                return
            }
            if (fieldType === PDFFormDesignerController.Dropdown) {
                root.pendingFocusFieldId = ""
                if (!root.formsActive) {
                    Qt.callLater(function() {
                        root.openActiveDropdown()
                    })
                }
                return
            }
            if (fieldType === PDFFormDesignerController.ListBox) {
                root.pendingFocusFieldId = ""
                Qt.callLater(function() {
                    root.focusActiveListBox()
                })
                return
            }
            if (fieldType !== PDFFormDesignerController.Text) {
                root.pendingFocusFieldId = ""
                return
            }
            root.pendingFocusFieldId = fieldId
            Qt.callLater(function() {
                root.focusPendingTextField()
            })
        }
        onActiveFieldChanged: root.syncActiveTextEditor()
        onMovingChanged: {
            if (!moving)
                return

            root.pendingFocusFieldId = ""
            if (singleLineTextEditorLoader.status === Loader.Ready) {
                const singleLineEditor
                    = singleLineTextEditorLoader.item as TextField
                if (singleLineEditor)
                    singleLineEditor.focus = false
            }
            if (multilineTextEditorLoader.status === Loader.Ready) {
                const multilineEditor
                    = multilineTextEditorLoader.item as TextArea
                if (multilineEditor)
                    multilineEditor.focus = false
            }
            if (activeDropdownEditorLoader.status === Loader.Ready) {
                const dropdown = activeDropdownEditorLoader.item as ComboBox
                if (dropdown) {
                    dropdown.popup.close()
                    dropdown.focus = false
                }
            }
            if (activeListBoxEditorLoader.status === Loader.Ready) {
                const listBox = activeListBoxEditorLoader.item as Item
                if (listBox)
                    listBox.focus = false
            }
            root.controller.clearTextFieldFocus()
        }
    }

    Item {
        id: activeFieldFrame

        property bool resizing: false
        property rect resizedRect: Qt.rect(0, 0, 0, 0)
        property string resizingFieldId: ""

        x: (resizing
            ? resizedRect.x
            : formFieldLayer.activeFieldRect.x) * root.width
        y: (resizing
            ? resizedRect.y
            : formFieldLayer.activeFieldRect.y) * root.height
        width: (resizing
            ? resizedRect.width
            : formFieldLayer.activeFieldRect.width) * root.width
        height: (resizing
            ? resizedRect.height
            : formFieldLayer.activeFieldRect.height) * root.height
        visible: formFieldLayer.activeFieldId.length > 0
        z: 4

        function beginResize(fieldId, fieldRect) {
            if (fieldId.length === 0 || fieldRect.width <= 0
                    || fieldRect.height <= 0)
                return

            resizingFieldId = fieldId
            resizedRect = fieldRect
            resizing = true
        }

        function updateResize(fieldRect) {
            if (resizing)
                resizedRect = fieldRect
        }

        function finishResize() {
            if (!resizing)
                return

            const fieldId = resizingFieldId
            const fieldRect = resizedRect
            if (fieldId.length > 0 && root.width > 0 && root.height > 0) {
                root.controller.setFieldRect(
                    fieldId,
                    fieldRect.x,
                    fieldRect.y,
                    fieldRect.width,
                    fieldRect.height)
            }
            resizing = false
            resizingFieldId = ""
        }

        function cancelResize() {
            resizing = false
            resizingFieldId = ""
        }

        Rectangle {
            anchors.fill: parent
            visible: formFieldLayer.activeFieldType
                !== PDFFormDesignerController.Text
                && (activeFieldFrame.resizing
                    || (formFieldLayer.activeFieldType
                        !== PDFFormDesignerController.Checkbox
                        && formFieldLayer.activeFieldType
                        !== PDFFormDesignerController.RadioButton
                        && formFieldLayer.activeFieldType
                            !== PDFFormDesignerController.Dropdown
                        && formFieldLayer.activeFieldType
                            !== PDFFormDesignerController.ListBox))
            color: "transparent"
            border.color: root.theme.foxitDarkOrange
            border.width: root.formsActive ? 2 : 0
            radius: 5
        }

        Loader {
            id: singleLineTextEditorLoader

            anchors.fill: parent
            active: activeFieldFrame.visible
                && formFieldLayer.activeFieldType
                    === PDFFormDesignerController.Text
                && !root.activeTextFieldMultiline
            sourceComponent: activeTextEditorComponent

            onLoaded: {
                root.loadedEditorFieldId = ""
                root.syncSingleLineTextEditor()
                root.focusPendingTextField()
            }
            onActiveChanged: {
                if (!active)
                    root.clearLoadedTextEditorWhenInactive()
            }
        }

        Loader {
            id: multilineTextEditorLoader

            anchors.fill: parent
            active: activeFieldFrame.visible
                && formFieldLayer.activeFieldType
                    === PDFFormDesignerController.Text
                && root.activeTextFieldMultiline
            sourceComponent: activeMultilineTextEditorComponent

            onLoaded: {
                root.loadedEditorFieldId = ""
                root.syncMultilineTextEditor()
                root.focusPendingTextField()
            }
            onActiveChanged: {
                if (!active)
                    root.clearLoadedTextEditorWhenInactive()
            }
        }

        Loader {
            id: activeDropdownEditorLoader

            anchors.fill: parent
            active: activeFieldFrame.visible
                && formFieldLayer.activeFieldType
                    === PDFFormDesignerController.Dropdown
            sourceComponent: activeDropdownEditorComponent
        }

        Loader {
            id: activeListBoxEditorLoader

            anchors.fill: parent
            active: activeFieldFrame.visible
                && formFieldLayer.activeFieldType
                    === PDFFormDesignerController.ListBox
            sourceComponent: activeListBoxEditorComponent
            onLoaded: root.focusActiveListBox()
        }

        ToolButton {
            id: deleteFieldButton

            x: activeFieldFrame.width - width / 2
            y: -height / 2
            width: 20
            height: 20
            visible: root.formsActive
                && !formFieldLayer.activeFieldSourceBacked
                && !root.activeTextFieldComb
                && formFieldLayer.activeFieldType
                    !== PDFFormDesignerController.Dropdown
                && formFieldLayer.activeFieldType
                    !== PDFFormDesignerController.ListBox
                && formFieldLayer.activeFieldType
                    !== PDFFormDesignerController.RadioButton
                && !activeFieldFrame.resizing
                && !formFieldLayer.moving
            enabled: visible
            focusPolicy: Qt.NoFocus
            text: "\u00d7"
            padding: 0
            font.pixelSize: 16
            font.bold: true
            z: 30
            //% "Delete"
            Accessible.name: qsTrId("Delete")
            onClicked: root.controller.removeField(
                formFieldLayer.activeFieldId)

            background: Rectangle {
                radius: 10
                color: deleteFieldButton.down
                    ? "#f5dfcf"
                    : deleteFieldButton.hovered
                        ? root.theme.foxitPaleOrange
                        : "white"
                border.color: root.theme.foxitDarkOrange
                border.width: 1
            }
        }

        Item {
            id: moveFieldHandle

            x: -width / 2
            y: -height / 2
            width: root.resizeHandleHitSize
            height: root.resizeHandleHitSize
            visible: formFieldLayer.moveEnabled
                && (formFieldLayer.activeFieldType
                        === PDFFormDesignerController.Text
                    || formFieldLayer.activeFieldType
                        === PDFFormDesignerController.Dropdown
                    || formFieldLayer.activeFieldType
                        === PDFFormDesignerController.ListBox
                    || formFieldLayer.activeFieldType
                        === PDFFormDesignerController.Checkbox
                    || formFieldLayer.activeFieldType
                        === PDFFormDesignerController.RadioButton)
                && !activeFieldFrame.resizing
            z: 40

            Rectangle {
                anchors.centerIn: parent
                width: 14
                height: 14
                radius: 3
                color: "white"
                border.color: root.theme.foxitDarkOrange
                border.width: 1
                Accessible.ignored: true

                Item {
                    anchors.centerIn: parent
                    width: 6
                    height: 6
                    Accessible.ignored: true

                    Rectangle {
                        x: 0
                        y: 0
                        width: 2
                        height: 2
                        radius: 1
                        color: root.theme.foxitDarkOrange
                    }
                    Rectangle {
                        x: 4
                        y: 0
                        width: 2
                        height: 2
                        radius: 1
                        color: root.theme.foxitDarkOrange
                    }
                    Rectangle {
                        x: 0
                        y: 4
                        width: 2
                        height: 2
                        radius: 1
                        color: root.theme.foxitDarkOrange
                    }
                    Rectangle {
                        x: 4
                        y: 4
                        width: 2
                        height: 2
                        radius: 1
                        color: root.theme.foxitDarkOrange
                    }
                }
            }

            MouseArea {
                id: moveFieldMouse

                property point dragStartPosition: Qt.point(0, 0)

                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                hoverEnabled: true
                preventStealing: true
                cursorShape: Qt.SizeAllCursor
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                //% "Move form field"
                Accessible.name: qsTrId("MoveFormField")

                onPressed: mouse => {
                    forceActiveFocus()
                    root.pendingFocusFieldId = ""
                    activeFieldFrame.cancelResize()
                    dragStartPosition = mapToItem(
                        root,
                        mouse.x,
                        mouse.y)
                    if (!formFieldLayer.beginFieldMove(
                            formFieldLayer.activeFieldId)) {
                        mouse.accepted = false
                    }
                }
                onPositionChanged: mouse => {
                    if (!pressed || !formFieldLayer.moving
                            || root.width <= 0 || root.height <= 0)
                        return

                    const currentPosition = mapToItem(
                        root,
                        mouse.x,
                        mouse.y)
                    formFieldLayer.updateFieldMove(
                        (currentPosition.x - dragStartPosition.x)
                            / root.width,
                        (currentPosition.y - dragStartPosition.y)
                            / root.height)
                }
                onReleased: {
                    if (formFieldLayer.moving)
                        formFieldLayer.finishFieldMove()
                }
                onCanceled: formFieldLayer.cancelFieldMove()

                Keys.onPressed: event => {
                    const step = (event.modifiers & Qt.ShiftModifier)
                        ? 10
                        : 1
                    let deltaX = 0
                    let deltaY = 0
                    if (event.key === Qt.Key_Left)
                        deltaX = -step
                    else if (event.key === Qt.Key_Right)
                        deltaX = step
                    else if (event.key === Qt.Key_Up)
                        deltaY = -step
                    else if (event.key === Qt.Key_Down)
                        deltaY = step
                    else
                        return

                    if (root.width <= 0 || root.height <= 0
                            || !formFieldLayer.beginFieldMove(
                                formFieldLayer.activeFieldId))
                        return
                    formFieldLayer.updateFieldMove(
                        deltaX / root.width,
                        deltaY / root.height)
                    formFieldLayer.finishFieldMove()
                    event.accepted = true
                }
            }
        }

        EdgeResizeHandle {
            horizontal: 0
            vertical: -1
            resizeCursor: Qt.SizeVerCursor
        }

        EdgeResizeHandle {
            horizontal: 1
            vertical: 0
            resizeCursor: Qt.SizeHorCursor
        }

        EdgeResizeHandle {
            horizontal: 0
            vertical: 1
            resizeCursor: Qt.SizeVerCursor
        }

        EdgeResizeHandle {
            horizontal: -1
            vertical: 0
            resizeCursor: Qt.SizeHorCursor
        }
    }

    Component {
        id: hiddenCombCursorDelegate

        Item {
            width: 0
            height: 0
            visible: false
        }
    }

    Component {
        id: activeTextEditorComponent

        TextField {
            id: activeTextEditor

            readonly property int combSlotCount:
                root.activeTextFieldComb
                && formFieldLayer.activeFieldMaxLength <= 512
                && formFieldLayer.activeFieldMaxLength
                    <= Math.max(1, Math.floor(width))
                    ? formFieldLayer.activeFieldMaxLength
                    : 0
            readonly property real combSlotWidth: combSlotCount > 0
                ? width / combSlotCount
                : 0
            readonly property var combCharacters: combSlotCount > 0
                ? Array.from(text)
                : []
            readonly property int combSelectionStartSlot:
                combSlotForTextPosition(selectionStart)
            readonly property int combSelectionEndSlot:
                combSlotForTextPosition(selectionEnd)

            function combSlotForTextPosition(position) {
                const boundedPosition = Math.max(
                    0,
                    Math.min(text.length, Number(position)))
                return Math.min(
                    combSlotCount,
                    Array.from(text.slice(0, boundedPosition)).length)
            }

            function combTextPositionForSlot(slot) {
                const boundedSlot = Math.max(
                    0,
                    Math.min(
                        combCharacters.length,
                        Math.round(Number(slot))))
                return combCharacters.slice(0, boundedSlot).join("").length
            }

            function placeCombCursorAt(positionX) {
                if (combSlotCount <= 0 || combSlotWidth <= 0)
                    return

                const requestedSlot = Math.round(
                    Math.max(0, Math.min(width, Number(positionX)))
                        / combSlotWidth)
                deselect()
                cursorPosition = combTextPositionForSlot(requestedSlot)
            }

            enabled: root.fieldsInteractive
                && (root.formsActive
                    || !(formFieldLayer.activeFieldFlags
                         & PDFFormDesignerController.ReadOnly))
            clip: true
            selectByMouse: true
            activeFocusOnPress: true
            cursorDelegate: root.activeTextFieldComb
                ? hiddenCombCursorDelegate
                : null
            maximumLength: root.activeTextFieldInputLengthLimit
            leftPadding: combSlotCount > 0
                ? 0
                : Math.max(
                      formFieldLayer.textFieldMinimumHorizontalPaddingPoints
                          * root.pagePointScale,
                      height
                          * formFieldLayer.textFieldHorizontalPaddingRatio)
            rightPadding: leftPadding
            topPadding: 0
            bottomPadding: 0
            verticalAlignment: TextInput.AlignVCenter
            color: combSlotCount > 0 ? "transparent" : "#202124"
            font.family: "Helvetica"
            font.pixelSize: Math.max(
                1,
                formFieldLayer.textFieldFontPointSize
                    * root.pagePointScale)
            //% "Enter text"
            placeholderText: qsTrId("EnterFormText")
            placeholderTextColor: combSlotCount > 0
                ? "transparent"
                : palette.placeholderText

            onTextChanged: {
                if (root.syncingTextEditor)
                    return

                const maxLength = root.activeTextFieldInputLengthLimit
                if (root.loadedEditorFieldId.length > 0
                        && text.length > maxLength) {
                    root.restoreSingleLineTextLimit(
                        root.loadedEditorFieldId)
                    return
                }
                if (root.loadedEditorFieldId.length > 0
                        && text !== formFieldLayer.activeFieldValue) {
                    root.controller.setTextFieldText(
                        root.loadedEditorFieldId,
                        text)
                }
            }
            onActiveFocusChanged: {
                if (root.loadedEditorFieldId.length > 0)
                    root.controller.setTextFieldFocused(
                        root.loadedEditorFieldId,
                        activeFocus)
            }

            TapHandler {
                enabled: root.formsActive
                acceptedButtons: Qt.LeftButton
                onTapped: eventPoint => {
                    root.controller.selectTextField(
                        root.loadedEditorFieldId)
                    activeTextEditor.placeCombCursorAt(
                        eventPoint.position.x)
                }
            }

            Repeater {
                model: activeTextEditor.combSlotCount

                delegate: Item {
                    id: combCell

                    required property int index
                    readonly property bool selected:
                        index >= Math.min(
                            activeTextEditor.combSelectionStartSlot,
                            activeTextEditor.combSelectionEndSlot)
                        && index < Math.max(
                            activeTextEditor.combSelectionStartSlot,
                            activeTextEditor.combSelectionEndSlot)

                    x: index * activeTextEditor.combSlotWidth
                    width: activeTextEditor.combSlotWidth
                    height: activeTextEditor.height
                    z: 2
                    Accessible.ignored: true

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 2
                        visible: combCell.selected
                        color: activeTextEditor.selectionColor
                    }

                    Text {
                        anchors.fill: parent
                        text: index < activeTextEditor.combCharacters.length
                            ? activeTextEditor.combCharacters[index]
                            : ""
                        color: combCell.selected
                            ? activeTextEditor.selectedTextColor
                            : "#202124"
                        font.family: activeTextEditor.font.family
                        font.pixelSize: activeTextEditor.font.pixelSize
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                        Accessible.ignored: true
                    }
                }
            }

            background: Rectangle {
                color: "white"
                border.color: activeTextEditor.activeFocus
                        || root.formsActive
                    ? root.theme.foxitDarkOrange
                    : formFieldLayer.textFieldBorderColor
                border.width: activeTextEditor.activeFocus
                        || root.formsActive
                    ? 2
                    : formFieldLayer.textFieldBorderPointWidth
                        * root.pagePointScale
                radius: 0

                Repeater {
                    model: Math.max(
                        0,
                        activeTextEditor.combSlotCount - 1)

                    delegate: Rectangle {
                        required property int index

                        x: (index + 1)
                            * activeTextEditor.combSlotWidth
                            - width / 2
                        y: parent.border.width
                        width: 1
                        height: Math.max(
                            0,
                            parent.height - parent.border.width * 2)
                        color: parent.border.color
                        Accessible.ignored: true
                    }
                }
            }
        }
    }

    Component {
        id: activeMultilineTextEditorComponent

        TextArea {
            id: activeMultilineTextEditor

            enabled: root.fieldsInteractive
                && (root.formsActive
                    || !(formFieldLayer.activeFieldFlags
                         & PDFFormDesignerController.ReadOnly))
            clip: true
            selectByMouse: true
            activeFocusOnPress: true
            wrapMode: TextEdit.WrapAtWordBoundaryOrAnywhere
            leftPadding: Math.max(
                formFieldLayer.textFieldMinimumHorizontalPaddingPoints
                    * root.pagePointScale,
                height * formFieldLayer.textFieldHorizontalPaddingRatio)
            rightPadding: leftPadding
            topPadding: Math.max(2, leftPadding * 0.35)
            bottomPadding: topPadding
            color: "#202124"
            font.family: "Helvetica"
            font.pixelSize: Math.max(
                1,
                formFieldLayer.textFieldFontPointSize
                    * root.pagePointScale)
            //% "Enter text"
            placeholderText: qsTrId("EnterFormText")

            onTextChanged: {
                if (root.syncingTextEditor)
                    return

                const maxLength = root.activeTextFieldInputLengthLimit
                if (root.loadedEditorFieldId.length > 0
                        && text.length > maxLength) {
                    root.restoreMultilineTextLimit(
                        root.loadedEditorFieldId)
                    return
                }
                if (root.loadedEditorFieldId.length > 0
                        && text !== formFieldLayer.activeFieldValue) {
                    root.controller.setTextFieldText(
                        root.loadedEditorFieldId,
                        text)
                }
            }
            onActiveFocusChanged: {
                if (root.loadedEditorFieldId.length > 0) {
                    root.controller.setTextFieldFocused(
                        root.loadedEditorFieldId,
                        activeFocus)
                }
            }

            TapHandler {
                enabled: root.formsActive
                acceptedButtons: Qt.LeftButton
                onTapped: root.controller.selectTextField(
                    root.loadedEditorFieldId)
            }

            background: Rectangle {
                color: "white"
                border.color: activeMultilineTextEditor.activeFocus
                        || root.formsActive
                    ? root.theme.foxitDarkOrange
                    : formFieldLayer.textFieldBorderColor
                border.width: activeMultilineTextEditor.activeFocus
                        || root.formsActive
                    ? 2
                    : formFieldLayer.textFieldBorderPointWidth
                        * root.pagePointScale
                radius: 0
            }
        }
    }


    Component {
        id: activeDropdownEditorComponent

        ComboBox {
            id: activeDropdownEditor

            model: formFieldLayer.activeFieldOptions
            currentIndex: formFieldLayer.activeFieldOptions.indexOf(
                formFieldLayer.activeFieldValue)
            enabled: root.fieldsInteractive
                && (root.formsActive
                    || !(formFieldLayer.activeFieldFlags
                         & PDFFormDesignerController.ReadOnly))
            activeFocusOnTab: true
            leftPadding: 7
            rightPadding: 28

            onActivated: index => {
                if (index < 0
                        || index >= formFieldLayer.activeFieldOptions.length
                        || formFieldLayer.activeFieldId.length === 0) {
                    return
                }
                const field = root.controller.fieldData(
                    formFieldLayer.activeFieldId)
                const sourceBacked = Boolean(field.sourceBacked)
                const optionsDirty = Boolean(field.optionsDirty)
                const readOnly = (Number(field.flags)
                    & PDFFormDesignerController.ReadOnly) !== 0
                if (sourceBacked && !readOnly && !optionsDirty) {
                    root.controller.activateSourceDropdown(
                        formFieldLayer.activeFieldId,
                        index)
                } else {
                    root.controller.setFieldValue(
                        formFieldLayer.activeFieldId,
                        String(formFieldLayer.activeFieldOptions[index]))
                }
            }

            background: Rectangle {
                color: "white"
                border.color: activeDropdownEditor.activeFocus
                        || root.formsActive
                    ? root.theme.foxitDarkOrange
                    : "#7f8c99"
                border.width: activeDropdownEditor.activeFocus
                        || root.formsActive
                    ? 2
                    : 1
                radius: 5
            }
        }
    }

    Component {
        id: activeListBoxEditorComponent

        FocusScope {
            id: activeListBoxEditor

            property int navigationIndex: -1
            readonly property string navigationFieldId:
                formFieldLayer.activeFieldId
            readonly property int optionCount:
                formFieldLayer.activeFieldOptions.length
            readonly property real rowHeight: Math.max(
                1,
                formFieldLayer.textFieldFontPointSize
                    * formFieldLayer.listBoxRowHeightRatio
                    * formFieldLayer.pointScale)
            readonly property real contentInset: Math.max(
                1,
                formFieldLayer.textFieldBorderPointWidth
                    * formFieldLayer.pointScale)

            function selectedValues() {
                const values = []
                for (let index = 0;
                        index < formFieldLayer.activeFieldSelectedValues.length;
                        ++index) {
                    values.push(String(
                        formFieldLayer.activeFieldSelectedValues[index]))
                }
                return values
            }

            function firstSelectedOptionIndex() {
                const values = selectedValues()
                for (let index = 0; index < optionCount; ++index) {
                    if (values.indexOf(String(
                            formFieldLayer.activeFieldOptions[index])) >= 0) {
                        return index
                    }
                }
                return -1
            }

            function setNavigationIndex(index) {
                if (optionCount <= 0) {
                    navigationIndex = -1
                    return
                }

                navigationIndex = Math.max(
                    0,
                    Math.min(optionCount - 1, Number(index)))
                activeListBoxView.positionViewAtIndex(
                    navigationIndex,
                    ListView.Contain)
            }

            function resetNavigation() {
                const selectedIndex = firstSelectedOptionIndex()
                setNavigationIndex(selectedIndex >= 0 ? selectedIndex : 0)
                Qt.callLater(function() {
                    activeListBoxEditor.alignViewToSelection()
                })
            }

            function alignViewToSelection() {
                if (optionCount <= 0 || activeListBoxView.height <= 0)
                    return

                const selectedIndex = firstSelectedOptionIndex()
                const visibleRows = Math.max(
                    1,
                    Math.floor(activeListBoxView.height / rowHeight))
                const maximumTopIndex = Math.max(
                    0,
                    optionCount - visibleRows)
                const firstVisibleIndex = Math.max(
                    0,
                    Math.min(
                        maximumTopIndex,
                        (selectedIndex >= 0 ? selectedIndex : 0)
                            - Math.floor(visibleRows / 2)))
                activeListBoxView.positionViewAtIndex(
                    firstVisibleIndex,
                    ListView.Beginning)
            }

            function toggleOption(index) {
                if (index < 0
                        || index >= optionCount
                        || formFieldLayer.activeFieldId.length === 0) {
                    return
                }

                const field = root.controller.fieldData(
                    formFieldLayer.activeFieldId)
                const sourceBacked = Boolean(field.sourceBacked)
                const optionsDirty = Boolean(field.optionsDirty)
                const readOnly = (Number(field.flags)
                    & PDFFormDesignerController.ReadOnly) !== 0
                if (readOnly)
                    return

                if (sourceBacked && !optionsDirty) {
                    root.controller.toggleSourceChoiceOption(
                        formFieldLayer.activeFieldId,
                        index)
                } else {
                    const toggledOption = String(
                        formFieldLayer.activeFieldOptions[index])
                    const currentValues = selectedValues()
                    const removeOption
                        = currentValues.indexOf(toggledOption) >= 0
                    const updatedValues = []
                    for (let optionIndex = 0;
                            optionIndex < optionCount;
                            ++optionIndex) {
                        const option = String(
                            formFieldLayer.activeFieldOptions[optionIndex])
                        const selected = currentValues.indexOf(option) >= 0
                        if (optionIndex === index
                                ? !removeOption
                                : selected) {
                            updatedValues.push(option)
                        }
                    }
                    root.controller.setFieldSelectedValues(
                        formFieldLayer.activeFieldId,
                        updatedValues)
                }
            }

            function moveNavigation(offset) {
                if (optionCount <= 0)
                    return

                const currentIndex = navigationIndex >= 0
                    ? navigationIndex
                    : 0
                setNavigationIndex(currentIndex + offset)
            }

            enabled: root.fieldsInteractive
                && (root.formsActive
                    || !(formFieldLayer.activeFieldFlags
                         & PDFFormDesignerController.ReadOnly))
            activeFocusOnTab: true

            onNavigationFieldIdChanged: resetNavigation()
            onRowHeightChanged: resetNavigation()
            onOptionCountChanged: {
                if (navigationIndex < 0 || navigationIndex >= optionCount)
                    resetNavigation()
            }
            Component.onCompleted: resetNavigation()

            Keys.onPressed: event => {
                if (event.key === Qt.Key_Up) {
                    activeListBoxEditor.moveNavigation(-1)
                } else if (event.key === Qt.Key_Down) {
                    activeListBoxEditor.moveNavigation(1)
                } else if (event.key === Qt.Key_Home) {
                    activeListBoxEditor.setNavigationIndex(0)
                } else if (event.key === Qt.Key_End) {
                    activeListBoxEditor.setNavigationIndex(optionCount - 1)
                } else if (event.key === Qt.Key_Space
                           || event.key === Qt.Key_Return
                           || event.key === Qt.Key_Enter) {
                    activeListBoxEditor.toggleOption(navigationIndex)
                } else {
                    return
                }
                event.accepted = true
            }

            Rectangle {
                anchors.fill: parent
                color: "white"
                border.color: activeListBoxEditor.activeFocus
                        || root.formsActive
                    ? root.theme.foxitDarkOrange
                    : "#7f8c99"
                border.width: activeListBoxEditor.activeFocus
                        || root.formsActive
                    ? 2
                    : 1
                radius: 2
            }

            ListView {
                id: activeListBoxView

                anchors.fill: parent
                anchors.margins: activeListBoxEditor.contentInset
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                model: formFieldLayer.activeFieldOptions
                currentIndex: activeListBoxEditor.navigationIndex
                highlightMoveDuration: 0
                onHeightChanged: Qt.callLater(function() {
                    activeListBoxEditor.alignViewToSelection()
                })

                delegate: ItemDelegate {
                    id: listBoxOptionDelegate

                    required property int index
                    required property string modelData
                    readonly property bool optionSelected:
                        formFieldLayer.activeFieldSelectedValues.indexOf(
                            modelData) >= 0
                    readonly property bool navigationCurrent:
                        index === activeListBoxEditor.navigationIndex

                    width: ListView.view.width
                    height: activeListBoxEditor.rowHeight
                    leftPadding: 5
                    rightPadding: 5
                    topPadding: 0
                    bottomPadding: 0
                    text: modelData
                    focusPolicy: Qt.NoFocus
                    highlighted: optionSelected
                    Accessible.role: Accessible.CheckBox
                    Accessible.name: text
                    Accessible.checked: optionSelected
                    onClicked: {
                        activeListBoxEditor.forceActiveFocus()
                        activeListBoxEditor.setNavigationIndex(index)
                        activeListBoxEditor.toggleOption(index)
                    }

                    contentItem: Label {
                        text: listBoxOptionDelegate.text
                        color: listBoxOptionDelegate.highlighted
                            ? "white"
                            : "#202124"
                        font.family: "Helvetica"
                        font.pixelSize: Math.max(
                            1,
                            Math.round(
                                formFieldLayer.textFieldFontPointSize
                                    * formFieldLayer.pointScale))
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                    }

                    background: Rectangle {
                        color: listBoxOptionDelegate.highlighted
                            ? root.theme.foxitDarkOrange
                            : listBoxOptionDelegate.hovered
                                ? root.theme.foxitPaleOrange
                                : "transparent"
                        border.color:
                            listBoxOptionDelegate.navigationCurrent
                                && activeListBoxEditor.activeFocus
                            ? listBoxOptionDelegate.highlighted
                                ? "white"
                                : root.theme.foxitDarkOrange
                            : "transparent"
                        border.width:
                            listBoxOptionDelegate.navigationCurrent
                                && activeListBoxEditor.activeFocus
                            ? 1
                            : 0
                    }
                }

                ScrollBar.vertical: ScrollBar {
                    policy: ScrollBar.AsNeeded
                }
            }
        }
    }
}
