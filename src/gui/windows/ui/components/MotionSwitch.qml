import QtQuick
import QtQuick.Controls.Basic

Control {
    id: root

    required property var motion
    property alias text: label.text
    property bool checked: false
    property bool invalid: false
    readonly property real visualKnobX: knob.x
    signal toggled(bool checked)

    implicitWidth: 186
    implicitHeight: 36
    focusPolicy: Qt.StrongFocus
    opacity: enabled ? 1 : 0.48
    Accessible.role: Accessible.CheckBox
    Accessible.name: text
    Accessible.checked: checked

    contentItem: Item {
        id: visual
        scale: pointer.pressed ? 0.975 : (pointer.containsMouse ? 1.012 : 1)

        Behavior on scale {
            NumberAnimation {
                duration: root.motion.shortDuration
                easing.type: Easing.OutCubic
            }
        }

        Rectangle {
            anchors.fill: parent
            radius: 7
            color: pointer.containsMouse ? "#e2e9e6" : "#e9eeec"
            border.width: root.visualFocus || root.motion.highContrast ? 2 : 1
            border.color: root.invalid ? "#a53f4b"
                                       : (root.visualFocus ? "#218bd0" : "#c6d0cc")
            Behavior on color {
                ColorAnimation { duration: root.motion.shortDuration }
            }
            Behavior on border.color {
                ColorAnimation { duration: root.motion.shortDuration }
            }
        }

        Text {
            id: label
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            color: "#18211f"
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
            color: root.checked ? "#147d64" : "#91a09b"
            Behavior on color {
                ColorAnimation { duration: root.motion.mediumDuration }
            }

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

    MouseArea {
        id: pointer
        anchors.fill: parent
        z: 2
        enabled: root.enabled
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onPressed: root.forceActiveFocus()
        onClicked: root.toggled(!root.checked)
    }
    Keys.onSpacePressed: event => {
        if (!root.enabled) return
        root.toggled(!root.checked)
        event.accepted = true
    }

    Behavior on opacity {
        NumberAnimation { duration: root.motion.shortDuration }
    }
}
