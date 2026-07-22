import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import CcsTrans.Gui
import "../components" as Components

Item {
    id: root

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
                    onChosen: key => profilesController.selectProfile(key)
                }

                ScrollBar.vertical: ScrollBar {
                    id: profileScroll
                    policy: ScrollBar.AsNeeded
                    opacity: active || hovered ? 1 : 0
                    contentItem: Rectangle {
                        implicitWidth: 6
                        radius: 3
                        color: profileScroll.hovered
                               ? Theme.borderStrong : Theme.border
                    }
                    background: Item {}
                    Behavior on opacity {
                        NumberAnimation { duration: motionPolicy.mediumDuration }
                    }
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
                        profilesController.createProfile(text)
                        text = ""
                    }
                }
                Components.MotionButton {
                    motion: motionPolicy
                    compact: true
                    secondary: true
                    text: "+"
                    Accessible.name: "Add Profile"
                    enabled: newProfile.text.trim().length > 0
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
            spacing: 14

            Text {
                text: profilesController.profileKey.length > 0
                      ? "Profile configuration" : "No Profile selected"
                color: Theme.text
                font.pixelSize: 18
                font.bold: true
                renderType: Text.NativeRendering
            }

            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: 16
                rowSpacing: 12
                enabled: profilesController.profileKey.length > 0

                Text {
                    text: "Profile ID"
                    color: Theme.textMuted
                    font.pixelSize: 12
                    renderType: Text.NativeRendering
                }
                Components.MotionTextField {
                    Layout.fillWidth: true
                    motion: motionPolicy
                    text: profilesController.profileId
                    invalid: profilesController.profileId.trim().length === 0
                    Accessible.name: "Profile ID"
                    onTextEdited: profilesController.profileId = text
                }

                Text {
                    text: "Enabled"
                    color: Theme.textMuted
                    font.pixelSize: 12
                    renderType: Text.NativeRendering
                }
                Components.MotionSwitch {
                    motion: motionPolicy
                    text: profilesController.enabled ? "Enabled" : "Disabled"
                    checked: profilesController.enabled
                    onToggled: value => profilesController.enabled = value
                }
            }

            Text {
                text: "Fields"
                color: Theme.text
                font.pixelSize: 14
                font.bold: true
                renderType: Text.NativeRendering
            }

            ListView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 1
                model: guiState.profileFieldsModel
                boundsBehavior: Flickable.StopAtBounds

                delegate: Rectangle {
                    required property string displayName
                    required property string valueText
                    required property string applyImpact
                    width: ListView.view.width
                    height: 46
                    radius: Theme.radius
                    color: index % 2 === 0 ? Theme.surfaceMuted : Theme.surface

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        Text {
                            Layout.fillWidth: true
                            text: displayName
                            color: Theme.textMuted
                            font.pixelSize: 12
                            elide: Text.ElideRight
                            renderType: Text.NativeRendering
                        }
                        Text {
                            Layout.maximumWidth: parent.width * 0.58
                            text: valueText
                            color: Theme.text
                            font.pixelSize: 12
                            elide: Text.ElideMiddle
                            renderType: Text.NativeRendering
                        }
                    }
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
                    onClicked: profilesController.removeSelected()
                }
                Components.MotionButton {
                    motion: motionPolicy
                    text: "Save"
                    enabled: profilesController.dirty
                             && profilesController.profileId.trim().length > 0
                             && !commandDispatcher.busy
                    onClicked: profilesController.save()
                }
            }
        }
    }
}
