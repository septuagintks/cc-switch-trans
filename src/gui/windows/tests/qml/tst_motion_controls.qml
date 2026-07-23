import QtQuick
import QtQuick.Controls.Basic
import QtTest
import "../../ui/components" as Components

Item {
    id: scene
    width: 480
    height: 300

    QtObject {
        id: motion
        property bool reduceMotion: false
        property bool highContrast: false
        property int shortDuration: reduceMotion ? 0 : 40
        property int mediumDuration: reduceMotion ? 0 : 60
        property int movementDuration: reduceMotion ? 0 : 80
    }

    QtObject {
        id: fieldEditor
        function setFieldValue(key, value) {}
        function resetFieldValue(key) {}
    }

    Components.MotionButton {
        id: button
        x: 40
        y: 32
        width: implicitWidth
        height: implicitHeight
        text: "Save"
        motion: motion
    }

    Components.MotionSwitch {
        id: motionSwitch
        x: 40
        y: 110
        width: implicitWidth
        height: implicitHeight
        text: "Reduce motion"
        motion: motion
    }

    Components.MotionComboBox {
        id: combo
        x: 230
        y: 32
        width: 210
        model: ["responses", "chat completions", "usage"]
        currentIndex: 0
        motion: motion
    }

    Components.FieldEditorRow {
        id: emptyField
        x: 40
        y: 180
        width: 400
        editor: fieldEditor
        motion: motion
        fieldKey: "local.usage-path"
        displayName: "Local usage path"
        inputKind: "text"
        draftText: ""
        draftValue: undefined
        enumValues: []
        fieldRequired: false
        fieldError: ""
        applyImpact: "runtime_reload"
    }

    SignalSpy {
        id: buttonClickSpy
        target: button
        signalName: "clicked"
    }

    SignalSpy {
        id: switchToggleSpy
        target: motionSwitch
        signalName: "toggled"
    }

    TestCase {
        id: testCase
        name: "MotionControls"
        when: windowShown

        function init() {
            motion.reduceMotion = false;
            motionSwitch.checked = false;
            combo.currentIndex = 0;
            combo.popup.close();
            button.visualHovered = false;
            button.visualPressed = false;
            buttonClickSpy.clear();
            switchToggleSpy.clear();
            mouseMove(scene, scene.width - 2, scene.height - 2);
            waitForRendering(scene);
            wait(motion.mediumDuration + 10);
        }

        function test_hoverAndPressDoNotChangeLayout() {
            const geometry = [button.x, button.y, button.width, button.height];
            button.visualHovered = true;
            wait(motion.shortDuration + 10);
            verify(button.contentItem.scale > 1.0);
            compare([button.x, button.y, button.width, button.height], geometry);

            button.visualPressed = true;
            wait(motion.shortDuration + 10);
            verify(button.contentItem.scale < 1.0);
            button.visualPressed = false;
            button.clicked();
            compare(buttonClickSpy.count, 1);
            compare([button.x, button.y, button.width, button.height], geometry);
        }

        function test_reduceMotionKeepsFinalState() {
            motion.reduceMotion = true;
            button.visualHovered = true;
            wait(1);
            verify(Math.abs(button.contentItem.scale - 1.015) < 0.0001);

            motionSwitch.checked = true;
            wait(1);
            verify(motionSwitch.checked);
            verify(motionSwitch.visualKnobX > 3);
        }

        function test_switchRequestsStateWithoutBreakingOwnerBinding() {
            motionSwitch.checked = false;
            mouseClick(motionSwitch, motionSwitch.width - 12, motionSwitch.height / 2);
            tryCompare(switchToggleSpy, "count", 1);
            compare(switchToggleSpy.signalArguments[0][0], true);
            compare(motionSwitch.checked, false);

            switchToggleSpy.clear();
            motionSwitch.forceActiveFocus();
            keyClick(Qt.Key_Space);
            compare(switchToggleSpy.count, 1);
            compare(switchToggleSpy.signalArguments[0][0], true);
            compare(motionSwitch.checked, false);
        }

        function test_comboIgnoresWheelAndPopupFinishesOpen() {
            verify(!combo.wheelEnabled);
            compare(combo.currentIndex, 0);
            mouseMove(combo, combo.width / 2, combo.height / 2);
            mouseWheel(
                combo,
                combo.width / 2,
                combo.height / 2,
                0,
                -120,
                Qt.NoButton,
                Qt.NoModifier);
            compare(combo.currentIndex, 0);

            mouseClick(combo, combo.width / 2, combo.height / 2);
            tryCompare(combo.popup, "opened", true);
            wait(motion.mediumDuration + 20);
            verify(combo.popup.opened);
            verify(Math.abs(combo.popup.opacity - 1) < 0.0001);
            verify(Math.abs(combo.popup.scale - 1) < 0.0001);
        }

        function test_comboKeepsSelectionAndHoverHighlightsSeparate() {
            mouseClick(combo, combo.width / 2, combo.height / 2);
            tryCompare(combo.popup, "opened", true);

            const list = combo.popup.contentItem;
            tryVerify(() => list.itemAtIndex(0) !== null);
            tryVerify(() => list.itemAtIndex(1) !== null);
            const selected = list.itemAtIndex(0);
            const hovered = list.itemAtIndex(1);
            compare(selected.highlightLevel, 2);
            compare(hovered.highlightLevel, 0);

            mouseMove(hovered, hovered.width / 2, hovered.height / 2);
            tryCompare(hovered, "hovered", true);
            compare(selected.highlightLevel, 2);
            compare(hovered.highlightLevel, 1);

            mouseMove(scene, scene.width - 2, scene.height - 2);
            tryCompare(hovered, "hovered", false);
            compare(selected.highlightLevel, 2);
            compare(hovered.highlightLevel, 0);

            mouseClick(hovered, hovered.width / 2, hovered.height / 2);
            tryCompare(combo, "currentIndex", 1);
            tryCompare(combo.popup, "opened", false);
            wait(motion.shortDuration + 10);
            mouseClick(combo, combo.width / 2, combo.height / 2);
            tryCompare(combo.popup, "opened", true);
            tryVerify(() => combo.popup.contentItem.itemAtIndex(1) !== null);
            compare(combo.popup.contentItem.itemAtIndex(1).highlightLevel, 2);
        }

        function test_emptyFieldFillsEditorColumn() {
            verify(emptyField.valueControl !== null);
            verify(emptyField.valueControl.width >= 160);
            compare(emptyField.valueControl.height, 36);
        }
    }
}
