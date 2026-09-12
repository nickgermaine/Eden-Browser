import Eden.Ui
import QtQuick
import QtQuick.Controls

Popup {
    id: menu

    property var actions: []
    property Component headerComponent
    property real maximumHeight: 480
    property real preferredWidth: 240
    property double dismissedAt: 0

    signal triggered(var actionId)

    function tryOpen() {
        if (opened)
            return true;

        if (Date.now() - dismissedAt < 250)
            return false;

        open();
        return true;
    }

    function toggle() {
        if (opened) {
            close();
            return;
        }
        tryOpen();
    }

    padding: 6
    implicitWidth: preferredWidth
    implicitHeight: Math.min(menu.maximumHeight, menuList.contentHeight + topPadding + bottomPadding)
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onClosed: dismissedAt = Date.now()

    background: OverlaySurface {
        surfaceRadius: Theme.menuRadius
    }

    contentItem: ListView {
        id: menuList

        clip: true
        spacing: 2
        model: menu.actions

        header: Loader {
            width: menuList.width
            active: menu.headerComponent !== null
            sourceComponent: menu.headerComponent
        }

        delegate: Item {
            id: menuItem

            required property var modelData
            readonly property string actionTitle: modelData.title === undefined || modelData.title === null ? "" : String(modelData.title)
            readonly property string subtitle: modelData.subtitle === undefined || modelData.subtitle === null ? "" : String(modelData.subtitle)
            readonly property string iconName: modelData.icon === undefined || modelData.icon === null || String(modelData.icon).length === 0 ? (checked ? "check" : "menu-dots") : String(modelData.icon)
            readonly property bool actionEnabled: modelData.enabled === undefined || modelData.enabled
            readonly property bool hasChildren: modelData.children !== undefined && modelData.children.length > 0
            readonly property bool separator: modelData.separator === true
            readonly property bool checked: modelData.checked === true

            width: menuList.width
            height: separator ? 9 : subtitle.length > 0 ? 54 : 40

            Rectangle {
                visible: menuItem.separator
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                height: Theme.paneBorderWidth
                color: Theme.paneBorder
            }

            Rectangle {
                visible: !menuItem.separator
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
                visible: !menuItem.separator
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 10

                Icon {
                    id: menuIcon

                    anchors.verticalCenter: parent.verticalCenter
                    visible: name.length > 0
                    name: menuItem.iconName
                    color: menuItem.actionEnabled ? Theme.iconColor : Theme.disabledIconColor
                }

                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.max(0, parent.width - (menuIcon.visible ? menuIcon.width + parent.spacing : 0) - (menuItem.hasChildren ? 24 : 0))
                    spacing: 2

                    Text {
                        textFormat: Text.PlainText
                        width: parent.width
                        text: menuItem.actionTitle
                        color: menuItem.actionEnabled ? Theme.surfaceText : Theme.disabledText
                        font: Theme.labelFont
                        elide: Text.ElideRight
                    }

                    Text {
                        textFormat: Text.PlainText
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

                enabled: menuItem.actionEnabled && !menuItem.separator
            }

            TapHandler {
                id: menuTap

                enabled: menuItem.actionEnabled && !menuItem.separator
                onTapped: {
                    if (menuItem.hasChildren) {
                        childMenu.open();
                        return;
                    }
                    const rootMenu = menu;
                    const actionId = menuItem.modelData.id;
                    rootMenu.triggered(actionId);
                    if (rootMenu)
                        rootMenu.close();
                }
            }

            AnchoredSubmenu {
                id: childMenu

                anchorItem: menuItem
                parentMenu: menu
                anchorHovered: menuHover.hovered && menuItem.hasChildren
                preferredWidth: menu.preferredWidth
                maximumHeight: menu.maximumHeight
                implicitHeight: childList.contentHeight + topPadding + bottomPadding

                contentItem: ListView {
                    id: childList

                    clip: true
                    spacing: 2
                    model: menuItem.hasChildren ? menuItem.modelData.children : []

                    delegate: Rectangle {
                        id: childItem

                        required property var modelData
                        readonly property string actionTitle: modelData.title === undefined || modelData.title === null ? "" : String(modelData.title)
                        readonly property string avatarUrl: modelData.avatarUrl === undefined || modelData.avatarUrl === null ? "" : String(modelData.avatarUrl)
                        readonly property bool separator: modelData.separator === true
                        readonly property bool checked: modelData.checked === true
                        readonly property string iconName: modelData.icon === undefined || modelData.icon === null || String(modelData.icon).length === 0 ? (checked ? "check" : "menu-dots") : String(modelData.icon)
                        readonly property string trailingIconName: modelData.trailingIcon === undefined || modelData.trailingIcon === null ? "" : String(modelData.trailingIcon)

                        width: childList.width
                        height: separator ? 9 : 40
                        radius: Theme.controlRadius
                        color: !separator && (childHover.hovered || childTap.pressed) ? Theme.surfaceContainerHigh : "transparent"

                        Rectangle {
                            visible: childItem.separator
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            height: Theme.paneBorderWidth
                            color: Theme.paneBorder
                        }

                        Row {
                            visible: !childItem.separator
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            spacing: 10

                            ProfileAvatar {
                                anchors.verticalCenter: parent.verticalCenter
                                visible: childItem.avatarUrl.length > 0
                                avatarUrl: childItem.avatarUrl
                                displayName: childItem.actionTitle
                                avatarSize: 28
                            }

                            Icon {
                                anchors.verticalCenter: parent.verticalCenter
                                visible: childItem.avatarUrl.length === 0 && childItem.iconName.length > 0
                                name: childItem.iconName
                            }

                            Text {
                                textFormat: Text.PlainText
                                anchors.verticalCenter: parent.verticalCenter
                                width: Math.max(0, parent.width - (childItem.avatarUrl.length > 0 || childItem.iconName.length > 0 ? 38 : 0) - (childItem.trailingIconName.length > 0 ? 26 : 0))
                                text: childItem.actionTitle
                                color: Theme.surfaceText
                                font: Theme.labelFont
                                elide: Text.ElideRight
                            }

                            Icon {
                                anchors.verticalCenter: parent.verticalCenter
                                visible: childItem.trailingIconName.length > 0
                                name: childItem.trailingIconName
                                iconSize: 16
                            }
                        }

                        HoverHandler {
                            id: childHover

                            enabled: !childItem.separator && (childItem.modelData.enabled === undefined || childItem.modelData.enabled)
                        }

                        TapHandler {
                            id: childTap

                            enabled: !childItem.separator && (childItem.modelData.enabled === undefined || childItem.modelData.enabled)
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
