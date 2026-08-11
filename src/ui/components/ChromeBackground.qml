import Eden.Ui
import QtQuick
import QtQuick.Shapes

Item {
    id: root

    property real radius: 0
    property bool gradientEnabled: Theme.toolbarGradientEnabled
    property real gradientAngle: Theme.toolbarGradientAngle
    property real gradientMiddlePosition: Theme.toolbarGradientMiddlePosition
    property color startColor: Theme.toolbarGradientStart
    property color middleColor: Theme.toolbarGradientMiddle
    property color endColor: Theme.toolbarGradientEnd
    readonly property real effectiveRadius: Math.min(radius, width / 2, height / 2)
    readonly property real gradientRadians: gradientAngle * Math.PI / 180
    readonly property real gradientDirectionX: Math.cos(gradientRadians)
    readonly property real gradientDirectionY: Math.sin(gradientRadians)
    readonly property real gradientLength: Math.abs(gradientDirectionX) * width + Math.abs(gradientDirectionY) * height
    readonly property real gradientCenterX: width / 2
    readonly property real gradientCenterY: height / 2

    Rectangle {
        anchors.fill: parent
        color: root.startColor
        radius: root.effectiveRadius
        visible: !root.gradientEnabled
    }

    Shape {
        anchors.fill: parent
        visible: root.gradientEnabled

        ShapePath {
            strokeColor: "transparent"
            startX: root.effectiveRadius
            startY: 0

            PathLine {
                x: root.width - root.effectiveRadius
                y: 0
            }

            PathCubic {
                x: root.width
                y: root.effectiveRadius
                control1X: root.width - root.effectiveRadius * 0.447715
                control1Y: 0
                control2X: root.width
                control2Y: root.effectiveRadius * 0.447715
            }

            PathLine {
                x: root.width
                y: root.height - root.effectiveRadius
            }

            PathCubic {
                x: root.width - root.effectiveRadius
                y: root.height
                control1X: root.width
                control1Y: root.height - root.effectiveRadius * 0.447715
                control2X: root.width - root.effectiveRadius * 0.447715
                control2Y: root.height
            }

            PathLine {
                x: root.effectiveRadius
                y: root.height
            }

            PathCubic {
                x: 0
                y: root.height - root.effectiveRadius
                control1X: root.effectiveRadius * 0.447715
                control1Y: root.height
                control2X: 0
                control2Y: root.height - root.effectiveRadius * 0.447715
            }

            PathLine {
                x: 0
                y: root.effectiveRadius
            }

            PathCubic {
                x: root.effectiveRadius
                y: 0
                control1X: 0
                control1Y: root.effectiveRadius * 0.447715
                control2X: root.effectiveRadius * 0.447715
                control2Y: 0
            }

            fillGradient: LinearGradient {
                x1: root.gradientCenterX - root.gradientDirectionX * root.gradientLength / 2
                y1: root.gradientCenterY - root.gradientDirectionY * root.gradientLength / 2
                x2: root.gradientCenterX + root.gradientDirectionX * root.gradientLength / 2
                y2: root.gradientCenterY + root.gradientDirectionY * root.gradientLength / 2

                GradientStop {
                    position: 0
                    color: root.startColor
                }

                GradientStop {
                    position: root.gradientMiddlePosition
                    color: root.middleColor
                }

                GradientStop {
                    position: 1
                    color: root.endColor
                }

            }

        }

    }

}
