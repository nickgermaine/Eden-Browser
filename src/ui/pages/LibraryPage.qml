import Eden.Ui
import QtQuick

Rectangle {
    id: page

    required property var controller
    required property string kind
    property bool embedded: false
    readonly property string pageTitle: kind === "history" ? "History" : kind === "bookmarks" ? "Bookmarks" : "Downloads"

    color: Theme.surface

    Item {
        id: column

        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.topMargin: page.embedded ? 18 : 32
        anchors.bottomMargin: 16
        width: Math.min(parent.width - 48, 820)

        Item {
            id: titleRow

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 44

            Text {
                textFormat: Text.PlainText
                anchors.left: parent.left
                anchors.leftMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                text: page.pageTitle
                color: Theme.surfaceText
                font.family: Themes.fontFamily
                font.pixelSize: 24
                font.weight: Font.DemiBold
            }

            EdenButton {
                visible: page.kind !== "bookmarks"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                iconName: "trash"
                text: page.kind === "history" ? "Clear history" : "Clear finished"
                onClicked: {
                    if (page.kind === "history")
                        page.controller.clearHistory();
                    else
                        page.controller.downloads.clearFinished();
                }
            }
        }

        LibraryView {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: titleRow.bottom
            anchors.bottom: parent.bottom
            anchors.topMargin: 12
            controller: page.controller
            kind: page.kind
            showSearch: true
        }
    }
}
