pragma ComponentBehavior: Bound

import QtQuick
import TrePDF

Item {
    id: root

    required property AppTheme theme
    required property PDFEditController editController
    required property PDFFormController formController
    required property PDFFormDesignerController formDesignerController
    required property int page
    required property real pagePointScale
    required property bool pooled
    required property bool formsTabActive
    required property bool editTabActive
    required property bool formFieldsVisible
    required property bool formFieldsInteractive
    required property bool textFieldToolActive
    required property bool checkboxToolActive
    required property bool draftTextureCurrent
    required property bool draftTextureReady
    required property Item viewportItem
    required property real viewportContentX
    required property real viewportContentY
    required property var viewportTransformState

    readonly property bool formDesignMode: !pooled
        && (formsTabActive || editTabActive)
    readonly property bool formRuntimeInteractive: !pooled
        && formController.interactive
        && !formController.javascriptTransitionPending
        && !editController.saving
        && editController.tool === PDFEditController.BrowseTool
        && !textFieldToolActive
        && !checkboxToolActive

    function prepareForSessionChange() {
        editOverlay.prepareForSessionChange()
        formTextFieldOverlay.prepareForSessionChange()
    }

    function beginSelectedEditing() {
        editOverlay.beginSelectedEditing()
    }

    PDFFormView {
        id: formView

        anchors.fill: parent
        controller: root.pooled ? null : root.formController
        formDesignerController: root.pooled
            ? null
            : root.formDesignerController
        formDesignMode: root.formDesignMode
        page: root.page
        interactive: root.formRuntimeInteractive
        activeFocusOnTab: interactive
        z: 1

        Keys.onPressed: event => {
            event.accepted = choiceOverlay.handleKey(
                event.key,
                event.modifiers,
                event.text)
        }

        onChoiceFieldFocusFinished: (sourcePage,
                                     annotationIndex,
                                     identityToken,
                                     focused) => {
            choiceOverlay.completeComboFocus(
                sourcePage,
                annotationIndex,
                identityToken,
                focused)
        }

        FormChoiceOverlay {
            id: choiceOverlay

            anchors.fill: parent
            theme: root.theme
            fields: formView.choiceFields
            interactive: formView.interactive
            page: root.page
            focusedAnnotationIndex:
                root.formController.focusedPage === root.page
                ? root.formController.focusedAnnotationIndex
                : -1
            viewportItem: root.viewportItem
            viewportContentX: root.viewportContentX
            viewportContentY: root.viewportContentY
            viewportTransformState: root.viewportTransformState

            onFieldFocusRequested: (sourcePage,
                                    annotationIndex,
                                    identityToken) =>
                formView.focusChoiceField(
                    sourcePage,
                    annotationIndex,
                    identityToken)
            onOptionSelected: (sourcePage,
                               annotationIndex,
                               optionIndex,
                               selected,
                               toggle,
                               identityToken) => {
                formView.selectChoiceOption(
                    sourcePage,
                    annotationIndex,
                    optionIndex,
                    selected,
                    toggle,
                    identityToken)
            }
        }
    }

    EditOverlay {
        id: editOverlay

        anchors.fill: parent
        page: root.page
        pooled: root.pooled
        theme: root.theme
        controller: root.editController
        draftTextureCurrent: root.draftTextureCurrent
        draftTextureReady: root.draftTextureReady
        z: 2

        onPointerInteractionStarted: {
            root.formDesignerController.clearTextFieldFocus()
            root.formDesignerController.clearTextFieldSelection()
        }
    }

    FormTextFieldOverlay {
        id: formTextFieldOverlay

        anchors.fill: parent
        theme: root.theme
        controller: root.formDesignerController
        page: root.page
        pagePointScale: root.pagePointScale
        pooled: root.pooled
        formsActive: root.formsTabActive
        editActive: root.editTabActive
        fieldsVisible: root.formFieldsVisible
        fieldsInteractive: root.formFieldsInteractive
        textFieldToolActive: root.textFieldToolActive
        checkboxToolActive: root.checkboxToolActive
        z: 3
    }
}
