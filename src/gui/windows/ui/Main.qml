import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import CcsTrans.Gui
import "components" as Components
import "features" as Features

ApplicationWindow {
    id: window

    property int activeTab: 0

    width: 1020
    height: 680
    minimumWidth: 820
    minimumHeight: 540
    visible: false
    title: "ccs-trans"
    color: Theme.canvas
    onClosing: close => {
        close.accepted = false
        windowController.requestClose()
    }

    background: Rectangle { color: Theme.canvas }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 18
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 38
            spacing: 10

            Text {
                text: "ccs-trans"
                color: Theme.text
                font.pixelSize: 22
                font.bold: true
                renderType: Text.NativeRendering
            }

            Rectangle {
                width: 8
                height: 8
                radius: 4
                color: guiState.applicationState === "running"
                       ? Theme.accent
                       : (guiState.applicationState === "faulted"
                          ? Theme.danger : Theme.warning)
                Behavior on color {
                    ColorAnimation { duration: motionPolicy.shortDuration }
                }
            }

            Text {
                text: guiState.applicationState
                      + (guiState.listenerAddress.length > 1
                         ? "  ·  " + guiState.listenerAddress : "")
                color: Theme.textMuted
                font.pixelSize: 12
                renderType: Text.NativeRendering
            }

            Item { Layout.fillWidth: true }

            Components.MotionButton {
                motion: motionPolicy
                secondary: true
                text: "Start"
                enabled: guiState.canStart && !commandDispatcher.busy
                onClicked: commandDispatcher.startService()
            }
            Components.MotionButton {
                motion: motionPolicy
                secondary: true
                text: "Stop"
                enabled: guiState.canStop && !commandDispatcher.busy
                onClicked: commandDispatcher.stopService()
            }
            Components.MotionButton {
                motion: motionPolicy
                secondary: true
                text: "Reload"
                enabled: guiState.canReload && !commandDispatcher.busy
                onClicked: commandDispatcher.reloadService()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 38
            spacing: 6

            Components.NavigationTab {
                motion: motionPolicy
                text: "Profiles"
                selected: window.activeTab === 0
                onClicked: window.activeTab = 0
            }
            Components.NavigationTab {
                motion: motionPolicy
                text: "Rules"
                selected: window.activeTab === 1
                onClicked: window.activeTab = 1
            }
            Components.NavigationTab {
                motion: motionPolicy
                text: "Settings"
                selected: window.activeTab === 2
                onClicked: window.activeTab = 2
            }
            Item { Layout.fillWidth: true }
            Text {
                text: guiState.draftPhase + "  ·  r" + guiState.draftRevision
                color: guiState.draftDirty ? Theme.warning : Theme.textMuted
                font.pixelSize: 12
                renderType: Text.NativeRendering
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: Theme.border
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: window.activeTab

            Features.ProfilesPage {}
            Features.RulesPage {}
            Features.SettingsPage {}
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 44
            radius: Theme.radius
            color: guiState.lastCommandError !== ""
                   && guiState.lastCommandError !== "none"
                   ? "#f7e5e7" : Theme.surfaceMuted
            border.width: 1
            border.color: guiState.lastCommandError !== ""
                          && guiState.lastCommandError !== "none"
                          ? "#dfadb3" : Theme.border

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 8
                spacing: 8

                Text {
                    Layout.fillWidth: true
                    text: commandDispatcher.localError.length > 0
                          ? commandDispatcher.localError
                          : (guiState.lastCommandDetail.length > 0
                             ? guiState.lastCommandDetail
                             : (commandDispatcher.busy
                                ? "Applying command" : "Ready"))
                    color: guiState.lastCommandError !== ""
                           && guiState.lastCommandError !== "none"
                           ? Theme.danger : Theme.textMuted
                    font.pixelSize: 12
                    elide: Text.ElideRight
                    renderType: Text.NativeRendering
                }

                Components.MotionButton {
                    motion: motionPolicy
                    secondary: true
                    text: "Discard"
                    enabled: guiState.draftDirty && !commandDispatcher.busy
                    onClicked: commandDispatcher.discardDraft()
                }
                Components.MotionButton {
                    motion: motionPolicy
                    text: "Apply"
                    enabled: guiState.draftDirty && !commandDispatcher.busy
                    onClicked: commandDispatcher.applyDraft()
                }
            }
        }
    }
}
