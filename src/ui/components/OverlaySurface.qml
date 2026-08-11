import Eden.Ui
import QtQuick
import QtQuick.Effects

Item {
    id: root

    property color surfaceColor: Theme.surfaceContainer
    property real surfaceRadius: Theme.menuRadius

    RectangularShadow {
        anchors.fill: surface
        visible: Theme.overlayShadowBlur > 0 && Theme.overlayShadowColor.a > 0
        offset: Qt.vector2d(0, Theme.overlayShadowVerticalOffset)
        color: Theme.overlayShadowColor
        blur: Theme.overlayShadowBlur
        spread: Theme.overlayShadowSpread
        radius: surface.radius
    }

    Rectangle {
        id: surface

        anchors.fill: parent
        radius: root.surfaceRadius
        color: root.surfaceColor
        border.color: Theme.menuBorder
        border.width: Theme.menuBorderWidth
    }

}
