import Eden.Ui
import QtQuick
import QtQuick.Controls

Rectangle {
    id: sidebar

    required property var controller

    implicitWidth: controller.sidebarExpanded ? 240 : 48
    color: Theme.surfaceContainer
    radius: Theme.contentRadius
    border.width: Theme.paneBorderWidth
    border.color: Theme.paneBorder
    clip: true

    EdenButton {
        id: collapseButton

        anchors.top: parent.top
        anchors.right: parent.right
        width: 44
        iconName: "sidebar"
        filledIcon: sidebar.controller.sidebarExpanded
        onClicked: sidebar.controller.sidebarExpanded = !sidebar.controller.sidebarExpanded
    }

    ListView {
        id: tabsView

        anchors.top: collapseButton.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: newTabButton.top
        anchors.margins: 4
        spacing: 4
        clip: true
        model: sidebar.controller.tabs
        currentIndex: sidebar.controller.activeIndex

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
            required property bool isPinned
            required property bool isAudible
            required property bool isMuted
            required property var engineView
            required property string internalPage
            readonly property bool activeTab: ListView.isCurrentItem

            width: tabsView.width
            height: 42
            radius: Theme.cardRadius
            color: "transparent"

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
                opacity: hover.hovered && !tab.activeTab ? 1 : 0

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
                anchors.rightMargin: 4
                spacing: 10

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
                    visible: sidebar.controller.sidebarExpanded
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.max(0, parent.width - tabIcon.width - 48)
                    text: tab.title.length > 0 ? tab.title : tab.url.toString()
                    color: Theme.surfaceText
                    font: Theme.bodyFont
                    elide: Text.ElideRight
                }

                EdenButton {
                    visible: sidebar.controller.sidebarExpanded && hover.hovered
                    width: 28
                    height: 28
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: "close"
                    onClicked: sidebar.controller.closeTab(tab.index)
                }

            }

            HoverHandler {
                id: hover
            }

            TapHandler {
                acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                onTapped: (_, button) => {
                    if (button === Qt.MiddleButton)
                        sidebar.controller.closeTab(tab.index);
                    else
                        sidebar.controller.activeIndex = tab.index;
                }
            }

            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: tabMenu.open()
            }

            DragHandler {
                id: dragHandler

                yAxis.enabled: true
                xAxis.enabled: false
                onActiveChanged: {
                    if (!active) {
                        const targetIndex = Math.max(0, Math.min(tabsView.count - 1, Math.floor((tab.y + tab.height / 2) / (tab.height + tabsView.spacing))));
                        sidebar.controller.tabs.moveTab(tab.index, targetIndex);
                    }
                }
            }

            EdenMenu {
                id: tabMenu

                x: tab.width
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
                    "icon": "muted"
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
                        sidebar.controller.newTab();
                    else if (actionId === "reload" && tab.engineView)
                        tab.engineView.reload();
                    else if (actionId === "duplicate")
                        sidebar.controller.tabs.duplicateTab(tab.index);
                    else if (actionId === "pin")
                        sidebar.controller.tabs.pinTab(tab.index, !tab.isPinned);
                    else if (actionId === "mute")
                        sidebar.controller.tabs.toggleMuted(tab.index);
                    else if (actionId === "close")
                        sidebar.controller.closeTab(tab.index);
                    else if (actionId === "close_others")
                        sidebar.controller.tabs.closeOthers(tab.index);
                }
            }

        }

    }

    EdenButton {
        id: newTabButton

        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        width: sidebar.controller.sidebarExpanded ? parent.width - 8 : 40
        iconName: "add"
        text: sidebar.controller.sidebarExpanded ? "New tab" : ""
        onClicked: sidebar.controller.newTab()
    }

    Behavior on implicitWidth {
        NumberAnimation {
            duration: Theme.mediumDuration
            easing.type: Easing.OutCubic
        }

    }

}
