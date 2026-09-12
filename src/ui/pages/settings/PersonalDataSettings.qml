import Eden.Ui
import QtQuick
import QtQuick.Controls

Item {
    id: page

    required property var controller
    readonly property var vault: controller.credentialVault
    property var profiles: []

    function refresh() {
        profiles = vault ? vault.formProfiles() : [];
    }

    Component.onCompleted: refresh()

    Connections {
        function onFormProfilesChanged() {
            page.refresh();
        }

        target: page.vault
    }

    Popup {
        id: editor

        property int profileId: 0
        property var values: ({})

        function openProfile(profile) {
            profileId = profile && profile.id ? profile.id : 0;
            values = {
                "label": profile && profile.label ? profile.label : "",
                "name": profile && profile.name ? profile.name : "",
                "email": profile && profile.email ? profile.email : "",
                "phone": profile && profile.phone ? profile.phone : "",
                "addressLine1": profile && profile.addressLine1 ? profile.addressLine1 : "",
                "addressLine2": profile && profile.addressLine2 ? profile.addressLine2 : "",
                "city": profile && profile.city ? profile.city : "",
                "region": profile && profile.region ? profile.region : "",
                "postalCode": profile && profile.postalCode ? profile.postalCode : "",
                "country": profile && profile.country ? profile.country : ""
            };
            open();
        }

        parent: page
        x: Math.round((page.width - width) / 2)
        y: Math.max(20, Math.round((page.height - height) / 2))
        width: Math.min(560, page.width - 48)
        height: Math.min(620, page.height - 40)
        popupType: Popup.Item
        padding: 18
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: OverlaySurface {
            surfaceRadius: Theme.windowRadius
        }

        contentItem: Column {
            spacing: 14

            Row {
                width: parent.width

                Text {
                    textFormat: Text.PlainText
                    width: parent.width - closeButton.width
                    anchors.verticalCenter: parent.verticalCenter
                    text: editor.profileId > 0 ? "Edit personal data" : "Add personal data"
                    color: Theme.surfaceText
                    font: Theme.titleFont
                }

                EdenButton {
                    id: closeButton

                    iconName: "close"
                    Accessible.name: "Close"
                    onClicked: editor.close()
                }
            }

            Flickable {
                width: parent.width
                height: parent.height - actionRow.height - 60
                contentHeight: fields.implicitHeight
                clip: true

                Grid {
                    id: fields

                    width: parent.width
                    columns: 2
                    columnSpacing: 10
                    rowSpacing: 10

                    Repeater {
                        model: [
                            {
                                "key": "label",
                                "hint": "Label"
                            },
                            {
                                "key": "name",
                                "hint": "Full name"
                            },
                            {
                                "key": "email",
                                "hint": "Email"
                            },
                            {
                                "key": "phone",
                                "hint": "Phone"
                            },
                            {
                                "key": "addressLine1",
                                "hint": "Address line 1"
                            },
                            {
                                "key": "addressLine2",
                                "hint": "Address line 2"
                            },
                            {
                                "key": "city",
                                "hint": "City"
                            },
                            {
                                "key": "region",
                                "hint": "State or province"
                            },
                            {
                                "key": "postalCode",
                                "hint": "Postal code"
                            },
                            {
                                "key": "country",
                                "hint": "Country"
                            }
                        ]

                        delegate: EdenTextField {
                            required property var modelData

                            width: (fields.width - 10) / 2
                            placeholderText: modelData.hint
                            text: editor.values[modelData.key] || ""
                            onTextEdited: editor.values[modelData.key] = text
                        }
                    }
                }
            }

            Row {
                id: actionRow

                anchors.right: parent.right
                spacing: 10

                EdenButton {
                    text: "Cancel"
                    onClicked: editor.close()
                }

                EdenButton {
                    text: "Save"
                    enabled: editor.values.label && editor.values.label.trim().length > 0
                    onClicked: {
                        if (page.vault)
                            page.vault.saveFormProfile(editor.profileId, editor.values);

                        editor.close();
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

                Column {
                    width: parent.width - addButton.width
                    spacing: 3

                    Text {
                        textFormat: Text.PlainText
                        text: "Personal Data"
                        color: Theme.surfaceText
                        font.family: Themes.fontFamily
                        font.pixelSize: 30
                        font.weight: Font.DemiBold
                    }

                    Text {
                        textFormat: Text.PlainText
                        text: "Addresses and contact details used to fill forms."
                        color: Theme.surfaceVariantText
                        font: Theme.bodyFont
                    }
                }

                EdenButton {
                    id: addButton

                    text: "Add"
                    iconName: "add"
                    onClicked: editor.openProfile({})
                }
            }

            Text {
                textFormat: Text.PlainText
                visible: page.profiles.length === 0
                text: "No personal data saved yet."
                color: Theme.surfaceVariantText
                font: Theme.bodyFont
            }

            Repeater {
                model: page.profiles

                delegate: Rectangle {
                    required property var modelData

                    width: parent.width
                    height: 72
                    radius: Theme.cardRadius
                    color: Theme.surfaceContainer

                    Row {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: 8

                        Column {
                            width: parent.width - editButton.width - deleteButton.width - 24
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 4

                            Text {
                                textFormat: Text.PlainText
                                width: parent.width
                                text: modelData.label
                                color: Theme.surfaceText
                                font: Theme.titleFont
                                elide: Text.ElideRight
                            }

                            Text {
                                textFormat: Text.PlainText
                                width: parent.width
                                text: modelData.name || modelData.email || modelData.addressLine1 || "Saved personal data"
                                color: Theme.surfaceVariantText
                                font: Theme.bodyFont
                                elide: Text.ElideRight
                            }
                        }

                        EdenButton {
                            id: editButton

                            text: "Edit"
                            onClicked: editor.openProfile(modelData)
                        }

                        EdenButton {
                            id: deleteButton

                            text: "Delete"
                            iconName: "trash"
                            onClicked: page.vault.removeFormProfile(modelData.id)
                        }
                    }
                }
            }
        }
    }
}
