import Eden.Ui
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Effects

Window {
    id: root

    property string page: "chooser"
    property string selectedProfileId
    property string selectedProfileName
    property string selectedProfileAvatarUrl
    property bool forgottenMode: false
    property string inlineError
    property int cooldownSeconds: 0
    property bool verifying: false

    function showPassword(profileId, forgotten) {
        const row = Profiles.profiles.indexOfProfile(profileId);
        if (row < 0) {
            return;
        }

        const profile = Profiles.profiles.profileAt(row);
        selectedProfileId = profileId;
        selectedProfileName = profile.displayName;
        selectedProfileAvatarUrl = profile.avatarUrl;
        forgottenMode = forgotten === true;
        inlineError = "";
        cooldownSeconds = Profiles.passwordCooldownSeconds(profileId);
        page = "password";
        passwordField.text = "";
        passwordField.forceActiveFocus();
    }

    function showCreate() {
        inlineError = "";
        page = "create";
        createNameField.forceActiveFocus();
    }

    function backToChooser() {
        if (verifying) {
            return;
        }

        passwordField.text = "";
        createPasswordField.text = "";
        createConfirmField.text = "";
        inlineError = "";
        forgottenMode = false;
        page = "chooser";
        profileGrid.forceActiveFocus();
    }

    function activateEntry(profileId) {
        Profiles.activateProfile(profileId);
    }

    width: 720
    height: 520
    minimumWidth: 600
    minimumHeight: 440
    visible: true
    flags: (transientParent ? Qt.Dialog : Qt.Window) | Qt.FramelessWindowHint
    modality: transientParent ? Qt.WindowModal : Qt.NonModal
    color: "transparent"
    title: "Choose a profile"

    Connections {
        function onPasswordRequired(profileId) {
            root.showPassword(profileId, false);
        }

        function onUnlockFailed(profileId, message, cooldown) {
            if (profileId !== root.selectedProfileId) {
                return;
            }

            root.verifying = false;
            root.inlineError = message;
            root.cooldownSeconds = cooldown;
            passwordField.text = "";
            if (cooldown === 0) {
                passwordField.forceActiveFocus();
            }
        }

        function onCooldownFinished(profileId) {
            if (profileId !== root.selectedProfileId) {
                return;
            }

            root.cooldownSeconds = 0;
            root.inlineError = "";
            if (root.page === "password") {
                passwordField.forceActiveFocus();
            }
        }

        function onUnlockSucceeded(profileId) {
            if (profileId === root.selectedProfileId) {
                root.verifying = false;
                passwordField.text = "";
            }
        }

        function onCreateFailed(message) {
            root.verifying = false;
            root.inlineError = message;
        }

        function onForgottenDeletionConfirmationRequired(profileId, displayName) {
            forgottenConfirm.profileId = profileId;
            forgottenConfirm.profileName = displayName;
            forgottenConfirm.open();
        }

        function onOperationFailed(profileId, message) {
            if (profileId === root.selectedProfileId) {
                root.inlineError = message;
            }
        }

        target: Profiles
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

            DragHandler {
                target: null
                enabled: !root.transientParent
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
                onClicked: {
                    if (Profiles.browserWindowCount === 0) {
                        Profiles.quitApplication();
                    } else {
                        root.close();
                    }
                }
            }
        }

        Item {
            id: pageStack

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: chrome.bottom
            anchors.bottom: parent.bottom

            Item {
                id: chooserPage

                anchors.fill: parent
                visible: opacity > 0
                opacity: root.page === "chooser" ? 1 : 0
                enabled: root.page === "chooser"

                Text {
                    id: chooserTitle

                    textFormat: Text.PlainText
                    anchors.top: parent.top
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "Choose a profile"
                    color: Theme.surfaceText
                    font: Theme.titleFont
                }

                GridView {
                    id: profileGrid

                    readonly property int columns: Math.max(1, Math.floor(width / cellWidth))

                    anchors.top: chooserTitle.bottom
                    anchors.topMargin: 28
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: Math.min(parent.width - 64, Math.max(1, Math.floor((parent.width - 64) / 148)) * 148)
                    height: parent.height - chooserTitle.height - 44
                    cellWidth: 148
                    cellHeight: 168
                    clip: true
                    focus: true
                    keyNavigationEnabled: true
                    model: Profiles.profiles
                    activeFocusOnTab: true
                    Keys.onReturnPressed: {
                        if (currentIndex >= 0 && currentIndex < count) {
                            root.activateEntry(Profiles.profiles.profileAt(currentIndex).profileId);
                        }
                    }
                    Keys.onEnterPressed: Keys.returnPressed(event)
                    Keys.onEscapePressed: {
                        if (Profiles.browserWindowCount > 0) {
                            root.close();
                        }
                    }

                    footer: Item {
                        width: profileGrid.cellWidth
                        height: profileGrid.cellHeight

                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 10
                            radius: Theme.cardRadius
                            color: addHover.hovered || addCard.activeFocus ? Theme.surfaceContainerHigh : "transparent"
                            border.width: addCard.activeFocus ? Theme.focusBorderWidth : 0
                            border.color: Theme.focusBorder

                            Column {
                                anchors.centerIn: parent
                                spacing: 12

                                Icon {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    name: "add"
                                    iconSize: 44
                                }

                                Text {
                                    textFormat: Text.PlainText
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: "Add profile"
                                    color: Theme.surfaceText
                                    font: Theme.labelFont
                                }
                            }

                            Item {
                                id: addCard

                                anchors.fill: parent
                                activeFocusOnTab: true
                                Accessible.role: Accessible.Button
                                Accessible.name: "Add profile"
                                Keys.onReturnPressed: root.showCreate()
                                Keys.onEnterPressed: root.showCreate()
                            }

                            HoverHandler {
                                id: addHover
                            }

                            TapHandler {
                                onTapped: root.showCreate()
                            }
                        }
                    }

                    delegate: Item {
                        id: profileCard

                        required property int index
                        required property string profileId
                        required property string displayName
                        required property string avatarUrl
                        required property bool protectedProfile
                        required property bool currentProfile
                        required property string lifecycle

                        width: profileGrid.cellWidth
                        height: profileGrid.cellHeight

                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 10
                            radius: Theme.cardRadius
                            color: cardHover.hovered || profileCard.GridView.isCurrentItem ? Theme.surfaceContainerHigh : "transparent"
                            border.width: profileCard.GridView.isCurrentItem && profileGrid.activeFocus ? Theme.focusBorderWidth : 0
                            border.color: Theme.focusBorder
                            Accessible.role: Accessible.Button
                            Accessible.name: "Profile: " + profileCard.displayName + (profileCard.protectedProfile ? ", locked" : "")

                            Column {
                                anchors.centerIn: parent
                                spacing: 12

                                Item {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    width: 72
                                    height: 72

                                    ProfileAvatar {
                                        anchors.fill: parent
                                        avatarUrl: profileCard.avatarUrl
                                        displayName: profileCard.displayName
                                        avatarSize: 72
                                    }

                                    Rectangle {
                                        anchors.right: parent.right
                                        anchors.bottom: parent.bottom
                                        width: 24
                                        height: 24
                                        radius: 12
                                        color: Theme.surfaceContainerHighest
                                        visible: profileCard.protectedProfile

                                        Icon {
                                            anchors.centerIn: parent
                                            name: "lock"
                                            iconSize: 14
                                        }
                                    }
                                }

                                Text {
                                    textFormat: Text.PlainText
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    width: profileGrid.cellWidth - 36
                                    horizontalAlignment: Text.AlignHCenter
                                    text: profileCard.lifecycle === "Creating" ? profileCard.displayName + " (repair)" : profileCard.displayName
                                    color: Theme.surfaceText
                                    font: Theme.labelFont
                                    elide: Text.ElideRight
                                }
                            }

                            HoverHandler {
                                id: cardHover
                            }

                            TapHandler {
                                onTapped: {
                                    profileGrid.currentIndex = profileCard.index;
                                    if (profileCard.lifecycle === "Creating") {
                                        repairDialog.openFor(profileCard.profileId, profileCard.displayName);
                                    } else {
                                        root.activateEntry(profileCard.profileId);
                                    }
                                }
                            }
                        }
                    }
                }

                transform: Translate {
                    x: root.page === "chooser" ? 0 : -40

                    Behavior on x {
                        NumberAnimation {
                            duration: Theme.mediumDuration
                            easing.type: Easing.OutCubic
                        }
                    }
                }

                Behavior on opacity {
                    NumberAnimation {
                        duration: Theme.mediumDuration
                        easing.type: Easing.OutCubic
                    }
                }
            }

            Item {
                id: passwordPage

                anchors.fill: parent
                visible: opacity > 0
                opacity: root.page === "password" ? 1 : 0
                enabled: root.page === "password"

                Column {
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 96, 360)
                    spacing: 16

                    ProfileAvatar {
                        anchors.horizontalCenter: parent.horizontalCenter
                        avatarUrl: root.selectedProfileAvatarUrl
                        displayName: root.selectedProfileName
                        avatarSize: 88
                    }

                    Text {
                        textFormat: Text.PlainText
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        text: root.selectedProfileName
                        color: Theme.surfaceText
                        font: Theme.titleFont
                        elide: Text.ElideRight
                    }

                    EdenTextField {
                        id: passwordField

                        width: parent.width
                        echoMode: TextInput.Password
                        inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhHiddenText
                        placeholderText: "Password"
                        enabled: !root.verifying && root.cooldownSeconds === 0 && !root.forgottenMode
                        Accessible.name: "Password for " + root.selectedProfileName
                        visible: !root.forgottenMode
                        onAccepted: signInButton.clicked()
                        Keys.onEscapePressed: root.backToChooser()
                    }

                    EdenButton {
                        id: signInButton

                        anchors.horizontalCenter: parent.horizontalCenter
                        visible: !root.forgottenMode
                        enabled: !root.verifying && root.cooldownSeconds === 0 && passwordField.text.length > 0
                        text: root.verifying ? "" : "Sign in"
                        iconName: root.verifying ? "refresh" : ""
                        onClicked: {
                            if (root.verifying || passwordField.text.length === 0) {
                                return;
                            }

                            root.verifying = true;
                            root.inlineError = "";
                            const value = passwordField.text;
                            passwordField.text = "";
                            Profiles.submitPassword(root.selectedProfileId, value);
                        }

                        Icon {
                            anchors.centerIn: parent
                            visible: root.verifying
                            spinning: root.verifying
                            name: "refresh"
                        }
                    }

                    Text {
                        textFormat: Text.PlainText
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        visible: root.inlineError.length > 0 || root.cooldownSeconds > 0
                        text: root.cooldownSeconds > 0 ? "Try again in " + root.cooldownSeconds + " seconds" : root.inlineError
                        color: Theme.error
                        font: Theme.labelFont
                        Accessible.role: Accessible.AlertMessage
                        Accessible.name: text
                    }

                    Column {
                        width: parent.width
                        spacing: 12
                        visible: root.forgottenMode

                        Text {
                            textFormat: Text.PlainText
                            width: parent.width
                            wrapMode: Text.WordWrap
                            horizontalAlignment: Text.AlignHCenter
                            text: "Deleting this profile permanently removes its browsing data, settings, and saved sign-ins. Type the profile name to continue."
                            color: Theme.surfaceVariantText
                            font: Theme.bodyFont
                        }

                        EdenTextField {
                            id: forgottenNameField

                            width: parent.width
                            placeholderText: root.selectedProfileName
                            Accessible.name: "Type the profile name to confirm deletion"
                        }

                        EdenButton {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: "Delete profile forever"
                            iconName: "trash"
                            enabled: forgottenNameField.text === root.selectedProfileName
                            onClicked: Profiles.requestForgottenPasswordDeletion(root.selectedProfileId, forgottenNameField.text)
                        }
                    }

                    Row {
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: 16

                        EdenButton {
                            text: "Back"
                            enabled: !root.verifying
                            onClicked: {
                                if (root.forgottenMode) {
                                    root.forgottenMode = false;
                                    forgottenNameField.text = "";
                                } else {
                                    root.backToChooser();
                                }
                            }
                        }

                        EdenButton {
                            visible: !root.forgottenMode
                            text: "Forgot password?"
                            enabled: !root.verifying
                            onClicked: {
                                root.inlineError = "";
                                root.forgottenMode = true;
                            }
                        }
                    }
                }

                transform: Translate {
                    x: root.page === "password" ? 0 : 40

                    Behavior on x {
                        NumberAnimation {
                            duration: Theme.mediumDuration
                            easing.type: Easing.OutCubic
                        }
                    }
                }

                Behavior on opacity {
                    NumberAnimation {
                        duration: Theme.mediumDuration
                        easing.type: Easing.OutCubic
                    }
                }
            }

            Item {
                id: createPage

                anchors.fill: parent
                visible: opacity > 0
                opacity: root.page === "create" ? 1 : 0
                enabled: root.page === "create"

                Flickable {
                    anchors.fill: parent
                    anchors.margins: 24
                    contentHeight: createColumn.height
                    clip: true

                    Column {
                        id: createColumn

                        anchors.horizontalCenter: parent.horizontalCenter
                        width: Math.min(parent.width, 360)
                        spacing: 14

                        Item {
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: 88
                            height: 88

                            Rectangle {
                                anchors.fill: parent
                                radius: width / 2
                                color: Theme.primaryContainer
                                visible: createAvatarPreview.avatarUrl.length === 0

                                Text {
                                    textFormat: Text.PlainText
                                    anchors.centerIn: parent
                                    text: createNameField.text.trim().length > 0 ? createNameField.text.trim().charAt(0).toUpperCase() : "?"
                                    color: Theme.primaryContainerText
                                    font.family: Theme.titleFont.family
                                    font.pixelSize: 36
                                    font.weight: Font.DemiBold
                                }
                            }

                            ProfileAvatar {
                                id: createAvatarPreview

                                property url chosenSource

                                anchors.fill: parent
                                avatarUrl: chosenSource.toString()
                                displayName: createNameField.text
                                avatarSize: 88
                                visible: chosenSource.toString().length > 0
                            }
                        }

                        EdenButton {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: "Choose image"
                            iconName: "gallery-edit"
                            onClicked: createAvatarDialog.open()
                        }

                        EdenTextField {
                            id: createNameField

                            width: parent.width
                            placeholderText: "Profile name"
                            Accessible.name: "Profile name"
                        }

                        EdenButton {
                            id: createPasswordToggle

                            property bool requirePassword: false

                            width: parent.width
                            text: requirePassword ? "Password required" : "Require a password"
                            iconName: requirePassword ? "check" : "lock"
                            onClicked: requirePassword = !requirePassword
                        }

                        EdenTextField {
                            id: createPasswordField

                            width: parent.width
                            visible: createPasswordToggle.requirePassword
                            echoMode: TextInput.Password
                            inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhHiddenText
                            placeholderText: "Password"
                            Accessible.name: "New profile password"
                        }

                        EdenTextField {
                            id: createConfirmField

                            width: parent.width
                            visible: createPasswordToggle.requirePassword
                            echoMode: TextInput.Password
                            inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhHiddenText
                            placeholderText: "Confirm password"
                            Accessible.name: "Confirm new profile password"
                        }

                        Text {
                            textFormat: Text.PlainText
                            width: parent.width
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                            visible: root.page === "create" && root.inlineError.length > 0
                            text: root.inlineError
                            color: Theme.error
                            font: Theme.labelFont
                            Accessible.role: Accessible.AlertMessage
                            Accessible.name: text
                        }

                        Row {
                            anchors.horizontalCenter: parent.horizontalCenter
                            spacing: 16

                            EdenButton {
                                text: "Cancel"
                                onClicked: root.backToChooser()
                            }

                            EdenButton {
                                text: "Create"
                                enabled: createNameField.text.trim().length > 0
                                onClicked: {
                                    root.inlineError = "";
                                    const password = createPasswordToggle.requirePassword ? createPasswordField.text : "";
                                    const confirmation = createPasswordToggle.requirePassword ? createConfirmField.text : "";
                                    Profiles.createProfile(createNameField.text, createPasswordToggle.requirePassword, password, confirmation, createAvatarPreview.chosenSource);
                                    createPasswordField.text = "";
                                    createConfirmField.text = "";
                                }
                            }
                        }
                    }
                }

                transform: Translate {
                    x: root.page === "create" ? 0 : 40

                    Behavior on x {
                        NumberAnimation {
                            duration: Theme.mediumDuration
                            easing.type: Easing.OutCubic
                        }
                    }
                }

                Behavior on opacity {
                    NumberAnimation {
                        duration: Theme.mediumDuration
                        easing.type: Easing.OutCubic
                    }
                }
            }
        }
    }

    FileDialog {
        id: createAvatarDialog

        fileMode: FileDialog.OpenFile
        nameFilters: ["Images (*.png *.jpg *.jpeg *.webp *.gif *.bmp)"]
        onAccepted: createAvatarPreview.chosenSource = selectedFile
    }

    Popup {
        id: repairDialog

        property string profileId
        property string profileName

        function openFor(id, name) {
            profileId = id;
            profileName = name;
            open();
        }

        parent: shell
        anchors.centerIn: parent
        width: 380
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
                text: repairDialog.profileName + " was interrupted while it was being created. Keep its data and finish setting it up, or delete it."
                color: Theme.surfaceText
                font: Theme.bodyFont
            }

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 12

                EdenButton {
                    text: "Finish setup"
                    onClicked: {
                        Profiles.resolveCreatingProfile(repairDialog.profileId, true);
                        repairDialog.close();
                    }
                }

                EdenButton {
                    text: "Delete"
                    iconName: "trash"
                    onClicked: {
                        Profiles.resolveCreatingProfile(repairDialog.profileId, false);
                        repairDialog.close();
                    }
                }
            }
        }
    }

    Popup {
        id: forgottenConfirm

        property string profileId
        property string profileName

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
                text: "Permanently delete " + forgottenConfirm.profileName + "? Its history, bookmarks, downloads, saved sign-ins, settings, and site data will be removed forever. This cannot be undone."
                color: Theme.surfaceText
                font: Theme.bodyFont
            }

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 12

                EdenButton {
                    text: "Keep profile"
                    onClicked: {
                        Profiles.confirmForgottenPasswordDeletion(forgottenConfirm.profileId, false);
                        forgottenConfirm.close();
                    }
                }

                EdenButton {
                    text: "Delete forever"
                    iconName: "trash"
                    onClicked: {
                        Profiles.confirmForgottenPasswordDeletion(forgottenConfirm.profileId, true);
                        forgottenConfirm.close();
                        root.backToChooser();
                    }
                }
            }
        }
    }
}
