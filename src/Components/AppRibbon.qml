pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TrePDF

Column {
    id: root

    required property AppTheme theme
    required property PDFDocument document

    signal openRequested()
    signal pageRequested(int page)
    signal zoomOutRequested()
    signal zoomInRequested()
    signal fitWidthRequested()
    signal toolRequested(string toolName)

    spacing: 0

    Component {
        id: viewRibbonComponent

        ViewRibbon {
            theme: root.theme
            document: root.document
            onOpenRequested: root.openRequested()
            onPageRequested: page => root.pageRequested(page)
            onZoomOutRequested: root.zoomOutRequested()
            onZoomInRequested: root.zoomInRequested()
            onFitWidthRequested: root.fitWidthRequested()
            onToolRequested: toolName => root.toolRequested(toolName)
        }
    }

    Component {
        id: editRibbonComponent

        EditRibbon {
            theme: root.theme
            document: root.document
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

                RibbonTabButton {
                    theme: root.theme
                    text: qsTr("View")
                }

                RibbonTabButton {
                    theme: root.theme
                    text: qsTr("Edit")
                }

                RibbonTabButton {
                    theme: root.theme
                    text: qsTr("E-Sign")
                }
            }

            Item {
                Layout.fillWidth: true
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
