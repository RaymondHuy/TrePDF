pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TrePDF

Rectangle {
    id: root
    objectName: "pageThumbnailPanel"

    required property AppTheme theme
    required property PDFDocumentSession session
    required property int currentPage
    property bool navigationEnabled: true
    property int pageMetricsRevision: 0
    readonly property bool expanded: session.thumbnailsVisible
    readonly property bool quarterTurn: session.viewRotation % 180 !== 0

    signal pageRequested(int page)

    color: theme.ribbonSurface

    Connections {
        target: root.session.document
        function onPageStructureChanged() { ++root.pageMetricsRevision }
        function onSourceChanged() { ++root.pageMetricsRevision }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.minimumHeight: 44
            Layout.maximumHeight: 44
            color: root.theme.ribbonSurface

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: root.expanded ? 8 : 4
                anchors.rightMargin: root.expanded ? 8 : 4
                spacing: 8

                ToolButton {
                    id: toggleButton
                    objectName: "toggleThumbnailsButton"
                    Layout.minimumWidth: 28
                    Layout.maximumWidth: 28
                    Layout.minimumHeight: 28
                    Layout.maximumHeight: 28
                    padding: 5
                    icon.source: "../assets/icons/panel-left.svg"
                    icon.width: 18
                    icon.height: 18
                    icon.color: root.expanded ? root.theme.foxitDarkOrange
                        : root.theme.ribbonTextColor
                    //% "Page thumbnails"
                    text: qsTrId("PageThumbnails")
                    display: AbstractButton.IconOnly
                    checkable: true
                    checked: root.expanded
                    onCheckedChanged: root.session.thumbnailsVisible = checked
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    ToolTip.text: text

                    background: Rectangle {
                        radius: 4
                        color: toggleButton.down ? "#f5dfcf"
                            : toggleButton.hovered || toggleButton.checked
                                ? root.theme.foxitPaleOrange : "transparent"
                        border.color: toggleButton.activeFocus
                            ? root.theme.foxitOrange : "transparent"
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: root.expanded
                    text: toggleButton.text
                    font.pixelSize: 12
                    font.bold: true
                    color: root.theme.ribbonTextColor
                    elide: Text.ElideRight
                }
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: root.theme.ribbonBorder
            }
        }

        Loader {
            id: listLoader
            objectName: "thumbnailListLoader"
            Layout.fillWidth: true
            Layout.fillHeight: true
            active: root.expanded && root.session.document.loaded
            visible: active

            sourceComponent: Component {
                ListView {
                    id: thumbnailList
                    objectName: "thumbnailList"
                    clip: true
                    reuseItems: true
                    cacheBuffer: 200
                    spacing: 4
                    topMargin: 8
                    bottomMargin: 8
                    model: root.session.document.pageCount
                    currentIndex: root.currentPage
                    keyNavigationEnabled: false
                    highlightFollowsCurrentItem: false
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar { }

                    function revealCurrentPage() {
                        if (!moving && currentIndex >= 0 && currentIndex < count)
                            positionViewAtIndex(currentIndex, ListView.Contain)
                    }
                    onCurrentIndexChanged: Qt.callLater(revealCurrentPage)
                    onCountChanged: Qt.callLater(revealCurrentPage)
                    onHeightChanged: Qt.callLater(revealCurrentPage)
                    Component.onCompleted: Qt.callLater(revealCurrentPage)

                    delegate: ItemDelegate {
                        id: thumbnail
                        objectName: "pageThumbnail_" + index
                        required property int index
                        property bool pooled: false
                        readonly property size pageSize: {
                            const revision = root.pageMetricsRevision
                            return root.session.document.pageSize(index)
                        }
                        readonly property real pageScale: pageSize.width > 0 && pageSize.height > 0
                            ? Math.min(144 / (root.quarterTurn ? pageSize.height : pageSize.width),
                                       168 / (root.quarterTurn ? pageSize.width : pageSize.height))
                            : 0

                        width: thumbnailList.width - 12
                        x: 6
                        height: 200
                        padding: 4
                        focusPolicy: Qt.StrongFocus
                        enabled: root.navigationEnabled
                        //% "Page %1"
                        text: qsTrId("PageNumber").arg(index + 1)
                        Accessible.name: text
                        Accessible.role: Accessible.Button
                        onClicked: root.pageRequested(index)
                        Keys.onUpPressed: root.pageRequested(Math.max(0, root.currentPage - 1))
                        Keys.onDownPressed: root.pageRequested(Math.min(thumbnailList.count - 1,
                                                                       root.currentPage + 1))
                        ListView.onPooled: pooled = true
                        ListView.onReused: pooled = false

                        background: Rectangle {
                            radius: 4
                            color: thumbnail.index === root.currentPage
                                ? root.theme.foxitPaleOrange
                                : thumbnail.hovered || thumbnail.activeFocus ? "#f1efed" : "transparent"
                            border.color: thumbnail.activeFocus
                                ? root.theme.foxitOrange : "transparent"
                        }

                        contentItem: Item {
                            Item {
                                id: previewFrame
                                anchors.horizontalCenter: parent.horizontalCenter
                                y: 4
                                width: (root.quarterTurn ? thumbnail.pageSize.height
                                    : thumbnail.pageSize.width) * thumbnail.pageScale
                                height: (root.quarterTurn ? thumbnail.pageSize.width
                                    : thumbnail.pageSize.height) * thumbnail.pageScale

                                PDFPageView {
                                    id: preview
                                    objectName: "thumbnailPreview"
                                    anchors.centerIn: parent
                                    // No Edit controller: thumbnails show saved page content,
                                    // without suppressing source objects for editor-only overlays.
                                    document: thumbnail.pooled ? null : root.session.document
                                    formController: thumbnail.pooled ? null : root.session.formController
                                    page: thumbnail.index
                                    zoom: thumbnail.pageScale
                                    width: thumbnail.pageSize.width * thumbnail.pageScale
                                    height: thumbnail.pageSize.height * thumbnail.pageScale
                                    rotation: root.session.viewRotation
                                    visible: !thumbnail.pooled
                                    pageColor: "white"
                                }

                                Rectangle {
                                    anchors.fill: parent
                                    color: "transparent"
                                    border.width: thumbnail.index === root.currentPage ? 2 : 1
                                    border.color: thumbnail.index === root.currentPage
                                        ? root.theme.foxitOrange : "#c6cad1"
                                }
                            }

                            Label {
                                anchors.horizontalCenter: parent.horizontalCenter
                                anchors.bottom: parent.bottom
                                text: String(thumbnail.index + 1)
                                font.pixelSize: 12
                                font.bold: thumbnail.index === root.currentPage
                                color: thumbnail.index === root.currentPage
                                    ? root.theme.foxitDarkOrange : root.theme.ribbonTextColor
                            }
                        }
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.margins: 8
            visible: root.expanded && root.session.hasDrafts
            //% "Save to update thumbnail content."
            text: qsTrId("SaveToUpdateThumbnails")
            wrapMode: Text.WordWrap
            font.pixelSize: 11
            color: "#6b625d"
        }

        Item {
            Layout.fillHeight: true
            visible: !root.expanded
        }
    }

    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: root.theme.ribbonBorder
    }
}
