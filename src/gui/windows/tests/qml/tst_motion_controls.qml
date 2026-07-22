import QtQuick
import QtQuick.Controls.Basic
import QtTest
import "../../ui/components" as Components

TestCase {
    id: testCase
    name: "MotionControls"
    when: windowShown
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
        text: "Save"
        motion: motion
    }

    Components.MotionSwitch {
        id: motionSwitch
        x: 40
        y: 110
        text: "Reduce motion"
        motion: motion
    }

    SignalSpy {
        id: buttonClickSpy
        target: button
        signalName: "clicked"
    }

    function init() {
        motion.reduceMotion = false
        motionSwitch.checked = false
        button.visualHovered = false
        button.visualPressed = false
        buttonClickSpy.clear()
        mouseMove(testCase, width - 2, height - 2)
        waitForRendering(testCase)
        wait(motion.mediumDuration + 10)
    }

    function test_hoverAndPressDoNotChangeLayout() {
        const geometry = [button.x, button.y, button.width, button.height]
        button.visualHovered = true
        wait(motion.shortDuration + 10)
        verify(button.contentItem.scale > 1.0)
        compare([button.x, button.y, button.width, button.height], geometry)

        button.visualPressed = true
        wait(motion.shortDuration + 10)
        verify(button.contentItem.scale < 1.0)
        button.visualPressed = false
        button.clicked()
        compare(buttonClickSpy.count, 1)
        compare([button.x, button.y, button.width, button.height], geometry)
    }

    function test_reduceMotionKeepsFinalState() {
        motion.reduceMotion = true
        button.visualHovered = true
        wait(1)
        verify(Math.abs(button.contentItem.scale - 1.015) < 0.0001)

        motionSwitch.checked = true
        wait(1)
        verify(motionSwitch.checked)
        verify(motionSwitch.visualKnobX > 3)
    }
}
