import Eden.Ui
import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Shapes

Window {
    id: root

    property alias windowController: controller
    readonly property bool edgeToEdge: controller.contentFullscreen || root.visibility === Window.Maximized || root.visibility === Window.FullScreen
    readonly property string tabLayout: controller.profileSettings ? controller.profileSettings.tabLayout : "horizontal"

    width: 1360
    height: 860
    minimumWidth: 900
    minimumHeight: 600
    visible: true
    flags: Qt.Window | Qt.FramelessWindowHint
    color: "transparent"
    title: controller.mode === "private" ? "Private Browsing · Eden" : controller.currentEngine && controller.currentEngine.title.length > 0 ? controller.currentEngine.title + " · Eden" : "Eden"
    Component.onCompleted: {
        controller.shortcuts.attach(root);
    }
    onActiveChanged: {
        if (!active) {
            if (mainMenuLoader.item)
                mainMenuLoader.item.close();

            if (securityPopoverLoader.item)
                securityPopoverLoader.item.close();

            if (backHistoryMenuLoader.item)
                backHistoryMenuLoader.item.close();

            if (forwardHistoryMenuLoader.item)
                forwardHistoryMenuLoader.item.close();

            if (profileMenuLoader.item)
                profileMenuLoader.item.close();

        }
    }
    onClosing: controller.prepareToClose()

    WindowController {
        id: controller

        objectName: "windowController"
    }

    WindowFrame {
        id: frame

        window: root
    }

    RectangularShadow {
        anchors.fill: shell
        visible: !root.edgeToEdge && Theme.windowShadowExtent > 0
        offset: Qt.vector2d(0, Theme.windowShadowAmbientVerticalOffset)
        color: Theme.windowShadowAmbientColor
        blur: Theme.windowShadowAmbientBlur
        spread: 0
        radius: shell.radius
    }

    RectangularShadow {
        anchors.fill: shell
        visible: !root.edgeToEdge && Theme.windowShadowExtent > 0
        offset: Qt.vector2d(Theme.windowShadowHorizontalOffset, Theme.windowShadowVerticalOffset)
        color: Theme.windowShadowColor
        blur: Theme.windowShadowBlur
        spread: Theme.windowShadowSpread
        radius: shell.radius
    }

    Rectangle {
        id: shell

        anchors.fill: parent
        anchors.margins: root.edgeToEdge ? 0 : Theme.windowShadowExtent
        radius: root.edgeToEdge ? 0 : Theme.windowRadius
        color: controller.mode === "private" ? Theme.privateBackground : Theme.surface
        border.width: root.edgeToEdge ? 0 : Theme.windowBorderWidth
        border.color: Theme.windowBorder
        clip: true

        ChromeBackground {
            anchors.fill: parent
            radius: shell.radius
        }

        Rectangle {
            id: topChrome

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: controller.contentFullscreen ? 0 : root.tabLayout === "horizontal" ? 96 : 56
            visible: !controller.contentFullscreen
            radius: shell.radius
            color: "transparent"

            Shape {
                id: tabBarSurface

                readonly property real cornerRadius: Math.min(topChrome.radius, width / 2, height)

                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: 48
                visible: root.tabLayout === "horizontal"

                ShapePath {
                    fillColor: Theme.tabBarBackground
                    strokeColor: "transparent"
                    startX: 0
                    startY: tabBarSurface.height

                    PathLine {
                        x: 0
                        y: tabBarSurface.cornerRadius
                    }

                    PathCubic {
                        x: tabBarSurface.cornerRadius
                        y: 0
                        control1X: 0
                        control1Y: tabBarSurface.cornerRadius * 0.447715
                        control2X: tabBarSurface.cornerRadius * 0.447715
                        control2Y: 0
                    }

                    PathLine {
                        x: tabBarSurface.width - tabBarSurface.cornerRadius
                        y: 0
                    }

                    PathCubic {
                        x: tabBarSurface.width
                        y: tabBarSurface.cornerRadius
                        control1X: tabBarSurface.width - tabBarSurface.cornerRadius * 0.447715
                        control1Y: 0
                        control2X: tabBarSurface.width
                        control2Y: tabBarSurface.cornerRadius * 0.447715
                    }

                    PathLine {
                        x: tabBarSurface.width
                        y: tabBarSurface.height
                    }

                    PathLine {
                        x: 0
                        y: tabBarSurface.height
                    }

                }

            }

            Item {
                id: moveArea

                anchors.fill: parent
                anchors.topMargin: root.tabLayout === "horizontal" ? tabStrip.height : 0
                z: 0

                DragHandler {
                    target: null
                    dragThreshold: 4
                    onActiveChanged: {
                        if (active)
                            frame.startSystemMove();

                    }
                }

                TapHandler {
                    onDoubleTapped: frame.toggleMaximized()
                }

            }

            TabStrip {
                id: tabStrip

                anchors.left: parent.left
                anchors.right: windowButtons.left
                anchors.top: parent.top
                height: 48
                controller: root.windowController
                visible: root.tabLayout === "horizontal"
                opacity: visible ? 1 : 0
                z: 1
                onSystemMoveRequested: frame.startSystemMove()
                onToggleMaximizedRequested: frame.toggleMaximized()

                Behavior on opacity {
                    NumberAnimation {
                        duration: Theme.mediumDuration
                        easing.type: Easing.OutCubic
                    }

                }

            }

            Item {
                id: toolbar

                anchors.left: parent.left
                anchors.right: root.tabLayout === "horizontal" ? parent.right : windowButtons.left
                anchors.bottom: parent.bottom
                height: 44
                z: 1

                Row {
                    id: navigationButtons

                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2

                    EdenButton {
                        id: backButton

                        iconName: "arrow-left"
                        buttonRadius: 20
                        enabled: controller.currentEngine && controller.currentEngine.canGoBack
                        onClicked: controller.back()

                        TapHandler {
                            acceptedButtons: Qt.RightButton
                            onTapped: {
                                backHistoryMenuLoader.active = true;
                                backHistoryMenuLoader.item.actions = controller.navigationHistory(-1);
                                backHistoryMenuLoader.item.open();
                            }
                        }

                    }

                    EdenButton {
                        id: forwardButton

                        iconName: "arrow-right"
                        buttonRadius: 20
                        enabled: controller.currentEngine && controller.currentEngine.canGoForward
                        onClicked: controller.forward()

                        TapHandler {
                            acceptedButtons: Qt.RightButton
                            onTapped: {
                                forwardHistoryMenuLoader.active = true;
                                forwardHistoryMenuLoader.item.actions = controller.navigationHistory(1);
                                forwardHistoryMenuLoader.item.open();
                            }
                        }

                    }

                    EdenButton {
                        buttonRadius: 20
                        iconName: controller.currentEngine && controller.currentEngine.loading ? "close" : "refresh"
                        onClicked: controller.currentEngine && controller.currentEngine.loading ? controller.stop() : controller.reload()
                    }

                }

                Omnibox {
                    id: omnibox

                    anchors.left: navigationButtons.right
                    anchors.right: profileButton.left
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    height: 40
                    controller: root.windowController
                    onSecurityRequested: {
                        securityPopoverLoader.active = true;
                        securityPopoverLoader.item.toggle();
                    }
                }

                Item {
                    id: profileButton

                    anchors.right: menuButton.left
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    width: 32
                    height: 32
                    Accessible.role: Accessible.Button
                    Accessible.name: "Profile: " + controller.profileDisplayName

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -2
                        radius: 20
                        color: profileButtonHover.hovered ? Theme.surfaceContainerHigh : "transparent"
                        border.width: controller.mode === "private" ? 2 : 0
                        border.color: Theme.privatePrimary
                    }

                    ProfileAvatar {
                        anchors.fill: parent
                        avatarUrl: controller.profileAvatarUrl
                        displayName: controller.profileDisplayName
                        avatarSize: 22
                    }

                    HoverHandler {
                        id: profileButtonHover
                    }

                    TapHandler {
                        onTapped: {
                            profileMenuLoader.active = true;
                            profileMenuLoader.item.open();
                        }
                    }

                }

                EdenButton {
                    id: menuButton

                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    buttonRadius: 20
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: "menu-dots"
                    onClicked: {
                        mainMenuLoader.active = true;
                        mainMenuLoader.item.toggle();
                    }
                }

            }

            Row {
                id: windowButtons

                anchors.right: parent.right
                anchors.top: root.tabLayout === "horizontal" ? parent.top : undefined
                anchors.bottom: root.tabLayout === "horizontal" ? undefined : parent.bottom
                height: 48
                z: 2

                EdenButton {
                    anchors.verticalCenter: parent.verticalCenter
                    buttonRadius: 20
                    iconFamily: "material"
                    iconName: "minimize"
                    onClicked: frame.minimize()
                }

                EdenButton {
                    anchors.verticalCenter: parent.verticalCenter
                    iconFamily: "material"
                    buttonRadius: 20
                    iconName: root.visibility === Window.Maximized ? "restore" : "maximize"
                    onClicked: frame.toggleMaximized()
                }

                EdenButton {
                    anchors.verticalCenter: parent.verticalCenter
                    iconFamily: "material"
                    buttonRadius: 20
                    iconName: "close"
                    onClicked: frame.close()
                }

            }

        }

        Item {
            id: workspace

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: topChrome.bottom
            anchors.bottom: parent.bottom
            anchors.margins: controller.contentFullscreen ? 0 : Theme.workspaceInset

            Loader {
                id: sidebarLoader

                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: active && item ? item.implicitWidth : 0
                active: !controller.contentFullscreen && root.tabLayout === "sidebar"
                opacity: root.tabLayout === "sidebar" ? 1 : 0
                z: 2
                Component.onCompleted: setSource(Qt.resolvedUrl("../components/TabSidebar.qml"), {
                    "controller": root.windowController
                })

                transform: Scale {
                    id: sidebarScale

                    origin.x: 0
                    origin.y: sidebarLoader.height / 2
                    xScale: root.tabLayout === "sidebar" ? 1 : 0.96

                    Behavior on xScale {
                        NumberAnimation {
                            duration: Theme.mediumDuration
                            easing.type: Easing.OutCubic
                        }

                    }

                }

                Behavior on opacity {
                    NumberAnimation {
                        duration: Theme.mediumDuration
                        easing.type: Easing.OutCubic
                    }

                }

            }

            Item {
                id: viewportFrame

                anchors.left: root.tabLayout === "sidebar" ? sidebarLoader.right : parent.left
                anchors.leftMargin: root.tabLayout === "sidebar" ? Theme.workspaceGap : 0
                anchors.right: paneLoader.active ? paneLoader.left : parent.right
                anchors.rightMargin: paneLoader.active ? Theme.workspaceGap : 0
                anchors.top: parent.top
                anchors.bottom: parent.bottom

                Rectangle {
                    anchors.fill: parent
                    radius: Theme.contentRadius
                    color: Theme.surface
                    border.width: Theme.contentBorderWidth
                    border.color: Theme.contentBorder
                }

                Item {
                    id: viewport

                    readonly property bool devToolsDockedRight: controller.currentEngine && controller.currentEngine.devToolsOpen && controller.currentEngine.devToolsPlacement === 0
                    readonly property bool devToolsDockedBottom: controller.currentEngine && controller.currentEngine.devToolsOpen && controller.currentEngine.devToolsPlacement === 1

                    anchors.left: parent.left
                    anchors.right: devToolsDockedRight ? devToolsDockFrame.left : parent.right
                    anchors.rightMargin: devToolsDockedRight ? Theme.workspaceGap : 0
                    anchors.top: parent.top
                    anchors.bottom: devToolsDockedBottom ? devToolsDockFrame.top : parent.bottom
                    anchors.bottomMargin: devToolsDockedBottom ? Theme.workspaceGap : 0
                    clip: true

                    Repeater {
                        model: controller.tabs

                        delegate: Item {
                            id: tabViewportDelegate

                            required property int index
                            required property var engineView
                            required property string internalPage
                            required property url url

                            objectName: "tabViewport"
                            anchors.fill: parent
                            visible: controller.activeIndex === index
                            Component.onCompleted: {
                                if (engineView)
                                    engineView.attach(engineHost);

                            }
                            onEngineViewChanged: {
                                if (engineView)
                                    engineView.attach(engineHost);

                            }

                            Item {
                                id: engineHost

                                anchors.fill: parent
                                visible: internalPage.length === 0
                                activeFocusOnTab: true
                                focusPolicy: Qt.StrongFocus
                            }

                            Loader {
                                id: settingsPageLoader

                                anchors.fill: parent
                                active: internalPage === "settings"
                                Component.onCompleted: setSource(Qt.resolvedUrl("../pages/SettingsPage.qml"), {
                                    "controller": root.windowController
                                })

                                Binding {
                                    target: settingsPageLoader.item
                                    property: "pageUrl"
                                    value: tabViewportDelegate.url
                                    when: settingsPageLoader.item !== null
                                }

                            }

                            Loader {
                                anchors.fill: parent
                                active: internalPage === "theme-editor"
                                Component.onCompleted: setSource(Qt.resolvedUrl("../pages/ThemeEditorPage.qml"), {
                                    "controller": root.windowController
                                })
                            }

                            Loader {
                                anchors.fill: parent
                                active: internalPage === "newtab"
                                source: Qt.resolvedUrl("../pages/NewTabPage.qml")
                            }

                        }

                    }

                    Rectangle {
                        anchors.fill: parent
                        visible: !controller.tabs || controller.tabs.count === 0
                        color: Theme.surface

                        Text {
                            anchors.centerIn: parent
                            text: "A quiet place for your next tab"
                            color: Theme.surfaceVariantText
                            font: Theme.titleFont
                        }

                    }

                }

                Item {
                    id: devToolsDockFrame

                    readonly property bool dockedRight: controller.currentEngine && controller.currentEngine.devToolsPlacement === 0 ? true : false

                    visible: devToolsDock.active
                    x: dockedRight ? parent.width - width : 0
                    y: dockedRight ? 0 : parent.height - height
                    width: dockedRight ? Math.max(280, Math.min(Math.round(parent.width * 0.8), controller.devToolsPaneWidth)) : parent.width
                    height: dockedRight ? parent.height : Math.max(180, Math.min(Math.round(parent.height * 0.8), controller.devToolsPaneHeight))
                    z: 5

                    Loader {
                        id: devToolsDock

                        anchors.fill: parent
                        active: controller.currentEngine && controller.currentEngine.devToolsOpen && controller.currentEngine.devToolsPlacement !== 2
                        Component.onCompleted: setSource(Qt.resolvedUrl("../panes/DevToolsPane.qml"), {
                            "engineView": controller.currentEngine
                        })

                        Binding {
                            target: devToolsDock.item
                            property: "engineView"
                            value: controller.currentEngine
                            when: devToolsDock.item !== null
                        }

                    }

                }

                Item {
                    visible: devToolsDock.active && devToolsDockFrame.dockedRight
                    anchors.right: devToolsDockFrame.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: Theme.workspaceGap
                    z: 6

                    HoverHandler {
                        cursorShape: Qt.SizeHorCursor
                    }

                    DragHandler {
                        target: null
                        onActiveChanged: {
                            if (active)
                                controller.beginDevToolsPaneResize();
                            else
                                controller.commitDevToolsPaneSize();
                        }
                        onTranslationChanged: {
                            if (active)
                                controller.resizeDevToolsPane(translation.x, true);

                        }
                    }

                }

                Item {
                    visible: devToolsDock.active && !devToolsDockFrame.dockedRight
                    anchors.bottom: devToolsDockFrame.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: Theme.workspaceGap
                    z: 6

                    HoverHandler {
                        cursorShape: Qt.SizeVerCursor
                    }

                    DragHandler {
                        target: null
                        onActiveChanged: {
                            if (active)
                                controller.beginDevToolsPaneResize();
                            else
                                controller.commitDevToolsPaneSize();
                        }
                        onTranslationChanged: {
                            if (active)
                                controller.resizeDevToolsPane(translation.y, false);

                        }
                    }

                }

                ChromeCornerMask {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    corner: "top-left"
                    surfaceHeight: shell.height
                    surfaceWidth: shell.width
                    surfaceX: workspace.x + viewportFrame.x
                    surfaceY: workspace.y + viewportFrame.y
                    z: 3
                }

                ChromeCornerMask {
                    anchors.right: parent.right
                    anchors.top: parent.top
                    corner: "top-right"
                    surfaceHeight: shell.height
                    surfaceWidth: shell.width
                    surfaceX: workspace.x + viewportFrame.x + viewportFrame.width - width
                    surfaceY: workspace.y + viewportFrame.y
                    z: 3
                }

                ChromeCornerMask {
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    corner: "bottom-right"
                    surfaceHeight: shell.height
                    surfaceWidth: shell.width
                    surfaceX: workspace.x + viewportFrame.x + viewportFrame.width - width
                    surfaceY: workspace.y + viewportFrame.y + viewportFrame.height - height
                    z: 3
                }

                ChromeCornerMask {
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                    corner: "bottom-left"
                    surfaceHeight: shell.height
                    surfaceWidth: shell.width
                    surfaceX: workspace.x + viewportFrame.x
                    surfaceY: workspace.y + viewportFrame.y + viewportFrame.height - height
                    z: 3
                }

                Rectangle {
                    anchors.fill: parent
                    radius: Theme.contentRadius
                    color: "transparent"
                    border.width: Theme.contentBorderWidth
                    border.color: Theme.contentBorder
                    z: 4
                }

            }

            Loader {
                id: paneLoader

                readonly property url requestedSource: controller.openPane === "history" ? Qt.resolvedUrl("../panes/HistoryPane.qml") : controller.openPane === "bookmarks" ? Qt.resolvedUrl("../panes/BookmarksPane.qml") : Qt.resolvedUrl("../panes/DownloadsPane.qml")

                anchors.top: parent.top
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                width: active ? Math.min(controller.paneWidth, Math.max(260, workspace.width - sidebarLoader.width - 380)) : 0
                z: 5
                active: !controller.contentFullscreen && controller.openPane.length > 0
                Component.onCompleted: setSource(requestedSource, {
                    "controller": root.windowController
                })
                onRequestedSourceChanged: setSource(requestedSource, {
                    "controller": root.windowController
                })
            }

            Item {
                id: paneResizeHandle

                anchors.left: paneLoader.left
                anchors.leftMargin: -Theme.workspaceGap
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: Theme.workspaceGap
                visible: paneLoader.active
                z: 6

                HoverHandler {
                    cursorShape: Qt.SizeHorCursor
                }

                DragHandler {
                    target: null
                    xAxis.enabled: true
                    yAxis.enabled: false
                    onActiveChanged: {
                        if (active)
                            controller.beginPaneResize();

                    }
                    onTranslationChanged: {
                        if (active)
                            controller.resizePane(translation.x);

                    }
                }

            }

            Loader {
                anchors.top: parent.top
                anchors.right: viewportFrame.right
                anchors.margins: 12
                active: controller.findVisible
                z: 8
                Component.onCompleted: setSource(Qt.resolvedUrl("../components/FindBar.qml"), {
                    "controller": root.windowController
                })
            }

        }

        Loader {
            id: securityPopoverLoader

            active: false
            asynchronous: false
            Component.onCompleted: setSource(Qt.resolvedUrl("../components/SecurityPopover.qml"), {
                "controller": root.windowController,
                "parent": shell,
                "x": Qt.binding(() => {
                    return toolbar.x + omnibox.x;
                }),
                "y": Qt.binding(() => {
                    return topChrome.height;
                }),
                "z": 12
            })
        }

        Loader {
            id: javaScriptDialogLoader

            active: false
            asynchronous: false
            Component.onCompleted: setSource(Qt.resolvedUrl("JavaScriptDialog.qml"), {
                "controller": root.windowController,
                "popupParent": shell
            })
        }

        Loader {
            id: fileDialogLoader

            active: false
            asynchronous: false
            Component.onCompleted: setSource(Qt.resolvedUrl("FilePickerDialog.qml"), {
                "controller": root.windowController
            })
        }

        Loader {
            id: permissionPromptLoader

            active: false
            asynchronous: false
            Component.onCompleted: setSource(Qt.resolvedUrl("PermissionPrompt.qml"), {
                "controller": root.windowController,
                "popupParent": shell,
                "anchorX": Qt.binding(() => {
                    return toolbar.x + omnibox.x;
                }),
                "anchorY": Qt.binding(() => {
                    return topChrome.height;
                })
            })
        }

        Loader {
            id: displayCapturePromptLoader

            active: false
            asynchronous: false
            Component.onCompleted: setSource(Qt.resolvedUrl("DisplayCapturePrompt.qml"), {
                "controller": root.windowController,
                "popupParent": shell
            })
        }

        Loader {
            id: backHistoryMenuLoader

            active: false
            asynchronous: false

            sourceComponent: Component {
                EdenMenu {
                    parent: shell
                    x: toolbar.x + navigationButtons.x + backButton.x
                    y: topChrome.height
                    preferredWidth: 360
                    maximumHeight: shell.height - y - 16
                    z: 12
                    onTriggered: (historyOffset) => {
                        return controller.navigateHistory(historyOffset);
                    }
                }

            }

        }

        Loader {
            id: forwardHistoryMenuLoader

            active: false
            asynchronous: false

            sourceComponent: Component {
                EdenMenu {
                    parent: shell
                    x: toolbar.x + navigationButtons.x + forwardButton.x
                    y: topChrome.height
                    preferredWidth: 360
                    maximumHeight: shell.height - y - 16
                    z: 12
                    onTriggered: (historyOffset) => {
                        return controller.navigateHistory(historyOffset);
                    }
                }

            }

        }

        Loader {
            id: pageContextMenuLoader

            active: false
            asynchronous: false

            sourceComponent: Component {
                EdenMenu {
                    readonly property bool devToolsSurface: controller.pageContextMenuSurface === "devtools"
                    readonly property real surfaceX: devToolsSurface ? workspace.x + viewportFrame.x + devToolsDockFrame.x + Theme.contentBorderWidth : workspace.x + viewportFrame.x
                    readonly property real surfaceY: devToolsSurface ? workspace.y + viewportFrame.y + devToolsDockFrame.y + 52 + Theme.contentBorderWidth : workspace.y + viewportFrame.y

                    parent: shell
                    x: Math.max(8, Math.min(shell.width - width - 8, surfaceX + controller.pageContextMenuPosition.x))
                    y: Math.max(8, Math.min(shell.height - height - 8, surfaceY + controller.pageContextMenuPosition.y))
                    preferredWidth: 260
                    maximumHeight: shell.height - 16
                    z: 14
                    actions: controller.pageContextMenuActions
                    onClosed: controller.dismissPageContextMenu()
                    onTriggered: (command) => {
                        return controller.executePageContextMenuCommand(command);
                    }
                }

            }

        }

        Loader {
            id: autofillMenuLoader

            active: false
            asynchronous: false

            sourceComponent: Component {
                EdenMenu {
                    parent: shell
                    x: Math.max(8, Math.min(shell.width - width - 8, workspace.x + viewportFrame.x + controller.autofillPopupPosition.x))
                    y: Math.max(8, Math.min(shell.height - height - 8, workspace.y + viewportFrame.y + controller.autofillPopupPosition.y + 4))
                    preferredWidth: 320
                    maximumHeight: shell.height - 16
                    z: 14
                    actions: controller.autofillSuggestions
                    onTriggered: (command) => {
                        return controller.fillAutofillSuggestion(command);
                    }
                }

            }

        }

        Instantiator {
            model: controller.commandPaletteVisible ? 1 : 0

            delegate: CommandPalette {
                controller: root.windowController
                parent: shell
                anchors.centerIn: parent
                z: 20
                Component.onCompleted: open()
            }

        }

        Connections {
            function onCloseWindowRequested() {
                root.close();
            }

            function onPageContextMenuRequested() {
                if (controller.pageContextMenuSurface === "devtools" && controller.currentEngine && controller.currentEngine.devToolsPlacement === 2)
                    return ;

                pageContextMenuLoader.active = true;
                if (!pageContextMenuLoader.item.tryOpen())
                    controller.dismissPageContextMenu();

            }

            function onAutofillRequested() {
                autofillMenuLoader.active = true;
                if (autofillMenuLoader.item)
                    autofillMenuLoader.item.tryOpen();

            }

            function onCredentialStateChanged() {
                if (controller.autofillSuggestions.length === 0 && autofillMenuLoader.item)
                    autofillMenuLoader.item.close();

            }

            function onJavaScriptDialogChanged() {
                if (controller.javaScriptDialog.id === undefined && javaScriptDialogLoader.item)
                    javaScriptDialogLoader.item.close();

            }

            function onJavaScriptDialogRequested() {
                javaScriptDialogLoader.active = true;
                javaScriptDialogLoader.item.open();
            }

            function onFileDialogChanged() {
                if (controller.fileDialog.id === undefined && fileDialogLoader.item)
                    fileDialogLoader.item.close();

            }

            function onFileDialogRequested() {
                fileDialogLoader.active = true;
                fileDialogLoader.item.open();
            }

            function onPermissionRequestChanged() {
                if (controller.permissionRequest.id === undefined && permissionPromptLoader.item)
                    permissionPromptLoader.item.close();

            }

            function onPermissionRequestRequested() {
                permissionPromptLoader.active = true;
                permissionPromptLoader.item.open();
            }

            function onDisplayCaptureRequestChanged() {
                if (controller.displayCaptureRequest.id === undefined && displayCapturePromptLoader.item) {
                    displayCapturePromptLoader.item.close();
                }

            }

            function onDisplayCaptureRequestRequested() {
                displayCapturePromptLoader.active = true;
                displayCapturePromptLoader.item.open();
            }

            target: controller
        }

        Loader {
            id: profileMenuLoader

            active: false
            asynchronous: false

            sourceComponent: Component {
                ProfileMenu {
                    parent: shell
                    controller: root.windowController
                    x: shell.width - width - 56
                    y: topChrome.height
                    z: 12
                }

            }

        }

        Rectangle {
            id: transientToast

            property string message

            function show(text) {
                message = text;
                opacity = 1;
                toastTimer.restart();
            }

            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 24
            width: Math.min(shell.width - 48, toastText.implicitWidth + 40)
            height: 44
            radius: 22
            color: Theme.surfaceContainerHighest
            border.width: Theme.menuBorderWidth
            border.color: Theme.menuBorder
            opacity: 0
            visible: opacity > 0
            z: 30
            Accessible.role: Accessible.AlertMessage
            Accessible.name: message

            Text {
                id: toastText

                anchors.centerIn: parent
                text: transientToast.message
                color: Theme.surfaceText
                font: Theme.labelFont
            }

            Timer {
                id: toastTimer

                interval: 6000
                onTriggered: transientToast.opacity = 0
            }

            Behavior on opacity {
                NumberAnimation {
                    duration: Theme.mediumDuration
                    easing.type: Easing.OutCubic
                }

            }

        }

        Loader {
            id: signOutConfirmLoader

            active: false
            asynchronous: false

            sourceComponent: Component {
                Popup {
                    id: signOutConfirm

                    property string profileId
                    property int downloadCount

                    function openFor(id, count) {
                        profileId = id;
                        downloadCount = count;
                        open();
                    }

                    parent: shell
                    anchors.centerIn: parent
                    width: 380
                    modal: true
                    padding: 20
                    closePolicy: Popup.NoAutoClose

                    background: OverlaySurface {
                        surfaceRadius: Theme.menuRadius
                    }

                    contentItem: Column {
                        spacing: 14

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            text: signOutConfirm.downloadCount === 1 ? "1 download is still running. Signing out will cancel it." : signOutConfirm.downloadCount + " downloads are still running. Signing out will cancel them."
                            color: Theme.surfaceText
                            font: Theme.bodyFont
                        }

                        Row {
                            anchors.horizontalCenter: parent.horizontalCenter
                            spacing: 12

                            EdenButton {
                                text: "Stay signed in"
                                onClicked: {
                                    Profiles.confirmSignOut(signOutConfirm.profileId, false);
                                    signOutConfirm.close();
                                }
                            }

                            EdenButton {
                                text: "Sign out"
                                iconName: "logout"
                                onClicked: {
                                    Profiles.confirmSignOut(signOutConfirm.profileId, true);
                                    signOutConfirm.close();
                                }
                            }

                        }

                    }

                }

            }

        }

        Connections {
            function onTransientMessageRequested(message) {
                transientToast.show(message);
            }

            target: controller
        }

        Connections {
            function onSignOutConfirmationRequired(profileId, downloadCount) {
                if (profileId !== controller.profileId)
                    return ;

                if (root.active || Profiles.browserWindowCount === 1) {
                    signOutConfirmLoader.active = true;
                    signOutConfirmLoader.item.openFor(profileId, downloadCount);
                }
            }

            target: Profiles
        }

        Loader {
            id: mainMenuLoader

            active: false
            asynchronous: false

            sourceComponent: Component {
                EdenMenu {
                    parent: shell
                    x: shell.width - width - 12
                    y: topChrome.height
                    z: 12
                    actions: [{
                        "id": "new_tab",
                        "title": "New tab",
                        "icon": "add"
                    }, {
                        "id": "new_window",
                        "title": "New window",
                        "icon": "square-top-down"
                    }, {
                        "id": "private_window",
                        "title": "New private window",
                        "icon": "incognito"
                    }, {
                        "id": "bookmarks",
                        "title": "Bookmarks",
                        "icon": "bookmark-circle"
                    }, {
                        "id": "history",
                        "title": "History",
                        "icon": "history"
                    }, {
                        "id": "downloads",
                        "title": "Downloads",
                        "icon": "round-transfer-vertical"
                    }, {
                        "id": "devtools",
                        "title": "Developer tools",
                        "icon": "programming"
                    }, {
                        "id": "about_eden",
                        "title": "About Eden",
                        "icon": "info-circle"
                    }, {
                        "id": "settings",
                        "title": "Settings",
                        "icon": "settings"
                    }, {
                        "id": "quit",
                        "title": "Quit",
                        "icon": "power"
                    }]
                    onTriggered: (actionId) => {
                        if (actionId === "new_tab")
                            controller.newTabAndFocusOmnibox();
                        else if (actionId === "new_window")
                            controller.openNewWindow(false);
                        else if (actionId === "private_window")
                            controller.openNewWindow(true);
                        else if (actionId === "bookmarks")
                            controller.openPane = "bookmarks";
                        else if (actionId === "history")
                            controller.openPane = "history";
                        else if (actionId === "downloads")
                            controller.openPane = "downloads";
                        else if (actionId === "about_eden")
                            controller.openAboutTab();
                        else
                            controller.shortcuts.execute(actionId);
                    }
                }

            }

        }

        ResizeHandle {
            edges: Qt.LeftEdge
            cursorShape: Qt.SizeHorCursor
            width: 8
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            visible: !root.edgeToEdge
            z: 100
        }

        ResizeHandle {
            edges: Qt.RightEdge
            cursorShape: Qt.SizeHorCursor
            width: 8
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            visible: !root.edgeToEdge
            z: 100
        }

        ResizeHandle {
            edges: Qt.TopEdge
            cursorShape: Qt.SizeVerCursor
            height: 8
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            visible: !root.edgeToEdge
            z: 100
        }

        ResizeHandle {
            edges: Qt.BottomEdge
            cursorShape: Qt.SizeVerCursor
            height: 8
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            visible: !root.edgeToEdge
            z: 100
        }

        ResizeHandle {
            edges: Qt.LeftEdge | Qt.TopEdge
            cursorShape: Qt.SizeFDiagCursor
            width: 12
            height: 12
            anchors.left: parent.left
            anchors.top: parent.top
            visible: !root.edgeToEdge
            z: 101
        }

        ResizeHandle {
            edges: Qt.RightEdge | Qt.TopEdge
            cursorShape: Qt.SizeBDiagCursor
            width: 12
            height: 12
            anchors.right: parent.right
            anchors.top: parent.top
            visible: !root.edgeToEdge
            z: 101
        }

        ResizeHandle {
            edges: Qt.LeftEdge | Qt.BottomEdge
            cursorShape: Qt.SizeBDiagCursor
            width: 12
            height: 12
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            visible: !root.edgeToEdge
            z: 101
        }

        ResizeHandle {
            edges: Qt.RightEdge | Qt.BottomEdge
            cursorShape: Qt.SizeFDiagCursor
            width: 12
            height: 12
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            visible: !root.edgeToEdge
            z: 101
        }

        component ResizeHandle: Item {
            required property int edges
            property int cursorShape: Qt.ArrowCursor

            HoverHandler {
                cursorShape: parent.cursorShape
            }

            DragHandler {
                target: null
                acceptedButtons: Qt.LeftButton
                onActiveChanged: {
                    if (active)
                        frame.startSystemResize(parent.edges);

                }
            }

        }

    }

    Loader {
        active: controller.currentEngine && controller.currentEngine.devToolsOpen && controller.currentEngine.devToolsPlacement === 2
        asynchronous: false
        Component.onCompleted: setSource(Qt.resolvedUrl("DevToolsWindow.qml"), {
            "controller": root.windowController,
            "hostWindow": root
        })
    }

}
