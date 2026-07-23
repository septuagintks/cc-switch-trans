import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import CcsTrans.Gui

Popup {
    id: root

    required property var motion
    property string titleText
    property string messageText
    property string detailText
    property string primaryText: "Continue"
    property string secondaryText
    property string cancelText: "Cancel"
    property bool primaryDanger: false
    property bool primaryEnabled: true
    property bool secondaryVisible: secondaryText.length > 0
    property bool cancelVisible: true
    signal primaryTriggered()
    signal secondaryTriggered()
    signal cancelled()

    parent: Overlay.overlay
    anchors.centerIn: Overlay.overlay
    width: Math.min(460, Overlay.overlay.width - 48)
    modal: true
    focus: true
    padding: 20
    closePolicy: Popup.NoAutoClose

    Overlay.modal: Rectangle {
        color: "#66000000"
        Behavior on opacity {
            NumberAnimation { duration: root.motion.shortDuration }
        }
    }

    contentItem: ColumnLayout {
        spacing: 12

        Text {
            Layout.fillWidth: true
            text: root.titleText
            color: Theme.text
            font.pixelSize: 17
            font.bold: true
            wrapMode: Text.WordWrap
            renderType: Text.NativeRendering
        }

        Text {
            Layout.fillWidth: true
            text: root.messageText
            color: Theme.text
            font.pixelSize: 13
            wrapMode: Text.WordWrap
            visible: text.length > 0
            renderType: Text.NativeRendering
        }

        Text {
            Layout.fillWidth: true
            text: root.detailText
            color: Theme.textMuted
            font.pixelSize: 11
            wrapMode: Text.WrapAnywhere
            visible: text.length > 0
            renderType: Text.NativeRendering
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 4
            spacing: 8
            Item { Layout.fillWidth: true }
            MotionButton {
                motion: root.motion
                secondary: true
                text: root.cancelText
                visible: root.cancelVisible
                onClicked: root.cancelled()
            }
            MotionButton {
                motion: root.motion
                secondary: true
                text: root.secondaryText
                visible: root.secondaryVisible
                onClicked: root.secondaryTriggered()
            }
            MotionButton {
                motion: root.motion
                danger: root.primaryDanger
                text: root.primaryText
                enabled: root.primaryEnabled
                onClicked: root.primaryTriggered()
            }
        }
    }

    background: Rectangle {
        radius: Theme.radius
        color: Theme.surface
        border.width: motion.highContrast ? 2 : 1
        border.color: motion.highContrast ? Theme.text : Theme.borderStrong
    }

    enter: Transition {
        NumberAnimation {
            property: "opacity"
            from: 0
            to: 1
            duration: root.motion.shortDuration
        }
        NumberAnimation {
            property: "scale"
            from: 0.97
            to: 1
            duration: root.motion.shortDuration
            easing.type: Easing.OutCubic
        }
    }
    exit: Transition {
        NumberAnimation {
            property: "opacity"
            to: 0
            duration: root.motion.shortDuration
        }
    }

    Keys.onEscapePressed: event => {
        if (root.cancelVisible) root.cancelled()
        event.accepted = true
    }
}
