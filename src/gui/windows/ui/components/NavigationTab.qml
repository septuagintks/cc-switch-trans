import QtQuick
import QtQuick.Controls.Basic
import CcsTrans.Gui

Button {
    id: root

    required property var motion
    property bool selected: false

    implicitWidth: 112
    implicitHeight: 38
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    background: null

    contentItem: Item {
        scale: root.down ? 0.97 : (root.hovered ? 1.012 : 1)
        Behavior on scale {
            NumberAnimation {
                duration: root.motion.shortDuration
                easing.type: Easing.OutCubic
            }
        }

        Rectangle {
            anchors.fill: parent
            radius: Theme.radius
            color: root.selected ? "#dcece6"
                                 : (root.hovered ? Theme.surfaceMuted : "transparent")
            border.width: root.visualFocus ? 2 : 0
            border.color: Theme.focus
            Behavior on color {
                ColorAnimation { duration: root.motion.shortDuration }
            }
        }

        Text {
            anchors.centerIn: parent
            text: root.text
            color: root.selected ? Theme.accent : Theme.textMuted
            font.pixelSize: 13
            font.bold: root.selected
            renderType: Text.NativeRendering
        }
    }
}
