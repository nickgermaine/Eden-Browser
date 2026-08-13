import Eden.Ui
import QtQuick
import QtQuick.Controls

Item {
    id: page

    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth

        Column {
            width: Math.min(820, Math.max(480, parent.width - 64))
            x: Math.round((parent.width - width) / 2)
            topPadding: 32
            bottomPadding: 48
            spacing: 20

            Text {
                text: "Engine"
                color: Theme.surfaceText
                font.family: Themes.fontFamily
                font.pixelSize: 30
                font.weight: Font.DemiBold
            }

            Rectangle {
                width: parent.width
                height: engineColumn.implicitHeight + 32
                radius: Theme.cardRadius
                color: Theme.surfaceContainer

                Column {
                    id: engineColumn

                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 12

                    Text {
                        text: "Default rendering engine"
                        color: Theme.surfaceText
                        font: Theme.titleFont
                    }

                    Text {
                        width: parent.width
                        text: "New tabs use this engine. Existing tabs switch now, while background tabs wait until you open them."
                        color: Theme.surfaceVariantText
                        font: Theme.bodyFont
                        wrapMode: Text.Wrap
                    }

                    Rectangle {
                        width: parent.width
                        height: 44
                        radius: Math.min(Theme.controlRadius, height / 2)
                        color: engineMenu.opened || engineHover.hovered ? Theme.surfaceContainerHighest : Theme.surfaceContainerHigh
                        border.width: engineMenu.opened ? Theme.focusBorderWidth : 0
                        border.color: Theme.focusBorder

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 16
                            anchors.right: engineChevron.left
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: Settings.defaultEngineName
                            color: Theme.surfaceText
                            font: Theme.bodyFont
                            elide: Text.ElideRight
                        }

                        Icon {
                            id: engineChevron

                            anchors.right: parent.right
                            anchors.rightMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            name: engineMenu.opened ? "chevron-up" : "chevron-down"
                        }

                        HoverHandler {
                            id: engineHover

                            cursorShape: Qt.PointingHandCursor
                        }

                        TapHandler {
                            onTapped: engineMenu.open()
                        }

                        EdenMenu {
                            id: engineMenu

                            y: parent.height + 4
                            preferredWidth: parent.width
                            actions: Settings.engineActions
                            onTriggered: (actionId) => {
                                return Settings.selectDefaultEngine(actionId);
                            }
                        }

                    }

                }

            }

        }

    }

}
