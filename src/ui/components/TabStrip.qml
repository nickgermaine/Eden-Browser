import Eden.Ui
import QtQuick
import QtQuick.Controls

Item {
    id: strip

    required property var controller
    property bool verticalDropLayout: false
    readonly property real dropNormalExtent: renderedTabWidth
    readonly property real dropPinnedExtent: Theme.pinnedTabWidth
    readonly property real dropSpacing: 4
    readonly property var tabModel: controller ? controller.tabs : null
    readonly property int tabCount: tabModel ? tabModel.count : 0
    readonly property int pinnedTabCount: tabModel ? tabModel.pinnedCount : 0
    readonly property int normalTabCount: Math.max(0, tabCount - pinnedTabCount)
    readonly property real baseTabsWidth: Math.max(0, width - newTabButton.width - 12)
    readonly property real minimumTabsContentWidth: pinnedTabCount * Theme.pinnedTabWidth + normalTabCount * Theme.tabMinimumWidth + Math.max(0, tabCount - 1) * 4
    readonly property bool overflowing: minimumTabsContentWidth > baseTabsWidth
    readonly property real fittedTabWidth: normalTabCount > 0 ? (baseTabsWidth - pinnedTabCount * Theme.pinnedTabWidth - Math.max(0, tabCount - 1) * 4) / normalTabCount : Theme.tabMaximumWidth
    readonly property real renderedTabWidth: Math.max(Theme.tabMinimumWidth, Math.min(Theme.tabMaximumWidth, fittedTabWidth))
    readonly property real renderedTabsContentWidth: pinnedTabCount * Theme.pinnedTabWidth + normalTabCount * renderedTabWidth + Math.max(0, tabCount - 1) * 4

    signal systemMoveRequested()
    signal toggleMaximizedRequested()

    objectName: "tabDropArea"
    implicitHeight: 44

    TabStripNavigator {
        id: navigator

        view: tabsView
    }

    EdenButton {
        id: previousTabsButton

        anchors.left: parent.left
        anchors.leftMargin: 4
        anchors.verticalCenter: parent.verticalCenter
        width: strip.overflowing ? 32 : 0
        height: 32
        visible: strip.overflowing
        enabled: !tabsView.atXBeginning
        iconName: "arrow-left"
        onClicked: navigator.snap(-1)

        Timer {
            running: previousTabsButton.down
            interval: 60
            repeat: true
            onTriggered: navigator.slide(-14)
        }

    }

    ListView {
        id: tabsView

        objectName: "tabDropView"
        anchors.left: previousTabsButton.right
        anchors.right: nextTabsButton.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.leftMargin: strip.overflowing ? 4 : 8
        anchors.rightMargin: strip.overflowing ? 4 : 0
        orientation: ListView.Horizontal
        spacing: 4
        clip: !strip.controller.tabDragTorn
        z: strip.controller.tabDragTorn ? 5 : 0
        boundsBehavior: Flickable.StopAtBounds
        model: strip.tabModel
        currentIndex: strip.controller.activeIndex
        onCurrentIndexChanged: navigator.reveal(currentIndex)

        add: Transition {
            NumberAnimation {
                properties: "opacity,scale"
                from: 0
                to: 1
                duration: Theme.shortDuration
                easing.type: Easing.OutCubic
            }

        }

        remove: Transition {
            NumberAnimation {
                properties: "opacity,scale"
                to: 0
                duration: Theme.shortDuration
                easing.type: Easing.OutCubic
            }

        }

        move: Transition {
            NumberAnimation {
                properties: "x,y"
                duration: Theme.shortDuration
                easing.type: Easing.OutBack
            }

        }

        displaced: Transition {
            NumberAnimation {
                properties: "x,y"
                duration: Theme.shortDuration
                easing.type: Easing.OutBack
            }

        }

        delegate: Rectangle {
            id: tab

            required property int index
            required property string title
            required property url url
            required property url favicon
            required property bool isLoading
            required property int progress
            required property bool isPinned
            required property bool isAudible
            required property bool isMuted
            required property var engineView
            required property string internalPage
            readonly property bool activeTab: ListView.isCurrentItem
            readonly property bool dragging: strip.controller.tabDragIndex === index

            width: isPinned ? Theme.pinnedTabWidth : Math.max(Theme.tabMinimumWidth, Math.min(Theme.tabMaximumWidth, strip.fittedTabWidth))
            height: 36
            anchors.verticalCenter: parent ? parent.verticalCenter : undefined
            radius: Theme.cardRadius
            color: "transparent"
            scale: dragging ? 1.04 : 1
            z: dragging ? 3 : 1

            Rectangle {
                anchors.fill: parent
                radius: tab.radius
                color: Theme.surfaceContainerHighest
                opacity: tab.activeTab ? 1 : 0

                Behavior on opacity {
                    NumberAnimation {
                        duration: Theme.shortDuration
                        easing.type: Easing.OutCubic
                    }

                }

            }

            Rectangle {
                anchors.fill: parent
                radius: tab.radius
                color: Theme.surfaceContainerHigh
                opacity: pointer.hovered && !tab.activeTab ? 1 : 0

                Behavior on opacity {
                    NumberAnimation {
                        duration: Theme.shortDuration
                        easing.type: Easing.OutCubic
                    }

                }

            }

            Row {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 6
                spacing: 8

                TabIcon {
                    id: tabIcon

                    anchors.verticalCenter: parent.verticalCenter
                    favicon: tab.favicon
                    loading: tab.isLoading
                    muted: tab.isMuted
                    audible: tab.isAudible
                    internalPage: tab.internalPage
                }

                Text {
                    visible: !tab.isPinned
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.max(0, parent.width - tabIcon.width - 44)
                    text: tab.title.length > 0 ? tab.title : tab.url.toString()
                    color: Theme.surfaceText
                    font: Theme.bodyFont
                    elide: Text.ElideRight
                }

                EdenButton {
                    visible: !tab.isPinned && (pointer.hovered || tab.activeTab)
                    anchors.verticalCenter: parent.verticalCenter
                    width: 28
                    height: 28
                    iconName: "close"
                    onClicked: strip.controller.closeTab(tab.index)
                }

            }

            HoverHandler {
                id: pointer
            }

            TapHandler {
                acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                onTapped: (_, button) => {
                    if (button === Qt.MiddleButton)
                        strip.controller.closeTab(tab.index);
                    else
                        strip.controller.activeIndex = tab.index;
                }
            }

            DragHandler {
                id: dragHandler

                target: null
                xAxis.enabled: true
                yAxis.enabled: true
                onActiveChanged: {
                    if (active)
                        strip.controller.beginTabDrag(tab.index, tab, dragHandler.centroid.pressPosition.x, dragHandler.centroid.pressPosition.y);

                }
            }

            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: tabMenu.open()
            }

            EdenMenu {
                id: tabMenu

                y: tab.height
                actions: [{
                    "id": "new",
                    "title": "New tab",
                    "icon": "add"
                }, {
                    "id": "reload",
                    "title": "Reload",
                    "icon": "refresh"
                }, {
                    "id": "duplicate",
                    "title": "Duplicate",
                    "icon": "copy"
                }, {
                    "id": "pin",
                    "title": tab.isPinned ? "Unpin" : "Pin",
                    "icon": "pin"
                }, {
                    "id": "mute",
                    "title": tab.isMuted ? "Unmute" : "Mute",
                    "icon": tab.isMuted ? "volume" : "muted"
                }, {
                    "id": "close",
                    "title": "Close",
                    "icon": "close"
                }, {
                    "id": "close_others",
                    "title": "Close others",
                    "icon": "close"
                }]
                onTriggered: (actionId) => {
                    if (actionId === "new")
                        strip.controller.newTabAndFocusOmnibox();
                    else if (actionId === "reload" && tab.engineView)
                        tab.engineView.reload();
                    else if (actionId === "duplicate")
                        strip.controller.tabs.duplicateTab(tab.index);
                    else if (actionId === "pin")
                        strip.controller.tabs.pinTab(tab.index, !tab.isPinned);
                    else if (actionId === "mute")
                        strip.controller.tabs.toggleMuted(tab.index);
                    else if (actionId === "close")
                        strip.controller.closeTab(tab.index);
                    else if (actionId === "close_others")
                        strip.controller.tabs.closeOthers(tab.index);
                }
            }

            transform: Translate {
                id: dragTranslation

                x: strip.controller.tabDragRevision >= 0 ? strip.controller.tabDragTranslation(tab.index, tab.x) : 0

                Behavior on x {
                    enabled: !tab.dragging

                    NumberAnimation {
                        duration: Theme.shortDuration
                        easing.type: Easing.OutCubic
                    }

                }

            }

            Behavior on scale {
                NumberAnimation {
                    duration: Theme.shortDuration
                    easing.type: Easing.OutCubic
                }

            }

        }

    }

    EdenButton {
        id: nextTabsButton

        anchors.right: newTabButton.left
        anchors.verticalCenter: parent.verticalCenter
        width: strip.overflowing ? 32 : 0
        height: 32
        visible: strip.overflowing
        enabled: !tabsView.atXEnd
        iconName: "arrow-right"
        onClicked: navigator.snap(1)

        Timer {
            running: nextTabsButton.down
            interval: 60
            repeat: true
            onTriggered: navigator.slide(14)
        }

    }

    EdenButton {
        id: newTabButton

        anchors.verticalCenter: parent.verticalCenter
        x: strip.overflowing ? strip.width - width - 4 : Math.min(tabsView.x + strip.renderedTabsContentWidth + 6, strip.width - width - 4)
        iconName: "add"
        onClicked: strip.controller.newTabAndFocusOmnibox()
    }

    Item {
        anchors.left: newTabButton.right
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        visible: !strip.overflowing

        DragHandler {
            target: null
            dragThreshold: 4
            onActiveChanged: {
                if (active)
                    strip.systemMoveRequested();

            }
        }

        TapHandler {
            onDoubleTapped: strip.toggleMaximizedRequested()
        }

    }

    DropArea {
        anchors.fill: parent
        keys: ["application/x-eden-tab"]
        onEntered: (drag) => {
            return strip.controller.tabDragEntered(strip, drag.x, drag.y);
        }
        onPositionChanged: (drag) => {
            return strip.controller.tabDragMoved(strip, drag.x, drag.y);
        }
        onExited: strip.controller.tabDragLeft()
        onDropped: (drop) => {
            if (strip.controller.tabDragDropped(strip, drop.x, drop.y))
                drop.acceptProposedAction();

        }
    }

}
