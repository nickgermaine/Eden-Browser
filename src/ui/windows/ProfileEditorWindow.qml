import Eden.Ui
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Effects

Window {
    id: root

    required property ProfileEditorController editor
    readonly property bool nameDirty: nameField.text !== editor.displayName
    readonly property bool hasUnsavedChanges: nameDirty || editor.avatarPending
    property bool closeConfirmed: false

    width: 480
    height: 660
    minimumWidth: 420
    minimumHeight: 560
    visible: true
    flags: Qt.Dialog | Qt.FramelessWindowHint
    modality: Qt.WindowModal
    color: "transparent"
    title: "Customize profile"
    onClosing: close => {
        if (root.hasUnsavedChanges && !root.closeConfirmed) {
            close.accepted = false;
            discardDialog.open();
        }
    }

    Connections {
        function onCloseRequested() {
            root.closeConfirmed = true;
            root.close();
        }

        function onPasswordChangeSucceeded() {
            currentPasswordField.text = "";
            newPasswordField.text = "";
            confirmPasswordField.text = "";
        }

        target: root.editor
    }

    WindowInputRegion {
        window: root
        rect: Qt.rect(shell.x, shell.y, shell.width, shell.height)
        radius: shell.radius
    }

    RectangularShadow {
        anchors.fill: shell
        visible: Theme.windowShadowExtent > 0
        offset: Qt.vector2d(Theme.windowShadowHorizontalOffset, Theme.windowShadowVerticalOffset)
        color: Theme.windowShadowColor
        blur: Theme.windowShadowBlur
        spread: Theme.windowShadowSpread
        radius: shell.radius
    }

    Rectangle {
        id: shell

        anchors.fill: parent
        anchors.margins: Theme.windowShadowExtent
        radius: Theme.windowRadius
        color: Theme.surface
        border.width: Theme.windowBorderWidth
        border.color: Theme.windowBorder
        clip: true

        Item {
            id: chrome

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 48

            Text {
                textFormat: Text.PlainText
                anchors.left: parent.left
                anchors.leftMargin: 20
                anchors.verticalCenter: parent.verticalCenter
                text: "Customize profile"
                color: Theme.surfaceText
                font: Theme.titleFont
            }

            DragHandler {
                target: null
                enabled: false
                dragThreshold: 4
                onActiveChanged: {
                    if (active) {
                        root.startSystemMove();
                    }
                }
            }

            EdenButton {
                anchors.right: parent.right
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                iconFamily: "material"
                iconName: "close"
                Accessible.name: "Close"
                onClicked: root.close()
            }
        }

        Flickable {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: chrome.bottom
            anchors.bottom: parent.bottom
            anchors.margins: 24
            anchors.topMargin: 8
            contentHeight: editorColumn.height
            clip: true

            Column {
                id: editorColumn

                width: parent.width
                spacing: 18

                Item {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 96
                    height: 96

                    ProfileAvatar {
                        anchors.fill: parent
                        avatarUrl: root.editor.avatarUrl
                        displayName: root.editor.displayName
                        avatarSize: 96
                    }

                    Icon {
                        anchors.centerIn: parent
                        visible: root.editor.avatarPending
                        spinning: root.editor.avatarPending
                        name: "refresh"
                    }
                }

                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 10

                    EdenButton {
                        text: "Choose image"
                        iconName: "gallery-edit"
                        enabled: !root.editor.busy
                        onClicked: avatarDialog.open()
                    }

                    EdenButton {
                        text: "Remove image"
                        iconName: "trash"
                        enabled: !root.editor.busy
                        onClicked: root.editor.removeAvatar()
                    }
                }

                Item {
                    width: parent.width
                    height: 44

                    EdenButton {
                        anchors.fill: parent
                        text: "Color"
                        enabled: !root.editor.busy
                        onClicked: root.editor.cycleColor()
                    }

                    Rectangle {
                        anchors.right: parent.right
                        anchors.rightMargin: 16
                        anchors.verticalCenter: parent.verticalCenter
                        width: 18
                        height: 18
                        radius: 9
                        color: root.editor.color
                        border.width: 1
                        border.color: Theme.outline
                    }
                }

                Column {
                    width: parent.width
                    spacing: 8

                    Text {
                        textFormat: Text.PlainText
                        text: "Name"
                        color: Theme.surfaceVariantText
                        font: Theme.labelFont
                    }

                    Item {
                        width: parent.width
                        height: 44

                        EdenTextField {
                            id: nameField

                            anchors.left: parent.left
                            anchors.right: saveNameButton.left
                            anchors.rightMargin: 10
                            Component.onCompleted: text = root.editor.displayName
                            Accessible.name: "Profile name"
                            onAccepted: saveNameButton.clicked()
                        }

                        EdenButton {
                            id: saveNameButton

                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            text: "Save"
                            enabled: root.nameDirty && !root.editor.busy
                            onClicked: root.editor.rename(nameField.text)
                        }
                    }
                }

                Column {
                    width: parent.width
                    spacing: 8

                    Text {
                        textFormat: Text.PlainText
                        text: root.editor.protectedProfile ? "Password" : "Password protection"
                        color: Theme.surfaceVariantText
                        font: Theme.labelFont
                    }

                    Text {
                        textFormat: Text.PlainText
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: "Require a password to open this profile in Eden. This locks the profile inside Eden and does not encrypt its files."
                        color: Theme.surfaceVariantText
                        font.family: Theme.bodyFont.family
                        font.pixelSize: 12
                    }

                    EdenTextField {
                        id: currentPasswordField

                        width: parent.width
                        visible: root.editor.protectedProfile
                        echoMode: TextInput.Password
                        inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhHiddenText
                        placeholderText: "Current password"
                        Accessible.name: "Current password"
                    }

                    EdenTextField {
                        id: newPasswordField

                        width: parent.width
                        echoMode: TextInput.Password
                        inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhHiddenText
                        placeholderText: root.editor.protectedProfile ? "New password" : "Password"
                        Accessible.name: "New password"
                    }

                    EdenTextField {
                        id: confirmPasswordField

                        width: parent.width
                        echoMode: TextInput.Password
                        inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhHiddenText
                        placeholderText: "Confirm password"
                        Accessible.name: "Confirm password"
                    }

                    Row {
                        spacing: 10

                        EdenButton {
                            text: root.editor.protectedProfile ? "Change password" : "Set password"
                            enabled: !root.editor.busy && newPasswordField.text.length > 0
                            onClicked: {
                                const current = currentPasswordField.text;
                                const next = newPasswordField.text;
                                const confirmation = confirmPasswordField.text;
                                root.editor.setPassword(current, next, confirmation);
                            }
                        }

                        EdenButton {
                            visible: root.editor.protectedProfile
                            text: "Remove password"
                            enabled: !root.editor.busy && currentPasswordField.text.length > 0
                            onClicked: root.editor.removePassword(currentPasswordField.text)
                        }
                    }
                }

                Text {
                    textFormat: Text.PlainText
                    width: parent.width
                    wrapMode: Text.WordWrap
                    visible: root.editor.errorMessage.length > 0
                    text: root.editor.errorMessage
                    color: Theme.error
                    font: Theme.labelFont
                    Accessible.role: Accessible.AlertMessage
                    Accessible.name: text
                }

                Rectangle {
                    width: parent.width
                    height: 1
                    color: Theme.outline
                    opacity: 0.4
                }

                Column {
                    width: parent.width
                    spacing: 8

                    Text {
                        textFormat: Text.PlainText
                        text: "Delete profile"
                        color: Theme.error
                        font: Theme.labelFont
                    }

                    Text {
                        textFormat: Text.PlainText
                        width: parent.width
                        wrapMode: Text.WordWrap
                        visible: !root.editor.canDelete
                        text: "Create another profile before deleting this one"
                        color: Theme.surfaceVariantText
                        font.family: Theme.bodyFont.family
                        font.pixelSize: 12
                    }

                    EdenButton {
                        text: "Delete profile"
                        iconName: "trash"
                        enabled: root.editor.canDelete && !root.editor.busy
                        onClicked: deleteDialog.open()
                    }
                }
            }
        }
    }

    FileDialog {
        id: avatarDialog

        fileMode: FileDialog.OpenFile
        nameFilters: ["Images (*.png *.jpg *.jpeg *.webp *.gif *.bmp)"]
        onAccepted: root.editor.chooseAvatar(selectedFile)
    }

    Popup {
        id: discardDialog

        parent: shell
        anchors.centerIn: parent
        width: 340
        modal: true
        padding: 20
        closePolicy: Popup.CloseOnEscape

        background: OverlaySurface {
            surfaceRadius: Theme.menuRadius
        }

        contentItem: Column {
            spacing: 14

            Text {
                textFormat: Text.PlainText
                width: parent.width
                wrapMode: Text.WordWrap
                text: "Discard changes?"
                color: Theme.surfaceText
                font: Theme.bodyFont
            }

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 12

                EdenButton {
                    text: "Keep editing"
                    onClicked: discardDialog.close()
                }

                EdenButton {
                    text: "Discard"
                    onClicked: {
                        root.closeConfirmed = true;
                        discardDialog.close();
                        root.close();
                    }
                }
            }
        }
    }

    Popup {
        id: deleteDialog

        parent: shell
        anchors.centerIn: parent
        width: 400
        modal: true
        padding: 20
        closePolicy: Popup.CloseOnEscape

        background: OverlaySurface {
            surfaceRadius: Theme.menuRadius
        }

        contentItem: Column {
            spacing: 14

            Text {
                textFormat: Text.PlainText
                width: parent.width
                wrapMode: Text.WordWrap
                text: "Permanently delete " + root.editor.displayName + "? Its history, bookmarks, downloads, saved sign-ins, settings, extensions, and site data will be removed forever." + (root.editor.activeDownloadCount > 0 ? " " + root.editor.activeDownloadCount + " active downloads will be cancelled." : "")
                color: Theme.surfaceText
                font: Theme.bodyFont
            }

            EdenTextField {
                id: deletePasswordField

                width: parent.width
                visible: root.editor.protectedProfile
                echoMode: TextInput.Password
                inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhHiddenText
                placeholderText: "Current password"
                Accessible.name: "Current password to confirm deletion"
            }

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 12

                EdenButton {
                    text: "Keep profile"
                    onClicked: deleteDialog.close()
                }

                EdenButton {
                    text: "Delete forever"
                    iconName: "trash"
                    enabled: !root.editor.protectedProfile || deletePasswordField.text.length > 0
                    onClicked: {
                        const password = deletePasswordField.text;
                        deletePasswordField.text = "";
                        deleteDialog.close();
                        root.editor.requestDeletion(password);
                    }
                }
            }
        }
    }
}
