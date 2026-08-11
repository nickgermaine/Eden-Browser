import Eden.Ui
import QtQuick
import QtQuick.Controls

Popup {
    id: popover

    required property var controller
    property var engine: controller.currentEngine

    width: 320
    height: content.implicitHeight + 24
    padding: 12
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: OverlaySurface {
        surfaceRadius: Theme.cardRadius
    }

    contentItem: Column {
        id: content

        spacing: 10

        Row {
            spacing: 10

            Icon {
                name: popover.engine && popover.engine.securityState === "secure" ? "lock" : "danger"
            }

            Text {
                text: popover.engine && popover.engine.securityState === "secure" ? "Connection is secure" : "Connection details"
                color: Theme.surfaceText
                font: Theme.titleFont
            }

        }

        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: popover.engine ? popover.engine.url.toString() : ""
            color: Theme.surfaceVariantText
            font: Theme.bodyFont
        }

        Rectangle {
            width: parent.width
            height: 1
            color: Theme.outline
        }

        Text {
            text: "Site permissions"
            color: Theme.surfaceText
            font: Theme.labelFont
        }

        Text {
            text: "Permissions use browser defaults in this phase."
            color: Theme.surfaceVariantText
            font: Theme.bodyFont
        }

    }

}
