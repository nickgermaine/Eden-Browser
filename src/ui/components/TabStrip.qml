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
    readonly property real edgePadding: 4
    readonly property real leadingTabsPadding: 8
    readonly property real newTabSpacing: 6
    readonly property var tabModel: controller ? controller.tabs : null
    readonly property int tabCount: tabModel ? tabModel.count : 0
    readonly property int pinnedTabCount: tabModel ? tabModel.pinnedCount : 0
    readonly property int normalTabCount: Math.max(0, tabCount - pinnedTabCount)
    readonly property real baseTabsWidth: Math.max(0, width - newTabButton.width - 2 * edgePadding - leadingTabsPadding - newTabSpacing)
    readonly property real minimumTabsContentWidth: pinnedTabCount * Theme.pinnedTabWidth + normalTabCount * Theme.tabMinimumWidth + Math.max(0, tabCount - 1) * dropSpacing
    readonly property bool overflowing: minimumTabsContentWidth > baseTabsWidth
    readonly property real fittedTabWidth: normalTabCount > 0 ? (baseTabsWidth - pinnedTabCount * Theme.pinnedTabWidth - Math.max(0, tabCount - 1) * dropSpacing) / normalTabCount : Theme.tabMaximumWidth
    readonly property real renderedTabWidth: Math.max(Theme.tabMinimumWidth, Math.min(Theme.tabMaximumWidth, fittedTabWidth))
    readonly property real renderedTabsContentWidth: pinnedTabCount * Theme.pinnedTabWidth + normalTabCount * renderedTabWidth + Math.max(0, tabCount - 1) * dropSpacing

    signal systemMoveRequested
    signal toggleMaximizedRequested

    onRenderedTabsContentWidthChanged: navigator.scheduleRelayout(overflowing, controller.activeIndex)
    onWidthChanged: navigator.scheduleRelayout(overflowing, controller.activeIndex)
    objectName: "tabDropArea"
    implicitHeight: 44

    TabStripNavigator {
        id: navigator

        view: tabsView
    }

    EdenButton {
        id: previousTabsButton

        anchors.left: parent.left
        anchors.leftMargin: strip.edgePadding
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
        anchors.leftMargin: strip.overflowing ? strip.dropSpacing : strip.leadingTabsPadding
        anchors.rightMargin: strip.overflowing ? strip.dropSpacing : 0
        orientation: ListView.Horizontal
        spacing: strip.dropSpacing
        clip: !strip.controller.tabDragTorn
        z: strip.controller.tabDragTorn ? 5 : 0
        boundsBehavior: Flickable.StopAtBounds
        model: strip.tabModel
        currentIndex: strip.controller.activeIndex
        onCurrentIndexChanged: navigator.reveal(currentIndex)
        onWidthChanged: navigator.scheduleRelayout(strip.overflowing, currentIndex)
        Component.onCompleted: navigator.scheduleRelayout(strip.overflowing, currentIndex)

        add: Transition {
            NumberAnimation {
                property: "opacity"
                from: 0
                to: 1
                duration: Theme.shortDuration
                easing.type: Easing.OutCubic
            }
        }

        remove: Transition {
            NumberAnimation {
                property: "opacity"
                to: 0
                duration: 0
                easing.type: Easing.OutCubic
            }
        }

        removeDisplaced: Transition {
            NumberAnimation {
                properties: "x,y"
                duration: 0
            }
        }

        move: Transition {
            NumberAnimation {
                properties: "x,y"
                duration: Theme.shortDuration
                easing.type: Easing.OutBack
            }

            NumberAnimation {
                property: "opacity"
                to: 1
                duration: Theme.shortDuration
            }
        }

        displaced: Transition {
            NumberAnimation {
                properties: "x,y"
                duration: Theme.shortDuration
                easing.type: Easing.OutBack
            }

            NumberAnimation {
                property: "opacity"
                to: 1
                duration: Theme.shortDuration
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
            required property var activityIndicators
            required property bool isAudible
            required property bool isMuted
            required property var engineView
            required property string internalPage
            required property string engineName
            required property var tabId
            required property bool discarded
            readonly property bool activeTab: ListView.isCurrentItem
            readonly property bool dragging: strip.controller.tabDragIndex === index
            property bool previewSuppressed: false

            width: isPinned ? Theme.pinnedTabWidth : strip.renderedTabWidth
            height: 36
            anchors.verticalCenter: parent ? parent.verticalCenter : undefined
            radius: Theme.cardRadius
            color: "transparent"
            scale: dragging ? 1.04 : 1
            z: dragging ? 3 : 1
            onActiveTabChanged: {
                if (activeTab)
                    opacity = 1;
            }

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
                    textFormat: Text.PlainText
                    visible: !tab.isPinned
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.max(0, parent.width - tabIcon.width - activity.width - 52)
                    text: tab.title.length > 0 ? tab.title : tab.url.toString()
                    color: Theme.surfaceText
                    font: Theme.bodyFont
                    elide: Text.ElideRight
                }

                TabActivity {
                    id: activity
                    anchors.verticalCenter: parent.verticalCenter
                    indicators: tab.activityIndicators
                    compact: tab.width < indicators.length * 20 + 104
                    visible: !tab.isPinned && tab.width >= 108 && indicators.length > 0
                    width: visible ? implicitWidth : 0
                }

                EdenButton {
                    visible: !tab.isPinned && tab.width >= 80 && (pointer.hovered || tab.activeTab)
                    anchors.verticalCenter: parent.verticalCenter
                    width: 28
                    height: 28
                    iconName: "close"
                    onClicked: strip.controller.closeTab(tab.index)
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
                visible: (tab.isPinned || tab.width < 108) && indicators.length > 0
            }

            HoverHandler {
                id: pointer

                onHoveredChanged: {
                    if (!hovered)
                        tab.previewSuppressed = false;
                }
            }

            TapHandler {
                acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                onPressedChanged: {
                    if (pressed)
                        tab.previewSuppressed = true;
                }
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
                onPressedChanged: {
                    if (pressed)
                        tab.previewSuppressed = true;
                }
                onTapped: tabMenu.toggle()
            }

            EdenMenu {
                id: tabMenu

                y: tab.height
                onAboutToShow: actions = strip.controller.tabContextMenuActions(tab.index)
                onTriggered: actionId => {
                    return strip.controller.executeTabContextMenuCommand(tab.index, actionId);
                }
            }

            TabPreview {
                controller: strip.controller
                tabIndex: tab.index
                anchorItem: tab
                anchorHovered: pointer.hovered && !tab.previewSuppressed && !tabMenu.opened && !tab.dragging
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

        buttonRadius: 20
        anchors.verticalCenter: parent.verticalCenter
        x: strip.overflowing ? strip.width - width - strip.edgePadding : Math.min(tabsView.x + strip.renderedTabsContentWidth + strip.newTabSpacing, strip.width - width - strip.edgePadding)
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
        onEntered: drag => {
            return strip.controller.tabDragEntered(strip, drag.x, drag.y);
        }
        onPositionChanged: drag => {
            return strip.controller.tabDragMoved(strip, drag.x, drag.y);
        }
        onExited: strip.controller.tabDragLeft()
        onDropped: drop => {
            if (strip.controller.tabDragDropped(strip, drop.x, drop.y))
                drop.acceptProposedAction();
        }
    }
}
