import Eden.Ui
import QtQuick
import QtQuick.Controls

Item {
    id: root

    required property url favicon
    required property bool loading
    required property bool muted
    required property bool audible
    required property string internalPage
    readonly property bool faviconAvailable: internalPage.length === 0 && !loading && faviconImage.status === Image.Ready

    implicitWidth: Theme.iconSize
    implicitHeight: Theme.iconSize

    Image {
        id: faviconImage

        anchors.fill: parent
        source: root.favicon
        sourceSize: Qt.size(Math.round(root.width * Screen.devicePixelRatio), Math.round(root.height * Screen.devicePixelRatio))
        fillMode: Image.PreserveAspectFit
        asynchronous: true
        cache: true
        visible: root.faviconAvailable
    }

    Icon {
        anchors.fill: parent
        name: root.internalPage.length > 0 ? "settings" : root.loading ? "refresh" : "globe"
        spinning: root.loading
        color: Theme.iconColor
        visible: !root.faviconAvailable
    }
}
