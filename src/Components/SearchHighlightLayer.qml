pragma ComponentBehavior: Bound

import QtQuick
import TrePDF

Item {
    id: root

    required property PDFSearchController controller
    required property int page
    required property bool pooled
    readonly property var highlights: {
        const revision = controller.resultsRevision
        return !pooled && revision >= 0
            ? controller.highlightsForPage(page)
            : []
    }

    enabled: false
    visible: !pooled && highlights.length > 0

    Repeater {
        model: root.highlights

        delegate: Rectangle {
            required property var modelData

            x: Number(modelData.x) * root.width
            y: Number(modelData.y) * root.height
            width: Number(modelData.width) * root.width
            height: Number(modelData.height) * root.height
            color: Boolean(modelData.current)
                ? "#66f57c00"
                : "#55ffeb3b"
            border.width: Boolean(modelData.current) ? 1 : 0
            border.color: "#d96c00"
            radius: 1
            Accessible.ignored: true
        }
    }
}
