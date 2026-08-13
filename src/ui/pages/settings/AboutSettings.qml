pragma ComponentBehavior: Bound

import Eden.Ui
import QtQuick
import QtQuick.Controls

Item {
    id: page

    property string view: "about"
    property var currentLicense: null
    property string licenseText: ""
    readonly property var licenses: [
        {
            "name": "Eden Browser",
            "license": "GNU General Public License v3.0",
            "detail": "This application and its source code",
            "file": "eden-gpl-3.0.txt"
        },
        {
            "name": "Qt 6",
            "license": "GNU Lesser General Public License v3.0",
            "detail": "Application framework, QML runtime, and web engine",
            "file": "qt-lgpl-3.0.txt"
        },
        {
            "name": "Chromium",
            "license": "BSD 3-Clause License",
            "detail": "The Blink rendering engine and browser platform by the Chromium Authors",
            "file": "chromium-bsd-3-clause.txt"
        },
        {
            "name": "Chromium Embedded Framework",
            "license": "BSD 3-Clause License",
            "detail": "Embedding framework for the Blink engine by Marshall A. Greenblatt",
            "file": "cef-bsd-3-clause.txt"
        },
        {
            "name": "Solar Icon Set",
            "license": "Creative Commons Attribution 4.0",
            "detail": "Interface icons by 480 Design",
            "file": "solar-cc-by-4.0.txt"
        }
    ]

    function openLicense(entry) {
        page.currentLicense = entry;
        page.licenseText = Settings.licenseText(entry.file);
        page.view = "license";
    }

    ScrollView {
        id: scroll

        anchors.fill: parent
        contentWidth: availableWidth

        Column {
            width: Math.min(680, Math.max(440, scroll.availableWidth - 64))
            x: Math.round((scroll.availableWidth - width) / 2)
            topPadding: 32
            bottomPadding: 48
            spacing: 20

            Column {
                width: parent.width
                visible: page.view === "about"
                spacing: 20

                Column {
                    width: parent.width
                    spacing: 14

                    Image {
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: 96
                        height: 96
                        source: "qrc:/qt/qml/Eden/Ui/resources/eden-logo.png"
                        fillMode: Image.PreserveAspectFit
                        smooth: true
                        mipmap: true
                        sourceSize: Qt.size(192, 192)
                    }

                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "Eden Browser"
                        color: Theme.surfaceText
                        font.family: Themes.fontFamily
                        font.pixelSize: 30
                        font.weight: Font.DemiBold
                    }

                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: versionLabel.implicitWidth + 24
                        height: 28
                        radius: 14
                        color: Theme.primaryContainer

                        Text {
                            id: versionLabel

                            anchors.centerIn: parent
                            text: Qt.application.version.length > 0 ? Qt.application.version : "dev"
                            color: Theme.primaryContainerText
                            font: Theme.labelFont
                        }
                    }

                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "A performance-obsessed web browser."
                        color: Theme.surfaceVariantText
                        font: Theme.bodyFont
                    }
                }

                Rectangle {
                    width: parent.width
                    height: detailsColumn.implicitHeight + 16
                    radius: Theme.cardRadius
                    color: Theme.surfaceContainer

                    Column {
                        id: detailsColumn

                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 8

                        AboutRow {
                            label: "Version"
                            value: Qt.application.version.length > 0 ? Qt.application.version : "dev"
                        }

                        RowDivider {}

                        AboutRow {
                            label: "Web engines"
                            value: Engines.engines.map(engine => {
                                return engine.name;
                            }).join(", ")
                        }

                        RowDivider {}

                        AboutRow {
                            label: "Copyright"
                            value: "© 2026 Nick Germaine"
                        }
                    }
                }

                Rectangle {
                    width: parent.width
                    height: licensesLink.implicitHeight + 16
                    radius: Theme.cardRadius
                    color: Theme.surfaceContainer

                    Column {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 8

                        AboutRow {
                            id: licensesLink

                            label: "Open source licenses"
                            value: ""
                            trailingIcon: "arrow-right"
                            interactive: true
                            onActivated: page.view = "licenses"
                        }
                    }
                }
            }

            Column {
                width: parent.width
                visible: page.view === "licenses"
                spacing: 20

                EdenButton {
                    text: "About"
                    iconName: "arrow-left"
                    onClicked: page.view = "about"
                }

                Text {
                    text: "Open source licenses"
                    color: Theme.surfaceText
                    font.family: Themes.fontFamily
                    font.pixelSize: 30
                    font.weight: Font.DemiBold
                }

                Text {
                    width: parent.width
                    text: "Eden Browser is built with these open source components."
                    color: Theme.surfaceVariantText
                    font: Theme.bodyFont
                    wrapMode: Text.Wrap
                }

                Rectangle {
                    width: parent.width
                    height: licensesColumn.implicitHeight + 16
                    radius: Theme.cardRadius
                    color: Theme.surfaceContainer

                    Column {
                        id: licensesColumn

                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 8

                        Repeater {
                            model: page.licenses

                            delegate: Column {
                                id: licenseEntry

                                required property var modelData
                                required property int index

                                width: licensesColumn.width

                                RowDivider {
                                    visible: licenseEntry.index > 0
                                }

                                AboutRow {
                                    label: licenseEntry.modelData.name
                                    value: licenseEntry.modelData.license
                                    detail: licenseEntry.modelData.detail
                                    trailingIcon: "arrow-right"
                                    interactive: true
                                    onActivated: page.openLicense(licenseEntry.modelData)
                                }
                            }
                        }
                    }
                }
            }

            Column {
                width: parent.width
                visible: page.view === "license"
                spacing: 20

                EdenButton {
                    text: "Licenses"
                    iconName: "arrow-left"
                    onClicked: page.view = "licenses"
                }

                Column {
                    width: parent.width
                    spacing: 4

                    Text {
                        text: page.currentLicense ? page.currentLicense.name : ""
                        color: Theme.surfaceText
                        font.family: Themes.fontFamily
                        font.pixelSize: 30
                        font.weight: Font.DemiBold
                    }

                    Text {
                        width: parent.width
                        text: page.currentLicense ? page.currentLicense.license : ""
                        color: Theme.surfaceVariantText
                        font: Theme.bodyFont
                        wrapMode: Text.Wrap
                    }
                }

                Rectangle {
                    width: parent.width
                    height: licenseTextItem.implicitHeight + 32
                    radius: Theme.cardRadius
                    color: Theme.surfaceContainer

                    TextEdit {
                        id: licenseTextItem

                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 16
                        text: page.licenseText
                        readOnly: true
                        selectByMouse: true
                        wrapMode: TextEdit.Wrap
                        color: Theme.surfaceVariantText
                        selectionColor: Theme.primary
                        selectedTextColor: Theme.primaryText
                        font.family: Themes.fontFamily
                        font.pixelSize: 12
                    }
                }
            }
        }
    }

    component AboutRow: Item {
        id: aboutRow

        property string label
        property string value
        property string detail: ""
        property string trailingIcon: ""
        property bool interactive: false

        signal activated

        width: parent ? parent.width : 0
        implicitHeight: detail.length > 0 ? 58 : 44

        Rectangle {
            anchors.fill: parent
            radius: Theme.controlRadius
            color: Theme.surfaceContainerHigh
            opacity: aboutRow.interactive && (rowHover.hovered || rowTap.pressed) ? 1 : 0

            Behavior on opacity {
                NumberAnimation {
                    duration: Theme.shortDuration
                    easing.type: Easing.OutCubic
                }
            }
        }

        Row {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 10

            Column {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - (rowValue.visible ? rowValue.width + parent.spacing : 0) - (rowIcon.visible ? rowIcon.width + parent.spacing : 0)
                spacing: 2

                Text {
                    width: parent.width
                    text: aboutRow.label
                    color: Theme.surfaceText
                    font: Theme.labelFont
                    elide: Text.ElideRight
                }

                Text {
                    width: parent.width
                    visible: aboutRow.detail.length > 0
                    text: aboutRow.detail
                    color: Theme.surfaceVariantText
                    font.family: Themes.fontFamily
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }
            }

            Text {
                id: rowValue

                anchors.verticalCenter: parent.verticalCenter
                visible: aboutRow.value.length > 0
                text: aboutRow.value
                color: Theme.surfaceVariantText
                font: Theme.bodyFont
            }

            Icon {
                id: rowIcon

                anchors.verticalCenter: parent.verticalCenter
                visible: aboutRow.trailingIcon.length > 0
                name: aboutRow.trailingIcon
            }
        }

        HoverHandler {
            id: rowHover

            enabled: aboutRow.interactive
            cursorShape: Qt.PointingHandCursor
        }

        TapHandler {
            id: rowTap

            enabled: aboutRow.interactive
            onTapped: aboutRow.activated()
        }
    }

    component RowDivider: Rectangle {
        width: parent ? parent.width - 24 : 0
        anchors.horizontalCenter: parent ? parent.horizontalCenter : undefined
        height: 1
        color: Qt.alpha(Theme.outline, 0.25)
    }
}
