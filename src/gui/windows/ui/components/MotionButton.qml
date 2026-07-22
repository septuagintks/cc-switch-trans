import QtQuick
import QtQuick.Controls.Basic

Button {
    id: root

    required property var motion
    property color baseColor: motion.highContrast ? palette.button : "#263238"
    property color hoverColor: motion.highContrast ? palette.highlight : "#37474f"
    property color pressedColor: motion.highContrast ? palette.highlight : "#182126"
    property color textColor: motion.highContrast ? palette.buttonText : "#f7f9fa"
    property bool visualHovered: hovered
    property bool visualPressed: down

    implicitWidth: 148
    implicitHeight: 38
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    background: null

    contentItem: Item {
        id: visual
        scale: root.visualPressed ? 0.97 : (root.visualHovered ? 1.015 : 1.0)

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
                                      : (root.visualHovered ? root.hoverColor : root.baseColor)
            border.width: root.visualFocus ? 2 : 1
            border.color: root.visualFocus ? "#38a3ff" : "#52646d"

            Behavior on color {
                ColorAnimation { duration: root.motion.shortDuration }
            }
            Behavior on border.color {
                ColorAnimation { duration: root.motion.shortDuration }
            }
        }

        Text {
            anchors.centerIn: parent
            text: root.text
            color: root.textColor
            font.pixelSize: 13
            renderType: Text.NativeRendering
        }
    }
}
