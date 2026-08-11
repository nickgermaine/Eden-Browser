pragma ComponentBehavior: Bound

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

            width: menuList.width
            height: subtitle.length > 0 ? 54 : 40

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
                    width: Math.max(0, parent.width - (menuIcon.visible ? menuIcon.width + parent.spacing : 0))
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

            }

            HoverHandler {
                id: menuHover

                enabled: menuItem.actionEnabled
            }

            TapHandler {
                id: menuTap

                enabled: menuItem.actionEnabled
                onTapped: {
                    menu.triggered(menuItem.modelData.id);
                    menu.close();
                }
            }

        }

    }

}
