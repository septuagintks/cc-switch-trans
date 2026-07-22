import QtQuick
import QtQuick.Controls.Basic

Item {
    id: root

    required property var motion
    required property string stableKey
    required property string profileName
    required property bool profileEnabled
    required property int ruleCount
    required property bool selected
    required property bool modelSyncActive
    signal chosen(string stableKey)

    height: 54

    Item {
        id: visual
        anchors.fill: parent
        anchors.margins: 3
        scale: tap.pressed ? 0.985 : (hover.hovered ? 1.008 : 1.0)

        Behavior on scale {
            enabled: !root.modelSyncActive
            NumberAnimation {
                duration: root.motion.shortDuration
                easing.type: Easing.OutCubic
            }
        }

        Rectangle {
            anchors.fill: parent
            radius: 7
            color: root.selected ? "#dceef8" : (hover.hovered ? "#edf2f4" : "transparent")
            border.width: root.selected ? 1 : 0
            border.color: "#7ab6d6"
            Behavior on color {
                enabled: !root.modelSyncActive
                ColorAnimation { duration: root.motion.shortDuration }
            }
        }

        Rectangle {
            width: 8
            height: 8
            radius: 4
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            color: root.profileEnabled ? "#16875b" : "#a5afb4"
            Behavior on color {
                enabled: !root.modelSyncActive
                ColorAnimation { duration: root.motion.shortDuration }
            }
        }

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 30
            anchors.right: countText.left
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            text: root.profileName
            color: "#172126"
            elide: Text.ElideRight
            font.pixelSize: 13
            renderType: Text.NativeRendering
        }

        Text {
            id: countText
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            text: root.ruleCount
            color: "#607078"
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
    }

    HoverHandler { id: hover }
    TapHandler {
        id: tap
        onTapped: root.chosen(root.stableKey)
    }
}
