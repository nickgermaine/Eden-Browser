import Eden.Ui
import QtQuick

Rectangle {
    id: pane

    required property var engineView
    property var attachedEngine
    readonly property bool dockedRight: engineView ? engineView.devToolsPlacement === 0 : false
    readonly property bool separateWindow: engineView ? engineView.devToolsPlacement === 2 : false

    color: Theme.surfaceContainer
    radius: Theme.contentRadius
    border.width: Theme.paneBorderWidth
    border.color: Theme.paneBorder
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
        height: 52

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
                iconName: pane.dockedRight ? "chevron-down" : "sidebar"
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

    Item {
        id: devToolsHost

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        anchors.margins: Theme.contentBorderWidth
        activeFocusOnTab: true
        focusPolicy: Qt.StrongFocus
        clip: true
    }
}
