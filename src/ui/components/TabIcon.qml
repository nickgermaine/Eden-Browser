import Eden.Ui
import QtQuick

Item {
    id: root

    required property url favicon
    required property bool loading
    required property bool muted
    required property bool audible
    required property string internalPage
    readonly property bool faviconAvailable: internalPage.length === 0 && !loading && !muted && !audible && faviconImage.status === Image.Ready

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
        name: root.internalPage.length > 0 ? "settings" : root.loading ? "refresh" : root.muted ? "muted" : root.audible ? "volume" : "globe"
        spinning: root.loading
        visible: !root.faviconAvailable
    }

}
