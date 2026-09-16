import QtQuick
import QtQuick.Shapes

Item {
    id: root

    property string stampName: "Approved"
    property string stampText: stampName.toUpperCase()
    property color stampColor: "#2e7d32"
    property string stampArtwork: ""

    readonly property bool hasArtwork: stampArtwork.length > 0
    readonly property real shortestSide: Math.min(width, height)
    readonly property real outerBorderWidth: Math.max(
        1,
        Math.min(3, shortestSide * 0.04))
    readonly property real inset: Math.max(
        outerBorderWidth + 1,
        shortestSide * 0.1)

    Rectangle {
        anchors.fill: parent
        color: root.hasArtwork ? root.stampColor : "transparent"
        border.width: root.hasArtwork ? 0 : root.outerBorderWidth
        border.color: root.stampColor
        Accessible.ignored: true
    }

    Shape {
        width: 160
        height: 50
        visible: root.hasArtwork
        preferredRendererType: Shape.CurveRenderer
        transform: Scale {
            xScale: root.width / 160
            yScale: root.height / 50
        }

        ShapePath {
            fillColor: "#ffffff"
            strokeColor: "transparent"
            strokeWidth: 0
            fillRule: ShapePath.WindingFill

            PathSvg { path: root.stampArtwork }
        }

        Accessible.ignored: true
    }

    Rectangle {
        anchors.fill: parent
        anchors.margins: root.inset
        visible: !root.hasArtwork
        color: "transparent"
        border.width: Math.max(1, root.outerBorderWidth * 0.55)
        border.color: root.stampColor
        Accessible.ignored: true
    }

    Text {
        anchors.fill: parent
        visible: !root.hasArtwork
        anchors.leftMargin: root.inset * 1.8
        anchors.rightMargin: root.inset * 1.8
        anchors.topMargin: root.inset * 1.15
        anchors.bottomMargin: root.inset * 1.15
        text: root.stampText.length > 0
            ? root.stampText
            : root.stampName.toUpperCase()
        color: root.stampColor
        font.bold: true
        font.capitalization: Font.AllUppercase
        font.letterSpacing: Math.max(0, root.shortestSide * 0.015)
        font.pixelSize: Math.max(6, root.shortestSide * 0.4)
        fontSizeMode: Text.Fit
        minimumPixelSize: 5
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        maximumLineCount: 1
        Accessible.ignored: true
    }

    Accessible.ignored: true
}
