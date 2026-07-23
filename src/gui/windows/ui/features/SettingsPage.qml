import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import CcsTrans.Gui
import "../components" as Components

Item {
    id: root

    signal reloadRequested()

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: "Settings"
                color: Theme.text
                font.pixelSize: 18
                font.bold: true
                renderType: Text.NativeRendering
            }
            Text {
                text: settingsController.dirty ? "Unsaved" : "Saved"
                color: settingsController.dirty ? Theme.warning : Theme.textMuted
                font.pixelSize: 12
                renderType: Text.NativeRendering
            }
        }

        ScrollView {
            id: settingsScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ScrollBar.vertical: Components.MotionScrollBar { motion: motionPolicy }

            ColumnLayout {
                width: settingsScroll.availableWidth
                spacing: 10

                Text {
                    Layout.fillWidth: true
                    text: "Configuration"
                    color: Theme.text
                    font.pixelSize: 14
                    font.bold: true
                    renderType: Text.NativeRendering
                }

                Repeater {
                    model: settingsController.fieldsModel
                    delegate: Components.FieldEditorRow {
                        Layout.fillWidth: true
                        editor: settingsController
                        motion: motionPolicy
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 1
                    color: Theme.border
                }

                Text {
                    Layout.fillWidth: true
                    text: "Window"
                    color: Theme.text
                    font.pixelSize: 14
                    font.bold: true
                    renderType: Text.NativeRendering
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Components.MotionSwitch {
                        Layout.fillWidth: true
                        motion: motionPolicy
                        text: "Lightweight mode"
                        checked: settingsController.lightweightMode
                        onToggled: value => settingsController.setLightweightMode(value)
                    }
                    Components.MotionSwitch {
                        Layout.fillWidth: true
                        motion: motionPolicy
                        text: "Reduce motion"
                        checked: motionPolicy.reduceMotion
                        onToggled: value => motionPolicy.reduceMotion = value
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 1
                    color: Theme.border
                }

                Text {
                    Layout.fillWidth: true
                    text: "Storage"
                    color: Theme.text
                    font.pixelSize: 14
                    font.bold: true
                    renderType: Text.NativeRendering
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: storageColumn.implicitHeight + 24
                    radius: Theme.radius
                    color: migrationController.state === "ready"
                           ? Theme.surfaceMuted : "#f7eee1"
                    border.width: motionPolicy.highContrast ? 2 : 1
                    border.color: migrationController.state === "ready"
                                  ? Theme.border : Theme.warning

                    ColumnLayout {
                        id: storageColumn
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 12
                        spacing: 5

                        RowLayout {
                            Layout.fillWidth: true
                            Text {
                                Layout.fillWidth: true
                                text: "Profile database"
                                color: Theme.text
                                font.pixelSize: 13
                                font.bold: true
                                renderType: Text.NativeRendering
                            }
                            Text {
                                text: migrationController.state
                                color: migrationController.state === "ready"
                                       ? Theme.accent : Theme.warning
                                font.pixelSize: 12
                                renderType: Text.NativeRendering
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: migrationController.detail.length > 0
                                  ? migrationController.detail
                                  : "Storage status is not available yet."
                            color: Theme.textMuted
                            font.pixelSize: 11
                            wrapMode: Text.WrapAnywhere
                            renderType: Text.NativeRendering
                        }
                        Text {
                            Layout.fillWidth: true
                            text: migrationController.databasePath
                            color: Theme.textMuted
                            font.pixelSize: 10
                            elide: Text.ElideMiddle
                            renderType: Text.NativeRendering
                        }
                        Text {
                            Layout.fillWidth: true
                            text: migrationController.backupDirectory.length > 0
                                  ? "Backups: " + migrationController.backupDirectory : ""
                            color: Theme.textMuted
                            font.pixelSize: 10
                            elide: Text.ElideMiddle
                            visible: text.length > 0
                            renderType: Text.NativeRendering
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Item { Layout.fillWidth: true }
                            Components.MotionButton {
                                motion: motionPolicy
                                secondary: true
                                text: "Inspect"
                                enabled: !commandDispatcher.busy
                                onClicked: migrationController.inspect()
                            }
                            Components.MotionButton {
                                motion: motionPolicy
                                text: "Migrate"
                                enabled: migrationController.actionAvailable
                                onClicked: migrationController.requestMigration()
                            }
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Text {
                Layout.fillWidth: true
                text: settingsController.valid ? "" : "Fix the highlighted fields before saving."
                color: Theme.danger
                font.pixelSize: 11
                elide: Text.ElideRight
                renderType: Text.NativeRendering
            }
            Components.MotionButton {
                motion: motionPolicy
                secondary: true
                text: "Reset"
                enabled: settingsController.dirty && !commandDispatcher.busy
                onClicked: settingsController.resetLocalDraft()
            }
            Components.MotionButton {
                motion: motionPolicy
                secondary: true
                text: "Reload draft"
                enabled: !commandDispatcher.busy
                onClicked: root.reloadRequested()
            }
            Components.MotionButton {
                motion: motionPolicy
                text: "Save"
                enabled: settingsController.dirty
                         && settingsController.valid
                         && !commandDispatcher.busy
                onClicked: settingsController.save()
            }
            Components.MotionButton {
                motion: motionPolicy
                danger: true
                text: "Quit"
                enabled: !commandDispatcher.busy
                onClicked: commandDispatcher.quitApplication()
            }
        }
    }
}
