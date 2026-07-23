import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import CcsTrans.Gui
import "." as Components

Item {
    id: root

    required property var editor
    required property var motion
    required property string fieldKey
    required property string displayName
    required property string inputKind
    required property string draftText
    required property var draftValue
    required property var enumValues
    required property bool fieldRequired
    required property string fieldError
    required property string applyImpact
    readonly property var enumChoices: fieldRequired
        ? enumValues : ["Not set"].concat(enumValues)
    readonly property var valueControl: valueLoader.item

    implicitHeight: fieldError.length > 0 ? 78 : 52

    RowLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 48
        spacing: 12

        ColumnLayout {
            Layout.preferredWidth: 220
            Layout.minimumWidth: 150
            Layout.fillHeight: true
            spacing: 1

            Text {
                Layout.fillWidth: true
                text: root.displayName
                color: Theme.text
                font.pixelSize: 12
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
                renderType: Text.NativeRendering
            }
            Text {
                Layout.fillWidth: true
                text: root.applyImpact === "service_restart" ? "Service restart" : "Runtime reload"
                color: Theme.textMuted
                font.pixelSize: 10
                elide: Text.ElideRight
                renderType: Text.NativeRendering
            }
        }

        Loader {
            id: valueLoader
            Layout.fillWidth: true
            Layout.minimumWidth: 160
            Layout.preferredWidth: Math.max(160, root.width - 232)
            Layout.preferredHeight: Theme.controlHeight
            sourceComponent: root.inputKind === "boolean"
                ? booleanEditor
                : (root.inputKind === "enumeration" ? enumEditor : textEditor)
        }
    }

    Text {
        anchors.left: parent.left
        anchors.leftMargin: 232
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.topMargin: 48
        text: root.fieldError
        color: Theme.danger
        font.pixelSize: 11
        elide: Text.ElideRight
        visible: text.length > 0
        renderType: Text.NativeRendering
    }

    Component {
        id: textEditor
        Components.MotionTextField {
            anchors.fill: parent
            motion: root.motion
            text: root.draftText
            invalid: root.fieldError.length > 0
            Accessible.name: root.displayName
            onTextEdited: root.editor.setFieldValue(root.fieldKey, text)
        }
    }

    Component {
        id: enumEditor
        Components.MotionComboBox {
            anchors.fill: parent
            motion: root.motion
            model: root.enumChoices
            invalid: root.fieldError.length > 0
            currentIndex: root.fieldRequired
                ? Math.max(0, root.enumValues.indexOf(root.draftText))
                : (root.draftText.length === 0
                   ? 0 : Math.max(1, root.enumValues.indexOf(root.draftText) + 1))
            Accessible.name: root.displayName
            onActivated: index => {
                if (!root.fieldRequired && index === 0)
                    root.editor.resetFieldValue(root.fieldKey)
                else
                    root.editor.setFieldValue(
                        root.fieldKey,
                        root.enumValues[index - (root.fieldRequired ? 0 : 1)])
            }
        }
    }

    Component {
        id: booleanEditor
        Components.MotionSwitch {
            anchors.fill: parent
            motion: root.motion
            text: root.draftValue ? "Enabled" : "Disabled"
            checked: Boolean(root.draftValue)
            invalid: root.fieldError.length > 0
            Accessible.name: root.displayName
            onToggled: value => root.editor.setFieldValue(root.fieldKey, value)
        }
    }
}
