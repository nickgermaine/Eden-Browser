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
        buttonRadius: 20
        iconName: omnibox.engine && omnibox.engine.securityState === "secure" ? "lock" : omnibox.engine && omnibox.engine.securityState === "insecure" ? "info-circle" : "tuning"
        enabled: !!omnibox.engine
        Accessible.name: "Page information"
        onClicked: omnibox.securityRequested()
    }

    EdenTextField {
        id: field

        objectName: "omniboxField"
        anchors.left: securityButton.right
        anchors.right: keyButton.visible ? keyButton.left : bookmarkButton.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        leftPadding: 8
        rightPadding: 8
        placeholderText: "Search or enter address"
        selectByMouse: true
        text: omnibox.controller.displayUrl
        onPressed: {
            if (omnibox.engine)
                omnibox.engine.releaseFocus();

        }
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
        id: keyButton

        anchors.right: bookmarkButton.left
        anchors.verticalCenter: parent.verticalCenter
        width: visible ? 36 : 0
        height: 36
        visible: omnibox.controller.credentialKeyVisible
        iconName: "password"
        filledIcon: omnibox.controller.credentialPrompt.mode !== undefined
        onClicked: credentialPopup.opened ? credentialPopup.close() : credentialPopup.open()
    }

    EdenButton {
        id: bookmarkButton

        anchors.right: parent.right
        anchors.rightMargin: 2
        anchors.verticalCenter: parent.verticalCenter
        width: 36
        height: 36
        iconName: "bookmark-circle"
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
            if (omnibox.engine)
                omnibox.engine.releaseFocus();

            field.forceActiveFocus();
            field.selectAll();
        }

        function onCredentialPromptRequested() {
            credentialPopup.open();
        }

        target: omnibox.controller
    }

    Popup {
        id: credentialPopup

        property bool passwordRevealed: false

        popupType: Popup.Item
        parent: omnibox
        x: Math.max(0, omnibox.width - width)
        y: omnibox.height + 4
        width: Math.min(380, omnibox.width)
        padding: 12
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onOpened: passwordRevealed = false

        background: OverlaySurface {
            surfaceRadius: Theme.menuRadius
        }

        contentItem: Column {
            spacing: 10

            Column {
                visible: omnibox.controller.credentialPrompt.mode !== undefined
                width: parent.width
                spacing: 8

                Text {
                    width: parent.width
                    text: omnibox.controller.credentialPrompt.mode === "update" ? "Update saved password?" : "Save password?"
                    color: Theme.surfaceText
                    font: Theme.titleFont
                }

                Text {
                    width: parent.width
                    text: omnibox.controller.credentialPrompt.site || ""
                    color: Theme.surfaceVariantText
                    font: Theme.bodyFont
                    elide: Text.ElideRight
                }

                EdenTextField {
                    width: parent.width
                    text: omnibox.controller.credentialDraftUsername
                    placeholderText: "Email or username"
                    Accessible.name: "Email or username"
                    onTextEdited: omnibox.controller.credentialDraftUsername = text
                }

                Item {
                    width: parent.width
                    height: 44

                    EdenTextField {
                        id: credentialPasswordField

                        anchors.fill: parent
                        rightPadding: 48
                        text: omnibox.controller.credentialDraftPassword
                        echoMode: credentialPopup.passwordRevealed ? TextInput.Normal : TextInput.Password
                        inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhHiddenText
                        placeholderText: "Password"
                        Accessible.name: "Password"
                        onTextEdited: omnibox.controller.credentialDraftPassword = text
                    }

                    EdenButton {
                        anchors.right: parent.right
                        anchors.rightMargin: 2
                        anchors.verticalCenter: parent.verticalCenter
                        width: 40
                        height: 40
                        iconName: "eye"
                        filledIcon: credentialPopup.passwordRevealed
                        Accessible.name: credentialPopup.passwordRevealed ? "Hide password" : "Show password"
                        onClicked: credentialPopup.passwordRevealed = !credentialPopup.passwordRevealed
                    }

                }

                Row {
                    spacing: 8

                    EdenButton {
                        text: omnibox.controller.credentialPrompt.mode === "update" ? "Update" : "Save"
                        enabled: omnibox.controller.credentialDraftPassword.length > 0
                        onClicked: {
                            omnibox.controller.acceptCredentialPrompt();
                            credentialPopup.close();
                        }
                    }

                    EdenButton {
                        text: "Not now"
                        onClicked: {
                            omnibox.controller.dismissCredentialPrompt();
                            credentialPopup.close();
                        }
                    }

                }

            }

            Text {
                visible: omnibox.controller.autofillSuggestions.length > 0
                text: "Auto-fill"
                color: Theme.surfaceText
                font: Theme.titleFont
            }

            Repeater {
                model: omnibox.controller.autofillSuggestions

                delegate: Rectangle {
                    required property var modelData

                    width: parent.width
                    height: 50
                    radius: Theme.suggestionRadius
                    color: fillHover.hovered ? Theme.surfaceContainerHighest : "transparent"

                    Column {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10

                        Text {
                            width: parent.width
                            text: modelData.title || ""
                            color: Theme.surfaceText
                            font: Theme.labelFont
                            elide: Text.ElideRight
                        }

                        Text {
                            width: parent.width
                            text: modelData.subtitle || ""
                            color: Theme.surfaceVariantText
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }

                    }

                    HoverHandler {
                        id: fillHover

                        cursorShape: Qt.PointingHandCursor
                    }

                    TapHandler {
                        onTapped: {
                            omnibox.controller.fillAutofillSuggestion(modelData.id);
                            credentialPopup.close();
                        }
                    }

                }

            }

        }

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
                        name: suggestion.kind === "tab" ? "window" : suggestion.kind === "bookmark" ? "bookmark-circle" : suggestion.kind === "history" ? "history" : "search"
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
