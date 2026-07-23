import QtQuick
import QtQuick.Controls.Basic
import CcsTrans.Gui

ScrollBar {
    id: root

    required property var motion

    policy: ScrollBar.AsNeeded
    hoverEnabled: true
    interactive: true
    opacity: active || hovered || pressed ? 1 : 0
    minimumSize: 0.08

    contentItem: Rectangle {
        implicitWidth: 7
        implicitHeight: 7
        radius: 3.5
        color: root.pressed || root.hovered ? Theme.borderStrong : Theme.border
        Behavior on color {
            ColorAnimation { duration: root.motion.shortDuration }
        }
    }
    background: Item {}

    Behavior on opacity {
        NumberAnimation { duration: root.motion.mediumDuration }
    }
}
