import Eden.Ui
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs

Item {
    id: page

    required property var controller

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
                text: "Appearance"
                color: Theme.surfaceText
                font.family: Themes.fontFamily
                font.pixelSize: 30
                font.weight: Font.DemiBold
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
                        text: "Tab layout"
                        color: Theme.surfaceText
                        font: Theme.titleFont
                    }

                    Row {
                        spacing: 8

                        EdenButton {
                            text: "Horizontal"
                            iconName: Settings.tabLayout === "horizontal" ? "check" : "window"
                            onClicked: Settings.tabLayout = "horizontal"
                        }

                        EdenButton {
                            text: "Sidebar"
                            iconName: Settings.tabLayout === "sidebar" ? "check" : "sidebar"
                            onClicked: Settings.tabLayout = "sidebar"
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
                        text: "Theme"
                        color: Theme.surfaceText
                        font: Theme.titleFont
                    }

                    Text {
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
                                required themeId
                                required themeName
                                required themeAuthor
                                required active

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
                        width: parent.width
                        text: "Theme folder: " + Themes.themeDirectory
                        color: Theme.surfaceVariantText
                        font: Theme.bodyFont
                        wrapMode: Text.WrapAnywhere
                    }

                    Text {
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
