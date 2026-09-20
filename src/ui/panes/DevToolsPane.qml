import Eden.Ui
import QtQuick

Rectangle {
    id: pane

    required property var engineView
    property var attachedEngine
    readonly property bool dockedRight: engineView ? engineView.devToolsPlacement === 0 : false
    readonly property bool separateWindow: engineView ? engineView.devToolsPlacement === 2 : false
    readonly property bool selfFramed: engineView ? engineView.devToolsSelfFramed : false
    property real surfaceX: 0
    property real surfaceY: 0
    property real surfaceWidth: width
    property real surfaceHeight: height

    color: "transparent"
    Component.onCompleted: {
        attachedEngine = engineView;
        if (attachedEngine)
            attachedEngine.attachDevTools(devToolsHost);
    }
    Component.onDestruction: {
        if (attachedEngine)
            attachedEngine.detachDevTools(devToolsHost);
    }
    onEngineViewChanged: {
        if (attachedEngine === engineView)
            return;

        if (attachedEngine)
            attachedEngine.detachDevTools(devToolsHost);

        attachedEngine = engineView;
        if (attachedEngine)
            attachedEngine.attachDevTools(devToolsHost);
    }

    Item {
        id: header

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: 16
        anchors.rightMargin: 8
        height: pane.selfFramed ? 0 : 52
        visible: !pane.selfFramed

        Text {
            textFormat: Text.PlainText
            anchors.left: parent.left
            anchors.right: controls.left
            anchors.verticalCenter: parent.verticalCenter
            text: "Developer tools"
            color: Theme.surfaceText
            font: Theme.titleFont
            elide: Text.ElideRight
        }

        Row {
            id: controls

            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter

            EdenButton {
                visible: !pane.separateWindow
                iconName: "sidebar-minimalistic"
                iconRotation: pane.dockedRight ? 90 : 0
                onClicked: {
                    if (pane.engineView)
                        pane.engineView.toggleDevToolsOrientation();
                }
            }

            EdenButton {
                iconName: pane.separateWindow ? "window" : "new-window"
                onClicked: {
                    if (pane.engineView)
                        pane.engineView.toggleDevToolsSeparate();
                }
            }

            EdenButton {
                iconName: "close"
                onClicked: {
                    if (pane.engineView)
                        pane.engineView.closeDevTools();
                }
            }
        }
    }

    Rectangle {
        id: devToolsFrame

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        radius: Theme.contentRadius
        color: pane.selfFramed ? "transparent" : Theme.surface
    }

    Item {
        id: devToolsHost

        anchors.fill: devToolsFrame
        anchors.margins: Theme.contentBorderWidth
        activeFocusOnTab: true
        focusPolicy: Qt.StrongFocus
        clip: true
    }

    ChromeCornerMask {
        anchors.left: devToolsFrame.left
        anchors.top: devToolsFrame.top
        corner: "top-left"
        surfaceHeight: pane.surfaceHeight
        surfaceWidth: pane.surfaceWidth
        surfaceX: pane.surfaceX + devToolsFrame.x
        visible: !pane.selfFramed
        surfaceY: pane.surfaceY + devToolsFrame.y
        z: 3
    }

    ChromeCornerMask {
        anchors.right: devToolsFrame.right
        anchors.top: devToolsFrame.top
        corner: "top-right"
        surfaceHeight: pane.surfaceHeight
        surfaceWidth: pane.surfaceWidth
        surfaceX: pane.surfaceX + devToolsFrame.x + devToolsFrame.width - width
        visible: !pane.selfFramed
        surfaceY: pane.surfaceY + devToolsFrame.y
        z: 3
    }

    ChromeCornerMask {
        anchors.right: devToolsFrame.right
        anchors.bottom: devToolsFrame.bottom
        corner: "bottom-right"
        surfaceHeight: pane.surfaceHeight
        surfaceWidth: pane.surfaceWidth
        surfaceX: pane.surfaceX + devToolsFrame.x + devToolsFrame.width - width
        visible: !pane.selfFramed
        surfaceY: pane.surfaceY + devToolsFrame.y + devToolsFrame.height - height
        z: 3
    }

    ChromeCornerMask {
        anchors.left: devToolsFrame.left
        anchors.bottom: devToolsFrame.bottom
        corner: "bottom-left"
        surfaceHeight: pane.surfaceHeight
        surfaceWidth: pane.surfaceWidth
        surfaceX: pane.surfaceX + devToolsFrame.x
        visible: !pane.selfFramed
        surfaceY: pane.surfaceY + devToolsFrame.y + devToolsFrame.height - height
        z: 3
    }

    Rectangle {
        anchors.fill: devToolsFrame
        radius: Theme.contentRadius
        color: "transparent"
        border.width: Theme.contentBorderWidth
        border.color: Theme.contentBorder
        visible: !pane.selfFramed
        z: 4
    }
}
