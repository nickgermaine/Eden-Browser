import Eden.Ui
import QtQuick
import QtQuick.Controls

Item {
    id: page

    property string title
    property string body

    ScrollView {
        id: scroll

        anchors.fill: parent
        contentWidth: availableWidth

        Column {
            width: Math.min(820, Math.max(480, scroll.availableWidth - 64))
            x: Math.round((scroll.availableWidth - width) / 2)
            topPadding: 32
            bottomPadding: 48
            spacing: 20

            Text {
                textFormat: Text.PlainText
                text: page.title
                color: Theme.surfaceText
                font.family: Themes.fontFamily
                font.pixelSize: 30
                font.weight: Font.DemiBold
            }

            Rectangle {
                width: parent.width
                height: bodyColumn.implicitHeight + 32
                radius: Theme.cardRadius
                color: Theme.surfaceContainer

                Column {
                    id: bodyColumn

                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 8

                    Text {
                        textFormat: Text.PlainText
                        width: parent.width
                        text: page.body
                        color: Theme.surfaceVariantText
                        font: Theme.bodyFont
                        wrapMode: Text.Wrap
                    }
                }
            }
        }
    }
}
