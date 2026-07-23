import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import CcsTrans.Gui
import "../components" as Components

Item {
    id: root

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
                text: guiState.selectedEnabledRuleCount + "/"
                      + guiState.selectedRuleCount + " enabled"
                color: Theme.textMuted
                font.pixelSize: 12
                renderType: Text.NativeRendering
            }
            Text {
                text: rulesController.dirty ? "Unsaved" : guiState.draftPhase
                color: rulesController.dirty ? Theme.warning : Theme.textMuted
                font.pixelSize: 12
                renderType: Text.NativeRendering
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: Theme.radius
            color: Theme.surface
            border.width: rulesController.error.length > 0
                          || rulesController.diagnostic.length > 0
                          ? 2 : (editor.activeFocus ? 2 : 1)
            border.color: rulesController.error.length > 0
                          || rulesController.diagnostic.length > 0
                          ? Theme.danger
                          : (editor.activeFocus ? Theme.focus : Theme.border)

            Behavior on border.color {
                ColorAnimation { duration: motionPolicy.shortDuration }
            }

            ScrollView {
                id: editorScroll
                anchors.fill: parent
                anchors.margins: 2
                clip: true
                contentWidth: Math.max(availableWidth, editor.implicitWidth)
                contentHeight: Math.max(availableHeight, editor.implicitHeight)
                ScrollBar.vertical: Components.MotionScrollBar { motion: motionPolicy }
                ScrollBar.horizontal: Components.MotionScrollBar {
                    motion: motionPolicy
                    policy: ScrollBar.AsNeeded
                }

                TextArea {
                    id: editor
                    width: Math.max(editorScroll.availableWidth, implicitWidth)
                    height: Math.max(editorScroll.availableHeight, implicitHeight)
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
                    selectByMouse: true
                    persistentSelection: true
                    background: Item {}
                    Accessible.name: "Rules editor"
                    onTextChanged: {
                        if (activeFocus && text !== rulesController.text)
                            rulesController.text = text
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Text {
                Layout.fillWidth: true
                text: rulesController.error.length > 0
                      ? rulesController.error : rulesController.diagnostic
                color: Theme.danger
                font.pixelSize: 11
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

    Connections {
        target: rulesController
        function onTextReplaced(value) {
            if (editor.text === value) return
            var cursor = editor.cursorPosition
            editor.text = value
            editor.cursorPosition = Math.min(cursor, value.length)
        }
    }
}
