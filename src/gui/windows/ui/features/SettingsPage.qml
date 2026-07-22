import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import CcsTrans.Gui
import "../components" as Components

Item {
    ScrollView {
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ColumnLayout {
            width: parent.width
            spacing: 14

            Text {
                text: "Settings"
                color: Theme.text
                font.pixelSize: 18
                font.bold: true
                renderType: Text.NativeRendering
            }

            Repeater {
                model: guiState.applicationFieldsModel
                delegate: Rectangle {
                    required property string displayName
                    required property string valueText
                    required property string applyImpact
                    Layout.fillWidth: true
                    implicitHeight: 50
                    radius: Theme.radius
                    color: Theme.surfaceMuted

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        anchors.rightMargin: 14
                        Text {
                            Layout.fillWidth: true
                            text: displayName
                            color: Theme.text
                            font.pixelSize: 13
                            elide: Text.ElideRight
                            renderType: Text.NativeRendering
                        }
                        Text {
                            Layout.maximumWidth: parent.width * 0.55
                            text: valueText
                            color: Theme.textMuted
                            font.pixelSize: 12
                            elide: Text.ElideMiddle
                            renderType: Text.NativeRendering
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 1
                color: Theme.border
            }

            RowLayout {
                Layout.fillWidth: true
                Text {
                    Layout.fillWidth: true
                    text: "Window"
                    color: Theme.text
                    font.pixelSize: 14
                    font.bold: true
                    renderType: Text.NativeRendering
                }
                Components.MotionSwitch {
                    motion: motionPolicy
                    text: "Lightweight mode"
                    checked: settingsController.lightweightMode
                    onToggled: value => settingsController.setLightweightMode(value)
                }
                Components.MotionSwitch {
                    motion: motionPolicy
                    text: "Reduce motion"
                    checked: motionPolicy.reduceMotion
                    onToggled: value => motionPolicy.reduceMotion = value
                }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 54
                radius: Theme.radius
                color: Theme.surfaceMuted

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 14
                    Text {
                        Layout.fillWidth: true
                        text: "Storage migration"
                        color: Theme.text
                        font.pixelSize: 13
                        renderType: Text.NativeRendering
                    }
                    Text {
                        text: migrationController.state
                        color: Theme.textMuted
                        font.pixelSize: 12
                        renderType: Text.NativeRendering
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Components.MotionButton {
                    motion: motionPolicy
                    secondary: true
                    text: "Reload draft"
                    enabled: !commandDispatcher.busy
                    onClicked: commandDispatcher.reloadDraft()
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
}
