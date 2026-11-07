import Eden.Ui
import QtQuick
import QtQuick.Effects

Item {
    id: view

    required property var controller
    required property string kind
    property bool showSearch: false
    property bool closePaneOnOpen: false
    property real sidePadding: 8
    readonly property var listModel: controller.createLibraryModel(kind, view)
    readonly property int count: list.count
    readonly property string emptyText: kind === "history" ? "No history yet" : kind === "bookmarks" ? "No bookmarks yet" : "No downloads yet"
    property int dragRow: -1
    property int dropRow: -1

    function openItem(url, closedId) {
        if (closedId !== 0)
            controller.reopenClosedTab(closedId);
        else
            controller.navigate(url);
        if (closePaneOnOpen)
            controller.openPane = "";
    }

    function formatBytes(value) {
        if (value < 1024)
            return value + " B";

        if (value < 1048576)
            return (value / 1024).toFixed(1) + " KB";

        if (value < 1073741824)
            return (value / 1048576).toFixed(1) + " MB";

        return (value / 1073741824).toFixed(2) + " GB";
    }

    EdenTextField {
        id: searchField

        visible: view.showSearch
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: view.sidePadding
        anchors.rightMargin: view.sidePadding
        height: visible ? implicitHeight : 0
        leftPadding: 44
        placeholderText: view.kind === "history" ? "Search history" : view.kind === "bookmarks" ? "Search bookmarks" : "Search downloads"
        onTextChanged: view.listModel.query = text

        Icon {
            anchors.left: parent.left
            anchors.leftMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            name: "search"
            color: Theme.surfaceVariantText
        }
    }

    ListView {
        id: list

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: searchField.bottom
        anchors.bottom: parent.bottom
        anchors.topMargin: view.showSearch ? 12 : 0
        anchors.leftMargin: view.sidePadding
        anchors.rightMargin: view.sidePadding
        spacing: 2
        clip: true
        cacheBuffer: 400
        reuseItems: false
        boundsBehavior: Flickable.StopAtBounds
        interactive: view.dragRow < 0
        model: view.listModel

        delegate: Item {
            id: row

            required property int index
            required property bool header
            required property string sectionId
            required property string sectionTitle
            required property bool collapsed
            required property string title
            required property url url
            required property url favicon
            required property double closedId
            required property string targetPath
            required property double receivedBytes
            required property double totalBytes
            required property string downloadState
            readonly property bool download: view.kind === "downloads"
            readonly property bool dragging: view.dragRow === index
            readonly property string primaryText: title.length > 0 ? title : url.toString()
            readonly property string secondaryText: {
                if (!download)
                    return url.toString();

                if (downloadState === "complete")
                    return view.formatBytes(receivedBytes) + " · " + url.toString();

                if (downloadState === "in_progress" || downloadState === "downloading")
                    return view.formatBytes(receivedBytes) + (totalBytes > 0 ? " of " + view.formatBytes(totalBytes) : "");

                return downloadState;
            }

            width: ListView.view.width
            height: header ? 36 : 52
            z: dragging ? 3 : 1

            Item {
                visible: row.header
                anchors.fill: parent

                Text {
                    textFormat: Text.PlainText
                    anchors.left: parent.left
                    anchors.right: sectionChevron.left
                    anchors.leftMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.sectionTitle
                    color: Theme.surfaceVariantText
                    font: Theme.labelFont
                    elide: Text.ElideRight
                }

                Icon {
                    id: sectionChevron

                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    name: "chevron-down"
                    iconSize: 16
                    color: Theme.surfaceVariantText
                    rotation: row.collapsed ? -90 : 0

                    Behavior on rotation {
                        NumberAnimation {
                            duration: Theme.shortDuration
                            easing.type: Easing.OutCubic
                        }
                    }
                }

                TapHandler {
                    enabled: row.header
                    onTapped: view.listModel.toggleSection(row.sectionId)
                }
            }

            Rectangle {
                id: card

                visible: !row.header
                width: parent.width
                height: parent.height
                radius: Theme.controlRadius
                color: row.dragging ? Theme.surfaceContainerHighest : rowHover.hovered ? Theme.surfaceContainerHigh : "transparent"

                SiteIcon {
                    id: siteIcon

                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    favicon: row.favicon
                    fallbackIcon: row.download ? "document-text" : "globe"
                }

                Column {
                    anchors.left: siteIcon.right
                    anchors.right: trailing.left
                    anchors.leftMargin: 12
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2

                    Text {
                        textFormat: Text.PlainText
                        width: parent.width
                        text: row.primaryText
                        color: Theme.surfaceText
                        font: Theme.labelFont
                        elide: Text.ElideRight
                    }

                    Item {
                        id: subtitleClip

                        readonly property bool overflowing: subtitle.implicitWidth > width

                        width: parent.width
                        height: subtitle.implicitHeight
                        clip: true
                        layer.enabled: overflowing

                        Text {
                            id: subtitle

                            textFormat: Text.PlainText
                            text: row.secondaryText
                            color: Theme.surfaceVariantText
                            font.family: Theme.bodyFont.family
                            font.pixelSize: 11
                        }

                        layer.effect: MultiEffect {
                            maskEnabled: true
                            maskSource: fadeMask
                            maskThresholdMin: 0.5
                            maskSpreadAtMin: 1
                        }
                    }

                    Rectangle {
                        visible: row.download && row.downloadState !== "complete" && row.totalBytes > 0
                        width: parent.width
                        height: 3
                        radius: 1.5
                        color: Theme.surfaceContainerHighest

                        Rectangle {
                            width: parent.width * Math.min(1, row.totalBytes > 0 ? row.receivedBytes / row.totalBytes : 0)
                            height: parent.height
                            radius: parent.radius
                            color: Theme.primary
                        }
                    }
                }

                Item {
                    id: fadeMask

                    width: subtitleClip.width
                    height: subtitleClip.height
                    visible: false
                    layer.enabled: subtitleClip.overflowing

                    Rectangle {
                        anchors.fill: parent

                        gradient: Gradient {
                            orientation: Gradient.Horizontal

                            GradientStop {
                                position: 0
                                color: "#ffffffff"
                            }

                            GradientStop {
                                position: Math.max(0, 1 - 40 / Math.max(41, fadeMask.width))
                                color: "#ffffffff"
                            }

                            GradientStop {
                                position: 1
                                color: "#00ffffff"
                            }
                        }
                    }
                }

                Row {
                    id: trailing

                    anchors.right: parent.right
                    anchors.rightMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2

                    EdenButton {
                        visible: row.download
                        iconName: "new-window"
                        enabled: row.downloadState === "complete"
                        Accessible.name: "Open download"
                        onClicked: view.controller.downloads.openDownload(row.targetPath)
                    }

                    EdenButton {
                        visible: row.download
                        iconName: "folder-open"
                        Accessible.name: "View in folder"
                        onClicked: view.controller.downloads.showInFolder(row.targetPath)
                    }

                    EdenButton {
                        visible: !row.download && row.closedId === 0 && (rowHover.hovered || hovered) && view.dragRow < 0
                        iconName: "trash"
                        Accessible.name: view.kind === "bookmarks" ? "Delete bookmark" : "Remove from history"
                        onClicked: {
                            if (view.kind === "bookmarks")
                                view.controller.bookmarks.remove(row.url);
                            else
                                view.controller.history.remove(row.url);
                        }
                    }
                }

                HoverHandler {
                    id: rowHover
                }

                TapHandler {
                    enabled: !row.header && !row.download
                    gesturePolicy: TapHandler.DragThreshold
                    onTapped: view.openItem(row.url, row.closedId)
                }

                DragHandler {
                    id: reorderDrag

                    enabled: !row.header && view.listModel.reorderable
                    target: card
                    xAxis.enabled: false
                    yAxis.minimum: -row.y + list.contentY
                    yAxis.maximum: list.contentY + list.height - row.y - row.height
                    onActiveChanged: {
                        if (active) {
                            view.dragRow = row.index;
                            view.dropRow = row.index;
                            return;
                        }
                        const from = view.dragRow;
                        const to = view.dropRow;
                        view.dragRow = -1;
                        view.dropRow = -1;
                        card.y = 0;
                        if (from >= 0 && to >= 0 && from !== to)
                            view.listModel.moveItem(from, to);
                    }
                    onTranslationChanged: {
                        if (!active)
                            return;

                        const stride = row.height + list.spacing;
                        const target = Math.round((row.y + card.y) / stride);
                        view.dropRow = Math.max(0, Math.min(list.count - 1, target));
                    }
                }
            }

            Rectangle {
                readonly property bool above: view.dropRow === row.index && view.dragRow > row.index
                readonly property bool below: view.dropRow === row.index && view.dragRow >= 0 && view.dragRow < row.index

                visible: above || below
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                y: above ? -2 : parent.height - 1
                height: 2
                radius: 1
                color: Theme.primary
            }
        }
    }

    Text {
        textFormat: Text.PlainText
        anchors.centerIn: list
        visible: list.count === 0
        text: view.listModel.query.length > 0 ? "No results" : view.emptyText
        color: Theme.surfaceVariantText
        font: Theme.bodyFont
    }
}
