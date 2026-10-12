pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TrePDF

Rectangle {
    id: root
    objectName: "headerSyncPanel"

    required property AppTheme theme
    required property PDFEditController editController
    property bool allMatchingPages: true
    property string pageRange: ""
    property string saveError: ""
    readonly property bool inputFocused: pageRangeInput.activeFocus
    readonly property bool syncActive: editController.headerSyncLoading
        || editController.headerSyncMatchCount > 0
        || editController.headerSyncError.length > 0

    signal pageScopeChangeRequested(bool allMatchingPages)
    signal pageRangeChangeRequested(string pages)

    implicitWidth: 264
    implicitHeight: panelContent.implicitHeight + 24
    color: theme.ribbonSurface

    component FieldLabel: Label {
        Layout.fillWidth: true
        color: root.theme.ribbonTextColor
        wrapMode: Text.Wrap
        font.pixelSize: 11
    }

    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 1
        color: root.theme.ribbonBorder
        Accessible.ignored: true
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: root.theme.ribbonBorder
        Accessible.ignored: true
    }

    ScrollView {
        id: panelScroll
        anchors.fill: parent
        anchors.margins: 12
        contentWidth: availableWidth
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ColumnLayout {
            id: panelContent
            width: panelScroll.availableWidth
            spacing: 8

            CheckBox {
                id: syncEnabled
                objectName: "headerSyncEnabled"
                Layout.fillWidth: true
                enabled: root.editController.headerSyncAvailable
                    && !root.editController.saving
                checked: root.syncActive
                //% "Apply to similar headers"
                text: qsTrId("HeaderSyncEnabled")
                palette.highlight: root.theme.foxitOrange
                contentItem: Label {
                    text: syncEnabled.text
                    leftPadding: syncEnabled.indicator.width + syncEnabled.spacing
                    color: root.theme.ribbonTextColor
                    wrapMode: Text.Wrap
                }
                onClicked: {
                    if (checked)
                        root.editController.findSimilarHeaders()
                    else
                        root.editController.cancelHeaderSync()
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: syncEnabled.checked
                spacing: 8

                FieldLabel {
                    //% "Choose Save to apply your changes to the matching pages and save the PDF."
                    text: qsTrId("HeaderSyncEditHint")
                }

                FieldLabel {
                    objectName: "headerSyncMatchSummary"
                    text: root.editController.headerSyncLoading
                        //% "Finding similar headers…"
                        ? qsTrId("HeaderSyncLoading")
                        //% "%1 matching pages (including this page)"
                        : qsTrId("HeaderSyncMatchCount")
                            .arg(root.editController.headerSyncMatchCount)
                    font.weight: Font.DemiBold
                }

                FieldLabel {
                    objectName: "headerSyncMatchPages"
                    visible: root.editController.headerSyncMatchPages.length > 0
                    //% "PDF pages: %1"
                    text: qsTrId("HeaderSyncMatchPages")
                        .arg(root.editController.headerSyncMatchPages)
                    wrapMode: Text.WrapAnywhere
                    maximumLineCount: 3
                    elide: Text.ElideRight
                }

                ButtonGroup { id: pageScope }

                RadioButton {
                    id: allPages
                    objectName: "headerSyncAllPages"
                    Layout.fillWidth: true
                    ButtonGroup.group: pageScope
                    checked: root.allMatchingPages
                    enabled: !root.editController.headerSyncLoading
                        && !root.editController.saving
                    //% "All matching pages"
                    text: qsTrId("HeaderSyncAllPages")
                    palette.highlight: root.theme.foxitOrange
                    onCheckedChanged: {
                        if (checked)
                            root.pageScopeChangeRequested(true)
                    }
                }

                RadioButton {
                    id: selectedPages
                    objectName: "headerSyncSelectedPages"
                    Layout.fillWidth: true
                    ButtonGroup.group: pageScope
                    checked: !root.allMatchingPages
                    enabled: !root.editController.headerSyncLoading
                        && !root.editController.saving
                    //% "Selected pages"
                    text: qsTrId("HeaderSelectedPages")
                    palette.highlight: root.theme.foxitOrange
                    onCheckedChanged: {
                        if (checked)
                            root.pageScopeChangeRequested(false)
                    }
                }

                TextField {
                    id: pageRangeInput
                    objectName: "headerSyncPageRange"
                    Layout.fillWidth: true
                    enabled: selectedPages.checked
                        && !root.editController.headerSyncLoading
                        && !root.editController.saving
                    maximumLength: 4096
                    selectByMouse: true
                    text: root.pageRange
                    onTextChanged: {
                        if (text !== root.pageRange)
                            root.pageRangeChangeRequested(text)
                    }
                    //% "For example: 1-3, 5"
                    placeholderText: qsTrId("HeaderPageRangePlaceholder")
                    //% "Selected pages"
                    Accessible.name: qsTrId("HeaderSelectedPages")
                    palette.highlight: root.theme.foxitOrange
                }

                FieldLabel {
                    objectName: "headerSyncError"
                    visible: text.length > 0
                    text: root.saveError.length > 0
                        ? root.saveError : root.editController.headerSyncError
                    color: "#b3261e"
                }
            }
        }
    }
}
