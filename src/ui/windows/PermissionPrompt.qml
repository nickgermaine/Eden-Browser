import Eden.Ui
import QtQuick
import QtQuick.Controls

Popup {
    id: permissionPrompt

    required property var controller
    required property Item popupParent
    required property real anchorX
    required property real anchorY
    readonly property var request: controller.permissionRequest

    parent: popupParent
    x: Math.max(12, Math.min(popupParent.width - width - 12, anchorX))
    y: anchorY
    width: 400
    padding: 20
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    z: 24
    onClosed: {
        if (controller.permissionRequest.id !== undefined)
            controller.dismissPermissionRequest();
    }

    background: OverlaySurface {
        surfaceRadius: Theme.cardRadius
    }

    contentItem: Column {
        spacing: 14

        Text {
            textFormat: Text.PlainText
            width: parent.width
            text: permissionPrompt.request.originLabel ? permissionPrompt.request.originLabel + " wants permission" : "Permission request"
            color: Theme.surfaceText
            font: Theme.titleFont
            wrapMode: Text.Wrap
        }

        Text {
            textFormat: Text.PlainText
            width: parent.width
            text: permissionPrompt.request.permissions ? "Allow access to " + permissionPrompt.request.permissions.join(", ") + "?" : ""
            color: Theme.surfaceVariantText
            font: Theme.bodyFont
            wrapMode: Text.Wrap
        }

        Row {
            anchors.right: parent.right
            spacing: 8

            EdenButton {
                text: "Block"
                onClicked: permissionPrompt.controller.resolvePermissionRequest(false)
            }

            EdenButton {
                text: "Allow"
                onClicked: permissionPrompt.controller.resolvePermissionRequest(true)
            }
        }
    }
}
