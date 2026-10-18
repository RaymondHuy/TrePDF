pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Flickable {
    id: root

    required property AppTheme theme
    readonly property string contactEmail: "trebase177@gmail.com"

    contentWidth: informationRow.implicitWidth + 32
    contentHeight: height
    flickableDirection: Flickable.HorizontalFlick
    boundsBehavior: Flickable.StopAtBounds
    clip: true

    RowLayout {
        id: informationRow

        x: 16
        y: 8
        height: root.height - 16
        spacing: 20

        Image {
            Layout.preferredWidth: 44
            Layout.preferredHeight: 44
            source: "qrc:/trepdf/branding/TrePDF-icon-512.png"
            sourceSize: Qt.size(88, 88)
            fillMode: Image.PreserveAspectFit
            Accessible.ignored: true
        }

        ColumnLayout {
            spacing: 4

            Label {
                //% "TrePDF"
                text: qsTrId("ApplicationTitle")
                color: root.theme.ribbonTextColor
                font.pixelSize: 17
                font.weight: Font.DemiBold
            }

            Label {
                objectName: "applicationVersionLabel"
                //% "Version %1"
                text: qsTrId("ApplicationVersionLabel").arg(Qt.application.version)
                color: root.theme.ribbonTextColor
                font.pixelSize: 13
            }
        }

        Rectangle {
            Layout.preferredWidth: 1
            Layout.preferredHeight: 46
            color: root.theme.ribbonBorder
            Accessible.ignored: true
        }

        ColumnLayout {
            spacing: 2

            Label {
                //% "Contact email"
                text: qsTrId("ApplicationContactEmail")
                color: root.theme.ribbonTextColor
                font.pixelSize: 12
            }

            TextField {
                id: emailField
                objectName: "applicationContactEmailField"

                Layout.preferredWidth: emailMetrics.advanceWidth + 16
                text: root.contactEmail
                readOnly: true
                selectByMouse: true
                activeFocusOnTab: true
                color: root.theme.ribbonTextColor
                font.pixelSize: 13
                padding: 4
                leftPadding: 0
                selectionColor: root.theme.foxitPaleOrange
                selectedTextColor: root.theme.ribbonTextColor
                //% "Contact email"
                Accessible.name: qsTrId("ApplicationContactEmail")
                background: Rectangle {
                    color: "transparent"
                    border.width: emailField.activeFocus ? 1 : 0
                    border.color: root.theme.foxitOrange
                    radius: 3
                }
            }
        }
    }

    TextMetrics {
        id: emailMetrics
        font: emailField.font
        text: root.contactEmail
    }
}
