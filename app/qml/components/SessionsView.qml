import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

ColumnLayout {
    anchors.fill: parent
    spacing: 0

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
                text: "Export runtime state"
                enabled: CortexApp.sessionActive
                onClicked: CortexFeatures.exportSession()
            }
            Button {
                text: "Detach"
                enabled: CortexApp.sessionActive
                onClicked: CortexApp.detachTarget()
            }
            Item { Layout.fillWidth: true }
            Label {
                text: CortexPayload.ready ? "Instrumentation connected" : "External session"
                color: CortexPayload.ready ? Theme.success : Theme.textMuted
                font.pixelSize: 10
            }
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
            Label { Layout.preferredWidth: 220; text: "TARGET"; color: Theme.textMuted; font.pixelSize: 10 }
            Label { Layout.preferredWidth: 120; text: "STATE"; color: Theme.textMuted; font.pixelSize: 10 }
            Label { Layout.fillWidth: true; text: "DETAILS"; color: Theme.textMuted; font.pixelSize: 10 }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 40
        color: Theme.background
        visible: CortexApp.sessionActive
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            spacing: 0
            Text { Layout.preferredWidth: 220; text: CortexApp.currentTargetName; color: Theme.text; font.pixelSize: 11; elide: Text.ElideRight }
            Text { Layout.preferredWidth: 120; text: CortexApp.sessionStatus; color: Theme.success; font.pixelSize: 11 }
            Text { Layout.fillWidth: true; text: CortexApp.currentTargetMeta; color: Theme.textMuted; font.pixelSize: 10; elide: Text.ElideRight }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    ColumnLayout {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.margins: 14
        spacing: 10

        Label {
            text: CortexApp.sessionActive ? "Capabilities" : "No active session"
            color: Theme.textBright
            font.pixelSize: 12
            font.bold: true
        }
        Text {
            Layout.fillWidth: true
            text: CortexApp.sessionActive ? CortexApp.capabilitySummary() : "Select a target from the top bar to create a Cortex session."
            color: Theme.textMuted
            font.pixelSize: 11
            wrapMode: Text.Wrap
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

        Label { text: "Last exported runtime state"; color: Theme.textBright; font.pixelSize: 12; font.bold: true }
        Text {
            Layout.fillWidth: true
            text: CortexFeatures.sessionExportPath.length > 0 ? CortexFeatures.sessionExportPath : "No export created in this application session."
            color: CortexFeatures.sessionExportPath.length > 0 ? Theme.text : Theme.textDisabled
            font.family: CortexFeatures.sessionExportPath.length > 0 ? Theme.monoFont : Theme.uiFont
            font.pixelSize: 10
            wrapMode: Text.WrapAnywhere
        }

        Label {
            text: CortexFeatures.lastError
            visible: text.length > 0
            color: Theme.error
            font.pixelSize: 10
        }
        Item { Layout.fillHeight: true }
    }
}
