pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Shapes

Shape {
    id: root

    required property color waveColor
    required property real pagePointScale

    // Match PDFium's generated Squiggly appearance: 1 pt stroke, 2 pt steps.
    readonly property real waveStrokeWidth: Math.max(0, pagePointScale)
    readonly property real waveDelta: Math.max(0, 2 * pagePointScale)
    readonly property string wavePath: {
        if (width <= 0 || height <= 0 || waveDelta <= 0)
            return ""

        const topY = Math.max(0, height - waveDelta)
        const bottomY = height
        let currentX = 0
        let currentY = topY
        let nextIsBottom = true
        let path = "M 0 " + topY

        while (currentX + waveDelta < width) {
            currentX += waveDelta
            currentY = nextIsBottom ? bottomY : topY
            path += " L " + currentX + " " + currentY
            nextIsBottom = !nextIsBottom
        }

        if (currentX < width) {
            const nextY = nextIsBottom ? bottomY : topY
            const progress = (width - currentX) / waveDelta
            const endY = currentY + (nextY - currentY) * progress
            path += " L " + width + " " + endY
        }
        return path
    }

    clip: true
    preferredRendererType: Shape.CurveRenderer

    ShapePath {
        fillColor: "transparent"
        strokeColor: root.waveColor
        strokeWidth: root.waveStrokeWidth
        capStyle: ShapePath.FlatCap
        joinStyle: ShapePath.MiterJoin

        PathSvg {
            path: root.wavePath
        }
    }
}
