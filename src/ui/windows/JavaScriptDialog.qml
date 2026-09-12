import Eden.Ui
import QtQuick
import QtQuick.Controls

Popup {
    id: javaScriptDialog

    required property var controller
    required property Item popupParent
    readonly property var dialog: controller.javaScriptDialog

    parent: popupParent
    x: Math.round((popupParent.width - width) / 2)
    y: Math.round((popupParent.height - height) / 2)
    width: 440
    padding: 24
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    z: 24
    onOpened: promptInput.text = dialog.defaultText || ""
    onClosed: {
        if (controller.javaScriptDialog.id !== undefined)
            controller.resolveJavaScriptDialog(false, "");
    }

    background: OverlaySurface {
        surfaceRadius: Theme.cardRadius
    }

    contentItem: Column {
        spacing: 16

        Text {
            textFormat: Text.PlainText
            width: parent.width
            text: javaScriptDialog.dialog.kind === "beforeUnload" ? "Leave this page?" : javaScriptDialog.dialog.originLabel || "Page message"
            color: Theme.surfaceText
            font: Theme.titleFont
            wrapMode: Text.Wrap
        }

        Text {
            textFormat: Text.PlainText
            width: parent.width
            text: javaScriptDialog.dialog.message || ""
            color: Theme.surfaceVariantText
            font: Theme.bodyFont
            wrapMode: Text.Wrap
        }

        EdenTextField {
            id: promptInput

            width: parent.width
            visible: javaScriptDialog.dialog.kind === "prompt"
        }

        Row {
            anchors.right: parent.right
            spacing: 8

            EdenButton {
                text: javaScriptDialog.dialog.kind === "beforeUnload" ? "Stay" : "Cancel"
                visible: javaScriptDialog.dialog.kind !== "alert"
                onClicked: javaScriptDialog.controller.resolveJavaScriptDialog(false, "")
            }

            EdenButton {
                text: javaScriptDialog.dialog.kind === "beforeUnload" ? "Leave" : "OK"
                onClicked: javaScriptDialog.controller.resolveJavaScriptDialog(true, promptInput.text)
            }
        }
    }
}
