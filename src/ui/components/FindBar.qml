import Eden.Ui
import QtQuick

Rectangle {
    id: findBar

    required property var controller

    width: 360
    height: 52
    radius: Theme.cardRadius
    color: Theme.surfaceContainer
    border.color: Theme.menuBorder
    border.width: Theme.menuBorderWidth

    Row {
        anchors.fill: parent
        anchors.margins: 4
        spacing: 4

        EdenTextField {
            id: field

            width: parent.width - 124
            placeholderText: "Find in page"
            onTextEdited: findBar.controller.find(text)
            Keys.onReturnPressed: findBar.controller.find(text)
            Component.onCompleted: forceActiveFocus()
        }

        EdenButton {
            iconName: "chevron-up"
            onClicked: findBar.controller.find(field.text, true)
        }

        EdenButton {
            iconName: "chevron-down"
            onClicked: findBar.controller.find(field.text)
        }

        EdenButton {
            iconName: "close"
            onClicked: findBar.controller.findVisible = false
        }

    }

}
