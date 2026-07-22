import QtQuick
import QtQuick.Controls.Basic

Control {
    id: root

    required property var motion
    property alias text: label.text
    property bool checked: false
    readonly property real visualKnobX: knob.x
    signal toggled(bool checked)

    implicitWidth: 186
    implicitHeight: 38
    focusPolicy: Qt.StrongFocus
    Accessible.role: Accessible.CheckBox
    Accessible.name: text
    Accessible.checked: checked

    contentItem: Item {
        id: visual
        scale: tap.pressed ? 0.975 : (hover.hovered ? 1.012 : 1.0)

        Behavior on scale {
            NumberAnimation {
                duration: root.motion.shortDuration
                easing.type: Easing.OutCubic
            }
        }

        Rectangle {
            anchors.fill: parent
            radius: 7
            color: hover.hovered ? "#e8edf0" : "#f2f5f6"
            border.width: root.visualFocus ? 2 : 1
            border.color: root.visualFocus ? "#147fc4" : "#b4c0c5"
            Behavior on color { ColorAnimation { duration: root.motion.shortDuration } }
            Behavior on border.color { ColorAnimation { duration: root.motion.shortDuration } }
        }

        Text {
            id: label
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            color: "#172126"
            font.pixelSize: 13
            renderType: Text.NativeRendering
        }

        Rectangle {
            id: track
            width: 38
            height: 20
            radius: 10
            anchors.right: parent.right
            anchors.rightMargin: 9
            anchors.verticalCenter: parent.verticalCenter
            color: root.checked ? "#16875b" : "#9aa8ae"
            Behavior on color { ColorAnimation { duration: root.motion.mediumDuration } }

            Rectangle {
                id: knob
                width: 14
                height: 14
                radius: 7
                y: 3
                x: root.checked ? track.width - width - 3 : 3
                color: "#ffffff"
                Behavior on x {
                    NumberAnimation {
                        duration: root.motion.mediumDuration
                        easing.type: Easing.OutCubic
                    }
                }
            }
        }
    }

    HoverHandler { id: hover }
    TapHandler {
        id: tap
        onTapped: {
            root.checked = !root.checked
            root.toggled(root.checked)
        }
    }
    Keys.onSpacePressed: {
        root.checked = !root.checked
        root.toggled(root.checked)
    }
}
