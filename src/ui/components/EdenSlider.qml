import Eden.Ui
import QtQuick
import QtQuick.Templates as T

T.Slider {
    id: control

    implicitWidth: 190
    implicitHeight: 40
    leftPadding: 8
    rightPadding: 8
    topPadding: 8
    bottomPadding: 8

    background: Item {
        x: control.leftPadding
        y: Math.round((control.height - height) / 2)
        width: control.availableWidth
        height: 8

        Rectangle {
            anchors.fill: parent
            radius: height / 2
            color: Theme.surfaceContainerHighest
        }

        Rectangle {
            width: control.visualPosition * parent.width
            height: parent.height
            radius: height / 2
            color: Theme.primary
        }

    }

    handle: Rectangle {
        x: control.leftPadding + control.visualPosition * (control.availableWidth - width)
        y: Math.round((control.height - height) / 2)
        width: control.pressed ? 22 : 18
        height: width
        radius: width / 2
        color: Theme.primary
        border.width: 3
        border.color: Theme.surface
        scale: control.pressed ? 1.08 : 1

        Behavior on scale {
            NumberAnimation {
                duration: Theme.shortDuration
                easing.type: Easing.OutCubic
            }

        }

    }

}
