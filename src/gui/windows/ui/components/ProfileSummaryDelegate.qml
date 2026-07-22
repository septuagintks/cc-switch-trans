import QtQuick
import CcsTrans.Gui

Item {
    id: root

    required property var motion
    required property string stableKey
    required property string profileId
    required property bool profileEnabled
    required property string profileProtocol
    required property string readiness
    required property var ruleCount
    required property bool selected
    signal chosen(string stableKey)

    height: 58

    Item {
        anchors.fill: parent
        anchors.margins: 3
        scale: tap.pressed ? 0.985 : (hover.hovered ? 1.008 : 1)
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
                                 : (hover.hovered ? Theme.surfaceMuted : "transparent")
            border.width: root.selected ? 1 : 0
            border.color: "#a8c9bc"
            Behavior on color {
                ColorAnimation { duration: root.motion.shortDuration }
            }
        }

        Rectangle {
            width: 7
            height: 7
            radius: 3.5
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            color: root.profileEnabled ? Theme.accent : Theme.borderStrong
        }

        Column {
            anchors.left: parent.left
            anchors.leftMargin: 30
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2

            Text {
                width: parent.width
                text: root.profileId
                color: Theme.text
                font.pixelSize: 13
                font.bold: root.selected
                elide: Text.ElideRight
                renderType: Text.NativeRendering
            }
            Text {
                width: parent.width
                text: (root.profileProtocol.length > 0
                       ? root.profileProtocol : root.readiness)
                      + "  ·  " + root.ruleCount + " rules"
                color: Theme.textMuted
                font.pixelSize: 11
                elide: Text.ElideRight
                renderType: Text.NativeRendering
            }
        }
    }

    HoverHandler { id: hover }
    TapHandler {
        id: tap
        onTapped: root.chosen(root.stableKey)
    }
}
