import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

Item {
    id: root
    anchors.fill: parent
    visible: CortexPrompt.active
    z: 1100

    property bool timedTest: CortexPrompt.kind === "timed_test"
    property bool answerReady: !timedTest || CortexPrompt.remainingMs <= 0

    onVisibleChanged: {
        if (visible) {
            answerField.text = ""
            if (answerReady && timedTest) answerField.forceActiveFocus()
        }
    }
    onAnswerReadyChanged: {
        if (visible && answerReady && timedTest) answerField.forceActiveFocus()
    }

    Rectangle {
        anchors.fill: parent
        color: "#99000000"
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        hoverEnabled: true
        onWheel: function(wheel) { wheel.accepted = true }
    }

    Rectangle {
        width: Math.min(560, parent.width - 48)
        implicitHeight: content.implicitHeight + 32
        height: implicitHeight
        anchors.centerIn: parent
        color: Theme.surfaceRaised
        border.width: 1
        border.color: Theme.borderStrong
        radius: Theme.radius

        ColumnLayout {
            id: content
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 16
            spacing: 12

            RowLayout {
                Layout.fillWidth: true
                Label {
                    text: root.timedTest ? "Human test requested" : "Human action requested"
                    color: Theme.textBright
                    font.family: Theme.uiFont
                    font.pixelSize: 13
                    font.bold: true
                }
                Item { Layout.fillWidth: true }
                Label {
                    text: "Prompt #" + CortexPrompt.promptId
                    color: Theme.textDisabled
                    font.family: Theme.monoFont
                    font.pixelSize: 10
                }
            }

            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

            Text {
                visible: root.timedTest
                Layout.fillWidth: true
                text: CortexPrompt.message
                color: Theme.text
                font.family: Theme.uiFont
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }

            ColumnLayout {
                visible: !root.timedTest
                Layout.fillWidth: true
                spacing: 6
                Label {
                    text: CortexPrompt.label
                    color: Theme.text
                    font.family: Theme.uiFont
                    font.pixelSize: 12
                    font.bold: true
                }
                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        text: CortexPrompt.currentValue.length > 0 ? CortexPrompt.currentValue : "Current value not provided"
                        color: CortexPrompt.currentValue.length > 0 ? Theme.textMuted : Theme.textDisabled
                        font.family: Theme.monoFont
                        font.pixelSize: 11
                    }
                    Label { text: "→"; color: Theme.textDisabled; font.pixelSize: 13 }
                    Label {
                        text: CortexPrompt.targetValue
                        color: Theme.success
                        font.family: Theme.monoFont
                        font.pixelSize: 11
                        font.bold: true
                    }
                    Item { Layout.fillWidth: true }
                }
            }

            Rectangle {
                visible: root.timedTest && !root.answerReady
                Layout.fillWidth: true
                Layout.preferredHeight: 34
                color: Theme.surface
                border.width: 1
                border.color: Theme.border
                radius: Theme.radius
                Label {
                    anchors.centerIn: parent
                    text: "Answer unlocks in " + Math.max(1, Math.ceil(CortexPrompt.remainingMs / 1000)) + "s"
                    color: Theme.warning
                    font.family: Theme.uiFont
                    font.pixelSize: 11
                }
            }

            TextField {
                id: answerField
                visible: root.timedTest
                Layout.fillWidth: true
                enabled: root.answerReady
                placeholderText: CortexPrompt.answerType === "number" ? "Enter the measured number" : "Enter the result"
                inputMethodHints: CortexPrompt.answerType === "number" ? Qt.ImhFormattedNumbersOnly : Qt.ImhNone
                Keys.onReturnPressed: {
                    if (submitButton.enabled) CortexPrompt.answer(answerField.text)
                }
            }

            Label {
                visible: CortexPrompt.lastError.length > 0
                Layout.fillWidth: true
                text: CortexPrompt.lastError
                color: Theme.error
                font.family: Theme.monoFont
                font.pixelSize: 10
                wrapMode: Text.Wrap
            }

            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Button {
                    id: submitButton
                    text: root.timedTest ? "Submit result" : "Done"
                    enabled: root.timedTest
                             ? root.answerReady && answerField.text.trim().length > 0
                             : true
                    onClicked: CortexPrompt.answer(root.timedTest ? answerField.text : "ack")
                }
            }
        }
    }
}
