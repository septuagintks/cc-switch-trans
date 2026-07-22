import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import CcsTrans.Gui
import "components" as Components

ApplicationWindow {
    id: window

    width: 960
    height: 620
    minimumWidth: 780
    minimumHeight: 500
    visible: true
    title: "ccs-trans"
    color: "#f4f6f7"

    Rectangle {
        anchors.fill: parent
        color: "#f4f6f7"

        RowLayout {
            anchors.fill: parent
            anchors.margins: 18
            spacing: 18

            Rectangle {
                Layout.preferredWidth: 300
                Layout.fillHeight: true
                radius: 8
                color: "#ffffff"
                border.width: 1
                border.color: "#c7d1d5"

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 10

                    Text {
                        text: "Profiles"
                        color: "#172126"
                        font.pixelSize: 18
                        font.bold: true
                        renderType: Text.NativeRendering
                    }

                    ListView {
                        id: profileList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        reuseItems: true
                        boundsBehavior: Flickable.StopAtBounds
                        model: profileModel
                        currentIndex: prototypeController.selectedIndex

                        delegate: Components.ProfileDelegate {
                            width: ListView.view.width
                            motion: motionPolicy
                            selected: stableKey === prototypeController.selectedKey
                            modelSyncActive: prototypeController.stressRunning
                            onChosen: key => prototypeController.setSelectedKey(key)
                        }

                        add: Transition {
                            enabled: !motionPolicy.reduceMotion &&
                                     !prototypeController.stressRunning
                            NumberAnimation {
                                properties: "opacity"
                                from: 0
                                to: 1
                                duration: motionPolicy.mediumDuration
                            }
                        }
                        moveDisplaced: Transition {
                            enabled: !motionPolicy.reduceMotion &&
                                     !prototypeController.stressRunning
                            NumberAnimation {
                                properties: "x,y"
                                duration: motionPolicy.movementDuration
                                easing.type: Easing.OutCubic
                            }
                        }

                        ScrollBar.vertical: ScrollBar {
                            id: profileScrollBar
                            policy: ScrollBar.AsNeeded
                            opacity: active || hovered ? 1 : 0
                            contentItem: Rectangle {
                                implicitWidth: 6
                                radius: 3
                                color: profileScrollBar.hovered ? "#65777f" : "#93a2a8"
                                Behavior on color {
                                    ColorAnimation { duration: motionPolicy.shortDuration }
                                }
                            }
                            background: Item {}
                            Behavior on opacity {
                                NumberAnimation { duration: motionPolicy.mediumDuration }
                            }
                        }
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 14

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    Text {
                        Layout.fillWidth: true
                        text: "ccs-trans"
                        color: "#172126"
                        font.pixelSize: 22
                        font.bold: true
                        renderType: Text.NativeRendering
                    }

                    Components.MotionSwitch {
                        motion: motionPolicy
                        text: "Reduce motion"
                        checked: motionPolicy.reduceMotion
                        onToggled: value => motionPolicy.reduceMotion = value
                    }

                    Components.MotionButton {
                        motion: motionPolicy
                        text: prototypeController.stressRunning ? "Updating..." : "Run model probe"
                        enabled: !prototypeController.stressRunning
                        onClicked: prototypeController.startStress(4096)
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: 8
                    color: "#ffffff"
                    border.width: 1
                    border.color: "#c7d1d5"

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 24
                        spacing: 16

                        Text {
                            text: prototypeController.selectedIndex >= 0
                                  ? "Selected profile " + (prototypeController.selectedIndex + 1)
                                  : "No profile selected"
                            color: "#172126"
                            font.pixelSize: 18
                            font.bold: true
                            renderType: Text.NativeRendering
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: 1
                            color: "#d8dfe2"
                        }

                        GridLayout {
                            columns: 2
                            columnSpacing: 22
                            rowSpacing: 12

                            Text { text: "Profiles"; color: "#607078" }
                            Text { text: profileModel.count; color: "#172126" }
                            Text { text: "Rules"; color: "#607078" }
                            Text { text: profileModel.totalRuleCount(); color: "#172126" }
                            Text { text: "Mutations"; color: "#607078" }
                            Text {
                                text: prototypeController.completedMutations
                                color: "#172126"
                            }
                        }

                        Item { Layout.fillHeight: true }

                        Text {
                            Layout.fillWidth: true
                            text: prototypeController.stressRunning
                                  ? "Applying incremental model updates"
                                  : "Model ready"
                            color: prototypeController.stressRunning ? "#a55a00" : "#16875b"
                            horizontalAlignment: Text.AlignRight
                            renderType: Text.NativeRendering
                            Behavior on color {
                                ColorAnimation { duration: motionPolicy.shortDuration }
                            }
                        }
                    }
                }
            }
        }
    }
}
