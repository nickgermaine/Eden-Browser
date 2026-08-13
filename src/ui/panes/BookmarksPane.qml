import Eden.Ui
import QtQuick

Rectangle {
    id: pane

    required property var controller

    color: Theme.surfaceContainer
    radius: Theme.contentRadius
    border.width: Theme.paneBorderWidth
    border.color: Theme.paneBorder

    Item {
        id: header

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: 16
        anchors.rightMargin: 8
        height: 52

        Text {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            text: "Bookmarks"
            color: Theme.surfaceText
            font: Theme.titleFont
        }

        EdenButton {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            iconName: "close"
            onClicked: pane.controller.openPane = ""
        }

    }

    ListView {
        id: bookmarksView

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        anchors.bottomMargin: 8
        spacing: 4
        clip: true
        model: pane.controller.bookmarks

        delegate: EdenButton {
            required property string title
            required property url url

            width: ListView.view.width
            height: 44
            text: title.length > 0 ? title : url.toString()
            iconName: "bookmark-circle"
            onClicked: {
                pane.controller.navigate(url);
                pane.controller.openPane = "";
            }
        }

    }

    Text {
        anchors.centerIn: parent
        visible: bookmarksView.count === 0
        text: "No bookmarks yet"
        color: Theme.surfaceVariantText
        font: Theme.bodyFont
    }

}
