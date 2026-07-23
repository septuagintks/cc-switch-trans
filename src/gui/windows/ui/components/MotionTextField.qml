import QtQuick
import QtQuick.Controls.Basic
import CcsTrans.Gui

TextField {
    id: root

    required property var motion
    property bool invalid: false

    implicitHeight: Theme.controlHeight
    leftPadding: 12
    rightPadding: 12
    topPadding: 0
    bottomPadding: 0
    verticalAlignment: TextInput.AlignVCenter
    color: Theme.text
    placeholderTextColor: Theme.textMuted
    selectionColor: Theme.blue
    selectedTextColor: "#ffffff"
    font.pixelSize: 13
    renderType: Text.NativeRendering
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true

    background: Rectangle {
        scale: root.activeFocus ? 1.002 : (root.hovered ? 1.004 : 1)
        radius: Theme.radius
        color: root.enabled ? Theme.surface : Theme.surfaceMuted
        border.width: root.activeFocus || root.motion.highContrast ? 2 : 1
        border.color: root.invalid ? Theme.danger
                                  : (root.activeFocus
                                     ? Theme.focus : Theme.border)
        Behavior on border.color {
            ColorAnimation { duration: root.motion.shortDuration }
        }
        Behavior on color {
            ColorAnimation { duration: root.motion.shortDuration }
        }
        Behavior on scale {
            NumberAnimation {
                duration: root.motion.shortDuration
                easing.type: Easing.OutCubic
            }
        }
    }

    HoverHandler { cursorShape: Qt.IBeamCursor }
}
