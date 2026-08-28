import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

ColumnLayout {
    anchors.fill: parent
    spacing: 0

    Component.onCompleted: if (CortexApp.sessionActive) CortexRuntime.callToolJson("health", "{}")

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 44
        color: Theme.background
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 5
            Button { text: "Health"; enabled: CortexApp.sessionActive; onClicked: CortexRuntime.callToolJson("health", "{}") }
            Button { text: "Status"; enabled: CortexApp.sessionActive; onClicked: CortexRuntime.callToolJson("status", "{}") }
            Button { text: "Tool manifest"; enabled: CortexApp.sessionActive; onClicked: CortexRuntime.callToolJson("tools", "{}") }
            Item { Layout.fillWidth: true }
            Rectangle { Layout.preferredWidth: 7; Layout.preferredHeight: 7; radius: 4; color: CortexPayload.ready ? Theme.success : Theme.textDisabled }
            Label { text: CortexPayload.status; color: Theme.textMuted; font.pixelSize: 10 }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 30
        color: Theme.panel
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            Label { text: "RUNTIME DIAGNOSTICS"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
            Item { Layout.fillWidth: true }
            Label { text: CortexRuntime.lastError; visible: text.length > 0; color: Theme.error; font.pixelSize: 10 }
        }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.fillHeight: true
        color: Theme.input
        border.width: 1
        border.color: Theme.border
        Flickable {
            anchors.fill: parent
            anchors.margins: 12
            clip: true
            contentWidth: width
            contentHeight: diagnosticText.implicitHeight
            TextEdit {
                id: diagnosticText
                width: parent.width
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.WrapAnywhere
                text: CortexRuntime.lastResult.length > 0 ? CortexRuntime.lastResult : "Runtime health, transport, process and hook diagnostics appear here."
                color: CortexRuntime.lastResult.length > 0 ? Theme.text : Theme.textDisabled
                selectionColor: Theme.accentDark
                selectedTextColor: Theme.textBright
                font.family: Theme.monoFont
                font.pixelSize: 11
            }
        }
    }
}
