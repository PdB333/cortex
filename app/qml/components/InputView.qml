import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

ColumnLayout {
    id: root
    anchors.fill: parent
    spacing: 0

    property bool backgroundText: false
    property bool sequenceRunning: CortexFeatures.inputSequenceStatus === "pending"
                                   || CortexFeatures.inputSequenceStatus === "running"
                                   || CortexFeatures.inputSequenceStatus === "cancelling"

    Timer {
        interval: 300
        repeat: true
        running: root.sequenceRunning && CortexApp.sessionActive
        onTriggered: CortexFeatures.refreshInputSequence()
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 44
        color: Theme.background
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 5
            TextField { id: vkField; Layout.preferredWidth: 100; placeholderText: "VK (0x20)" }
            TextField { id: holdField; Layout.preferredWidth: 90; text: "50"; placeholderText: "Hold ms" }
            Button {
                text: "Tap key"
                enabled: CortexApp.sessionActive && CortexApp.mutationPermission
                onClicked: CortexFeatures.sendKeyTap(parseInt(vkField.text), parseInt(holdField.text))
            }
            Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 18; color: Theme.borderStrong }
            Button {
                text: CortexFeatures.inputRecording ? "Stop recording" : "Record input"
                enabled: CortexApp.sessionActive && CortexApp.mutationPermission && !root.sequenceRunning
                onClicked: CortexFeatures.inputRecording ? CortexFeatures.stopInputRecording() : CortexFeatures.startInputRecording()
            }
            Item { Layout.fillWidth: true }
            Label {
                text: CortexFeatures.inputRecording ? "Recording user input" : "Input control"
                color: CortexFeatures.inputRecording ? Theme.mutation : Theme.textMuted
                font.pixelSize: 10
            }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 44
        color: Theme.panel
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 6
            Label { text: "Replay"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
            ComboBox {
                id: replayMode
                Layout.preferredWidth: 120
                model: ["os", "game", "dinput"]
                font.pixelSize: 10
            }
            Button {
                text: "Run recorded"
                enabled: CortexApp.sessionActive && CortexApp.mutationPermission
                         && CortexFeatures.inputRecordingJson.length > 0 && !root.sequenceRunning
                onClicked: CortexFeatures.replayRecordedInput(replayMode.currentText)
            }
            Button {
                text: "Refresh job"
                enabled: CortexApp.sessionActive && CortexFeatures.inputSequenceJobId > 0
                onClicked: CortexFeatures.refreshInputSequence()
            }
            Button {
                text: CortexFeatures.inputSequenceStatus === "cancelling" ? "Cancelling..." : "Cancel job"
                enabled: CortexApp.sessionActive && CortexApp.mutationPermission
                         && CortexFeatures.inputSequenceJobId > 0 && root.sequenceRunning
                         && CortexFeatures.inputSequenceStatus !== "cancelling"
                onClicked: CortexFeatures.cancelInputSequence()
            }
            Item { Layout.fillWidth: true }
            Label {
                text: CortexFeatures.inputSequenceJobId > 0
                      ? ("Job " + CortexFeatures.inputSequenceJobId + " | "
                         + CortexFeatures.inputSequenceStatus + " | "
                         + CortexFeatures.inputSequenceStepIndex + "/" + CortexFeatures.inputSequenceStepCount
                         + " | " + CortexFeatures.inputSequenceMode)
                      : "No replay job"
                color: root.sequenceRunning ? Theme.mutation : Theme.textMuted
                font.family: Theme.monoFont
                font.pixelSize: 9
            }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 64
        color: Theme.panel
        RowLayout {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 6
            TextField {
                id: textField
                Layout.fillWidth: true
                placeholderText: "Text to send to the target..."
            }
            Button {
                text: root.backgroundText ? "Background" : "Foreground"
                onClicked: root.backgroundText = !root.backgroundText
            }
            Button {
                text: "Send text"
                enabled: CortexApp.sessionActive && CortexApp.mutationPermission && textField.text.length > 0
                onClicked: CortexFeatures.sendText(textField.text, root.backgroundText)
            }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 30
        color: Theme.background
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            spacing: 8
            Label { text: "RECORDED SEQUENCE"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
            Label {
                visible: CortexFeatures.inputSequenceJobId > 0
                text: "replay " + CortexFeatures.inputSequenceStatus + "  "
                      + CortexFeatures.inputSequenceStepIndex + "/" + CortexFeatures.inputSequenceStepCount
                color: root.sequenceRunning ? Theme.mutation : Theme.textDisabled
                font.family: Theme.monoFont
                font.pixelSize: 9
            }
            Item { Layout.fillWidth: true }
            Label {
                text: CortexFeatures.lastError
                visible: text.length > 0
                color: Theme.error
                font.pixelSize: 10
                elide: Text.ElideRight
                Layout.maximumWidth: 420
            }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.fillHeight: true
        color: Theme.input
        border.width: 1
        border.color: Theme.border

        CortexFlickable {
            anchors.fill: parent
            anchors.margins: 10
            contentWidth: width
            contentHeight: Math.max(height, recordedText.implicitHeight)
            clip: true
            TextEdit {
                id: recordedText
                width: parent.width
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.WrapAnywhere
                text: CortexFeatures.inputRecordingJson.length > 0
                      ? CortexFeatures.inputRecordingJson
                      : (CortexFeatures.inputRecording ? "Recording..." : "Start input recording to capture a replayable sequence.")
                color: CortexFeatures.inputRecordingJson.length > 0 ? Theme.text : Theme.textDisabled
                selectionColor: Theme.accentDark
                selectedTextColor: Theme.textBright
                font.family: Theme.monoFont
                font.pixelSize: 11
            }
        }
    }
}