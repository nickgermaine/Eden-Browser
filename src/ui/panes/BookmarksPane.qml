import Eden.Ui
import QtQuick

Rectangle {
    id: pane

    required property var controller

    color: "transparent"

    Item {
        id: header

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: 16
        anchors.rightMargin: 8
        height: 52

        Text {
            textFormat: Text.PlainText
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            text: "Bookmarks"
            color: Theme.surfaceText
            font: Theme.titleFont
        }

        Row {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter

            EdenButton {
                iconName: "expand"
                Accessible.name: "Open bookmarks page"
                onClicked: {
                    pane.controller.openLibraryTab("bookmarks");
                    pane.controller.openPane = "";
                }
            }

            EdenButton {
                iconName: "close"
                onClicked: pane.controller.openPane = ""
            }
        }
    }

    LibraryView {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 8
        controller: pane.controller
        kind: "bookmarks"
        closePaneOnOpen: true
    }
}
