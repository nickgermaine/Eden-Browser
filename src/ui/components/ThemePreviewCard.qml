import Eden.Ui
import QtQuick

Rectangle {
    id: card

    property string themeId: "eden-default"
    property string themeName
    property string themeAuthor
    property bool active: false
    readonly property var preview: Themes.themePreview(themeId, Theme.dark)
    readonly property real miniScale: 0.55

    signal activated

    implicitHeight: cardColumn.implicitHeight + 24
    radius: Theme.cardRadius
    color: Theme.surfaceContainer
    border.width: active || cardHover.hovered ? 2 : 1
    border.color: active ? Theme.primary : cardHover.hovered ? Theme.outline : Qt.alpha(Theme.outline, 0.35)

    Column {
        id: cardColumn

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 12
        spacing: 10

        Item {
            id: miniWindow

            width: parent.width
            height: Math.round(width * 0.56)

            ChromeBackground {
                anchors.fill: parent
                radius: card.preview.windowRadius * card.miniScale
                gradientEnabled: card.preview.toolbarGradientEnabled
                gradientAngle: card.preview.toolbarGradientAngle
                gradientMiddlePosition: card.preview.toolbarGradientMiddlePosition
                startColor: card.preview.toolbarGradientStart
                middleColor: card.preview.toolbarGradientMiddle
                endColor: card.preview.toolbarGradientEnd
            }

            Rectangle {
                anchors.fill: parent
                radius: card.preview.windowRadius * card.miniScale
                color: "transparent"
                border.width: 1
                border.color: Qt.alpha(card.preview.outline, 0.6)
            }

            Column {
                anchors.fill: parent
                anchors.margins: 8
                spacing: 6

                Row {
                    id: miniTabRow

                    readonly property real tabWidth: (width - 2 * spacing) / 3

                    width: parent.width
                    height: 22
                    spacing: 4

                    Repeater {
                        model: 3

                        delegate: Rectangle {
                            id: miniTab

                            required property int index
                            readonly property bool activeTab: index === 0

                            width: miniTabRow.tabWidth
                            height: miniTabRow.height
                            radius: card.preview.cardRadius * card.miniScale
                            color: activeTab ? card.preview.surfaceContainerHighest : card.preview.tabBarBackground

                            Row {
                                anchors.left: parent.left
                                anchors.leftMargin: 7
                                anchors.right: parent.right
                                anchors.rightMargin: 7
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 5

                                Rectangle {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 7
                                    height: 7
                                    radius: 3.5
                                    color: miniTab.activeTab ? card.preview.primary : card.preview.iconColor
                                    opacity: miniTab.activeTab ? 1 : 0.65
                                }

                                Rectangle {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: parent.width - 12
                                    height: 4
                                    radius: 2
                                    color: miniTab.activeTab ? card.preview.surfaceText : card.preview.surfaceVariantText
                                    opacity: miniTab.activeTab ? 0.9 : 0.6
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    width: parent.width
                    height: 22
                    radius: Math.min(card.preview.controlRadius * card.miniScale, height / 2)
                    color: card.preview.surfaceContainerHigh

                    Row {
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 6

                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 7
                            height: 7
                            radius: 3.5
                            color: card.preview.iconColor
                            opacity: 0.8
                        }

                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: Math.round(miniWindow.width * 0.4)
                            height: 4
                            radius: 2
                            color: card.preview.surfaceVariantText
                            opacity: 0.7
                        }
                    }
                }

                Rectangle {
                    width: parent.width
                    height: Math.max(0, miniWindow.height - y - 16)
                    radius: card.preview.contentRadius * card.miniScale
                    color: card.preview.surface

                    Column {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.margins: 9
                        spacing: 6

                        Rectangle {
                            width: Math.round(miniWindow.width * 0.42)
                            height: 6
                            radius: 3
                            color: card.preview.surfaceText
                            opacity: 0.9
                        }

                        Rectangle {
                            width: Math.round(miniWindow.width * 0.74)
                            height: 4
                            radius: 2
                            color: card.preview.surfaceVariantText
                            opacity: 0.6
                        }

                        Rectangle {
                            width: Math.round(miniWindow.width * 0.6)
                            height: 4
                            radius: 2
                            color: card.preview.surfaceVariantText
                            opacity: 0.6
                        }

                        Rectangle {
                            width: 38
                            height: 13
                            radius: Math.min(card.preview.controlRadius * card.miniScale, height / 2)
                            color: card.preview.primary

                            Rectangle {
                                anchors.centerIn: parent
                                width: 20
                                height: 3
                                radius: 1.5
                                color: card.preview.primaryText
                                opacity: 0.9
                            }
                        }
                    }
                }
            }
        }

        Row {
            width: parent.width
            spacing: 8

            Column {
                width: parent.width - (activeCheck.visible ? activeCheck.width + parent.spacing : 0)
                spacing: 2

                Text {
                    textFormat: Text.PlainText
                    width: parent.width
                    text: card.themeName
                    color: Theme.surfaceText
                    font: Theme.labelFont
                    elide: Text.ElideRight
                }

                Text {
                    textFormat: Text.PlainText
                    width: parent.width
                    visible: card.themeAuthor.length > 0
                    text: card.themeAuthor
                    color: Theme.surfaceVariantText
                    font.family: Themes.fontFamily
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }
            }

            Icon {
                id: activeCheck

                anchors.verticalCenter: parent.verticalCenter
                visible: card.active
                name: "check"
                filled: true
                color: Theme.primary
            }
        }
    }

    HoverHandler {
        id: cardHover

        cursorShape: Qt.PointingHandCursor
    }

    TapHandler {
        onTapped: card.activated()
    }
}
