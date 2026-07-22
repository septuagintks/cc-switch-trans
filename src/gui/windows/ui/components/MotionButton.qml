import QtQuick
import QtQuick.Controls.Basic

Button {
    id: root

    required property var motion
    property bool secondary: false
    property bool danger: false
    property bool compact: false
    property color baseColor: danger ? "#a53f4b"
                                      : (secondary ? "#e9eeec" : "#147d64")
    property color hoverColor: danger ? "#ba4b58"
                                       : (secondary ? "#dce5e1" : "#1b9175")
    property color pressedColor: danger ? "#8d303b"
                                         : (secondary ? "#cbd7d2" : "#0f6954")
    property color textColor: secondary ? "#18211f" : "#ffffff"
    property bool visualHovered: hovered
    property bool visualPressed: down

    implicitWidth: compact ? 36 : 116
    implicitHeight: 36
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    opacity: enabled ? 1 : 0.48
    background: null

    contentItem: Item {
        id: visual
        scale: root.visualPressed ? 0.97 : (root.visualHovered ? 1.015 : 1)

        Behavior on scale {
            NumberAnimation {
                duration: root.motion.shortDuration
                easing.type: Easing.OutCubic
            }
        }

        Rectangle {
            anchors.fill: parent
            radius: 7
            color: root.visualPressed ? root.pressedColor
                                      : (root.visualHovered
                                         ? root.hoverColor : root.baseColor)
            border.width: root.visualFocus ? 2 : 1
            border.color: root.visualFocus ? "#218bd0"
                                           : (root.secondary
                                              ? "#c6d0cc" : root.baseColor)

            Behavior on color {
                ColorAnimation { duration: root.motion.shortDuration }
            }
            Behavior on border.color {
                ColorAnimation { duration: root.motion.shortDuration }
            }
        }

        Text {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            text: root.text
            color: root.textColor
            font.pixelSize: 13
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            renderType: Text.NativeRendering
        }
    }

    Behavior on opacity {
        NumberAnimation { duration: root.motion.shortDuration }
    }
}
