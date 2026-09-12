import Eden.Ui
import QtQuick
import QtQuick.Controls

Row {
    id: root

    required property var indicators
    property int iconSize: 16
    property bool compact: false
    spacing: 4
    visible: indicators.length > 0

    Repeater {
        model: root.compact && root.indicators.length > 0 ? [root.indicators[0]] : root.indicators

        delegate: Item {
            required property var modelData
            width: root.iconSize
            height: root.iconSize
            Accessible.role: Accessible.Indicator
            Accessible.name: root.compact ? modelData.summary : modelData.description
            ToolTip.visible: pointer.hovered
            ToolTip.text: root.compact ? modelData.summary : modelData.description
            ToolTip.delay: 300

            Icon {
                anchors.fill: parent
                name: modelData.icon
                color: modelData.capture ? Theme.error : Theme.iconColor
                filled: modelData.capture
            }

            HoverHandler {
                id: pointer
            }
        }
    }
}
