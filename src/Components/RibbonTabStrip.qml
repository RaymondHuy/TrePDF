pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Rectangle {
    id: root

    required property AppTheme theme
    property bool anySaving: false
    property bool hasUnsavedChanges: false
    property list<int> enabledTabs: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
    property alias currentIndex: tabBar.currentIndex

    function setCurrentIndex(index: int) {
        tabBar.setCurrentIndex(index)
    }

    implicitWidth: 860
    implicitHeight: Qt.platform.os === "ios" ? 44 : 34
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
            id: tabBar
            objectName: "ribbonTabs"

            Layout.preferredWidth: Qt.platform.os === "ios" ? 0 : 840
            Layout.fillWidth: Qt.platform.os === "ios"
            Layout.minimumWidth: 0
            clip: Qt.platform.os === "ios"
            Layout.fillHeight: true
            enabled: !root.anySaving
            spacing: 0
            background: Item {}

            RibbonTabButton {
                enabled: root.enabledTabs.indexOf(0) !== -1
                theme: root.theme
                //% "View"
                text: qsTrId("View")
            }

            RibbonTabButton {
                enabled: root.enabledTabs.indexOf(1) !== -1
                theme: root.theme
                //% "Edit"
                text: qsTrId("Edit")
            }

            RibbonTabButton {
                enabled: root.enabledTabs.indexOf(2) !== -1
                theme: root.theme
                //% "Comment"
                text: qsTrId("Comment")
            }

            RibbonTabButton {
                enabled: root.enabledTabs.indexOf(3) !== -1
                theme: root.theme
                //% "Forms"
                text: qsTrId("Forms")
            }

            RibbonTabButton {
                enabled: root.enabledTabs.indexOf(4) !== -1
                theme: root.theme
                //% "E-Sign"
                text: qsTrId("ESign")
            }

            RibbonTabButton {
                enabled: root.enabledTabs.indexOf(5) !== -1
                objectName: "protectTabButton"
                theme: root.theme
                //% "Protect"
                text: qsTrId("ProtectTab")
            }

            RibbonTabButton {
                enabled: root.enabledTabs.indexOf(6) !== -1
                theme: root.theme
                //% "Convert"
                text: qsTrId("ConvertTab")
            }

            RibbonTabButton {
                enabled: root.enabledTabs.indexOf(7) !== -1
                objectName: "headerTabButton"
                theme: root.theme
                //% "Header"
                text: qsTrId("HeaderTab")
            }

            RibbonTabButton {
                enabled: root.enabledTabs.indexOf(8) !== -1
                objectName: "footerTabButton"
                theme: root.theme
                //% "Footer"
                text: qsTrId("FooterTab")
            }

            RibbonTabButton {
                enabled: root.enabledTabs.indexOf(9) !== -1
                objectName: "settingsTabButton"
                theme: root.theme
                //% "Settings"
                text: qsTrId("SettingsTab")
            }
        }

        Item {
            Layout.fillWidth: true
            visible: Qt.platform.os !== "ios"
        }

        Rectangle {
            Layout.preferredWidth: draftStatusContent.implicitWidth + 20
            Layout.preferredHeight: 24
            Layout.alignment: Qt.AlignVCenter
            visible: root.hasUnsavedChanges || root.anySaving
            radius: 12
            color: root.theme.foxitPaleOrange
            border.color: root.theme.foxitBorderOrange
            border.width: 1
            Accessible.role: Accessible.StaticText
            Accessible.name: draftStatusLabel.text

            RowLayout {
                id: draftStatusContent

                anchors.centerIn: parent
                spacing: 6

                BusyIndicator {
                    objectName: "pdfSaveBusyIndicator"
                    Layout.preferredWidth: 16
                    Layout.preferredHeight: 16
                    padding: 0
                    palette.dark: root.theme.foxitDarkOrange
                    visible: root.anySaving
                    running: visible
                    Accessible.ignored: true
                }

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

                    text: root.anySaving ? savingStatusText : draftStatusText
                    color: root.theme.foxitDarkOrange
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                }
            }
        }
    }
}
