pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Shapes
import TrePDF

Item {
    id: root

    property int shapeType: PDFEditController.RectangleShape
    property point startRatio: Qt.point(0, 0)
    property point endRatio: Qt.point(1, 1)
    property color fillColor: "transparent"
    property color strokeColor: "#000000"
    property real strokeWidth: 2
    property int tableRows: 3
    property int tableColumns: 3
    property var tableRowStops: []
    property var tableColumnStops: []
    property var tableCellTexts: []
    property var tableCellRects: []
    property font tableTextFont: Qt.font({ pixelSize: 12 })
    property real tableTextScale: 1

    readonly property int boundedTableRows: Math.max(1, Math.min(20, tableRows))
    readonly property int boundedTableColumns: Math.max(1, Math.min(20, tableColumns))
    readonly property var tablePaths: {
        if (shapeType !== PDFEditController.TableShape)
            return []
        const paths = [[Qt.point(pathLeft, pathTop), Qt.point(pathRight, pathTop),
                        Qt.point(pathRight, pathBottom)],
                       [Qt.point(pathRight, pathBottom), Qt.point(pathLeft, pathBottom),
                        Qt.point(pathLeft, pathTop)]]
        for (let row = 1; row < boundedTableRows; ++row) {
            const stop = tableRowStops.length === boundedTableRows - 1
                ? Number(tableRowStops[row - 1]) : row / boundedTableRows
            const y = pathTop + (pathBottom - pathTop) * stop
            paths.push([Qt.point(pathLeft, y), Qt.point(pathRight, y)])
        }
        for (let column = 1; column < boundedTableColumns; ++column) {
            const stop = tableColumnStops.length === boundedTableColumns - 1
                ? Number(tableColumnStops[column - 1]) : column / boundedTableColumns
            const x = pathLeft + (pathRight - pathLeft) * stop
            paths.push([Qt.point(x, pathTop), Qt.point(x, pathBottom)])
        }
        return paths
    }

    readonly property real effectiveStrokeWidth: Math.max(0.75, strokeWidth)
    readonly property real inset: Math.min(
        effectiveStrokeWidth / 2,
        width / 4,
        height / 4)
    readonly property real pathLeft: inset
    readonly property real pathTop: inset
    readonly property real pathRight: Math.max(pathLeft, width - inset)
    readonly property real pathBottom: Math.max(pathTop, height - inset)
    readonly property real centerX: (pathLeft + pathRight) / 2
    readonly property real centerY: (pathTop + pathBottom) / 2
    readonly property real radiusX: (pathRight - pathLeft) / 2
    readonly property real radiusY: (pathBottom - pathTop) / 2
    readonly property real ellipseKappa: 0.5522847498307936
    readonly property point lineStart: Qt.point(
        boundedRatio(startRatio.x) * width,
        boundedRatio(startRatio.y) * height)
    readonly property point lineEnd: Qt.point(
        boundedRatio(endRatio.x) * width,
        boundedRatio(endRatio.y) * height)
    readonly property real lineDx: lineEnd.x - lineStart.x
    readonly property real lineDy: lineEnd.y - lineStart.y
    readonly property real lineLength: Math.max(
        0.001,
        Math.hypot(lineDx, lineDy))
    readonly property real arrowUnitX: lineDx / lineLength
    readonly property real arrowUnitY: lineDy / lineLength
    readonly property real arrowHeadLength: Math.min(
        Math.max(6, effectiveStrokeWidth * 4),
        lineLength * 0.35)
    readonly property real arrowHalfWidth: arrowHeadLength * 0.55
    readonly property point arrowHeadBase: Qt.point(
        lineEnd.x - arrowUnitX * arrowHeadLength,
        lineEnd.y - arrowUnitY * arrowHeadLength)
    readonly property point arrowHeadLeft: Qt.point(
        arrowHeadBase.x - arrowUnitY * arrowHalfWidth,
        arrowHeadBase.y + arrowUnitX * arrowHalfWidth)
    readonly property point arrowHeadRight: Qt.point(
        arrowHeadBase.x + arrowUnitY * arrowHalfWidth,
        arrowHeadBase.y - arrowUnitX * arrowHalfWidth)

    function boundedRatio(value) {
        return Math.max(0, Math.min(1, Number(value)))
    }

    Shape {
        anchors.fill: parent
        visible: root.shapeType === PDFEditController.TableShape
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            fillColor: "transparent"
            strokeColor: root.strokeColor
            strokeWidth: root.effectiveStrokeWidth
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin

            PathMultiline { paths: root.tablePaths }
        }
    }

    Repeater {
        model: root.shapeType === PDFEditController.TableShape
            && root.tableCellTexts.length === root.tableCellRects.length
            ? root.tableCellTexts.length : 0

        delegate: Text {
            required property int index
            readonly property rect cellRect: root.tableCellRects[index]
            objectName: "tableCellPreview" + index
            x: cellRect.x * root.width
            y: cellRect.y * root.height + root.tableTextScale
            width: cellRect.width * root.width / Math.max(0.01, root.tableTextScale)
            scale: root.tableTextScale
            transformOrigin: Item.TopLeft
            text: root.tableCellTexts[index]
            textFormat: Text.PlainText
            wrapMode: Text.WrapAtWordBoundaryOrAnywhere
            font: root.tableTextFont
            color: "#000000"
            Accessible.ignored: true
        }
    }

    Shape {
        anchors.fill: parent
        visible: root.shapeType === PDFEditController.RectangleShape
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            fillColor: root.fillColor
            strokeColor: root.strokeColor
            strokeWidth: root.effectiveStrokeWidth
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
        visible: root.shapeType === PDFEditController.EllipseShape
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            fillColor: root.fillColor
            strokeColor: root.strokeColor
            strokeWidth: root.effectiveStrokeWidth
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            startX: root.centerX
            startY: root.pathTop

            PathCubic {
                control1X: root.centerX
                    + root.ellipseKappa * root.radiusX
                control1Y: root.pathTop
                control2X: root.pathRight
                control2Y: root.centerY
                    - root.ellipseKappa * root.radiusY
                x: root.pathRight
                y: root.centerY
            }
            PathCubic {
                control1X: root.pathRight
                control1Y: root.centerY
                    + root.ellipseKappa * root.radiusY
                control2X: root.centerX
                    + root.ellipseKappa * root.radiusX
                control2Y: root.pathBottom
                x: root.centerX
                y: root.pathBottom
            }
            PathCubic {
                control1X: root.centerX
                    - root.ellipseKappa * root.radiusX
                control1Y: root.pathBottom
                control2X: root.pathLeft
                control2Y: root.centerY
                    + root.ellipseKappa * root.radiusY
                x: root.pathLeft
                y: root.centerY
            }
            PathCubic {
                control1X: root.pathLeft
                control1Y: root.centerY
                    - root.ellipseKappa * root.radiusY
                control2X: root.centerX
                    - root.ellipseKappa * root.radiusX
                control2Y: root.pathTop
                x: root.centerX
                y: root.pathTop
            }
        }
    }

    Shape {
        anchors.fill: parent
        visible: root.shapeType === PDFEditController.LineShape
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            fillColor: "transparent"
            strokeColor: root.strokeColor
            strokeWidth: root.effectiveStrokeWidth
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            startX: root.lineStart.x
            startY: root.lineStart.y

            PathLine { x: root.lineEnd.x; y: root.lineEnd.y }
        }
    }

    Shape {
        anchors.fill: parent
        visible: root.shapeType === PDFEditController.ArrowShape
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            fillColor: "transparent"
            strokeColor: root.strokeColor
            strokeWidth: root.effectiveStrokeWidth
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            startX: root.lineStart.x
            startY: root.lineStart.y

            PathLine { x: root.lineEnd.x; y: root.lineEnd.y }
            PathMove { x: root.arrowHeadLeft.x; y: root.arrowHeadLeft.y }
            PathLine { x: root.lineEnd.x; y: root.lineEnd.y }
            PathLine { x: root.arrowHeadRight.x; y: root.arrowHeadRight.y }
        }
    }

    Shape {
        anchors.fill: parent
        visible: root.shapeType === PDFEditController.TriangleShape
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            fillColor: root.fillColor
            strokeColor: root.strokeColor
            strokeWidth: root.effectiveStrokeWidth
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            startX: root.centerX
            startY: root.pathTop

            PathLine { x: root.pathRight; y: root.pathBottom }
            PathLine { x: root.pathLeft; y: root.pathBottom }
            PathLine { x: root.centerX; y: root.pathTop }
        }
    }

    Shape {
        anchors.fill: parent
        visible: root.shapeType === PDFEditController.DiamondShape
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            fillColor: root.fillColor
            strokeColor: root.strokeColor
            strokeWidth: root.effectiveStrokeWidth
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            startX: root.centerX
            startY: root.pathTop

            PathLine { x: root.pathRight; y: root.centerY }
            PathLine { x: root.centerX; y: root.pathBottom }
            PathLine { x: root.pathLeft; y: root.centerY }
            PathLine { x: root.centerX; y: root.pathTop }
        }
    }
}
