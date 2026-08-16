pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Shapes

Item {
    id: root

    property var strokes: []
    property color strokeColor: "#000000"
    property real strokeWidth: 1.5
    property real drawingOpacity: 1

    readonly property real effectiveStrokeWidth: Math.max(0.75, strokeWidth)
    readonly property color effectiveStrokeColor: Qt.rgba(
        strokeColor.r,
        strokeColor.g,
        strokeColor.b,
        Math.max(0, Math.min(1, drawingOpacity)))

    Accessible.ignored: true

    Repeater {
        model: root.strokes

        delegate: Shape {
            id: strokeShape

            required property var modelData

            anchors.fill: parent
            visible: modelData.length >= 2
            preferredRendererType: Shape.CurveRenderer

            ShapePath {
                scale: Qt.size(root.width, root.height)
                fillColor: "transparent"
                strokeColor: root.effectiveStrokeColor
                strokeWidth: root.effectiveStrokeWidth
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin

                PathPolyline { path: strokeShape.modelData }
            }
        }
    }
}
