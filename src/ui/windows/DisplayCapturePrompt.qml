import Eden.Ui
import QtQuick
import QtQuick.Controls

Popup {
    id: displayCapturePrompt

    required property var controller
    required property Item popupParent
    readonly property var request: controller.displayCaptureRequest

    parent: popupParent
    x: Math.round((popupParent.width - width) / 2)
    y: Math.round((popupParent.height - height) / 2)
    width: 460
    padding: 24
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    z: 24
    onClosed: {
        if (controller.displayCaptureRequest.id !== undefined)
            controller.resolveDisplayCaptureRequest();
    }

    background: OverlaySurface {
        surfaceRadius: Theme.cardRadius
    }

    contentItem: Column {
        spacing: 16

        Text {
            textFormat: Text.PlainText
            width: parent.width
            text: displayCapturePrompt.request.originLabel ? "Choose what to share with " + displayCapturePrompt.request.originLabel : "Choose what to share"
            color: Theme.surfaceText
            font: Theme.titleFont
            wrapMode: Text.Wrap
        }

        Text {
            textFormat: Text.PlainText
            width: parent.width
            text: "Your system will ask you to select the exact source next."
            color: Theme.surfaceVariantText
            font: Theme.bodyFont
            wrapMode: Text.Wrap
        }

        EdenButton {
            id: applicationButton

            width: parent.width
            height: 56
            iconName: "window"
            text: "Application window"
            onClicked: displayCapturePrompt.controller.resolveDisplayCaptureRequest("window")

            background: Rectangle {
                radius: Theme.controlRadius
                color: applicationButton.down || applicationButton.hovered ? Theme.surfaceContainerHighest : Theme.surfaceContainerHigh
                border.color: Theme.outline
                border.width: 1
            }
        }

        EdenButton {
            id: screenButton

            width: parent.width
            height: 56
            iconName: "monitor"
            text: "Entire screen"
            onClicked: displayCapturePrompt.controller.resolveDisplayCaptureRequest("screen")

            background: Rectangle {
                radius: Theme.controlRadius
                color: screenButton.down || screenButton.hovered ? Theme.surfaceContainerHighest : Theme.surfaceContainerHigh
                border.color: Theme.outline
                border.width: 1
            }
        }

        Row {
            anchors.right: parent.right

            EdenButton {
                text: "Cancel"
                onClicked: displayCapturePrompt.controller.resolveDisplayCaptureRequest()
            }
        }
    }
}
