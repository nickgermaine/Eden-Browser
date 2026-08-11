import Eden.Ui
import QtQuick

Rectangle {
    id: settingsPage

    required property var controller
    property string currentSection: "appearance"
    readonly property var sections: [{
        "id": "appearance",
        "title": "Appearance",
        "icon": "tuning"
    }, {
        "id": "search",
        "title": "Search",
        "icon": "search"
    }, {
        "id": "privacy",
        "title": "Privacy",
        "icon": "lock"
    }, {
        "id": "ai",
        "title": "AI",
        "icon": "star"
    }, {
        "id": "extensions",
        "title": "Extensions",
        "icon": "code"
    }, {
        "id": "about",
        "title": "About",
        "icon": "globe"
    }]

    color: Theme.surface

    Row {
        anchors.fill: parent

        Rectangle {
            id: sidebar

            width: 232
            height: parent.height
            color: Theme.surfaceContainerLow

            Column {
                id: sidebarHeader

                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 18
                spacing: 8

                Text {
                    text: "Settings"
                    color: Theme.surfaceText
                    font.family: Themes.fontFamily
                    font.pixelSize: 22
                    font.weight: Font.DemiBold
                }

            }

            ListView {
                id: navigationList

                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: sidebarHeader.bottom
                anchors.bottom: parent.bottom
                anchors.topMargin: 14
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 3
                clip: true
                model: settingsPage.sections

                delegate: Rectangle {
                    id: navigationItem

                    required property var modelData

                    width: navigationList.width
                    height: 40
                    radius: Theme.cardRadius
                    color: settingsPage.currentSection === modelData.id ? Theme.surfaceContainerHighest : navigationHover.hovered ? Theme.surfaceContainerHigh : "transparent"

                    Row {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 10

                        Icon {
                            anchors.verticalCenter: parent.verticalCenter
                            name: navigationItem.modelData.icon
                            filled: settingsPage.currentSection === navigationItem.modelData.id
                        }

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: navigationItem.modelData.title
                            color: Theme.surfaceText
                            font: Theme.labelFont
                        }

                    }

                    HoverHandler {
                        id: navigationHover
                    }

                    TapHandler {
                        onTapped: settingsPage.currentSection = navigationItem.modelData.id
                    }

                }

            }

        }

        Item {
            width: parent.width - sidebar.width
            height: parent.height

            Loader {
                anchors.fill: parent
                sourceComponent: {
                    if (settingsPage.currentSection === "appearance")
                        return appearancePage;

                    if (settingsPage.currentSection === "search")
                        return searchPage;

                    if (settingsPage.currentSection === "privacy")
                        return privacyPage;

                    if (settingsPage.currentSection === "ai")
                        return aiPage;

                    if (settingsPage.currentSection === "extensions")
                        return extensionsPage;

                    return aboutPage;
                }
            }

        }

    }

    Component {
        id: appearancePage

        AppearanceSettings {
            controller: settingsPage.controller
        }

    }

    Component {
        id: searchPage

        SearchSettings {
        }

    }

    Component {
        id: privacyPage

        PlaceholderSettings {
            title: "Privacy"
            body: "History and bookmarks are stored locally. Private windows do not write history or sessions."
        }

    }

    Component {
        id: aiPage

        PlaceholderSettings {
            title: "AI"
            body: "AI providers arrive at some point in time that is not before now."
        }

    }

    Component {
        id: extensionsPage

        PlaceholderSettings {
            title: "Extensions"
            body: "Extension support arrives after the CEF engine migration."
        }

    }

    Component {
        id: aboutPage

        AboutSettings {
        }

    }

}
