import Eden.Ui
import QtQuick
import QtQuick.Controls
import QtQuick.Effects

Item {
    id: root

    required property url favicon
    required property bool loading
    required property bool muted
    required property bool audible
    required property string internalPage
    property bool discarded: false
    readonly property bool faviconAvailable: internalPage.length === 0 && !loading && faviconImage.status === Image.Ready
    readonly property string internalIcon: internalPage === "history" ? "history" : internalPage === "bookmarks" ? "bookmark-circle" : internalPage === "downloads" ? "round-transfer-vertical" : "settings"

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
        opacity: root.discarded ? 0.72 : 1
        layer.enabled: root.discarded && root.faviconAvailable

        layer.effect: MultiEffect {
            saturation: -1
            brightness: Theme.dark ? 0.25 : -0.1
            contrast: 0.1
        }
    }

    Icon {
        anchors.fill: parent
        name: root.internalPage.length > 0 ? root.internalIcon : root.loading ? "refresh" : "globe"
        spinning: root.loading
        color: Theme.iconColor
        visible: !root.faviconAvailable
    }
}
