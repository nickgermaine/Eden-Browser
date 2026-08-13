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

}
