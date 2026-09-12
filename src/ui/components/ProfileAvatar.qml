import Eden.Ui
import QtQuick
import QtQuick.Effects

Item {
    id: avatar

    property string avatarUrl
    property string displayName
    property real avatarSize: 32

    implicitWidth: avatarSize
    implicitHeight: avatarSize
    Accessible.role: Accessible.Graphic
    Accessible.name: "Profile: " + displayName

    Item {
        id: imageLayer

        anchors.fill: parent
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
        }

    }

    Item {
        id: circleMask

        anchors.fill: parent
        layer.enabled: true
        layer.smooth: true
        visible: false

        Rectangle {
            anchors.fill: parent
            radius: Math.min(width, height) / 2
            color: "black"
        }

    }

    Icon {
        anchors.centerIn: parent
        visible: avatarImage.status !== Image.Ready
        name: "user-circle"
        iconSize: avatar.avatarSize
    }

}
