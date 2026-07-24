pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TrePDF

Column {
    id: root

    required property AppTheme theme
    required property PDFDocument document
    required property PDFEditController editController

    signal openRequested()
    signal saveRequested()
    signal saveAsRequested()
    signal pageRequested(int page)
    signal zoomOutRequested()
    signal zoomInRequested()
    signal fitWidthRequested()
    signal rotateRequested(int quarterTurns)
    signal toolRequested(string toolName)

    spacing: 0

    function syncEditTool() {
        if (!document.loaded || ribbonTabs.currentIndex !== 1) {
            editController.tool = PDFEditController.BrowseTool
        } else if (editController.tool === PDFEditController.BrowseTool) {
            editController.tool = PDFEditController.SelectTool
        }
    }

    Component.onCompleted: syncEditTool()
    onDocumentChanged: syncEditTool()
    onEditControllerChanged: syncEditTool()

    Connections {
        target: root.document

        function onLoadedChanged() {
            root.syncEditTool()
        }
    }

    Component {
        id: viewRibbonComponent

        ViewRibbon {
            theme: root.theme
            document: root.document
            editController: root.editController
            onOpenRequested: root.openRequested()
            onSaveRequested: root.saveRequested()
            onSaveAsRequested: root.saveAsRequested()
            onPageRequested: page => root.pageRequested(page)
            onZoomOutRequested: root.zoomOutRequested()
            onZoomInRequested: root.zoomInRequested()
            onFitWidthRequested: root.fitWidthRequested()
            onRotateRequested: quarterTurns => root.rotateRequested(quarterTurns)
            onToolRequested: toolName => root.toolRequested(toolName)
        }
    }

    Component {
        id: editRibbonComponent

        EditRibbon {
            theme: root.theme
            document: root.document
            editController: root.editController
            onToolRequested: toolName => root.toolRequested(toolName)
        }
    }

    Component {
        id: signRibbonComponent

        SignRibbon {
            theme: root.theme
            document: root.document
            onToolRequested: toolName => root.toolRequested(toolName)
        }
    }

    Rectangle {
        width: parent.width
        height: 34
        color: "#fffaf6"

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: root.theme.ribbonBorder
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 0

            TabBar {
                id: ribbonTabs

                Layout.preferredWidth: 312
                Layout.fillHeight: true
                spacing: 0
                background: Item {}
                onCurrentIndexChanged: root.syncEditTool()

                RibbonTabButton {
                    theme: root.theme
                    //% "View"
                    text: qsTrId("View")
                }

                RibbonTabButton {
                    theme: root.theme
                    //% "Edit"
                    text: qsTrId("Edit")
                }

                RibbonTabButton {
                    theme: root.theme
                    //% "E-Sign"
                    text: qsTrId("ESign")
                }
            }

            Item {
                Layout.fillWidth: true
            }

            Rectangle {
                Layout.preferredWidth: draftStatusLabel.implicitWidth + 20
                Layout.preferredHeight: 24
                Layout.alignment: Qt.AlignVCenter
                visible: root.editController.hasDrafts || root.editController.saving
                radius: 12
                color: root.theme.foxitPaleOrange
                border.color: root.theme.foxitBorderOrange
                border.width: 1
                Accessible.role: Accessible.StaticText
                Accessible.name: draftStatusLabel.text

                Label {
                    id: draftStatusLabel

                    readonly property string savingStatusText: {
                        //% "Saving PDF…"
                        return qsTrId("SavingPdf")
                    }
                    readonly property string draftStatusText: {
                        //% "Draft · Not saved"
                        return qsTrId("DraftNotSaved")
                    }

                    anchors.centerIn: parent
                    text: root.editController.saving
                        ? savingStatusText
                        : draftStatusText
                    color: root.theme.foxitDarkOrange
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                }
            }
        }
    }

    Rectangle {
        width: parent.width
        height: 78
        color: root.theme.ribbonSurface

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: root.theme.ribbonBorder
        }

        Loader {
            anchors.fill: parent
            sourceComponent: ribbonTabs.currentIndex === 0
                ? viewRibbonComponent
                : ribbonTabs.currentIndex === 1
                    ? editRibbonComponent
                    : signRibbonComponent
        }
    }
}
