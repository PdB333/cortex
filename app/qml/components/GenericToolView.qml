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
            Button { text: "New"; font.pixelSize: 11; enabled: false }
            Button { text: "Refresh"; font.pixelSize: 11; onClicked: CortexApp.refreshTargets() }
            Item { Layout.fillWidth: true }
            Label {
                text: CortexApp.sessionActive ? CortexApp.currentTargetName : "No target"
                color: Theme.textDisabled
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
            Label { Layout.preferredWidth: 260; text: "Name"; color: Theme.textMuted; font.pixelSize: 11 }
            Label { Layout.preferredWidth: 160; text: "Status"; color: Theme.textMuted; font.pixelSize: 11 }
            Label { Layout.fillWidth: true; text: "Details"; color: Theme.textMuted; font.pixelSize: 11 }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.fillHeight: true
        color: Theme.background

        RowLayout {
            visible: CortexApp.selectedSection === "Sessions" && CortexApp.sessionActive
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 34
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            Text { Layout.preferredWidth: 260; text: CortexApp.currentTargetName; color: Theme.text; font.pixelSize: 11 }
            Text { Layout.preferredWidth: 160; text: CortexApp.sessionStatus; color: Theme.success; font.pixelSize: 11 }
            Text { Layout.fillWidth: true; text: CortexApp.currentTargetMeta; color: Theme.textMuted; font.pixelSize: 11; elide: Text.ElideRight }
        }

        Text {
            visible: !(CortexApp.selectedSection === "Sessions" && CortexApp.sessionActive)
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.leftMargin: 12
            anchors.topMargin: 14
            text: CortexApp.sessionActive ? "This service is pending migration to the shared Cortex core." : "Select a target to continue."
            color: Theme.textDisabled
            font.pixelSize: 11
        }
    }
}
