import Eden.Ui
import QtQuick
import QtQuick.Controls

Rectangle {
    id: sidebar

    required property var controller
    property bool verticalDropLayout: true
    readonly property real dropNormalExtent: 42
    readonly property real dropPinnedExtent: 42
    readonly property real dropSpacing: 4

    objectName: "tabDropArea"
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

        objectName: "tabDropView"
        anchors.top: collapseButton.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: newTabButton.top
        anchors.margins: 4
        spacing: 4
        clip: !sidebar.controller.tabDragTorn
        z: sidebar.controller.tabDragTorn ? 5 : 0
        model: sidebar.controller.tabs
        currentIndex: sidebar.controller.activeIndex

        displaced: Transition {
            NumberAnimation {
                properties: "x,y"
                duration: Theme.shortDuration
                easing.type: Easing.OutBack
            }
        }

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

        delegate: Rectangle {
            id: tab

            required property int index
            required property string title
            required property url url
            required property url favicon
            required property bool isLoading
            required property bool isPinned
            required property var activityIndicators
            required property bool isAudible
            required property bool isMuted
            required property var engineView
            required property string internalPage
            required property string engineName
            required property var tabId
            required property bool discarded
            readonly property bool activeTab: ListView.isCurrentItem
            readonly property bool dragging: sidebar.controller.tabDragIndex === index
            property bool previewSuppressed: false

            width: tabsView.width
            height: 42
            radius: Theme.cardRadius
            color: "transparent"
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
                opacity: hover.hovered && !tab.activeTab ? 1 : 0

                Behavior on opacity {
                    NumberAnimation {
                        duration: Theme.shortDuration
                        easing.type: Easing.OutCubic
                    }
                }
            }

            Row {
                id: tabContent

                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: anchors.leftMargin - (closeButton.width - tabIcon.width) / 2
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
                    textFormat: Text.PlainText
                    visible: sidebar.controller.sidebarExpanded
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.max(0, tabContent.width - tabIcon.width - activity.width - closeButton.width - tabContent.spacing * (activity.visible ? 3 : 2))
                    text: tab.title.length > 0 ? tab.title : tab.url.toString()
                    color: Theme.surfaceText
                    font: Theme.bodyFont
                    elide: Text.ElideRight
                }

                TabActivity {
                    id: activity
                    anchors.verticalCenter: parent.verticalCenter
                    indicators: tab.activityIndicators
                    visible: sidebar.controller.sidebarExpanded && indicators.length > 0
                    width: visible ? implicitWidth : 0
                }

                EdenButton {
                    id: closeButton

                    visible: sidebar.controller.sidebarExpanded && hover.hovered
                    width: 28
                    height: 28
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: "close"
                    onClicked: sidebar.controller.closeTab(tab.index)
                }
            }

            TabActivity {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 2
                indicators: tab.activityIndicators
                compact: true
                iconSize: 12
                spacing: 1
                visible: !sidebar.controller.sidebarExpanded && indicators.length > 0
            }

            HoverHandler {
                id: hover

                onHoveredChanged: {
                    if (!hovered) {
                        tab.previewSuppressed = false;
                    }
                }
            }

            TapHandler {
                acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                onPressedChanged: {
                    if (pressed) {
                        tab.previewSuppressed = true;
                    }
                }
                onTapped: (_, button) => {
                    if (button === Qt.MiddleButton) {
                        sidebar.controller.closeTab(tab.index);
                    } else {
                        sidebar.controller.activeIndex = tab.index;
                    }
                }
            }

            TapHandler {
                acceptedButtons: Qt.RightButton
                onPressedChanged: {
                    if (pressed) {
                        tab.previewSuppressed = true;
                    }
                }
                onTapped: tabMenu.open()
            }

            DragHandler {
                id: dragHandler

                target: null
                yAxis.enabled: true
                xAxis.enabled: true
                onActiveChanged: {
                    if (active) {
                        sidebar.controller.beginTabDrag(tab.index, tab, dragHandler.centroid.pressPosition.x, dragHandler.centroid.pressPosition.y);
                    }
                }
            }

            EdenMenu {
                id: tabMenu

                x: tab.width
                onAboutToShow: actions = sidebar.controller.tabContextMenuActions(tab.index)
                onTriggered: actionId => {
                    return sidebar.controller.executeTabContextMenuCommand(tab.index, actionId);
                }
            }

            TabPreview {
                controller: sidebar.controller
                tabIndex: tab.index
                anchorItem: tab
                anchorHovered: hover.hovered && !tab.previewSuppressed && !tabMenu.opened && !tab.dragging
                vertical: true
            }

            transform: Translate {
                y: sidebar.controller.tabDragRevision >= 0 ? sidebar.controller.tabDragTranslation(tab.index, tab.y) : 0

                Behavior on y {
                    enabled: !tab.dragging

                    NumberAnimation {
                        duration: Theme.shortDuration
                        easing.type: Easing.OutCubic
                    }
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
        onClicked: sidebar.controller.newTabAndFocusOmnibox()
    }

    DropArea {
        anchors.fill: parent
        keys: ["application/x-eden-tab"]
        onEntered: drag => {
            return sidebar.controller.tabDragEntered(sidebar, drag.x, drag.y);
        }
        onPositionChanged: drag => {
            return sidebar.controller.tabDragMoved(sidebar, drag.x, drag.y);
        }
        onExited: sidebar.controller.tabDragLeft()
        onDropped: drop => {
            if (sidebar.controller.tabDragDropped(sidebar, drop.x, drop.y)) {
                drop.acceptProposedAction();
            }
        }
    }

    Behavior on implicitWidth {
        NumberAnimation {
            duration: Theme.mediumDuration
            easing.type: Easing.OutCubic
        }
    }
}
