import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import CcsTrans.Gui
import "components" as Components

ApplicationWindow {
    width: 960
    height: 620
    minimumWidth: 780
    minimumHeight: 500
    visible: true
    title: "ccs-trans"
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
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 14

            RowLayout {
                Layout.fillWidth: true

                Text {
                    Layout.fillWidth: true
                    text: "ccs-trans"
                    color: "#172126"
                    font.pixelSize: 22
                    font.bold: true
                    renderType: Text.NativeRendering
                }

                Components.MotionButton {
                    motion: motionPolicy
                    text: prototypeController.stressRunning
                          ? "Updating..." : "Run model probe"
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

                    Text {
                        text: prototypeController.selectedIndex >= 0
                              ? "Selected profile "
                                + (prototypeController.selectedIndex + 1)
                              : "No profile selected"
                        color: "#172126"
                        font.pixelSize: 18
                        font.bold: true
                        renderType: Text.NativeRendering
                    }

                    Item { Layout.fillHeight: true }

                    Text {
                        Layout.fillWidth: true
                        text: prototypeController.stressRunning
                              ? "Applying incremental model updates" : "Model ready"
                        color: prototypeController.stressRunning
                               ? "#a55a00" : "#16875b"
                        horizontalAlignment: Text.AlignRight
                        renderType: Text.NativeRendering
                    }
                }
            }
        }
    }
}
