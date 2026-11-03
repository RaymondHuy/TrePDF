pragma ComponentBehavior: Bound

import QtQuick
import TrePDF

Item {
    id: root

    required property TextEdit editor
    required property var characterSpans
    property bool active: true

    x: editor.x
    y: editor.y
    width: editor.width
    height: editor.height
    scale: editor.scale
    transformOrigin: Item.TopLeft
    visible: active && editor.visible
    Accessible.ignored: true

    Loader {
        anchors.fill: parent
        active: root.visible
        sourceComponent: Component {
            ShearedTextSlices {
                id: slices

                editor: root.editor
                characterSpans: root.characterSpans
                capture: texture

                ShaderEffectSource {
                    id: texture

                    // Capture only the visible source at its full scene scale,
                    // including ancestor zoom and the screen's pixel density.
                    width: root.width
                    height: root.height
                    sourceItem: root.editor
                    // The software layer stores a Y-down pixmap, while RHI
                    // uses the default texture orientation for image nodes.
                    textureMirroring: GraphicsInfo.api === GraphicsInfo.Software
                        ? ShaderEffectSource.NoMirroring : ShaderEffectSource.MirrorVertically
                    sourceRect: slices.captureRect
                    textureSize: slices.captureSize
                    hideSource: true
                    visible: false
                    smooth: true
                }
            }
        }
    }
}
