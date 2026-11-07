import Eden.Ui
import QtQuick

Item {
    id: root

    property url favicon
    property string fallbackIcon: "globe"
    property real iconExtent: Theme.iconSize

    implicitWidth: iconExtent
    implicitHeight: iconExtent

    Image {
        id: faviconImage

        anchors.fill: parent
        source: root.favicon
        sourceSize: Qt.size(Math.round(root.width * Screen.devicePixelRatio), Math.round(root.height * Screen.devicePixelRatio))
        fillMode: Image.PreserveAspectFit
        asynchronous: true
        cache: true
        visible: status === Image.Ready
    }

    Icon {
        anchors.fill: parent
        name: root.fallbackIcon
        iconSize: root.iconExtent
        visible: faviconImage.status !== Image.Ready
    }
}
