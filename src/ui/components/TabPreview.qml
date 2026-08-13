import Eden.Ui
import QtQuick
import QtQuick.Controls

Popup {
    id: preview

    required property var controller
    required property int tabIndex
    required property Item anchorItem
    property bool anchorHovered: false
    property bool vertical: false
    property var previewData: ({
    })
    readonly property bool pointerInside: anchorHovered || overlayHover.hovered

    function reposition() {
        if (!anchorItem || !parent)
            return ;

        const target = anchorItem.mapToItem(parent, vertical ? anchorItem.width + 8 : 0, vertical ? 0 : anchorItem.height + 8);
        x = Math.max(8, Math.min(parent.width - width - 8, target.x));
        y = Math.max(8, Math.min(parent.height - height - 8, target.y));
    }

    parent: Overlay.overlay
    width: 440
    height: previewColumn.implicitHeight + 24
    padding: 12
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onAboutToShow: {
        previewData = controller.tabPreview(tabIndex);
        reposition();
    }
    onOpened: reposition()
    onHeightChanged: {
        if (opened)
            reposition();

    }
    onClosed: previewData = ({
    })
    onPointerInsideChanged: {
        if (pointerInside)
            closeTimer.stop();
        else
            closeTimer.start();
    }

    Timer {
        id: closeTimer

        interval: 140
        onTriggered: {
            if (!preview.pointerInside)
                preview.close();

        }
    }

    Timer {
        interval: 500
        running: preview.anchorHovered && !preview.opened
        onTriggered: preview.open()
    }

    Connections {
        function onTabPreviewRevisionChanged() {
            if (preview.opened)
                preview.previewData = preview.controller.tabPreview(preview.tabIndex);

        }

        target: preview.controller
    }

    background: OverlaySurface {
        surfaceRadius: Theme.cardRadius
    }

    contentItem: Column {
        id: previewColumn

        spacing: 10

        HoverHandler {
            id: overlayHover
        }

        Rectangle {
            width: parent.width
            height: 220
            radius: Theme.cardRadius
            color: Theme.surfaceContainerHigh
            clip: true

            Image {
                anchors.fill: parent
                source: preview.previewData.thumbnail || preview.previewData.favicon || ""
                fillMode: preview.previewData.thumbnail ? Image.PreserveAspectCrop : Image.Pad
                asynchronous: true
                cache: false
            }

            Text {
                anchors.centerIn: parent
                visible: !preview.previewData.thumbnail && !preview.previewData.favicon
                text: "No preview captured"
                color: Theme.surfaceVariantText
                font: Theme.bodyFont
            }

        }

        Row {
            width: parent.width
            spacing: 8

            Text {
                width: parent.width - engineChip.width - parent.spacing
                text: preview.previewData.title || preview.previewData.url || "New tab"
                color: Theme.surfaceText
                font: Theme.titleFont
                elide: Text.ElideRight
            }

            Rectangle {
                id: engineChip

                width: engineLabel.implicitWidth + 16
                height: 28
                radius: height / 2
                color: Theme.surfaceContainerHighest

                Text {
                    id: engineLabel

                    anchors.centerIn: parent
                    text: preview.previewData.engine || ""
                    color: Theme.surfaceText
                    font: Theme.labelFont
                }

            }

        }

        Text {
            width: parent.width
            text: preview.previewData.url || ""
            color: Theme.surfaceVariantText
            font: Theme.bodyFont
            elide: Text.ElideMiddle
        }

        Rectangle {
            width: parent.width
            height: memoryColumn.implicitHeight + 20
            radius: Theme.cardRadius
            color: Theme.surfaceContainerHigh

            Column {
                id: memoryColumn

                anchors.fill: parent
                anchors.margins: 10
                spacing: 5

                Text {
                    text: "Tab renderer  " + (preview.previewData.rendererMemory || "Unavailable")
                    color: Theme.surfaceText
                    font: Theme.labelFont
                }

                Text {
                    text: preview.previewData.rendererAttribution || "Renderer mapping unavailable"
                    color: Theme.surfaceVariantText
                    font: Theme.bodyFont
                }

                Text {
                    text: "Shared processes  Browser " + (preview.previewData.sharedBrowserMemory || "Unavailable") + "   GPU " + (preview.previewData.sharedGpuMemory || "Unavailable") + "   Network " + (preview.previewData.sharedNetworkMemory || "Unavailable")
                    color: Theme.surfaceVariantText
                    font.family: Theme.bodyFont.family
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }

            }

        }

    }

}
