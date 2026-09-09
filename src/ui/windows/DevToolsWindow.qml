import Eden.Ui
import QtQuick

Window {
    id: devToolsWindow

    required property var controller
    required property Window hostWindow

    width: 900
    height: 650
    minimumWidth: 500
    minimumHeight: 360
    transientParent: hostWindow
    visible: true
    color: Theme.surface
    title: "Developer tools · Eden"
    onClosing: (close) => {
        close.accepted = false;
        if (controller.currentEngine)
            controller.currentEngine.closeDevTools();

    }

    Loader {
        id: devToolsLoader

        anchors.fill: parent
        Component.onCompleted: setSource(Qt.resolvedUrl("../panes/DevToolsPane.qml"), {
            "engineView": controller.currentEngine
        })

        Binding {
            target: devToolsLoader.item
            property: "engineView"
            value: controller.currentEngine
            when: devToolsLoader.item !== null
        }

    }

    Loader {
        id: contextMenuLoader

        active: false
        asynchronous: false

        sourceComponent: Component {
            EdenMenu {
                parent: devToolsWindow.contentItem
                x: Math.max(8, Math.min(devToolsWindow.width - width - 8, Theme.contentBorderWidth + controller.pageContextMenuPosition.x))
                y: Math.max(8, Math.min(devToolsWindow.height - height - 8, 52 + Theme.contentBorderWidth + controller.pageContextMenuPosition.y))
                preferredWidth: 280
                maximumHeight: devToolsWindow.height - 16
                z: 20
                actions: controller.pageContextMenuActions
                onClosed: controller.dismissPageContextMenu()
                onTriggered: (command) => {
                    return controller.executePageContextMenuCommand(command);
                }
            }

        }

    }

    Connections {
        function onPageContextMenuRequested() {
            if (controller.pageContextMenuSurface !== "devtools" || !controller.currentEngine || controller.currentEngine.devToolsPlacement !== 2)
                return ;

            contextMenuLoader.active = true;
            if (!contextMenuLoader.item.tryOpen())
                controller.dismissPageContextMenu();

        }

        target: controller
    }

}
