import Eden.Ui
import QtQuick

EdenMenu {
    id: menu

    required property var controller
    property int actionsRevision: 0

    preferredWidth: 280
    actions: {
        const revision = actionsRevision;
        return Profiles.profileMenuActions(controller.profileId, controller.mode);
    }
    headerComponent: profileHeader
    onTriggered: actionId => {
        const id = actionId.toString();
        if (id.startsWith("switch-profile:"))
            Profiles.switchToProfile(id.slice(15));
        else if (id === "add-profile")
            Profiles.openCreateWindow(menu.controller);
        else if (id === "customize-profile")
            Profiles.openEditor(menu.controller.profileId, menu.controller);
        else if (id === "sign-out")
            Profiles.signOut(menu.controller.profileId);
    }

    Connections {
        function onProfileMenuActionsChanged() {
            ++menu.actionsRevision;
        }

        target: Profiles
    }

    Component {
        id: profileHeader

        Item {
            width: ListView.view ? ListView.view.width : 268
            height: 72
            Accessible.role: Accessible.StaticText
            Accessible.name: "Profile: " + menu.controller.profileDisplayName + ", local profile"

            Row {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                height: 64
                spacing: 12

                ProfileAvatar {
                    anchors.verticalCenter: parent.verticalCenter
                    avatarUrl: menu.controller.profileAvatarUrl
                    displayName: menu.controller.profileDisplayName
                    avatarSize: 40
                }

                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 52
                    spacing: 2

                    Text {
                        textFormat: Text.PlainText
                        width: parent.width
                        text: menu.controller.profileDisplayName
                        color: Theme.surfaceText
                        font: Theme.labelFont
                        elide: Text.ElideRight
                    }

                    Text {
                        textFormat: Text.PlainText
                        text: "Local profile"
                        color: Theme.surfaceVariantText
                        font.family: Theme.bodyFont.family
                        font.pixelSize: 11
                    }
                }
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: Theme.outline
                opacity: 0.3
            }
        }
    }
}
