import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import CcsTrans.Gui
import "../components" as Components

Item {
    id: root

    property string pendingProfileKey
    property bool removePromptVisible: false

    function requestProfileSelection(key) {
        if (key === profilesController.profileKey) return
        if (profilesController.dirty || rulesController.dirty) {
            pendingProfileKey = key
            return
        }
        profilesController.selectProfile(key)
    }

    function syncCurrentIndex() {
        if (profilesController.profileKey.length === 0) {
            profileList.currentIndex = -1
            return
        }
        profileList.currentIndex = guiState.profilesModel.indexOfKey(
            Number(profilesController.profileKey))
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        ColumnLayout {
            Layout.preferredWidth: 292
            Layout.minimumWidth: 240
            Layout.fillHeight: true
            Layout.rightMargin: 16
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                Text {
                    Layout.fillWidth: true
                    text: "Profiles"
                    color: Theme.text
                    font.pixelSize: 18
                    font.bold: true
                    renderType: Text.NativeRendering
                }
                Text {
                    text: guiState.profilesModel.count
                    color: Theme.textMuted
                    font.pixelSize: 12
                    renderType: Text.NativeRendering
                }
            }

            ListView {
                id: profileList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                reuseItems: true
                boundsBehavior: Flickable.StopAtBounds
                model: guiState.profilesModel

                delegate: Components.ProfileSummaryDelegate {
                    width: ListView.view.width
                    motion: motionPolicy
                    selected: stableKey === profilesController.profileKey
                    onChosen: key => root.requestProfileSelection(key)
                }

                ScrollBar.vertical: Components.MotionScrollBar {
                    motion: motionPolicy
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Components.MotionTextField {
                    id: newProfile
                    Layout.fillWidth: true
                    motion: motionPolicy
                    placeholderText: "Profile ID"
                    Accessible.name: "New Profile ID"
                    onAccepted: {
                        if (!profilesController.dirty && !rulesController.dirty
                                && !commandDispatcher.busy) {
                            profilesController.createProfile(text)
                            text = ""
                        }
                    }
                }
                Components.MotionButton {
                    motion: motionPolicy
                    compact: true
                    secondary: true
                    text: "+"
                    Accessible.name: "Add Profile"
                    enabled: newProfile.text.trim().length > 0
                             && !profilesController.dirty
                             && !rulesController.dirty
                             && !commandDispatcher.busy
                    onClicked: {
                        profilesController.createProfile(newProfile.text)
                        newProfile.text = ""
                    }
                }
            }
        }

        Rectangle {
            Layout.preferredWidth: 1
            Layout.fillHeight: true
            color: Theme.border
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 22
            spacing: 12

            RowLayout {
                Layout.fillWidth: true
                Text {
                    Layout.fillWidth: true
                    text: profilesController.profileKey.length > 0
                          ? "Profile configuration" : "No Profile selected"
                    color: Theme.text
                    font.pixelSize: 18
                    font.bold: true
                    renderType: Text.NativeRendering
                }
                Components.MotionButton {
                    motion: motionPolicy
                    compact: true
                    secondary: true
                    text: "^"
                    Accessible.name: "Move Profile Up"
                    enabled: profilesController.profileKey.length > 0
                             && profileList.currentIndex > 0
                             && !commandDispatcher.busy
                    onClicked: profilesController.moveSelected(profileList.currentIndex - 1)
                }
                Components.MotionButton {
                    motion: motionPolicy
                    compact: true
                    secondary: true
                    text: "v"
                    Accessible.name: "Move Profile Down"
                    enabled: profilesController.profileKey.length > 0
                             && profileList.currentIndex >= 0
                             && profileList.currentIndex < profileList.count - 1
                             && !commandDispatcher.busy
                    onClicked: profilesController.moveSelected(profileList.currentIndex + 1)
                }
            }

            ListView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 8
                model: profilesController.fieldsModel
                boundsBehavior: Flickable.StopAtBounds

                delegate: Components.FieldEditorRow {
                    width: ListView.view.width
                    editor: profilesController
                    motion: motionPolicy
                }

                ScrollBar.vertical: Components.MotionScrollBar {
                    motion: motionPolicy
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Item { Layout.fillWidth: true }
                Components.MotionButton {
                    motion: motionPolicy
                    secondary: true
                    text: "Reset"
                    enabled: profilesController.dirty && !commandDispatcher.busy
                    onClicked: profilesController.resetLocalDraft()
                }
                Components.MotionButton {
                    motion: motionPolicy
                    danger: true
                    text: "Remove"
                    enabled: profilesController.profileKey.length > 0
                             && !commandDispatcher.busy
                    onClicked: root.removePromptVisible = true
                }
                Components.MotionButton {
                    motion: motionPolicy
                    text: "Save"
                    enabled: profilesController.dirty
                             && profilesController.valid
                             && !commandDispatcher.busy
                    onClicked: profilesController.save()
                }
            }
        }
    }

    Components.DecisionDialog {
        motion: motionPolicy
        visible: root.removePromptVisible
        titleText: "Remove Profile"
        messageText: profilesController.dirty || rulesController.dirty
                     ? "Remove this Profile and discard its unsaved editor input?"
                     : "Remove this Profile from the current draft?"
        primaryText: "Remove"
        primaryDanger: true
        onPrimaryTriggered: {
            root.removePromptVisible = false
            profilesController.removeSelected()
        }
        onCancelled: root.removePromptVisible = false
    }

    Components.DecisionDialog {
        motion: motionPolicy
        visible: root.pendingProfileKey.length > 0
        titleText: "Switch Profile"
        messageText: "Discard unsaved Profile and Rules editor input before switching?"
        primaryText: "Discard and switch"
        primaryDanger: true
        onPrimaryTriggered: {
            var key = root.pendingProfileKey
            root.pendingProfileKey = ""
            profilesController.selectProfile(key)
        }
        onCancelled: root.pendingProfileKey = ""
    }

    Connections {
        target: profilesController
        function onDraftChanged() { root.syncCurrentIndex() }
    }

    Component.onCompleted: syncCurrentIndex()
}
