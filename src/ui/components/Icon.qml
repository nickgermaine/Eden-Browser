import Eden.Ui
import QtQuick
import QtQuick.Effects

Item {
    id: icon

    property string name
    property string family: "solar"
    property bool filled: false
    property bool spinning: false
    property color color: Theme.iconColor
    property real iconSize: Theme.iconSize

    implicitWidth: iconSize
    implicitHeight: iconSize
    opacity: color.a
    onSpinningChanged: {
        if (!spinning)
            rotation = 0;

    }

    Glyph {
        source: icon.name.length > 0 ? (icon.family === "solar" ? "qrc:/qt/qml/Eden/Ui/resources/icons/solar/" + icon.name + "-" + Theme.iconStyle + ".svg" : "qrc:/qt/qml/Eden/Ui/resources/icons/" + icon.family + "/" + icon.name + ".svg") : ""
        shown: icon.filled ? 0 : 1
    }

    Glyph {
        source: icon.name.length > 0 ? (icon.family === "solar" ? "qrc:/qt/qml/Eden/Ui/resources/icons/solar/" + icon.name + "-" + Theme.activeIconStyle + ".svg" : "qrc:/qt/qml/Eden/Ui/resources/icons/" + icon.family + "/" + icon.name + ".svg") : ""
        shown: icon.filled ? 1 : 0
    }

    component Glyph: Item {
        property alias source: image.source
        property real shown: 0

        anchors.fill: parent
        opacity: shown
        visible: opacity > 0

        Image {
            id: image

            anchors.fill: parent
            fillMode: Image.PreserveAspectFit
            sourceSize: Qt.size(Math.round(icon.iconSize * Screen.devicePixelRatio), Math.round(icon.iconSize * Screen.devicePixelRatio))
            visible: false
        }

        MultiEffect {
            anchors.fill: parent
            source: image
            brightness: 1
            colorization: 1
            colorizationColor: icon.color
        }

        Behavior on shown {
            NumberAnimation {
                duration: 150
                easing.type: Easing.OutCubic
            }

        }

    }

    RotationAnimation on rotation {
        running: icon.spinning
        from: 0
        to: 360
        duration: 900
        loops: Animation.Infinite
    }

}
