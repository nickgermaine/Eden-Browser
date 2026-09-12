import Eden.Ui
import QtQuick
import QtQuick.Controls

Popup {
    id: submenu

    required property Item anchorItem
    required property Popup parentMenu
    property bool anchorHovered: false
    property real preferredWidth: 240
    property real maximumHeight: 480
    readonly property bool contentHovered: submenuHover.hovered
    readonly property bool pointerInside: anchorHovered || contentHovered

    function positionAtAnchor() {
        const rightAnchor = anchorItem.mapToItem(Overlay.overlay, anchorItem.width + parentMenu.padding + 2, 0);
        const leftAnchor = anchorItem.mapToItem(Overlay.overlay, -width - parentMenu.padding - 2, 0);
        x = rightAnchor.x + width <= Overlay.overlay.width - 8 ? rightAnchor.x : Math.max(8, leftAnchor.x);
        y = Math.max(8, Math.min(Overlay.overlay.height - height - 8, rightAnchor.y));
    }

    parent: Overlay.overlay
    width: preferredWidth
    height: Math.min(implicitHeight, maximumHeight)
    padding: 6
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onAboutToShow: positionAtAnchor()
    onPointerInsideChanged: {
        if (pointerInside)
            closeTimer.stop();
        else if (opened)
            closeTimer.restart();
    }

    HoverHandler {
        id: submenuHover

        parent: submenu.contentItem
    }

    Timer {
        interval: 160
        running: submenu.anchorHovered && !submenu.opened
        onTriggered: submenu.open()
    }

    Timer {
        id: closeTimer

        interval: 160
        onTriggered: {
            if (!submenu.pointerInside && submenu.opened)
                submenu.close();

        }
    }

    Connections {
        function onClosed() {
            submenu.close();
        }

        target: submenu.parentMenu
    }

    background: OverlaySurface {
        surfaceRadius: Theme.menuRadius
    }

}
