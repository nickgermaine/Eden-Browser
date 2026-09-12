import Eden.Ui
import QtQuick
import QtQuick.Controls

Popup {
    id: palette

    required property var controller

    width: 560
    height: 440
    modal: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onOpened: {
        query.text = "";
        controller.shortcuts.query = "";
        query.forceActiveFocus();
    }
    onClosed: controller.commandPaletteVisible = false
    padding: 10

    background: OverlaySurface {
        surfaceRadius: Theme.cardRadius
    }

    contentItem: Column {
        spacing: 8

        EdenTextField {
            id: query

            width: parent.width
            placeholderText: "Type a command"
            onTextEdited: palette.controller.shortcuts.query = text
            Keys.onPressed: event => {
                if (event.key === Qt.Key_Down) {
                    commands.currentIndex = Math.min(commands.count - 1, commands.currentIndex + 1);
                    event.accepted = true;
                } else if (event.key === Qt.Key_Up) {
                    commands.currentIndex = Math.max(0, commands.currentIndex - 1);
                    event.accepted = true;
                } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                    palette.controller.shortcuts.executeRow(commands.currentIndex);
                    palette.close();
                    event.accepted = true;
                }
            }
        }

        ListView {
            id: commands

            width: parent.width
            height: parent.height - query.height - parent.spacing
            model: palette.controller.shortcuts
            currentIndex: 0
            clip: true

            delegate: Rectangle {
                id: command

                required property int index
                required property string title
                required property string shortcut

                width: commands.width
                height: 46
                radius: Theme.suggestionRadius
                color: commands.currentIndex === index ? Theme.surfaceContainerHighest : hover.hovered ? Theme.surfaceContainerHigh : "transparent"

                Text {
                    textFormat: Text.PlainText
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: command.title
                    color: Theme.surfaceText
                    font: Theme.bodyFont
                }

                Text {
                    textFormat: Text.PlainText
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: command.shortcut
                    color: Theme.surfaceVariantText
                    font: Theme.labelFont
                }

                HoverHandler {
                    id: hover

                    onHoveredChanged: {
                        if (hovered)
                            commands.currentIndex = command.index;
                    }
                }

                TapHandler {
                    onTapped: {
                        palette.controller.shortcuts.executeRow(command.index);
                        palette.close();
                    }
                }
            }
        }
    }
}
