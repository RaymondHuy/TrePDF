pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import TrePDF

Item {
    id: root

    required property AppTheme theme
    required property PDFEditController controller
    required property int page
    required property bool pooled
    required property bool interactive
    required property bool showFrames

    readonly property var links: {
        controller.webLinksRevision
        return pooled ? [] : controller.webLinkDraftsForPage(page)
    }

    Repeater {
        model: root.links

        delegate: Button {
            id: linkRegion

            required property var modelData
            readonly property rect linkRect: modelData.rect

            x: linkRect.x * root.width
            y: linkRect.y * root.height
            width: linkRect.width * root.width
            height: linkRect.height * root.height
            padding: 0
            topInset: 0
            bottomInset: 0
            enabled: root.interactive
            focusPolicy: Qt.TabFocus
            Accessible.role: Accessible.Link
            Accessible.name: modelData.url
            onClicked: root.controller.activateWebLink(
                root.page, modelData.index, modelData.revision)

            contentItem: Item { }
            background: Rectangle {
                color: "transparent"
                border.width: root.showFrames || linkRegion.hovered
                    || linkRegion.visualFocus ? 1 : 0
                border.color: root.theme.foxitDarkOrange
            }

            HoverHandler {
                cursorShape: Qt.PointingHandCursor
                enabled: root.interactive
            }

            ToolTip.visible: hovered
            ToolTip.text: modelData.url
            ToolTip.delay: 500
        }
    }
}
