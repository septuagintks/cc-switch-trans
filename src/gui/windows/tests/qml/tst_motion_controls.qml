import QtQuick
import QtQuick.Controls.Basic
import QtTest
import "../../ui/components" as Components

Item {
    id: scene
    width: 480
    height: 220

    QtObject {
        id: motion
        property bool reduceMotion: false
        property bool highContrast: false
        property int shortDuration: reduceMotion ? 0 : 40
        property int mediumDuration: reduceMotion ? 0 : 60
        property int movementDuration: reduceMotion ? 0 : 80
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
    }
}
