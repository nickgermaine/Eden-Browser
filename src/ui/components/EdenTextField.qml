import Eden.Ui
import QtQuick
import QtQuick.Templates as T

T.TextField {
    id: control

    implicitHeight: 44
    leftPadding: 16
    rightPadding: 16
    topPadding: 0
    bottomPadding: 0
    verticalAlignment: TextInput.AlignVCenter
    color: Theme.surfaceText
    selectionColor: Theme.primary
    selectedTextColor: Theme.primaryText
    placeholderTextColor: Theme.outline
    font: Theme.bodyFont

    background: Rectangle {
        radius: Math.min(Theme.controlRadius, height / 2)
        color: control.activeFocus ? Theme.surfaceContainerHighest : Theme.surfaceContainerHigh
        border.width: control.activeFocus ? Theme.focusBorderWidth : 0
        border.color: Theme.focusBorder

        Behavior on color {
            ColorAnimation {
                duration: Theme.shortDuration
            }

        }

    }

}
