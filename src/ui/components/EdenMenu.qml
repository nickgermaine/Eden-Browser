import Eden.Ui
import QtQuick
import QtQuick.Controls

Popup {
    id: menu

    property var actions: []
    property real maximumHeight: 480
    property real preferredWidth: 240

    signal triggered(var actionId)

    padding: 6
    implicitWidth: preferredWidth
    implicitHeight: Math.min(menu.maximumHeight, menuList.contentHeight + topPadding + bottomPadding)
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: OverlaySurface {
        surfaceRadius: Theme.menuRadius
    }

    contentItem: ListView {
        id: menuList

        clip: true
        spacing: 2
        model: menu.actions

        delegate: Item {
            id: menuItem

            required property var modelData
            readonly property string subtitle: modelData.subtitle || ""
            readonly property bool actionEnabled: modelData.enabled === undefined || modelData.enabled
            readonly property bool hasChildren: modelData.children !== undefined && modelData.children.length > 0
            readonly property bool submenuPointerInside: menuHover.hovered || childMenuHover.hovered

            width: menuList.width
            height: subtitle.length > 0 ? 54 : 40
            onSubmenuPointerInsideChanged: {
                if (submenuPointerInside)
                    childCloseTimer.stop();
                else if (childMenu.opened)
                    childCloseTimer.restart();
            }

            Rectangle {
                anchors.fill: parent
                radius: Theme.controlRadius
                color: Theme.surfaceContainerHigh
                opacity: menuHover.hovered || menuTap.pressed ? 1 : 0

                Behavior on opacity {
                    NumberAnimation {
                        duration: Theme.shortDuration
                        easing.type: Easing.OutCubic
                    }

                }

            }

            Row {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 10

                Icon {
                    id: menuIcon

                    anchors.verticalCenter: parent.verticalCenter
                    visible: name.length > 0
                    name: menuItem.modelData.icon || ""
                    color: menuItem.actionEnabled ? Theme.iconColor : Theme.disabledIconColor
                }

                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.max(0, parent.width - (menuIcon.visible ? menuIcon.width + parent.spacing : 0) - (menuItem.hasChildren ? 24 : 0))
                    spacing: 2

                    Text {
                        width: parent.width
                        text: menuItem.modelData.title
                        color: menuItem.actionEnabled ? Theme.surfaceText : Theme.disabledText
                        font: Theme.labelFont
                        elide: Text.ElideRight
                    }

                    Text {
                        width: parent.width
                        visible: menuItem.subtitle.length > 0
                        text: menuItem.subtitle
                        color: Theme.surfaceVariantText
                        font.family: Theme.bodyFont.family
                        font.pixelSize: 11
                        elide: Text.ElideMiddle
                    }

                }

                Icon {
                    visible: menuItem.hasChildren
                    anchors.verticalCenter: parent.verticalCenter
                    name: "arrow-right"
                    color: menuItem.actionEnabled ? Theme.iconColor : Theme.disabledIconColor
                }

            }

            HoverHandler {
                id: menuHover

                enabled: menuItem.actionEnabled
            }

            TapHandler {
                id: menuTap

                enabled: menuItem.actionEnabled
                onTapped: {
                    if (menuItem.hasChildren) {
                        childMenu.open();
                        return ;
                    }
                    const rootMenu = menu;
                    const actionId = menuItem.modelData.id;
                    rootMenu.triggered(actionId);
                    if (rootMenu)
                        rootMenu.close();

                }
            }

            Timer {
                interval: 160
                running: menuHover.hovered && menuItem.hasChildren && !childMenu.opened
                onTriggered: childMenu.open()
            }

            Timer {
                id: childCloseTimer

                interval: 160
                onTriggered: {
                    if (!menuItem.submenuPointerInside && childMenu.opened)
                        childMenu.close();

                }
            }

            Popup {
                id: childMenu

                parent: Overlay.overlay
                width: menu.preferredWidth
                height: Math.min(menu.maximumHeight, childList.contentHeight + topPadding + bottomPadding)
                padding: 6
                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                onAboutToShow: {
                    const anchor = menuItem.mapToItem(Overlay.overlay, menuItem.width + menu.padding + 2, 0);
                    let targetX = anchor.x;
                    if (targetX + width > Overlay.overlay.width - 8)
                        targetX = Math.max(8, anchor.x - menuItem.width - menu.padding * 2 - width - 4);

                    x = targetX;
                    y = Math.max(8, Math.min(Overlay.overlay.height - height - 8, anchor.y));
                }

                background: OverlaySurface {
                    surfaceRadius: Theme.menuRadius
                }

                contentItem: ListView {
                    id: childList

                    clip: true
                    spacing: 2
                    model: menuItem.hasChildren ? menuItem.modelData.children : []

                    HoverHandler {
                        id: childMenuHover
                    }

                    delegate: Rectangle {
                        id: childItem

                        required property var modelData

                        width: childList.width
                        height: 40
                        radius: Theme.controlRadius
                        color: childHover.hovered || childTap.pressed ? Theme.surfaceContainerHigh : "transparent"

                        Text {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            text: childItem.modelData.title
                            color: Theme.surfaceText
                            font: Theme.labelFont
                            elide: Text.ElideRight
                        }

                        HoverHandler {
                            id: childHover
                        }

                        TapHandler {
                            id: childTap

                            onTapped: {
                                const rootMenu = menu;
                                const subMenu = childMenu;
                                const actionId = childItem.modelData.id;
                                rootMenu.triggered(actionId);
                                if (subMenu)
                                    subMenu.close();

                                if (rootMenu)
                                    rootMenu.close();

                            }
                        }

                    }

                }

            }

        }

    }

}
