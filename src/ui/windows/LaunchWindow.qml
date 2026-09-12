import Eden.Ui
import QtQuick
import QtQuick.Effects

Window {
    id: root

    width: 720
    height: 520
    minimumWidth: 600
    minimumHeight: 440
    visible: true
    flags: Qt.Window | Qt.FramelessWindowHint
    color: "transparent"
    title: "Eden"

    RectangularShadow {
        anchors.fill: shell
        visible: Theme.windowShadowExtent > 0
        offset: Qt.vector2d(Theme.windowShadowHorizontalOffset, Theme.windowShadowVerticalOffset)
        color: Theme.windowShadowColor
        blur: Theme.windowShadowBlur
        spread: Theme.windowShadowSpread
        radius: shell.radius
    }

    Rectangle {
        id: shell

        anchors.fill: parent
        anchors.margins: Theme.windowShadowExtent
        radius: Theme.windowRadius
        color: Theme.surface
        border.width: Theme.windowBorderWidth
        border.color: Theme.windowBorder

        Column {
            anchors.centerIn: parent
            spacing: 20
            visible: Profiles.startupState !== "recovery"

            Image {
                anchors.horizontalCenter: parent.horizontalCenter
                source: "qrc:/qt/qml/Eden/Ui/resources/eden-logo.png"
                sourceSize: Qt.size(96, 96)
                width: 96
                height: 96
            }
        }

        Column {
            anchors.centerIn: parent
            width: Math.min(parent.width - 96, 480)
            spacing: 16
            visible: Profiles.startupState === "recovery"

            Icon {
                anchors.horizontalCenter: parent.horizontalCenter
                name: "danger"
                iconSize: 44
                color: Theme.error
            }

            Text {
                textFormat: Text.PlainText
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: "Eden needs your attention"
                color: Theme.surfaceText
                font: Theme.titleFont
            }

            Text {
                textFormat: Text.PlainText
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: Profiles.recoveryMessage
                color: Theme.surfaceVariantText
                font: Theme.bodyFont
                Accessible.role: Accessible.AlertMessage
                Accessible.name: Profiles.recoveryMessage
            }

            EdenButton {
                anchors.horizontalCenter: parent.horizontalCenter
                visible: Profiles.recoveryCanRetry
                text: "Try again"
                onClicked: Profiles.retryProfileOpen()
            }

            EdenButton {
                anchors.horizontalCenter: parent.horizontalCenter
                visible: Profiles.recoveryCanRetry
                text: "Choose another profile"
                onClicked: Profiles.openChooser()
            }

            EdenButton {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Quit Eden"
                onClicked: Profiles.quitApplication()
            }
        }

        DragHandler {
            target: null
            dragThreshold: 4
            onActiveChanged: {
                if (active)
                    root.startSystemMove();
            }
        }
    }
}
