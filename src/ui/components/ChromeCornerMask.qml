import Eden.Ui
import QtQuick
import QtQuick.Shapes

Shape {
    id: root

    required property string corner
    required property real surfaceHeight
    required property real surfaceWidth
    required property real surfaceX
    required property real surfaceY
    readonly property bool isBottomCorner: corner === "bottom-left" || corner === "bottom-right"
    readonly property bool isRightCorner: corner === "top-right" || corner === "bottom-right"
    readonly property real gradientRadians: Theme.toolbarGradientAngle * Math.PI / 180
    readonly property real gradientDirectionX: Math.cos(gradientRadians)
    readonly property real gradientDirectionY: Math.sin(gradientRadians)
    readonly property real gradientLength: Math.abs(gradientDirectionX) * surfaceWidth + Math.abs(gradientDirectionY) * surfaceHeight
    readonly property real gradientCenterX: surfaceWidth / 2 - surfaceX
    readonly property real gradientCenterY: surfaceHeight / 2 - surfaceY
    readonly property real outerX: isRightCorner ? width : 0
    readonly property real outerY: isBottomCorner ? height : 0
    readonly property real arcStartX: corner === "top-right" ? 0 : width
    readonly property real arcStartY: corner === "bottom-left" ? height : 0
    readonly property real arcEndX: corner === "top-right" ? width : 0
    readonly property real arcEndY: corner === "bottom-left" ? 0 : height
    readonly property real control1X: corner === "top-left" || corner === "bottom-left" ? width * 0.447715 : corner === "top-right" ? width * 0.552285 : width
    readonly property real control1Y: corner === "bottom-right" ? height * 0.552285 : isBottomCorner ? height : 0
    readonly property real control2X: corner === "bottom-right" ? width * 0.552285 : isRightCorner ? width : 0
    readonly property real control2Y: corner === "top-left" || corner === "top-right" ? height * 0.447715 : corner === "bottom-left" ? height * 0.552285 : height

    width: Theme.contentRadius
    height: Theme.contentRadius

    ShapePath {
        strokeColor: "transparent"
        startX: root.outerX
        startY: root.outerY

        PathLine {
            x: root.arcStartX
            y: root.arcStartY
        }

        PathCubic {
            x: root.arcEndX
            y: root.arcEndY
            control1X: root.control1X
            control1Y: root.control1Y
            control2X: root.control2X
            control2Y: root.control2Y
        }

        PathLine {
            x: root.outerX
            y: root.outerY
        }

        fillGradient: LinearGradient {
            x1: root.gradientCenterX - root.gradientDirectionX * root.gradientLength / 2
            y1: root.gradientCenterY - root.gradientDirectionY * root.gradientLength / 2
            x2: root.gradientCenterX + root.gradientDirectionX * root.gradientLength / 2
            y2: root.gradientCenterY + root.gradientDirectionY * root.gradientLength / 2

            GradientStop {
                position: 0
                color: Theme.toolbarGradientStart
            }

            GradientStop {
                position: Theme.toolbarGradientMiddlePosition
                color: Theme.toolbarGradientEnabled ? Theme.toolbarGradientMiddle : Theme.toolbarGradientStart
            }

            GradientStop {
                position: 1
                color: Theme.toolbarGradientEnabled ? Theme.toolbarGradientEnd : Theme.toolbarGradientStart
            }

        }

    }

}
