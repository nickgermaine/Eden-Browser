import Eden.Ui
import QtQuick
import QtQuick.Controls

Item {
    id: page

    required property var controller
    property url pageUrl
    readonly property string path: controller.settingsPath(pageUrl)

    Loader {
        anchors.fill: parent
        sourceComponent: page.path.startsWith("/autofill/passwords") ? savedPasswordsPage : page.path.startsWith("/autofill/personal-data") ? personalDataPage : autofillHome
    }

    Component {
        id: savedPasswordsPage

        SavedPasswordsSettings {
            controller: page.controller
            pageUrl: page.pageUrl
        }
    }

    Component {
        id: personalDataPage

        PersonalDataSettings {
            controller: page.controller
        }
    }

    Component {
        id: autofillHome

        ScrollView {
            id: homeScroll

            contentWidth: availableWidth

            Column {
                width: Math.min(880, Math.max(520, homeScroll.availableWidth - 64))
                x: Math.round((homeScroll.availableWidth - width) / 2)
                topPadding: 32
                bottomPadding: 48
                spacing: 20

                Text {
                    textFormat: Text.PlainText
                    text: "Passwords & Auto-fill"
                    color: Theme.surfaceText
                    font.family: Themes.fontFamily
                    font.pixelSize: 30
                    font.weight: Font.DemiBold
                }

                Repeater {
                    model: [
                        {
                            "title": "Saved Passwords",
                            "body": "View, search, add, update, and remove saved sign-ins.",
                            "icon": "password",
                            "url": "eden://settings/autofill/passwords"
                        },
                        {
                            "title": "Personal Data",
                            "body": "Manage addresses and contact details used to fill forms.",
                            "icon": "user-circle",
                            "url": "eden://settings/autofill/personal-data"
                        }
                    ]

                    delegate: Rectangle {
                        id: destination

                        required property var modelData

                        width: parent.width
                        height: 82
                        radius: Theme.cardRadius
                        color: destinationHover.hovered ? Theme.surfaceContainerHigh : Theme.surfaceContainer

                        Row {
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 14

                            Icon {
                                anchors.verticalCenter: parent.verticalCenter
                                name: destination.modelData.icon
                            }

                            Column {
                                width: parent.width - 42
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 4

                                Text {
                                    textFormat: Text.PlainText
                                    width: parent.width
                                    text: destination.modelData.title
                                    color: Theme.surfaceText
                                    font: Theme.titleFont
                                }

                                Text {
                                    textFormat: Text.PlainText
                                    width: parent.width
                                    text: destination.modelData.body
                                    color: Theme.surfaceVariantText
                                    font: Theme.bodyFont
                                    elide: Text.ElideRight
                                }
                            }
                        }

                        HoverHandler {
                            id: destinationHover

                            cursorShape: Qt.PointingHandCursor
                        }

                        TapHandler {
                            onTapped: page.controller.updateInternalPageUrl(destination.modelData.url)
                        }
                    }
                }
            }
        }
    }
}
