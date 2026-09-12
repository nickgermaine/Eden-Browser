import Eden.Ui
import QtQuick
import QtQuick.Controls

Item {
    id: page

    required property var controller
    property url pageUrl
    readonly property var vault: controller.credentialVault
    readonly property string selectedSite: controller.settingsQueryValue(pageUrl, "site")
    readonly property bool showingSite: selectedSite.length > 0
    property var sites: []
    property var credentials: []

    function refresh() {
        sites = vault ? vault.credentialSites(searchField.text) : [];
        credentials = vault && selectedSite.length > 0 ? vault.credentialsForSite(selectedSite) : [];
    }

    Component.onCompleted: refresh()
    onSelectedSiteChanged: refresh()

    Connections {
        function onModelReset() {
            page.refresh();
        }

        target: page.vault
    }

    Popup {
        id: credentialEditor

        property int credentialId: 0

        function openCredential(id, site, username) {
            credentialId = id;
            siteField.text = site;
            usernameField.text = username;
            passwordField.text = id > 0 && page.vault ? page.vault.revealPassword(id) : "";
            revealPassword.checked = false;
            open();
        }

        parent: page
        x: Math.round((page.width - width) / 2)
        y: Math.max(24, Math.round((page.height - height) / 2))
        width: Math.min(480, page.width - 48)
        height: editorContent.implicitHeight + 36
        popupType: Popup.Item
        padding: 18
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onClosed: passwordField.text = ""

        background: OverlaySurface {
            surfaceRadius: Theme.windowRadius
        }

        contentItem: Column {
            id: editorContent

            spacing: 14

            Row {
                width: parent.width

                Text {
                    textFormat: Text.PlainText
                    width: parent.width - closeEditor.width
                    anchors.verticalCenter: parent.verticalCenter
                    text: credentialEditor.credentialId > 0 ? "Edit password" : "Add password"
                    color: Theme.surfaceText
                    font: Theme.titleFont
                }

                EdenButton {
                    id: closeEditor

                    iconName: "close"
                    Accessible.name: "Close"
                    onClicked: credentialEditor.close()
                }
            }

            Text {
                textFormat: Text.PlainText
                text: "Site"
                color: Theme.surfaceVariantText
                font: Theme.labelFont
            }

            EdenTextField {
                id: siteField

                width: parent.width
                placeholderText: "https://example.com"
            }

            Text {
                textFormat: Text.PlainText
                text: "Username or email"
                color: Theme.surfaceVariantText
                font: Theme.labelFont
            }

            EdenTextField {
                id: usernameField

                width: parent.width
            }

            Text {
                textFormat: Text.PlainText
                text: "Password"
                color: Theme.surfaceVariantText
                font: Theme.labelFont
            }

            EdenTextField {
                id: passwordField

                width: parent.width
                echoMode: revealPassword.checked ? TextInput.Normal : TextInput.Password
            }

            Row {
                width: parent.width
                spacing: 10

                EdenSwitch {
                    id: revealPassword

                    anchors.verticalCenter: parent.verticalCenter
                }

                Text {
                    textFormat: Text.PlainText
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Show password"
                    color: Theme.surfaceText
                    font: Theme.bodyFont
                }
            }

            Row {
                anchors.right: parent.right
                spacing: 10

                EdenButton {
                    text: "Cancel"
                    onClicked: credentialEditor.close()
                }

                EdenButton {
                    text: "Save"
                    enabled: siteField.text.trim().length > 0 && passwordField.text.length > 0
                    onClicked: {
                        if (page.vault)
                            page.vault.saveCredential(credentialEditor.credentialId, siteField.text, usernameField.text, passwordField.text);

                        credentialEditor.close();
                    }
                }
            }
        }
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
                    visible: page.showingSite
                    iconName: "arrow-left"
                    Accessible.name: "Back to saved passwords"
                    onClicked: page.controller.updateInternalPageUrl("eden://settings/autofill/passwords")
                }

                Column {
                    width: parent.width - addButton.width - (page.showingSite ? 54 : 0) - 12
                    spacing: 3

                    Text {
                        textFormat: Text.PlainText
                        width: parent.width
                        text: page.showingSite ? page.selectedSite : "Saved Passwords"
                        color: Theme.surfaceText
                        font.family: Themes.fontFamily
                        font.pixelSize: 30
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }

                    Text {
                        textFormat: Text.PlainText
                        visible: page.showingSite
                        text: page.credentials.length + (page.credentials.length === 1 ? " saved credential" : " saved credentials")
                        color: Theme.surfaceVariantText
                        font: Theme.bodyFont
                    }
                }

                EdenButton {
                    id: addButton

                    text: "Add"
                    iconName: "add"
                    onClicked: credentialEditor.openCredential(0, page.showingSite ? page.selectedSite : "", "")
                }
            }

            Text {
                textFormat: Text.PlainText
                visible: !page.vault || !page.vault.available
                width: parent.width
                text: page.vault && page.vault.error.length > 0 ? page.vault.error : "The encrypted vault is unavailable."
                color: Theme.error
                font: Theme.bodyFont
                wrapMode: Text.Wrap
            }

            EdenTextField {
                id: searchField

                visible: !page.showingSite
                width: parent.width
                placeholderText: "Search saved passwords"
                onTextChanged: page.refresh()
            }

            Text {
                textFormat: Text.PlainText
                visible: !page.showingSite && page.sites.length === 0 && page.vault && page.vault.available
                width: parent.width
                text: searchField.text.length > 0 ? "No saved passwords match your search." : "No saved passwords yet."
                color: Theme.surfaceVariantText
                font: Theme.bodyFont
            }

            Repeater {
                model: page.showingSite ? [] : page.sites

                delegate: Rectangle {
                    id: siteRow

                    required property var modelData

                    width: parent.width
                    height: 68
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
                            text: siteRow.modelData.credentialCount + (siteRow.modelData.credentialCount === 1 ? " credential" : " credentials")
                            color: Theme.surfaceVariantText
                            font: Theme.bodyFont
                        }
                    }

                    HoverHandler {
                        id: siteHover

                        cursorShape: Qt.PointingHandCursor
                    }

                    TapHandler {
                        onTapped: page.controller.updateInternalPageUrl(page.controller.passwordSiteUrl(siteRow.modelData.site))
                    }
                }
            }

            Repeater {
                model: page.showingSite ? page.credentials : []

                delegate: Rectangle {
                    id: credentialRow

                    required property var modelData
                    property string revealedPassword: ""

                    width: parent.width
                    height: credentialContent.implicitHeight + 28
                    radius: Theme.cardRadius
                    color: Theme.surfaceContainer

                    Column {
                        id: credentialContent

                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 14
                        spacing: 10

                        Text {
                            textFormat: Text.PlainText
                            width: parent.width
                            text: credentialRow.modelData.username || "No username"
                            color: Theme.surfaceText
                            font: Theme.titleFont
                            elide: Text.ElideRight
                        }

                        Row {
                            width: parent.width
                            spacing: 8

                            Text {
                                textFormat: Text.PlainText
                                width: parent.width - copyUsername.width
                                anchors.verticalCenter: parent.verticalCenter
                                text: credentialRow.modelData.username || "No username"
                                color: Theme.surfaceVariantText
                                font: Theme.bodyFont
                                elide: Text.ElideRight
                            }

                            EdenButton {
                                id: copyUsername

                                text: "Copy username"
                                enabled: credentialRow.modelData.username.length > 0
                                onClicked: page.controller.copyCredentialUsername(credentialRow.modelData.id)
                            }
                        }

                        Row {
                            width: parent.width
                            spacing: 8

                            Text {
                                textFormat: Text.PlainText
                                width: parent.width - revealButton.width - copyPassword.width - 16
                                anchors.verticalCenter: parent.verticalCenter
                                text: credentialRow.revealedPassword.length > 0 ? credentialRow.revealedPassword : "••••••••••••"
                                color: Theme.surfaceText
                                font: Theme.bodyFont
                                elide: Text.ElideRight
                            }

                            EdenButton {
                                id: revealButton

                                text: credentialRow.revealedPassword.length > 0 ? "Hide" : "Reveal"
                                onClicked: credentialRow.revealedPassword = credentialRow.revealedPassword.length > 0 ? "" : page.vault.revealPassword(credentialRow.modelData.id)
                            }

                            EdenButton {
                                id: copyPassword

                                text: "Copy password"
                                onClicked: page.controller.copyCredentialPassword(credentialRow.modelData.id)
                            }
                        }

                        Row {
                            anchors.right: parent.right
                            spacing: 8

                            EdenButton {
                                text: "Edit"
                                onClicked: credentialEditor.openCredential(credentialRow.modelData.id, credentialRow.modelData.site, credentialRow.modelData.username)
                            }

                            EdenButton {
                                text: "Delete"
                                iconName: "trash"
                                onClicked: page.vault.removeCredential(credentialRow.modelData.id)
                            }
                        }
                    }
                }
            }
        }
    }
}
