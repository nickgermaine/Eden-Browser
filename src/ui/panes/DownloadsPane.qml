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
            text: "Downloads"
            color: Theme.surfaceText
            font: Theme.titleFont
        }

        Row {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter

            EdenButton {
                iconName: "trash"
                onClicked: pane.controller.downloads.clearFinished()
            }

            EdenButton {
                iconName: "close"
                onClicked: pane.controller.openPane = ""
            }

        }

    }

    ListView {
        id: downloadsView

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        anchors.bottomMargin: 8
        spacing: 4
        clip: true
        model: pane.controller.downloads

        delegate: Rectangle {
            required property string fileName
            required property double receivedBytes
            required property double totalBytes
            required property string downloadState

            width: ListView.view.width
            height: 64
            color: "transparent"

            Text {
                anchors.left: parent.left
                anchors.top: parent.top
                text: fileName
                color: Theme.surfaceText
                font: Theme.bodyFont
            }

            Text {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                text: downloadState
                color: Theme.surfaceVariantText
                font: Theme.labelFont
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                height: 4
                radius: Math.min(Theme.controlRadius, height / 2)
                color: Theme.surfaceContainerHighest

                Rectangle {
                    width: parent.width * Math.min(1, totalBytes > 0 ? receivedBytes / totalBytes : 0)
                    height: parent.height
                    radius: Math.min(Theme.controlRadius, height / 2)
                    color: Theme.primary
                }

            }

        }

    }

    Text {
        anchors.centerIn: parent
        visible: downloadsView.count === 0
        text: "No downloads yet"
        color: Theme.surfaceVariantText
        font: Theme.bodyFont
    }

}
