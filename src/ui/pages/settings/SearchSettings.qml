import Eden.Ui
import QtQuick
import QtQuick.Controls

Item {
    id: page

    required property var controller
    readonly property var profileSettings: controller && controller.profileSettings ? controller.profileSettings : null

    ScrollView {
        id: scroll

        anchors.fill: parent
        contentWidth: availableWidth

        Column {
            width: Math.min(820, Math.max(480, scroll.availableWidth - 64))
            x: Math.round((scroll.availableWidth - width) / 2)
            topPadding: 32
            bottomPadding: 48
            spacing: 20

            Text {
                text: "Search"
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
                        text: "Search engine"
                        color: Theme.surfaceText
                        font: Theme.titleFont
                    }

                    Text {
                        width: parent.width
                        text: "Used by the address bar whenever the input is not a URL."
                        color: Theme.surfaceVariantText
                        font: Theme.bodyFont
                        wrapMode: Text.Wrap
                    }

                    Rectangle {
                        id: engineDropdown

                        width: parent.width
                        height: 44
                        radius: Math.min(Theme.controlRadius, height / 2)
                        color: engineMenu.opened ? Theme.surfaceContainerHighest : dropdownHover.hovered ? Theme.surfaceContainerHighest : Theme.surfaceContainerHigh
                        border.width: engineMenu.opened ? Theme.focusBorderWidth : 0
                        border.color: Theme.focusBorder

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 16
                            anchors.right: dropdownChevron.left
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: page.profileSettings ? (page.profileSettings.customSearchEngine ? "Custom" : page.profileSettings.searchEngine) : ""
                            color: Theme.surfaceText
                            font: Theme.bodyFont
                            elide: Text.ElideRight
                        }

                        Icon {
                            id: dropdownChevron

                            anchors.right: parent.right
                            anchors.rightMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            name: engineMenu.opened ? "chevron-up" : "chevron-down"
                        }

                        HoverHandler {
                            id: dropdownHover

                            cursorShape: Qt.PointingHandCursor
                        }

                        TapHandler {
                            onTapped: engineMenu.open()
                        }

                        EdenMenu {
                            id: engineMenu

                            y: parent.height + 4
                            preferredWidth: parent.width
                            maximumHeight: 380
                            actions: page.profileSettings ? page.profileSettings.searchEngineActions : []
                            onTriggered: (actionId) => {
                                if (page.profileSettings)
                                    page.profileSettings.selectSearchEngine(actionId);

                            }
                        }

                        Behavior on color {
                            ColorAnimation {
                                duration: Theme.shortDuration
                            }

                        }

                    }

                    Text {
                        visible: page.profileSettings ? page.profileSettings.customSearchEngine : false
                        text: "Search engine name"
                        color: Theme.surfaceVariantText
                        font: Theme.labelFont
                    }

                    EdenTextField {
                        visible: page.profileSettings ? page.profileSettings.customSearchEngine : false
                        width: parent.width
                        text: page.profileSettings ? page.profileSettings.searchEngine : ""
                        onEditingFinished: {
                            if (page.profileSettings)
                                page.profileSettings.searchEngine = text;

                        }
                    }

                    Text {
                        visible: page.profileSettings ? page.profileSettings.customSearchEngine : false
                        text: "Search URL, use %1 for the query"
                        color: Theme.surfaceVariantText
                        font: Theme.labelFont
                    }

                    EdenTextField {
                        visible: page.profileSettings ? page.profileSettings.customSearchEngine : false
                        width: parent.width
                        text: page.profileSettings ? page.profileSettings.searchUrl : ""
                        onEditingFinished: {
                            if (page.profileSettings)
                                page.profileSettings.searchUrl = text;

                        }
                    }

                }

            }

            Rectangle {
                width: parent.width
                height: suggestionsColumn.implicitHeight + 32
                radius: Theme.cardRadius
                color: Theme.surfaceContainer

                Column {
                    id: suggestionsColumn

                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 12

                    Text {
                        text: "Suggestions"
                        color: Theme.surfaceText
                        font: Theme.titleFont
                    }

                    EdenButton {
                        text: page.profileSettings && page.profileSettings.searchSuggestions ? "Search suggestions on" : "Search suggestions off"
                        iconName: page.profileSettings && page.profileSettings.searchSuggestions ? "check" : ""
                        onClicked: {
                            if (page.profileSettings)
                                page.profileSettings.searchSuggestions = !page.profileSettings.searchSuggestions;

                        }
                    }

                    Text {
                        width: parent.width
                        text: "Remote suggestions are available with DuckDuckGo and Google. Typed search text is sent to the selected provider after a short delay."
                        color: Theme.surfaceVariantText
                        font: Theme.bodyFont
                        wrapMode: Text.Wrap
                    }

                }

            }

        }

    }

}
