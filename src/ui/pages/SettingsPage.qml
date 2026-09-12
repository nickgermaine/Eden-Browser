import Eden.Ui
import QtQuick

Rectangle {
    id: settingsPage

    required property var controller
    property url pageUrl
    property string currentSection: "appearance"
    readonly property var sections: [
        {
            "id": "appearance",
            "title": "Appearance",
            "icon": "tuning"
        },
        {
            "id": "autofill",
            "title": "Passwords & Auto-fill",
            "icon": "password"
        },
        {
            "id": "search",
            "title": "Search",
            "icon": "search"
        },
        {
            "id": "privacy",
            "title": "Privacy",
            "icon": "lock"
        },
        {
            "id": "engine",
            "title": "Engine",
            "icon": "cpu-bolt"
        },
        {
            "id": "ai",
            "title": "AI Providers",
            "icon": "soundwave"
        },
        {
            "id": "extensions",
            "title": "Extensions",
            "icon": "code"
        },
        {
            "id": "about",
            "title": "About",
            "icon": "info-circle"
        }
    ]

    function sectionFromUrl(value) {
        const match = value.toString().match(/^eden:\/\/settings(?:\/([a-z-]+))?/);
        if (!match || !match[1])
            return "appearance";

        for (const section of sections) {
            if (section.id === match[1])
                return section.id;
        }
        return "appearance";
    }

    function activateSection(sectionId) {
        currentSection = sectionId;
        const target = sectionId === "appearance" ? "eden://settings" : "eden://settings/" + sectionId;
        controller.updateInternalPageUrl(target);
    }

    color: Theme.surface
    onPageUrlChanged: currentSection = sectionFromUrl(pageUrl)

    Rectangle {
        id: sidebar

        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.margins: 10
        width: 232
        radius: Theme.contentRadius
        color: Theme.surfaceContainerLow

        Column {
            id: sidebarHeader

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 18
            spacing: 8

            Text {
                textFormat: Text.PlainText
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
                        textFormat: Text.PlainText
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
                    onTapped: settingsPage.activateSection(navigationItem.modelData.id)
                }
            }
        }
    }

    Item {
        anchors.left: sidebar.right
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.leftMargin: 10

        Loader {
            anchors.fill: parent
            sourceComponent: {
                if (settingsPage.currentSection === "appearance")
                    return appearancePage;

                if (settingsPage.currentSection === "autofill")
                    return passwordsPage;

                if (settingsPage.currentSection === "search")
                    return searchPage;

                if (settingsPage.currentSection === "engine")
                    return enginePage;

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

    Component {
        id: appearancePage

        AppearanceSettings {
            controller: settingsPage.controller
        }
    }

    Component {
        id: enginePage

        EngineSettings {
            controller: settingsPage.controller
        }
    }

    Component {
        id: searchPage

        SearchSettings {
            controller: settingsPage.controller
        }
    }

    Component {
        id: passwordsPage

        PasswordSettings {
            controller: settingsPage.controller
            pageUrl: settingsPage.pageUrl
        }
    }

    Component {
        id: privacyPage

        PrivacySettings {
            controller: settingsPage.controller
            pageUrl: settingsPage.pageUrl
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
            body: "Extension support arrives after the engine migration."
        }
    }

    Component {
        id: aboutPage

        AboutSettings {}
    }
}
