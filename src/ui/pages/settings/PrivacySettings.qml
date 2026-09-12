import Eden.Ui
import QtQuick
import QtQuick.Controls

Item {
    id: page

    required property var controller
    property url pageUrl
    readonly property var store: controller.permissionStore
    readonly property string path: controller.settingsPath(pageUrl)
    readonly property string detailOrigin: controller.settingsQueryValue(pageUrl, "origin")
    readonly property bool showingDetail: detailOrigin.length > 0
    readonly property bool showingSites: path.startsWith("/privacy/site-settings")
    property var sites: []
    property var permissions: []

    function refresh() {
        sites = store ? store.sites(searchField.text) : [];
        permissions = store && showingDetail ? store.permissionsForOrigin(detailOrigin, true) : [];
    }

    Component.onCompleted: refresh()
    onDetailOriginChanged: refresh()

    Connections {
        function onPermissionsChanged(origin) {
            page.refresh();
        }

        target: page.store
    }

    ScrollView {
        id: scroll

        anchors.fill: parent
        contentWidth: availableWidth

        Column {
            width: Math.min(880, Math.max(520, scroll.availableWidth - 64))
            x: Math.round((scroll.availableWidth - width) / 2)
            topPadding: 32
            bottomPadding: 48
            spacing: 18

            Row {
                width: parent.width
                spacing: 12

                EdenButton {
                    visible: page.showingSites
                    iconName: "arrow-left"
                    Accessible.name: "Back"
                    onClicked: page.controller.updateInternalPageUrl(page.showingDetail ? "eden://settings/privacy/site-settings" : "eden://settings/privacy")
                }

                Column {
                    width: parent.width - (page.showingSites ? 54 : 0)
                    spacing: 3

                    Text {
                        textFormat: Text.PlainText
                        width: parent.width
                        text: page.showingDetail ? page.detailOrigin : page.showingSites ? "Site settings" : "Privacy"
                        color: Theme.surfaceText
                        font.family: Themes.fontFamily
                        font.pixelSize: 30
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }

                    Text {
                        textFormat: Text.PlainText
                        width: parent.width
                        text: page.showingDetail ? "Permissions saved for this website." : page.showingSites ? "Review permissions saved by website." : "Control website access and local privacy."
                        color: Theme.surfaceVariantText
                        font: Theme.bodyFont
                    }
                }
            }

            Rectangle {
                id: siteSettingsDestination

                visible: !page.showingSites
                width: parent.width
                height: 82
                radius: Theme.cardRadius
                color: siteSettingsHover.hovered ? Theme.surfaceContainerHigh : Theme.surfaceContainer

                Row {
                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 14

                    Icon {
                        anchors.verticalCenter: parent.verticalCenter
                        name: "lock"
                    }

                    Column {
                        width: parent.width - 42
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 4

                        Text {
                            textFormat: Text.PlainText
                            text: "Site settings"
                            color: Theme.surfaceText
                            font: Theme.titleFont
                        }

                        Text {
                            textFormat: Text.PlainText
                            text: "View and update permissions for every website."
                            color: Theme.surfaceVariantText
                            font: Theme.bodyFont
                        }
                    }
                }

                HoverHandler {
                    id: siteSettingsHover

                    cursorShape: Qt.PointingHandCursor
                }

                TapHandler {
                    onTapped: page.controller.updateInternalPageUrl("eden://settings/privacy/site-settings")
                }
            }

            EdenTextField {
                id: searchField

                visible: page.showingSites && !page.showingDetail
                width: parent.width
                placeholderText: "Search websites"
                onTextChanged: page.refresh()
            }

            Text {
                textFormat: Text.PlainText
                visible: page.showingSites && !page.showingDetail && page.sites.length === 0
                text: searchField.text.length > 0 ? "No websites match your search." : "No websites have requested permissions yet."
                color: Theme.surfaceVariantText
                font: Theme.bodyFont
            }

            Repeater {
                model: page.showingSites && !page.showingDetail ? page.sites : []

                delegate: Rectangle {
                    id: siteRow

                    required property var modelData

                    width: parent.width
                    height: 72
                    radius: Theme.cardRadius
                    color: siteHover.hovered ? Theme.surfaceContainerHigh : Theme.surfaceContainer

                    Column {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: 16
                        anchors.rightMargin: 16
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 4

                        Text {
                            textFormat: Text.PlainText
                            width: parent.width
                            text: siteRow.modelData.host
                            color: Theme.surfaceText
                            font: Theme.titleFont
                            elide: Text.ElideRight
                        }

                        Text {
                            textFormat: Text.PlainText
                            width: parent.width
                            text: siteRow.modelData.allowedCount + " allowed, " + siteRow.modelData.blockedCount + " blocked"
                            color: Theme.surfaceVariantText
                            font: Theme.bodyFont
                        }
                    }

                    HoverHandler {
                        id: siteHover

                        cursorShape: Qt.PointingHandCursor
                    }

                    TapHandler {
                        onTapped: page.controller.updateInternalPageUrl(page.controller.siteSettingsUrl(siteRow.modelData.origin))
                    }
                }
            }

            Repeater {
                model: page.showingDetail ? page.permissions : []

                delegate: Rectangle {
                    required property var modelData

                    width: parent.width
                    height: 60
                    radius: Theme.cardRadius
                    color: Theme.surfaceContainer

                    Column {
                        anchors.left: parent.left
                        anchors.leftMargin: 16
                        anchors.right: permissionSwitch.left
                        anchors.rightMargin: 16
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 3

                        Text {
                            textFormat: Text.PlainText
                            width: parent.width
                            text: modelData.title
                            color: Theme.surfaceText
                            font: Theme.titleFont
                            elide: Text.ElideRight
                        }

                        Text {
                            textFormat: Text.PlainText
                            text: modelData.state === "allowed" ? "Allowed" : modelData.state === "blocked" ? "Blocked" : "Ask when requested"
                            color: Theme.surfaceVariantText
                            font: Theme.bodyFont
                        }
                    }

                    EdenSwitch {
                        id: permissionSwitch

                        anchors.right: parent.right
                        anchors.rightMargin: 14
                        anchors.verticalCenter: parent.verticalCenter
                        checked: modelData.allowed
                        onToggled: {
                            if (page.store)
                                page.store.setPermission(page.detailOrigin, modelData.id, checked);
                        }
                    }
                }
            }
        }
    }
}
