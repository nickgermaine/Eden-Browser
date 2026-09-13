import Eden.Ui
import QtQuick
import QtQuick.Effects
import QtQuick.Shapes

Item {
    id: avatar

    property string avatarUrl
    property string displayName
    property real avatarSize: 32
    readonly property real diameter: Math.max(0, Math.min(width, height))

    implicitWidth: avatarSize
    implicitHeight: avatarSize
    Accessible.role: Accessible.Graphic
    Accessible.name: "Profile: " + displayName

    Item {
        id: imageLayer

        anchors.centerIn: parent
        width: avatar.diameter
        height: width
        visible: avatarImage.status === Image.Ready
        layer.enabled: true
        layer.smooth: true

        Image {
            id: avatarImage

            anchors.fill: parent
            source: avatar.avatarUrl
            sourceSize: Qt.size(Math.round(width * Screen.devicePixelRatio), Math.round(height * Screen.devicePixelRatio))
            fillMode: Image.PreserveAspectCrop
            cache: true
        }

        layer.effect: MultiEffect {
            maskEnabled: true
            maskSource: circleMask
            maskThresholdMin: 0.5
            maskSpreadAtMin: 1.0
        }
    }

    Shape {
        id: circleMask

        anchors.centerIn: parent
        width: avatar.diameter
        height: width
        preferredRendererType: Shape.CurveRenderer
        layer.enabled: true
        layer.smooth: true
        visible: false

        ShapePath {
            strokeWidth: -1
            fillColor: "white"

            PathAngleArc {
                centerX: circleMask.width / 2
                centerY: circleMask.height / 2
                radiusX: circleMask.width / 2
                radiusY: radiusX
                startAngle: 0
                sweepAngle: 360
            }
        }
    }

    Icon {
        anchors.centerIn: parent
        visible: avatarImage.status !== Image.Ready
        name: "user-circle"
        iconSize: avatar.diameter
    }
}
