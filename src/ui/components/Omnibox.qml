import Eden.Ui
import QtQuick
import QtQuick.Controls

Rectangle {
    id: omnibox

    required property var controller
    property var engine: controller.currentEngine

    signal securityRequested()

    implicitHeight: 40
    radius: Math.min(Theme.controlRadius, height / 2)
    color: field.activeFocus ? Theme.surfaceContainerHighest : Theme.surfaceContainerHigh
    border.width: field.activeFocus ? Theme.focusBorderWidth : 0
    border.color: Theme.focusBorder

    EdenButton {
        id: securityButton

        anchors.left: parent.left
        anchors.leftMargin: 2
        anchors.verticalCenter: parent.verticalCenter
        width: 36
        height: 36
        iconName: omnibox.engine && omnibox.engine.securityState === "secure" ? "lock" : "tuning"
        enabled: !!omnibox.engine
        onClicked: omnibox.securityRequested()
    }

    EdenTextField {
        id: field

        anchors.left: securityButton.right
        anchors.right: bookmarkButton.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        leftPadding: 8
        rightPadding: 8
        placeholderText: "Search or enter address"
        selectByMouse: true
        text: omnibox.controller.displayUrl
        onActiveFocusChanged: {
            if (activeFocus) {
                omnibox.controller.omnibox.query = "";
                text = omnibox.controller.currentUrl.toString();
                selectAll();
            } else {
                text = omnibox.controller.displayUrl;
                cursorPosition = 0;
                deselect();
            }
        }
        onTextEdited: {
            suggestions.currentIndex = 0;
            omnibox.controller.omnibox.query = text;
        }
        Keys.onPressed: (event) => {
            if (event.key === Qt.Key_Down) {
                suggestions.currentIndex = Math.min(suggestions.count - 1, suggestions.currentIndex + 1);
                event.accepted = true;
            } else if (event.key === Qt.Key_Up) {
                suggestions.currentIndex = Math.max(0, suggestions.currentIndex - 1);
                event.accepted = true;
            } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                if ((event.modifiers & Qt.ControlModifier) !== 0)
                    omnibox.controller.navigateText(text, true);
                else if (suggestions.count > 0)
                    omnibox.controller.activateSuggestion(Math.max(0, suggestions.currentIndex));
                else
                    omnibox.controller.navigateText(text, false);
                field.focus = false;
                event.accepted = true;
            } else if (event.key === Qt.Key_Escape) {
                field.focus = false;
                event.accepted = true;
            }
        }

        background: Item {
        }

    }

    Text {
        x: field.x + field.cursorRectangle.x + field.cursorRectangle.width
        anchors.verticalCenter: field.verticalCenter
        width: Math.max(0, bookmarkButton.x - x - 8)
        visible: field.activeFocus && suggestions.currentIndex === 0 && field.cursorPosition === field.length && field.selectionStart === field.selectionEnd && omnibox.controller.omnibox.query === field.text && text.length > 0
        text: omnibox.controller.omnibox ? omnibox.controller.omnibox.completionSuffix : ""
        color: Theme.completionHint
        font: Theme.bodyFont
        elide: Text.ElideRight
    }

    EdenButton {
        id: bookmarkButton

        anchors.right: parent.right
        anchors.rightMargin: 2
        anchors.verticalCenter: parent.verticalCenter
        width: 36
        height: 36
        iconName: "star"
        filledIcon: omnibox.controller.currentBookmarked
        enabled: !!omnibox.engine
        onClicked: omnibox.controller.toggleBookmark()
    }

    Connections {
        function onCurrentEngineChanged() {
            if (!field.activeFocus) {
                field.text = omnibox.controller.displayUrl;
                field.cursorPosition = 0;
            }
        }

        function onFocusOmniboxRequested() {
            field.forceActiveFocus();
            field.selectAll();
        }

        target: omnibox.controller
    }

    Popup {
        id: suggestionPopup

        popupType: Popup.Item
        parent: omnibox
        x: 0
        y: omnibox.height + 4
        width: omnibox.width
        height: Math.min(420, suggestions.contentHeight + 12)
        visible: suggestions.count > 0
        focus: false
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: 6

        background: OverlaySurface {
            surfaceRadius: Theme.menuRadius
        }

        contentItem: ListView {
            id: suggestions

            model: omnibox.controller.omnibox
            currentIndex: 0
            clip: true
            onCountChanged: currentIndex = count > 0 ? 0 : -1

            delegate: Rectangle {
                id: suggestion

                required property int index
                required property string title
                required property url url
                required property string kind

                width: suggestions.width
                height: 48
                radius: Theme.suggestionRadius
                color: suggestions.currentIndex === index ? Theme.surfaceContainerHighest : hover.hovered ? Theme.surfaceContainerHigh : "transparent"

                Row {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 12

                    Icon {
                        id: suggestionIcon

                        anchors.verticalCenter: parent.verticalCenter
                        name: suggestion.kind === "tab" ? "window" : suggestion.kind === "bookmark" ? "star" : suggestion.kind === "history" ? "history" : "search"
                    }

                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - suggestionIcon.width - parent.spacing

                        Text {
                            width: parent.width
                            text: suggestion.title
                            color: Theme.surfaceText
                            font: Theme.bodyFont
                            elide: Text.ElideRight
                        }

                        Text {
                            width: parent.width
                            text: suggestion.url.toString()
                            color: Theme.surfaceVariantText
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }

                    }

                }

                HoverHandler {
                    id: hover

                    onHoveredChanged: {
                        if (hovered)
                            suggestions.currentIndex = suggestion.index;

                    }
                }

                TapHandler {
                    onTapped: {
                        field.focus = false;
                        omnibox.controller.activateSuggestion(suggestion.index);
                    }
                }

            }

        }

    }

    Behavior on color {
        ColorAnimation {
            duration: Theme.shortDuration
        }

    }

}
