import QtQuick
import QtQuick.Controls.Basic
import CcsTrans.Gui
import "." as Components

ComboBox {
    id: root

    required property var motion
    property bool invalid: false

    implicitHeight: Theme.controlHeight
    hoverEnabled: true
    wheelEnabled: false
    focusPolicy: Qt.StrongFocus
    contentItem: Text {
        leftPadding: 12
        rightPadding: 30
        text: root.displayText
        color: root.enabled ? Theme.text : Theme.textMuted
        font.pixelSize: 13
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        renderType: Text.NativeRendering
    }

    indicator: Text {
        x: root.width - width - 12
        y: (root.height - height) / 2
        text: "v"
        color: root.down ? Theme.accent : Theme.textMuted
        font.pixelSize: 13
        renderType: Text.NativeRendering
    }

    background: Rectangle {
        scale: root.down ? 0.995 : (root.hovered ? 1.004 : 1)
        radius: Theme.radius
        color: root.enabled
               ? (root.hovered ? Theme.surfaceMuted : Theme.surface)
               : Theme.surfaceMuted
        border.width: root.activeFocus || root.motion.highContrast ? 2 : 1
        border.color: root.invalid ? Theme.danger
                                   : (root.activeFocus ? Theme.focus : Theme.border)
        Behavior on color {
            ColorAnimation { duration: root.motion.shortDuration }
        }
        Behavior on border.color {
            ColorAnimation { duration: root.motion.shortDuration }
        }
        Behavior on scale {
            NumberAnimation {
                duration: root.motion.shortDuration
                easing.type: Easing.OutCubic
            }
        }
    }

    delegate: ItemDelegate {
        id: option
        required property int index
        required property string modelData
        readonly property bool currentSelection: root.currentIndex === index
        readonly property int highlightLevel: currentSelection ? 2 : (hovered ? 1 : 0)
        width: root.width - 8
        height: 34
        leftPadding: 10
        rightPadding: 10
        highlighted: root.highlightedIndex === index
        hoverEnabled: true
        contentItem: Text {
            text: option.modelData
            color: Theme.text
            font.pixelSize: 13
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            renderType: Text.NativeRendering
        }
        background: Rectangle {
            radius: Theme.radius - 1
            color: option.highlightLevel === 2
                   ? Theme.selection
                   : (option.highlightLevel === 1 ? Theme.surfaceMuted : "transparent")
            Behavior on color {
                ColorAnimation { duration: root.motion.shortDuration }
            }
        }
    }

    popup: Popup {
        y: root.height + 4
        width: root.width
        padding: 4
        implicitHeight: Math.min(contentItem.implicitHeight + padding * 2, 240)
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: root.delegateModel
            currentIndex: root.highlightedIndex
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: Components.MotionScrollBar { motion: root.motion }
        }
        background: Rectangle {
            radius: Theme.radius
            color: Theme.surface
            border.width: 1
            border.color: Theme.borderStrong
        }
        enter: Transition {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: root.motion.shortDuration }
            NumberAnimation { property: "scale"; from: 0.98; to: 1; duration: root.motion.shortDuration }
        }
        exit: Transition {
            NumberAnimation { property: "opacity"; to: 0; duration: root.motion.shortDuration }
        }
    }
}
