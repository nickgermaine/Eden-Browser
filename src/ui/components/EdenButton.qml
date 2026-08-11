import Eden.Ui
import QtQuick
import QtQuick.Templates as T

T.Button {
    id: control

    property string iconName
    property string iconFamily: "solar"
    property bool filledIcon: false
    property int horizontalContentAlignment: text.length > 0 ? Qt.AlignLeft : Qt.AlignHCenter

    implicitWidth: text.length > 0 ? Math.max(48, contentItem.implicitWidth + 28) : 40
    implicitHeight: 40
    hoverEnabled: true

    contentItem: Item {
        implicitWidth: contentRow.implicitWidth
        implicitHeight: contentRow.implicitHeight

        Row {
            id: contentRow

            x: control.horizontalContentAlignment === Qt.AlignHCenter ? Math.round((parent.width - width) / 2) : 14
            y: Math.round((parent.height - height) / 2)
            spacing: 8

            Icon {
                visible: control.iconName.length > 0
                name: control.iconName
                family: control.iconFamily
                filled: control.filledIcon
                color: control.enabled ? Theme.iconColor : Qt.alpha(Theme.disabledIconColor, Theme.disabledIconOpacity)
                anchors.verticalCenter: parent.verticalCenter
            }

            Text {
                visible: control.text.length > 0
                text: control.text
                color: control.enabled ? Theme.surfaceText : Theme.disabledText
                font: Theme.labelFont
                anchors.verticalCenter: parent.verticalCenter
            }

        }

    }

    background: Rectangle {
        radius: Math.min(Theme.controlRadius, height / 2)
        color: control.down ? Theme.surfaceContainerHighest : control.hovered ? Theme.surfaceContainerHigh : "transparent"
        scale: control.down ? 0.94 : 1

        Behavior on scale {
            NumberAnimation {
                duration: Theme.shortDuration
                easing.type: Easing.OutCubic
            }

        }

    }

}
