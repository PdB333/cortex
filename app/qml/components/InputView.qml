import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

ColumnLayout {
    id: root
    anchors.fill: parent
    spacing: 0
    property bool backgroundText: false

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
                enabled: CortexApp.sessionActive && CortexApp.mutationPermission
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
        Layout.preferredHeight: 74
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
            Label { text: "RECORDED SEQUENCE"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
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

        Flickable {
            anchors.fill: parent
            anchors.margins: 10
            contentWidth: width
            contentHeight: recordedText.implicitHeight
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
