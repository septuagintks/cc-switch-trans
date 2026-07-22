import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import CcsTrans.Gui
import "../components" as Components

Item {
    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: rulesController.profileId.length > 0
                      ? "Rules · " + rulesController.profileId : "Rules"
                color: Theme.text
                font.pixelSize: 18
                font.bold: true
                elide: Text.ElideRight
                renderType: Text.NativeRendering
            }
            Text {
                text: guiState.draftPhase
                color: guiState.draftDirty ? Theme.warning : Theme.textMuted
                font.pixelSize: 12
                renderType: Text.NativeRendering
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: Theme.radius
            color: Theme.surface
            border.width: editor.activeFocus ? 2 : 1
            border.color: rulesController.diagnostic.length > 0
                          ? Theme.danger
                          : (editor.activeFocus ? Theme.focus : Theme.border)

            Behavior on border.color {
                ColorAnimation { duration: motionPolicy.shortDuration }
            }

            ScrollView {
                anchors.fill: parent
                anchors.margins: 2
                clip: true
                ScrollBar.horizontal.policy: ScrollBar.AsNeeded
                ScrollBar.vertical.policy: ScrollBar.AsNeeded

                TextArea {
                    id: editor
                    enabled: rulesController.profileId.length > 0
                    leftPadding: 12
                    rightPadding: 12
                    topPadding: 10
                    bottomPadding: 10
                    color: Theme.text
                    selectionColor: Theme.blue
                    selectedTextColor: "#ffffff"
                    font.family: "Cascadia Mono"
                    font.pixelSize: 13
                    wrapMode: TextEdit.NoWrap
                    background: Item {}
                    Accessible.name: "Rules editor"
                    onTextChanged: {
                        if (activeFocus && text !== rulesController.text)
                            rulesController.text = text
                    }

                    Binding {
                        target: editor
                        property: "text"
                        value: rulesController.text
                        when: !editor.activeFocus
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Text {
                Layout.fillWidth: true
                text: rulesController.diagnostic
                color: Theme.danger
                font.pixelSize: 12
                elide: Text.ElideRight
                renderType: Text.NativeRendering
            }
            Components.MotionButton {
                motion: motionPolicy
                secondary: true
                text: "Reset"
                enabled: rulesController.dirty && !commandDispatcher.busy
                onClicked: rulesController.resetLocalDraft()
            }
            Components.MotionButton {
                motion: motionPolicy
                secondary: true
                text: "Format"
                enabled: rulesController.profileId.length > 0
                         && !commandDispatcher.busy
                onClicked: rulesController.format()
            }
            Components.MotionButton {
                motion: motionPolicy
                text: "Save"
                enabled: rulesController.dirty && !commandDispatcher.busy
                onClicked: rulesController.save()
            }
        }
    }
}
