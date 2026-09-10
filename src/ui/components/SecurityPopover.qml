import Eden.Ui
import QtQuick
import QtQuick.Controls

Popup {
    id: popover

    required property var controller
    property var engine: controller.currentEngine
    readonly property var store: controller.permissionStore
    property var sitePermissions: []
    property double dismissedAt: 0

    function refreshPermissions() {
        sitePermissions = store ? store.permissionsForOrigin(controller.currentUrl, false) : [];
    }

    function toggle() {
        if (opened) {
            close();
            return ;
        }
        if (Date.now() - dismissedAt < 250)
            return ;

        open();
    }

    width: 360
    height: Math.min(520, content.implicitHeight + 24)
    popupType: Popup.Item
    padding: 12
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onOpened: refreshPermissions()
    onClosed: dismissedAt = Date.now()

    Connections {
        function onCurrentUrlChanged() {
            if (popover.opened)
                popover.refreshPermissions();

        }

        target: popover.controller
    }

    Connections {
        function onPermissionsChanged(origin) {
            if (popover.opened)
                popover.refreshPermissions();

        }

        target: popover.store
    }

    Popup {
        id: certificatePopup

        parent: popover.parent
        x: popover.x
        y: popover.y
        width: 480
        height: Math.min(660, popover.parent.height - 40)
        popupType: Popup.Item
        padding: 18
        modal: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: OverlaySurface {
            surfaceRadius: Theme.windowRadius
        }

        contentItem: ScrollView {
            contentWidth: availableWidth

            Column {
                width: parent.width
                spacing: 14

                Row {
                    width: parent.width

                    Text {
                        width: parent.width - closeCertificate.width
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Certificate details"
                        color: Theme.surfaceText
                        font: Theme.titleFont
                    }

                    EdenButton {
                        id: closeCertificate

                        iconName: "close"
                        Accessible.name: "Close certificate details"
                        onClicked: certificatePopup.close()
                    }

                }

                Repeater {
                    model: [{
                        "label": "Issued to",
                        "value": popover.engine ? popover.engine.certificateDetails.subject || popover.engine.certificateDetails.commonName || "Unknown" : "Unknown"
                    }, {
                        "label": "Issued by",
                        "value": popover.engine ? popover.engine.certificateDetails.issuer || "Unknown" : "Unknown"
                    }, {
                        "label": "Valid from",
                        "value": popover.engine && popover.engine.certificateDetails.validFrom ? popover.engine.certificateDetails.validFrom.toLocaleString() : "Unknown"
                    }, {
                        "label": "Valid until",
                        "value": popover.engine && popover.engine.certificateDetails.validUntil ? popover.engine.certificateDetails.validUntil.toLocaleString() : "Unknown"
                    }, {
                        "label": "Protocol",
                        "value": popover.engine ? popover.engine.certificateDetails.protocol || "Unknown" : "Unknown"
                    }, {
                        "label": "Serial number",
                        "value": popover.engine ? popover.engine.certificateDetails.serialNumber || "Unknown" : "Unknown"
                    }]

                    delegate: Column {
                        required property var modelData

                        width: parent.width
                        spacing: 3

                        Text {
                            text: modelData.label
                            color: Theme.surfaceVariantText
                            font: Theme.labelFont
                        }

                        Text {
                            width: parent.width
                            text: modelData.value
                            color: Theme.surfaceText
                            font: Theme.bodyFont
                            wrapMode: Text.WrapAnywhere
                        }

                    }

                }

                Text {
                    visible: popover.engine && popover.engine.certificateDetails.chain && popover.engine.certificateDetails.chain.length > 0
                    text: "Certificate chain"
                    color: Theme.surfaceText
                    font: Theme.titleFont
                }

                Repeater {
                    model: popover.engine && popover.engine.certificateDetails.chain ? popover.engine.certificateDetails.chain : []

                    delegate: Rectangle {
                        required property var modelData

                        width: parent.width
                        height: chainContent.implicitHeight + 24
                        radius: Theme.controlRadius
                        color: Theme.surfaceContainerHigh

                        Column {
                            id: chainContent

                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 12
                            spacing: 4

                            Text {
                                width: parent.width
                                text: modelData.subject || "Unknown certificate"
                                color: Theme.surfaceText
                                font: Theme.labelFont
                                elide: Text.ElideRight
                            }

                            Text {
                                width: parent.width
                                text: "Issued by " + (modelData.issuer || "Unknown")
                                color: Theme.surfaceVariantText
                                font: Theme.bodyFont
                                elide: Text.ElideRight
                            }

                            Text {
                                width: parent.width
                                text: "SHA-256 " + (modelData.sha256 || "Unknown")
                                color: Theme.surfaceVariantText
                                font: Theme.bodyFont
                                wrapMode: Text.WrapAnywhere
                            }

                        }

                    }

                }

            }

        }

    }

    background: OverlaySurface {
        surfaceRadius: Theme.cardRadius
    }

    contentItem: Flickable {
        contentHeight: content.implicitHeight
        clip: true

        Column {
            id: content

            width: parent.width
            spacing: 10

            Row {
                width: parent.width
                spacing: 10

                Icon {
                    anchors.verticalCenter: parent.verticalCenter
                    name: popover.engine && popover.engine.securityState === "secure" ? "lock" : popover.engine && popover.engine.certificateDetails.subject !== undefined ? "danger" : "info-circle"
                }

                Column {
                    width: parent.width - 34
                    spacing: 2

                    Text {
                        width: parent.width
                        text: popover.engine && popover.engine.securityState === "secure" ? "Connection is secure" : popover.engine && popover.engine.securityState === "insecure" ? "Connection is not secure" : "Page details"
                        color: Theme.surfaceText
                        font: Theme.titleFont
                    }

                    Text {
                        width: parent.width
                        text: popover.controller.currentUrl.host || popover.controller.currentUrl.toString()
                        color: Theme.surfaceVariantText
                        font: Theme.bodyFont
                        elide: Text.ElideRight
                    }

                }

            }

            EdenButton {
                width: parent.width
                visible: popover.engine && popover.engine.certificateDetails.subject !== undefined
                text: "Certificate details"
                iconName: "lock"
                onClicked: certificatePopup.open()
            }

            Rectangle {
                width: parent.width
                height: 1
                color: Theme.outline
            }

            Row {
                width: parent.width

                Text {
                    width: parent.width - siteSettingsButton.width
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Site permissions"
                    color: Theme.surfaceText
                    font: Theme.labelFont
                }

                EdenButton {
                    id: siteSettingsButton

                    text: "All settings"
                    onClicked: {
                        popover.controller.newTab(popover.controller.siteSettingsUrl(popover.controller.currentUrl));
                        popover.close();
                    }
                }

            }

            Text {
                visible: popover.sitePermissions.length === 0
                width: parent.width
                text: "This site has not requested any permissions."
                color: Theme.surfaceVariantText
                font: Theme.bodyFont
                wrapMode: Text.Wrap
            }

            Repeater {
                model: popover.sitePermissions

                delegate: Rectangle {
                    required property var modelData

                    width: content.width
                    height: 46
                    radius: Theme.controlRadius
                    color: Theme.surfaceContainerHigh

                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 12
                        anchors.right: permissionSwitch.left
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.title
                        color: Theme.surfaceText
                        font: Theme.bodyFont
                        elide: Text.ElideRight
                    }

                    EdenSwitch {
                        id: permissionSwitch

                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        checked: modelData.allowed
                        onToggled: {
                            if (popover.store)
                                popover.store.setPermission(popover.controller.currentUrl, modelData.id, checked);

                        }
                    }

                }

            }

        }

    }

}
