pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Rectangle {
    id: root

    required property AppTheme theme
    required property var model
    property int currentIndex: -1
    property bool saving: false

    signal tabRequested(int index)
    signal closeRequested(int index)
    signal openRequested()

    implicitHeight: Qt.platform.os === "ios" ? 44 : 38
    height: implicitHeight
    color: "#f6f1ed"

    function ensureCurrentTabVisible() {
        const tabItem = tabRepeater.itemAt(currentIndex)
        if (!tabItem)
            return

        const leftEdge = tabItem.x
        const rightEdge = tabItem.x + tabItem.width
        if (leftEdge < tabFlickable.contentX) {
            tabFlickable.contentX = leftEdge
        } else if (rightEdge > tabFlickable.contentX + tabFlickable.width) {
            tabFlickable.contentX = Math.min(
                Math.max(0, tabFlickable.contentWidth - tabFlickable.width),
                rightEdge - tabFlickable.width)
        }
    }

    onCurrentIndexChanged: Qt.callLater(root.ensureCurrentTabVisible)

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: root.theme.ribbonBorder
    }

    ToolButton {
        id: addTabButton

        anchors.top: parent.top
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: Qt.platform.os === "ios" ? 44 : 40
        enabled: !root.saving
        text: "+"
        font.pixelSize: 20
        //% "Open PDF"
        Accessible.name: qsTrId("OpenPdf")
        ToolTip.visible: hovered
        //% "Open PDF"
        ToolTip.text: qsTrId("OpenPdf")
        onClicked: root.openRequested()

        background: Rectangle {
            color: addTabButton.down
                ? "#eadbd0"
                : addTabButton.hovered ? root.theme.foxitPaleOrange : "transparent"
        }
    }

    Flickable {
        id: tabFlickable

        anchors.left: parent.left
        anchors.right: addTabButton.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        contentWidth: tabRow.width
        contentHeight: height
        flickableDirection: Flickable.HorizontalFlick
        boundsBehavior: Flickable.StopAtBounds
        clip: true

        Row {
            id: tabRow

            height: parent.height
            spacing: 1

            Item {
                width: 8
                height: tabRow.height
            }

            Repeater {
                id: tabRepeater

                model: root.model

                delegate: Item {
                    id: tabItem

                    required property int index
                    required property var session

                    width: Math.max(150, Math.min(240, titleLabel.implicitWidth + 72))
                    height: tabRow.height

                    TabButton {
                        id: tabButton

                        anchors.fill: parent
                        rightPadding: 34
                        checked: root.currentIndex === tabItem.index
                        enabled: !root.saving
                        Accessible.name: tabItem.session.title
                        Accessible.description: tabItem.session.hasDrafts
                            ? draftStatusText
                            : ""
                        ToolTip.visible: hovered
                        ToolTip.text: tabItem.session.filePath
                        onClicked: root.tabRequested(tabItem.index)

                        readonly property string draftStatusText: {
                            //% "Draft · Not saved"
                            return qsTrId("DraftNotSaved")
                        }

                        contentItem: RowLayout {
                            spacing: 7

                            Rectangle {
                                Layout.preferredWidth: 7
                                Layout.preferredHeight: 7
                                Layout.alignment: Qt.AlignVCenter
                                radius: 4
                                color: root.theme.foxitOrange
                                visible: tabItem.session.hasDrafts
                                Accessible.ignored: true
                            }

                            Label {
                                id: titleLabel

                                Layout.fillWidth: true
                                text: tabItem.session.title
                                color: root.theme.ribbonTextColor
                                elide: Text.ElideMiddle
                                font.pixelSize: 12
                                font.weight: tabButton.checked
                                    ? Font.DemiBold
                                    : Font.Normal
                            }
                        }

                        background: Rectangle {
                            radius: 6
                            color: tabButton.checked
                                ? root.theme.ribbonSurface
                                : tabButton.hovered ? "#fff8f2" : "transparent"
                            border.color: tabButton.checked
                                ? root.theme.ribbonBorder
                                : "transparent"
                            border.width: tabButton.checked ? 1 : 0
                        }
                    }

                    ToolButton {
                        id: closeTabButton

                        anchors.right: parent.right
                        anchors.rightMargin: 4
                        anchors.verticalCenter: parent.verticalCenter
                        width: 28
                        height: 28
                        visible: !tabItem.session.saving
                        enabled: !root.saving
                        text: "\u00d7"
                        font.pixelSize: 17
                        //% "Close “%1”"
                        Accessible.name: qsTrId("CloseDocumentTab").arg(
                            tabItem.session.title)
                        ToolTip.visible: hovered
                        //% "Close tab"
                        ToolTip.text: qsTrId("CloseTab")
                        onClicked: root.closeRequested(tabItem.index)

                        background: Rectangle {
                            radius: 4
                            color: closeTabButton.down
                                ? "#eadbd0"
                                : closeTabButton.hovered ? "#f5e5da" : "transparent"
                        }
                    }

                    BusyIndicator {
                        anchors.right: parent.right
                        anchors.rightMargin: 9
                        anchors.verticalCenter: parent.verticalCenter
                        width: 18
                        height: 18
                        visible: tabItem.session.saving
                        running: visible
                    }
                }
            }
        }
    }
}
