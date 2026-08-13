import Eden.Ui
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs

Rectangle {
    id: editorPage

    required property var controller
    readonly property var emptyGradient: ({
        "mode": "solid",
        "angle": 90,
        "startColor": "transparent",
        "middleColor": "transparent",
        "endColor": "transparent",
        "middlePosition": 0.5
    })

    color: Theme.surface
    Component.onCompleted: Themes.beginEditing(Themes.activeThemeId)
    Component.onDestruction: Themes.cancelEditing()

    FileDialog {
        id: exportDialog

        title: "Export Eden theme"
        nameFilters: ["Eden themes (*.json)"]
        fileMode: FileDialog.SaveFile
        defaultSuffix: "json"
        onAccepted: Themes.exportEditing(selectedFile)
    }

    Rectangle {
        id: sidebar

        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.margins: 10
        width: 232
        radius: Theme.contentRadius
        color: Theme.surfaceContainerLow

        Column {
            id: sidebarHeader

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 18
            spacing: 8

            Text {
                text: "Theme Designer"
                color: Theme.surfaceText
                font.family: Themes.fontFamily
                font.pixelSize: 22
                font.weight: Font.DemiBold
            }

            Text {
                width: parent.width
                text: "Shared values and both appearance modes live together. Every valid edit previews immediately."
                color: Theme.surfaceVariantText
                font: Theme.bodyFont
                wrapMode: Text.Wrap
            }

        }

        ListView {
            id: navigationList

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: sidebarHeader.bottom
            anchors.bottom: sidebarFooter.top
            anchors.topMargin: 14
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 3
            clip: true
            model: Themes.editorTokens.navigationSections

            delegate: Rectangle {
                id: navigationItem

                required property var modelData

                width: navigationList.width
                height: 40
                radius: Theme.cardRadius
                color: tokenList.visibleSection === modelData.title ? Theme.surfaceContainerHighest : navigationHover.hovered ? Theme.surfaceContainerHigh : "transparent"

                Text {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: 14
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: navigationItem.modelData.title
                    color: Theme.surfaceText
                    font: Theme.labelFont
                    elide: Text.ElideRight
                }

                HoverHandler {
                    id: navigationHover
                }

                TapHandler {
                    onTapped: {
                        Themes.editorTokens.selectedSection = navigationItem.modelData.title;
                        const targetIndex = Themes.editorTokens.firstIndexForSection(navigationItem.modelData.title);
                        if (targetIndex < 0)
                            tokenList.positionViewAtBeginning();
                        else
                            tokenList.positionViewAtIndex(targetIndex, ListView.Beginning);
                    }
                }

            }

        }

        Column {
            id: sidebarFooter

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 18
            spacing: 5

            Text {
                text: Themes.editorDirty ? "Unsaved changes" : "All changes saved"
                color: Themes.editorDirty ? Theme.primary : Theme.surfaceVariantText
                font: Theme.labelFont
            }

            Text {
                width: parent.width
                text: "Alpha is supported by every color picker."
                color: Theme.surfaceVariantText
                font.pixelSize: 12
                font.family: Themes.fontFamily
                wrapMode: Text.Wrap
            }

        }

    }

    Item {
        anchors.left: sidebar.right
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.leftMargin: 10

        Rectangle {
            id: actionBar

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 80
            color: "transparent"

            Column {
                anchors.left: parent.left
                anchors.leftMargin: 24
                anchors.verticalCenter: parent.verticalCenter
                width: Math.max(160, parent.width - actionButtons.width - 72)
                spacing: 2

                Text {
                    text: tokenList.visibleSection
                    color: Theme.surfaceText
                    font.family: Themes.fontFamily
                    font.pixelSize: 25
                    font.weight: Font.DemiBold
                }

                Text {
                    width: parent.width
                    text: Themes.editorName.length > 0 ? Themes.editorName : "Untitled theme"
                    color: Theme.surfaceVariantText
                    font: Theme.bodyFont
                    elide: Text.ElideRight
                }

            }

            Row {
                id: actionButtons

                anchors.right: parent.right
                anchors.rightMargin: 20
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4

                EdenButton {
                    text: "Revert"
                    enabled: Themes.editorDirty
                    onClicked: Themes.revertEditing()
                }

                EdenButton {
                    text: "Save"
                    enabled: Themes.editorDirty
                    onClicked: Themes.saveEditing()
                }

                EdenButton {
                    text: "Save as new"
                    onClicked: Themes.saveEditingAs()
                }

                EdenButton {
                    text: "Export"
                    onClicked: exportDialog.open()
                }

            }

        }

        ListView {
            id: tokenList

            readonly property string visibleSection: !headerItem || contentY < headerItem.height || currentSection.length === 0 ? "Overview" : currentSection

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: actionBar.bottom
            anchors.bottom: parent.bottom
            clip: true
            model: Themes.editorTokens
            section.property: "tokenSection"
            section.criteria: ViewSection.FullString
            section.labelPositioning: ViewSection.InlineLabels

            header: Item {
                width: tokenList.width
                height: overviewContent.implicitHeight + 48

                Column {
                    id: overviewContent

                    x: 24
                    y: 24
                    width: parent.width - 48
                    spacing: 14

                    Text {
                        text: "Overview"
                        color: Theme.surfaceText
                        font.family: Themes.fontFamily
                        font.pixelSize: 24
                        font.weight: Font.DemiBold
                    }

                    Text {
                        width: parent.width
                        text: Themes.editorTokens.sectionDescription("Overview")
                        color: Theme.surfaceVariantText
                        font: Theme.bodyFont
                        wrapMode: Text.Wrap
                    }

                    Rectangle {
                        width: parent.width
                        height: identityRow.implicitHeight + 28
                        radius: Theme.cardRadius
                        color: Theme.surfaceContainer

                        Row {
                            id: identityRow

                            anchors.fill: parent
                            anchors.margins: 14
                            spacing: 12

                            IdentityField {
                                width: (parent.width - 24) / 3
                                label: "Theme ID"
                                detail: "Lowercase sharing identifier."
                                value: Themes.editorId
                                onEdited: (value) => {
                                    return Themes.editorId = value;
                                }
                            }

                            IdentityField {
                                width: (parent.width - 24) / 3
                                label: "Display name"
                                detail: "Name shown in Settings."
                                value: Themes.editorName
                                onEdited: (value) => {
                                    return Themes.editorName = value;
                                }
                            }

                            IdentityField {
                                width: (parent.width - 24) / 3
                                label: "Author"
                                detail: "Creator included in exports."
                                value: Themes.editorAuthor
                                onEdited: (value) => {
                                    return Themes.editorAuthor = value;
                                }
                            }

                        }

                    }

                    Text {
                        width: parent.width
                        visible: Themes.lastError.length > 0
                        text: Themes.lastError
                        color: Theme.error
                        font: Theme.bodyFont
                        wrapMode: Text.Wrap
                    }

                }

            }

            section.delegate: Item {
                required property string section

                width: tokenList.width
                height: sectionHeading.implicitHeight + 34

                Column {
                    id: sectionHeading

                    x: 24
                    y: 20
                    width: parent.width - 48
                    spacing: 4

                    Text {
                        text: sectionHeading.parent.section
                        color: Theme.surfaceText
                        font.family: Themes.fontFamily
                        font.pixelSize: 23
                        font.weight: Font.DemiBold
                    }

                    Text {
                        width: parent.width
                        text: Themes.editorTokens.sectionDescription(sectionHeading.parent.section)
                        color: Theme.surfaceVariantText
                        font: Theme.bodyFont
                        wrapMode: Text.Wrap
                    }

                }

            }

            delegate: Item {
                id: tokenEditor

                required property string tokenPath
                required property string tokenLabel
                required property string tokenDescription
                required property string tokenSection
                required property string tokenKind
                required property var tokenValue
                required property real tokenMinimum
                required property real tokenMaximum
                required property string tokenUnit
                required property var tokenOptions
                required property bool tokenPaired
                required property string tokenLightPath
                required property string tokenDarkPath
                required property var tokenLightValue
                required property var tokenDarkValue

                width: tokenList.width
                height: tokenCard.implicitHeight + 10

                Rectangle {
                    id: tokenCard

                    x: 24
                    width: parent.width - 48
                    implicitHeight: Math.max(86, tokenRow.implicitHeight + 26)
                    height: implicitHeight
                    radius: Theme.cardRadius
                    color: Theme.surfaceContainer

                    Row {
                        id: tokenRow

                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.margins: 13
                        spacing: 18

                        Column {
                            width: Math.max(180, parent.width * 0.32)
                            spacing: 4

                            Text {
                                text: tokenEditor.tokenLabel
                                color: Theme.surfaceText
                                font: Theme.labelFont
                            }

                            Text {
                                width: parent.width
                                text: tokenEditor.tokenDescription
                                color: Theme.surfaceVariantText
                                font: Theme.bodyFont
                                wrapMode: Text.Wrap
                            }

                        }

                        Row {
                            width: parent.width - parent.children[0].width - parent.spacing
                            spacing: 12

                            TokenControl {
                                visible: !tokenEditor.tokenPaired
                                width: visible ? Math.min(460, parent.width) : 0
                                path: tokenEditor.tokenPath
                                appearanceLabel: "SHARED"
                                kind: tokenEditor.tokenKind
                                value: tokenEditor.tokenPaired ? (tokenEditor.tokenKind === "gradient" ? editorPage.emptyGradient : "") : tokenEditor.tokenValue
                                minimum: tokenEditor.tokenMinimum
                                maximum: tokenEditor.tokenMaximum
                                unit: tokenEditor.tokenUnit
                                options: tokenEditor.tokenOptions
                            }

                            TokenControl {
                                visible: tokenEditor.tokenPaired
                                width: visible ? (parent.width - parent.spacing) / 2 : 0
                                path: tokenEditor.tokenLightPath
                                appearanceLabel: "LIGHT"
                                kind: tokenEditor.tokenKind
                                value: tokenEditor.tokenPaired ? tokenEditor.tokenLightValue : (tokenEditor.tokenKind === "gradient" ? editorPage.emptyGradient : "")
                                minimum: tokenEditor.tokenMinimum
                                maximum: tokenEditor.tokenMaximum
                                unit: tokenEditor.tokenUnit
                                options: tokenEditor.tokenOptions
                            }

                            TokenControl {
                                visible: tokenEditor.tokenPaired
                                width: visible ? (parent.width - parent.spacing) / 2 : 0
                                path: tokenEditor.tokenDarkPath
                                appearanceLabel: "DARK"
                                kind: tokenEditor.tokenKind
                                value: tokenEditor.tokenPaired ? tokenEditor.tokenDarkValue : (tokenEditor.tokenKind === "gradient" ? editorPage.emptyGradient : "")
                                minimum: tokenEditor.tokenMinimum
                                maximum: tokenEditor.tokenMaximum
                                unit: tokenEditor.tokenUnit
                                options: tokenEditor.tokenOptions
                            }

                        }

                    }

                }

            }

        }

    }

    component IdentityField: Column {
        required property string label
        required property string detail
        required property string value

        signal edited(string value)

        spacing: 5

        Text {
            text: parent.label
            color: Theme.surfaceText
            font: Theme.labelFont
        }

        Text {
            width: parent.width
            text: parent.detail
            color: Theme.surfaceVariantText
            font.pixelSize: 11
            font.family: Themes.fontFamily
            wrapMode: Text.Wrap
        }

        EdenTextField {
            width: parent.width
            text: parent.value
            onTextEdited: parent.edited(text)
        }

    }

    component TokenControl: Column {
        id: control

        required property string path
        required property string appearanceLabel
        required property string kind
        required property var value
        required property real minimum
        required property real maximum
        required property string unit
        required property var options
        readonly property var gradientValue: kind === "gradient" ? value : editorPage.emptyGradient

        spacing: 6

        Text {
            text: control.appearanceLabel
            color: Theme.primary
            font.pixelSize: 11
            font.family: Themes.fontFamily
            font.weight: Font.DemiBold
        }

        ColorDialog {
            id: colorDialog

            title: "Choose color"
            selectedColor: control.kind === "color" ? control.value : "transparent"
            options: ColorDialog.ShowAlphaChannel
            onAccepted: Themes.setEditorColor(control.path, selectedColor)
        }

        ColorDialog {
            id: gradientStartDialog

            title: "Choose gradient start"
            selectedColor: control.gradientValue.startColor
            options: ColorDialog.ShowAlphaChannel
            onAccepted: Themes.setEditorGradientValue(control.path, "startColor", selectedColor)
        }

        ColorDialog {
            id: gradientMiddleDialog

            title: "Choose gradient middle"
            selectedColor: control.gradientValue.middleColor
            options: ColorDialog.ShowAlphaChannel
            onAccepted: Themes.setEditorGradientValue(control.path, "middleColor", selectedColor)
        }

        ColorDialog {
            id: gradientEndDialog

            title: "Choose gradient end"
            selectedColor: control.gradientValue.endColor
            options: ColorDialog.ShowAlphaChannel
            onAccepted: Themes.setEditorGradientValue(control.path, "endColor", selectedColor)
        }

        Row {
            visible: control.kind === "color"
            width: parent.width
            spacing: 8

            ColorSwatch {
                colorValue: control.value
                onActivated: colorDialog.open()
            }

            EdenTextField {
                width: parent.width - 52
                text: control.value
                onEditingFinished: Themes.setEditorToken(control.path, text)
            }

        }

        Row {
            visible: control.kind === "integer" || control.kind === "real"
            width: parent.width
            spacing: 7

            EdenSlider {
                width: Math.max(80, parent.width - numericValue.width - numericUnit.width - 14)
                anchors.verticalCenter: parent.verticalCenter
                from: control.minimum
                to: control.maximum
                stepSize: control.kind === "integer" ? 1 : 0.01
                value: Number(control.value)
                snapMode: Slider.SnapAlways
                onMoved: Themes.setEditorToken(control.path, control.kind === "integer" ? Math.round(value) : value.toFixed(2))
            }

            EdenTextField {
                id: numericValue

                width: 66
                text: control.value
                inputMethodHints: Qt.ImhFormattedNumbersOnly
                onEditingFinished: Themes.setEditorToken(control.path, text)
            }

            Text {
                id: numericUnit

                anchors.verticalCenter: parent.verticalCenter
                width: control.unit.length > 0 ? 24 : 0
                visible: width > 0
                text: control.unit
                color: Theme.surfaceVariantText
                font: Theme.bodyFont
            }

        }

        EdenTextField {
            visible: control.kind === "text"
            width: parent.width
            text: control.value
            onEditingFinished: Themes.setEditorToken(control.path, text)
        }

        Flow {
            visible: control.kind === "choice"
            width: parent.width
            spacing: 4

            Repeater {
                model: control.options

                delegate: EdenButton {
                    required property string modelData

                    text: modelData
                    iconName: control.value === modelData ? "check" : ""
                    onClicked: Themes.setEditorToken(control.path, modelData)
                }

            }

        }

        Column {
            visible: control.kind === "gradient"
            width: parent.width
            spacing: 7

            Row {
                width: parent.width
                spacing: 4

                EdenButton {
                    width: (parent.width - parent.spacing) / 2
                    text: "Solid"
                    iconName: control.gradientValue.mode === "solid" ? "check" : ""
                    onClicked: Themes.setEditorGradientValue(control.path, "mode", "solid")
                }

                EdenButton {
                    width: (parent.width - parent.spacing) / 2
                    text: "Linear"
                    iconName: control.gradientValue.mode === "linear" ? "check" : ""
                    onClicked: Themes.setEditorGradientValue(control.path, "mode", "linear")
                }

            }

            Row {
                width: parent.width
                spacing: 6

                ColorSwatch {
                    width: control.gradientValue.mode === "linear" ? (parent.width - 12) / 3 : 44
                    colorValue: control.gradientValue.startColor
                    onActivated: gradientStartDialog.open()
                }

                ColorSwatch {
                    visible: control.gradientValue.mode === "linear"
                    width: visible ? (parent.width - 12) / 3 : 0
                    colorValue: control.gradientValue.middleColor
                    onActivated: gradientMiddleDialog.open()
                }

                ColorSwatch {
                    visible: control.gradientValue.mode === "linear"
                    width: visible ? (parent.width - 12) / 3 : 0
                    colorValue: control.gradientValue.endColor
                    onActivated: gradientEndDialog.open()
                }

            }

            Row {
                visible: control.gradientValue.mode === "linear"
                width: parent.width
                spacing: 7

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 44
                    text: "Angle"
                    color: Theme.surfaceVariantText
                    font.pixelSize: 11
                    font.family: Themes.fontFamily
                }

                EdenSlider {
                    width: parent.width - 102
                    from: 0
                    to: 360
                    stepSize: 1
                    value: Number(control.gradientValue.angle)
                    onMoved: Themes.setEditorGradientValue(control.path, "angle", Math.round(value))
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 44
                    text: Math.round(Number(control.gradientValue.angle)) + "°"
                    color: Theme.surfaceText
                    font: Theme.bodyFont
                }

            }

            Row {
                visible: control.gradientValue.mode === "linear"
                width: parent.width
                spacing: 7

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 44
                    text: "Middle"
                    color: Theme.surfaceVariantText
                    font.pixelSize: 11
                    font.family: Themes.fontFamily
                }

                EdenSlider {
                    width: parent.width - 102
                    from: 0
                    to: 1
                    stepSize: 0.01
                    value: Number(control.gradientValue.middlePosition)
                    onMoved: Themes.setEditorGradientValue(control.path, "middlePosition", value.toFixed(2))
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 44
                    text: Math.round(Number(control.gradientValue.middlePosition) * 100) + "%"
                    color: Theme.surfaceText
                    font: Theme.bodyFont
                }

            }

        }

    }

    component ColorSwatch: Rectangle {
        required property color colorValue

        signal activated()

        width: 44
        height: 42
        radius: Theme.cardRadius
        color: Theme.surfaceContainerHighest
        border.width: swatchHover.hovered ? 2 : 1
        border.color: swatchHover.hovered ? Theme.primary : Theme.outline

        Rectangle {
            anchors.fill: parent
            anchors.margins: 4
            radius: Math.max(0, parent.radius - 4)
            color: parent.colorValue
        }

        HoverHandler {
            id: swatchHover

            cursorShape: Qt.PointingHandCursor
        }

        TapHandler {
            onTapped: parent.activated()
        }

    }

}
