import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

ColumnLayout {
    anchors.fill: parent
    spacing: 0

    Component.onCompleted: {
        if (CortexApp.sessionActive && CortexPayload.ready) CortexDebugger.refreshRuntime()
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
            Button {
                text: CortexPayload.ready ? "Runtime On" : "Enable Runtime"
                enabled: CortexApp.sessionActive && !CortexPayload.ready
                onClicked: CortexDebugger.enableRuntime()
            }
            Button {
                text: "Refresh"
                enabled: CortexApp.sessionActive
                onClicked: CortexPayload.ready ? CortexDebugger.refreshRuntime() : CortexDebugger.enableRuntime()
            }
            TextField { id: addressField; Layout.preferredWidth: 180; placeholderText: "Address (0x...)" }
            ComboBox { id: kindBox; Layout.preferredWidth: 125; model: ["software", "hw_execute", "hw_write", "hw_readwrite"] }
            ComboBox { id: actionBox; Layout.preferredWidth: 90; model: ["pause", "log"] }
            Button {
                text: "Add"
                enabled: CortexPayload.ready && CortexApp.mutationPermission && addressField.text.length > 0
                onClicked: CortexDebugger.addBreakpoint(addressField.text, kindBox.currentText, actionBox.currentText)
            }
            Item { Layout.fillWidth: true }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 32
        color: Theme.panel
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            spacing: 0
            Label { Layout.preferredWidth: 60; text: "ID"; color: Theme.textMuted; font.pixelSize: 10 }
            Label { Layout.preferredWidth: 190; text: "ADDRESS"; color: Theme.textMuted; font.pixelSize: 10 }
            Label { Layout.preferredWidth: 140; text: "KIND"; color: Theme.textMuted; font.pixelSize: 10 }
            Label { Layout.preferredWidth: 90; text: "ACTION"; color: Theme.textMuted; font.pixelSize: 10 }
            Label { Layout.preferredWidth: 100; text: "HITS"; color: Theme.textMuted; font.pixelSize: 10 }
            Item { Layout.fillWidth: true }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    ListView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        model: CortexDebugger.breakpoints
        delegate: Rectangle {
            required property var modelData
            width: ListView.view.width
            height: 34
            color: index % 2 ? Theme.background : Theme.surface
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 8
                spacing: 0
                Text { Layout.preferredWidth: 60; text: modelData.id; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 10 }
                Text { Layout.preferredWidth: 190; text: modelData.address; color: Theme.codeNumber; font.family: Theme.monoFont; font.pixelSize: 10 }
                Text { Layout.preferredWidth: 140; text: modelData.kind; color: Theme.text; font.pixelSize: 10 }
                Text { Layout.preferredWidth: 90; text: modelData.action; color: modelData.action === "pause" ? Theme.mutation : Theme.textMuted; font.pixelSize: 10 }
                Text { Layout.preferredWidth: 100; text: modelData.hitCount; color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 10 }
                Item { Layout.fillWidth: true }
                Button {
                    text: "Remove"
                    enabled: CortexApp.mutationPermission
                    onClicked: CortexDebugger.removeBreakpoint(modelData.id)
                }
            }
            Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border; opacity: 0.55 }
        }
        Text {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.margins: 14
            visible: parent.count === 0
            text: CortexApp.sessionActive ? "No breakpoints." : "Select a target to manage breakpoints."
            color: Theme.textDisabled
            font.pixelSize: 11
        }
    }

    Label {
        Layout.fillWidth: true
        Layout.preferredHeight: CortexDebugger.lastError.length ? 28 : 0
        visible: CortexDebugger.lastError.length > 0
        leftPadding: 12
        verticalAlignment: Text.AlignVCenter
        text: CortexDebugger.lastError
        color: Theme.error
        font.pixelSize: 10
    }
}
