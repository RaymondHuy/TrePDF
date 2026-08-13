import QtQuick
import QtQuick.Shapes

Item {
    id: root

    property string drawingKind: "square"
    property var vertices: []
    property color strokeColor: "#000000"
    property color interiorColor: "transparent"
    property bool hasInterior: false
    property real borderWidth: 1.5
    property int borderStyle: 0
    property var dashPattern: []
    property real dashScale: 1
    property int borderEffect: 0
    property int lineStartStyle: 0
    property int lineEndStyle: 0
    property real drawingOpacity: 1

    readonly property real effectiveStrokeWidth: Math.max(0.75, borderWidth)
    readonly property color effectiveStrokeColor: Qt.rgba(
        strokeColor.r,
        strokeColor.g,
        strokeColor.b,
        1)
    readonly property color effectiveInteriorColor: Qt.rgba(
        interiorColor.r,
        interiorColor.g,
        interiorColor.b,
        1)
    readonly property var effectiveDashPattern: {
        const result = []
        for (let index = 0; index < dashPattern.length; ++index) {
            result.push(
                Number(dashPattern[index])
                    * dashScale / effectiveStrokeWidth)
        }
        return result
    }
    readonly property point firstPoint: scaledPoint(0)
    readonly property point lastPoint: scaledPoint(vertices.length - 1)
    readonly property real rawLeft: Math.min(firstPoint.x, lastPoint.x)
    readonly property real rawTop: Math.min(firstPoint.y, lastPoint.y)
    readonly property real rawRight: Math.max(firstPoint.x, lastPoint.x)
    readonly property real rawBottom: Math.max(firstPoint.y, lastPoint.y)
    readonly property real inset: Math.min(
        effectiveStrokeWidth / 2,
        Math.abs(rawRight - rawLeft) / 4,
        Math.abs(rawBottom - rawTop) / 4)
    readonly property real pathLeft: rawLeft + inset
    readonly property real pathTop: rawTop + inset
    readonly property real pathRight: rawRight - inset
    readonly property real pathBottom: rawBottom - inset
    readonly property real centerX: (pathLeft + pathRight) / 2
    readonly property real centerY: (pathTop + pathBottom) / 2
    readonly property real radiusX: Math.max(0, (pathRight - pathLeft) / 2)
    readonly property real radiusY: Math.max(0, (pathBottom - pathTop) / 2)
    readonly property real ellipseKappa: 0.5522847498307936
    readonly property var openPath: scaledVertices(false)
    readonly property var closedPath: scaledVertices(true)

    opacity: Math.max(0, Math.min(1, drawingOpacity))
    Accessible.ignored: true

    function scaledPoint(index) {
        if (index < 0 || index >= vertices.length)
            return Qt.point(0, 0)
        const vertex = vertices[index]
        return Qt.point(Number(vertex.x) * width, Number(vertex.y) * height)
    }

    function scaledVertices(closePath) {
        const result = []
        for (let index = 0; index < vertices.length; ++index)
            result.push(scaledPoint(index))
        if (closePath && result.length > 0)
            result.push(result[0])
        return result
    }

    Shape {
        anchors.fill: parent
        visible: root.drawingKind === "square" && root.vertices.length >= 2
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            fillColor: root.hasInterior
                ? root.effectiveInteriorColor
                : "transparent"
            strokeColor: root.effectiveStrokeColor
            strokeWidth: root.effectiveStrokeWidth
            strokeStyle: root.borderStyle === 1
                ? ShapePath.DashLine
                : ShapePath.SolidLine
            dashPattern: root.effectiveDashPattern
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            startX: root.pathLeft
            startY: root.pathTop

            PathLine { x: root.pathRight; y: root.pathTop }
            PathLine { x: root.pathRight; y: root.pathBottom }
            PathLine { x: root.pathLeft; y: root.pathBottom }
            PathLine { x: root.pathLeft; y: root.pathTop }
        }
    }

    Shape {
        anchors.fill: parent
        visible: root.drawingKind === "circle" && root.vertices.length >= 2
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            fillColor: root.hasInterior
                ? root.effectiveInteriorColor
                : "transparent"
            strokeColor: root.effectiveStrokeColor
            strokeWidth: root.effectiveStrokeWidth
            strokeStyle: root.borderStyle === 1
                ? ShapePath.DashLine
                : ShapePath.SolidLine
            dashPattern: root.effectiveDashPattern
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            startX: root.centerX + root.radiusX
            startY: root.centerY

            PathCubic {
                control1X: root.centerX + root.radiusX
                control1Y: root.centerY + root.ellipseKappa * root.radiusY
                control2X: root.centerX + root.ellipseKappa * root.radiusX
                control2Y: root.centerY + root.radiusY
                x: root.centerX
                y: root.centerY + root.radiusY
            }
            PathCubic {
                control1X: root.centerX - root.ellipseKappa * root.radiusX
                control1Y: root.centerY + root.radiusY
                control2X: root.centerX - root.radiusX
                control2Y: root.centerY + root.ellipseKappa * root.radiusY
                x: root.centerX - root.radiusX
                y: root.centerY
            }
            PathCubic {
                control1X: root.centerX - root.radiusX
                control1Y: root.centerY - root.ellipseKappa * root.radiusY
                control2X: root.centerX - root.ellipseKappa * root.radiusX
                control2Y: root.centerY - root.radiusY
                x: root.centerX
                y: root.centerY - root.radiusY
            }
            PathCubic {
                control1X: root.centerX + root.ellipseKappa * root.radiusX
                control1Y: root.centerY - root.radiusY
                control2X: root.centerX + root.radiusX
                control2Y: root.centerY - root.ellipseKappa * root.radiusY
                x: root.centerX + root.radiusX
                y: root.centerY
            }
        }
    }

    Shape {
        anchors.fill: parent
        visible: (root.drawingKind === "line"
                  || root.drawingKind === "polyLine")
            && root.vertices.length >= 2
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            fillColor: "transparent"
            strokeColor: root.effectiveStrokeColor
            strokeWidth: root.effectiveStrokeWidth
            strokeStyle: root.borderStyle === 1
                ? ShapePath.DashLine
                : ShapePath.SolidLine
            dashPattern: root.effectiveDashPattern
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin

            PathPolyline { path: root.openPath }
        }
    }

    Shape {
        anchors.fill: parent
        visible: root.drawingKind === "polygon" && root.vertices.length >= 3
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            fillColor: root.hasInterior
                ? root.effectiveInteriorColor
                : "transparent"
            strokeColor: root.effectiveStrokeColor
            strokeWidth: root.effectiveStrokeWidth
            strokeStyle: root.borderStyle === 1
                ? ShapePath.DashLine
                : ShapePath.SolidLine
            dashPattern: root.effectiveDashPattern
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin

            PathPolyline { path: root.closedPath }
        }
    }
}
