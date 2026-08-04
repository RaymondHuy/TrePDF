pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import TrePDF

Item {
    id: root

    required property AppTheme theme
    required property PDFDocumentSession session
    property bool formsTabActive: false
    property bool editTabActive: false
    property bool formFieldsVisible: false
    property bool formFieldsInteractive: false
    property bool textFieldToolActive: false
    property bool checkboxToolActive: false
    property bool radioButtonToolActive: false
    property bool dropdownToolActive: false
    property string transientStatus
    readonly property string selectedFormFieldId:
        session.formDesignerController.selectedFieldId
    readonly property var selectedFormField: selectedFormFieldId.length > 0
        ? session.formDesignerController.fieldData(selectedFormFieldId)
        : ({})
    readonly property bool textFieldOptionsPanelActive: formsTabActive
        && selectedFormFieldId.length > 0
        && Number(selectedFormField.type)
            === PDFFormDesignerController.Text
    readonly property bool dropdownOptionsPanelActive: formsTabActive
        && selectedFormFieldId.length > 0
        && Number(selectedFormField.type)
            === PDFFormDesignerController.Dropdown
    readonly property bool radioButtonOptionsPanelActive: formsTabActive
        && selectedFormFieldId.length > 0
        && Number(selectedFormField.type)
            === PDFFormDesignerController.RadioButton
    readonly property bool hasPendingPanelEdits: {
        if (textFieldOptionsPanelLoader.status === Loader.Ready) {
            const textPanel = textFieldOptionsPanelLoader.item
                as TextFieldOptionsPanel
            if (textPanel && textPanel.maxLengthEditPending)
                return true
        }

        if (radioButtonOptionsPanelLoader.status === Loader.Ready) {
            const radioPanel = radioButtonOptionsPanelLoader.item
                as RadioButtonOptionsPanel
            if (radioPanel && radioPanel.groupNameEditPending)
                return true
        }
        return false
    }

    signal openRequested()

    onTextFieldOptionsPanelActiveChanged: {
        if (textFieldOptionsPanelActive)
            session.panelCollapsed = false
    }

    function setZoom(value) {
        session.zoom = Math.max(0.1, Math.min(5.0, value))
    }

    function fitToWidth() {
        const maximumViewPageWidth = session.viewRotation % 180 === 0
            ? session.document.maximumPageWidth
            : session.document.maximumPageHeight
        if (!session.document.loaded
                || maximumViewPageWidth <= 0
                || documentView.viewportWidth <= 0) {
            return
        }

        setZoom((documentView.viewportWidth - 96)
                / maximumViewPageWidth)
    }

    function navigateToPage(page) {
        documentView.navigateToPage(page)
    }

    function rotateView(quarterTurns) {
        if (!session.document.loaded)
            return

        const currentPage = session.document.currentPage
        session.rotateView(quarterTurns)
        Qt.callLater(function() {
            if (root.session.document.loaded)
                documentView.navigateToPage(currentPage)
        })
    }

    function prepareInlineStyleChange() {
        return session.editController.textEditActive
            || documentView.beginSelectedEditing()
    }

    function initializeView() {
        if (session.initialFitPending) {
            fitToWidth()
            session.initialFitPending = false
            Qt.callLater(function() {
                if (root.session.document.loaded)
                    documentView.navigateToPage(root.session.document.currentPage)
            })
        } else {
            documentView.restorePosition(session.scrollX, session.scrollY)
        }
    }

    function commitPendingPanelEdits() {
        if (textFieldOptionsPanelLoader.status === Loader.Ready) {
            const textPanel = textFieldOptionsPanelLoader.item
                as TextFieldOptionsPanel
            if (textPanel && !textPanel.commitPendingEdit())
                return false
        }

        if (radioButtonOptionsPanelLoader.status === Loader.Ready) {
            const radioPanel = radioButtonOptionsPanelLoader.item
                as RadioButtonOptionsPanel
            if (radioPanel && !radioPanel.commitPendingEdit())
                return false
        }
        return true
    }

    function prepareForDeactivation() {
        if (!commitPendingPanelEdits())
            return false

        documentView.prepareForSessionChange()
        session.scrollX = documentView.contentX
        session.scrollY = documentView.contentY
        session.prepareForDeactivation()
        return true
    }

    Component.onDestruction: prepareForDeactivation()

    Timer {
        interval: 0
        running: true
        repeat: false
        onTriggered: root.initializeView()
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        DocumentView {
            id: documentView

            Layout.fillWidth: true
            Layout.fillHeight: true
            theme: root.theme
            document: root.session.document
            editController: root.session.editController
            formController: root.session.formController
            formDesignerController: root.session.formDesignerController
            formsTabActive: root.formsTabActive
            editTabActive: root.editTabActive
            formFieldsVisible: root.formFieldsVisible
            formFieldsInteractive: root.formFieldsInteractive
            textFieldToolActive: root.textFieldToolActive
            checkboxToolActive: root.checkboxToolActive
            radioButtonToolActive: root.radioButtonToolActive
            dropdownToolActive: root.dropdownToolActive
            zoom: root.session.zoom
            viewRotation: root.session.viewRotation
            transientStatus: root.transientStatus
            onOpenRequested: root.openRequested()
        }

        Loader {
            id: textFieldOptionsPanelLoader

            Layout.preferredWidth: active
                ? root.session.panelCollapsed ? 42 : 264
                : 0
            Layout.fillHeight: true
            active: root.session.document.loaded
                && root.textFieldOptionsPanelActive
            visible: active

            sourceComponent: Component {
                TextFieldOptionsPanel {
                    theme: root.theme
                    controller: root.session.formDesignerController
                    fieldId: root.selectedFormFieldId
                    collapsed: root.session.panelCollapsed
                    onCollapsedChangeRequested: collapsed => {
                        root.session.panelCollapsed = collapsed
                    }
                }
            }
        }

        Loader {
            id: dropdownOptionsPanelLoader

            Layout.preferredWidth: active
                ? root.session.panelCollapsed ? 42 : 264
                : 0
            Layout.fillHeight: true
            active: root.session.document.loaded
                && root.dropdownOptionsPanelActive
            visible: active

            sourceComponent: Component {
                DropdownOptionsPanel {
                    theme: root.theme
                    controller: root.session.formDesignerController
                    fieldId: root.selectedFormFieldId
                    collapsed: root.session.panelCollapsed
                    onCollapsedChangeRequested: collapsed => {
                        root.session.panelCollapsed = collapsed
                    }
                }
            }
        }

        Loader {
            id: textFormatPanelLoader

            Layout.preferredWidth: active
                ? root.session.panelCollapsed ? 42 : 264
                : 0
            Layout.fillHeight: true
            active: root.session.document.loaded
                && !root.textFieldOptionsPanelActive
                && !root.dropdownOptionsPanelActive
                && !root.radioButtonOptionsPanelActive
                && root.session.editController.tool !== PDFEditController.BrowseTool
            visible: active

            sourceComponent: Component {
                TextFormatPanel {
                    theme: root.theme
                    collapsed: root.session.panelCollapsed
                    availableFontFamilies:
                        root.session.editController.availableFontFamilies
                    fontFamiliesForFilter: function(filter) {
                        return root.session.editController.fontFamiliesForFilter(filter)
                    }
                    selectedFontFamily:
                        root.session.editController.selectedFontFamily
                    selectedFontBold: root.session.editController.selectedFontBold
                    selectedFontItalic:
                        root.session.editController.selectedFontItalic
                    selectedFontUnderline:
                        root.session.editController.selectedFontUnderline
                    selectedColor: root.session.editController.selectedColor
                    selectedParagraphAlignment:
                        root.session.editController.selectedParagraphAlignment
                    formattingEnabled:
                        root.session.editController.selectedEditable
                        && (root.session.editController.selectedKind === "text"
                            || root.session.editController.selectedKind
                                === "addedText")
                    onFontFamilyChangeRequested: fontFamily => {
                        root.session.editController.setSelectedFontFamily(fontFamily)
                    }
                    onFontBoldChangeRequested: enabled => {
                        const editWasActive = root.session.editController.textEditActive
                        if (root.prepareInlineStyleChange()) {
                            root.session.editController.setSelectedFontBold(
                                editWasActive
                                    ? enabled
                                    : !root.session.editController.selectedFontBold)
                        }
                    }
                    onFontItalicChangeRequested: enabled => {
                        const editWasActive = root.session.editController.textEditActive
                        if (root.prepareInlineStyleChange()) {
                            root.session.editController.setSelectedFontItalic(
                                editWasActive
                                    ? enabled
                                    : !root.session.editController.selectedFontItalic)
                        }
                    }
                    onFontUnderlineChangeRequested: enabled => {
                        const editWasActive = root.session.editController.textEditActive
                        if (root.prepareInlineStyleChange()) {
                            root.session.editController.setSelectedFontUnderline(
                                editWasActive
                                    ? enabled
                                    : !root.session.editController.selectedFontUnderline)
                        }
                    }
                    onFontColorChangeRequested: color => {
                        root.session.editController.setSelectedColor(color)
                    }
                    onParagraphAlignmentChangeRequested: alignment => {
                        root.session.editController.setSelectedParagraphAlignment(
                            alignment)
                    }
                    onCollapsedChangeRequested: collapsed => {
                        root.session.panelCollapsed = collapsed
                    }
                }
            }
        }

        Loader {
            id: radioButtonOptionsPanelLoader

            Layout.preferredWidth: active
                ? root.session.panelCollapsed ? 42 : 264
                : 0
            Layout.fillHeight: true
            active: root.session.document.loaded
                && root.radioButtonOptionsPanelActive
            visible: active

            sourceComponent: Component {
                RadioButtonOptionsPanel {
                    theme: root.theme
                    controller: root.session.formDesignerController
                    fieldId: root.selectedFormFieldId
                    collapsed: root.session.panelCollapsed
                    onCollapsedChangeRequested: collapsed => {
                        root.session.panelCollapsed = collapsed
                    }
                }
            }
        }
    }
}
