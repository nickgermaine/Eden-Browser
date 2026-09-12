import Eden.Ui
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs

Item {
    id: page

    required property var controller
    readonly property var profileSettings: controller && controller.profileSettings ? controller.profileSettings : null

    FileDialog {
        id: themeFileDialog

        title: "Install an Eden theme"
        nameFilters: ["Eden themes (*.json)"]
        fileMode: FileDialog.OpenFile
        onAccepted: Themes.installTheme(selectedFile)
    }

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
                textFormat: Text.PlainText
                text: "Appearance"
                color: Theme.surfaceText
                font.family: Themes.fontFamily
                font.pixelSize: 30
                font.weight: Font.DemiBold
            }

            Rectangle {
                width: parent.width
                height: startupColumn.implicitHeight + 32
                radius: Theme.cardRadius
                color: Theme.surfaceContainer

                Column {
                    id: startupColumn

                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 12

                    Text {
                        textFormat: Text.PlainText
                        text: "Startup and new tabs"
                        color: Theme.surfaceText
                        font: Theme.titleFont
                    }

                    Text {
                        textFormat: Text.PlainText
                        width: parent.width
                        text: "Your home page opens when there are no tabs to restore."
                        color: Theme.surfaceVariantText
                        font: Theme.bodyFont
                        wrapMode: Text.Wrap
                    }

                    EdenTextField {
                        width: parent.width
                        text: page.profileSettings ? page.profileSettings.homePageUrl : ""
                        placeholderText: "Home page URL"
                        onTextEdited: {
                            if (page.profileSettings)
                                page.profileSettings.homePageUrl = text;
                        }
                    }

                    Text {
                        textFormat: Text.PlainText
                        text: "New tabs open"
                        color: Theme.surfaceText
                        font: Theme.labelFont
                    }

                    Flow {
                        width: parent.width
                        spacing: 8

                        EdenButton {
                            text: "New tab page"
                            iconName: page.profileSettings && page.profileSettings.newTabBehavior === "new-tab-page" ? "check" : ""
                            onClicked: {
                                if (page.profileSettings)
                                    page.profileSettings.newTabBehavior = "new-tab-page";
                            }
                        }

                        EdenButton {
                            text: "Home page"
                            iconName: page.profileSettings && page.profileSettings.newTabBehavior === "home-page" ? "check" : ""
                            onClicked: {
                                if (page.profileSettings)
                                    page.profileSettings.newTabBehavior = "home-page";
                            }
                        }

                        EdenButton {
                            text: "Custom URL"
                            iconName: page.profileSettings && page.profileSettings.newTabBehavior === "custom-url" ? "check" : ""
                            onClicked: {
                                if (page.profileSettings)
                                    page.profileSettings.newTabBehavior = "custom-url";
                            }
                        }
                    }

                    EdenTextField {
                        width: parent.width
                        visible: page.profileSettings && page.profileSettings.newTabBehavior === "custom-url"
                        text: page.profileSettings ? page.profileSettings.newTabUrl : ""
                        placeholderText: "New tab URL"
                        onTextEdited: {
                            if (page.profileSettings)
                                page.profileSettings.newTabUrl = text;
                        }
                    }

                    Text {
                        textFormat: Text.PlainText
                        width: parent.width
                        visible: page.profileSettings && page.profileSettings.newTabBehavior === "custom-url"
                        text: "Changes save automatically for this profile."
                        color: Theme.surfaceVariantText
                        font: Theme.bodyFont
                        wrapMode: Text.Wrap
                    }
                }
            }

            Rectangle {
                width: parent.width
                height: schemeColumn.implicitHeight + 32
                radius: Theme.cardRadius
                color: Theme.surfaceContainer

                Column {
                    id: schemeColumn

                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 12

                    Text {
                        textFormat: Text.PlainText
                        text: "Color scheme"
                        color: Theme.surfaceText
                        font: Theme.titleFont
                    }

                    Row {
                        spacing: 8

                        Repeater {
                            model: ["system", "light", "dark"]

                            delegate: EdenButton {
                                required property string modelData

                                text: modelData.charAt(0).toUpperCase() + modelData.slice(1)
                                filledIcon: Settings.theme === modelData
                                iconName: Settings.theme === modelData ? "check" : ""
                                onClicked: Settings.theme = modelData
                            }
                        }
                    }
                }
            }

            Rectangle {
                width: parent.width
                height: layoutColumn.implicitHeight + 32
                radius: Theme.cardRadius
                color: Theme.surfaceContainer

                Column {
                    id: layoutColumn

                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 12

                    Text {
                        textFormat: Text.PlainText
                        text: "Tab layout"
                        color: Theme.surfaceText
                        font: Theme.titleFont
                    }

                    Row {
                        spacing: 8

                        EdenButton {
                            text: "Horizontal"
                            iconName: page.profileSettings && page.profileSettings.tabLayout === "horizontal" ? "check" : "window"
                            onClicked: {
                                if (page.profileSettings)
                                    page.profileSettings.tabLayout = "horizontal";
                            }
                        }

                        EdenButton {
                            text: "Sidebar"
                            iconName: page.profileSettings && page.profileSettings.tabLayout === "sidebar" ? "check" : "sidebar"
                            onClicked: {
                                if (page.profileSettings)
                                    page.profileSettings.tabLayout = "sidebar";
                            }
                        }
                    }
                }
            }

            Rectangle {
                width: parent.width
                height: themesColumn.implicitHeight + 32
                radius: Theme.cardRadius
                color: Theme.surfaceContainer

                Column {
                    id: themesColumn

                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 12

                    Text {
                        textFormat: Text.PlainText
                        text: "Theme"
                        color: Theme.surfaceText
                        font: Theme.titleFont
                    }

                    Text {
                        textFormat: Text.PlainText
                        width: parent.width
                        text: "Every preview is drawn from the theme file itself, in the current color scheme."
                        color: Theme.surfaceVariantText
                        font: Theme.bodyFont
                        wrapMode: Text.Wrap
                    }

                    Grid {
                        id: themeGrid

                        readonly property int cardColumns: themesColumn.width >= 640 ? 3 : 2

                        width: parent.width
                        columns: cardColumns
                        spacing: 12

                        Repeater {
                            model: Themes

                            delegate: ThemePreviewCard {
                                width: (themeGrid.width - (themeGrid.cardColumns - 1) * themeGrid.spacing) / themeGrid.cardColumns
                                onActivated: Themes.activateTheme(themeId)
                            }
                        }
                    }

                    Row {
                        spacing: 8

                        EdenButton {
                            text: "Edit active theme"
                            onClicked: page.controller.openThemeEditorTab()
                        }

                        EdenButton {
                            text: "Install theme"
                            onClicked: themeFileDialog.open()
                        }

                        EdenButton {
                            text: "Reload themes"
                            onClicked: Themes.refresh()
                        }
                    }

                    Text {
                        textFormat: Text.PlainText
                        width: parent.width
                        text: "Theme folder: " + Themes.themeDirectory
                        color: Theme.surfaceVariantText
                        font: Theme.bodyFont
                        wrapMode: Text.WrapAnywhere
                    }

                    Text {
                        textFormat: Text.PlainText
                        width: parent.width
                        visible: Themes.lastError.length > 0
                        text: Themes.lastError
                        color: Theme.error
                        font: Theme.bodyFont
                        wrapMode: Text.Wrap
                    }
                }
            }
        }
    }
}
