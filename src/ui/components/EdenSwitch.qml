import Eden.Ui
import QtQuick
import QtQuick.Controls

Switch {
    id: control

    implicitWidth: 42
    implicitHeight: 24
    padding: 0

    indicator: Rectangle {
        implicitWidth: 42
        implicitHeight: 24
        radius: 12
        color: control.checked ? Theme.primary : Theme.surfaceContainerHighest
        border.width: 1
        border.color: control.checked ? Theme.primary : Theme.outline

        Rectangle {
            x: control.checked ? parent.width - width - 3 : 3
            anchors.verticalCenter: parent.verticalCenter
            width: 18
            height: 18
            radius: 9
            color: control.checked ? Theme.onPrimary : Theme.surfaceVariantText

            Behavior on x {
                NumberAnimation {
                    duration: 140
                }

            }

        }

    }

    contentItem: Item {
    }

}
