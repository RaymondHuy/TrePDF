pragma ComponentBehavior: Bound

import QtQuick
import TrePDF

Item {
    id: root

    required property AppTheme theme
    required property PDFTabModel tabModel

    signal tabRequested(int index)
    signal closeRequested(PDFDocumentSession session)
    signal openRequested()

    implicitHeight: tabs.implicitHeight
    height: implicitHeight

    function ensureCurrentTabVisible() {
        tabs.ensureCurrentTabVisible()
    }

    DocumentTabs {
        id: tabs

        anchors.fill: parent
        theme: root.theme
        model: root.tabModel
        currentIndex: root.tabModel.currentIndex
        saving: root.tabModel.anySaving
        onTabRequested: index => root.tabRequested(index)
        onCloseRequested: index => root.closeRequested(
            root.tabModel.sessionAt(index))
        onOpenRequested: root.openRequested()
    }
}
