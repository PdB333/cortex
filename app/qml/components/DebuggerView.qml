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
            spacing: 4
            Button { text: "Continue"; font.pixelSize: 11; enabled: false }
            Button { text: "Pause"; font.pixelSize: 11; enabled: false }
            Button { text: "Step Into"; font.pixelSize: 11; enabled: false }
            Button { text: "Step Over"; font.pixelSize: 11; enabled: false }
            Item { Layout.fillWidth: true }
            Label { text: CortexApp.sessionActive ? "Debugger backend pending" : "No active session"; color: Theme.textMuted; font.pixelSize: 11 }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    SplitView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        orientation: Qt.Horizontal

        Rectangle {
            SplitView.fillWidth: true
            SplitView.minimumWidth: 420
            color: Theme.background
            ColumnLayout {
                anchors.fill: parent
                spacing: 0
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 32
                    color: Theme.panel
                    Text { anchors.left: parent.left; anchors.leftMargin: 12; anchors.verticalCenter: parent.verticalCenter; text: "DISASSEMBLY"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
                    Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
                }
                Text { Layout.leftMargin: 12; Layout.topMargin: 12; text: "No instruction pointer."; color: Theme.textDisabled; font.family: Theme.monoFont; font.pixelSize: 11 }
                Item { Layout.fillHeight: true }
            }
        }

        Rectangle {
            SplitView.preferredWidth: 330
            SplitView.minimumWidth: 250
            color: Theme.surface
            ColumnLayout {
                anchors.fill: parent
                spacing: 0
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 32
                    color: Theme.panel
                    Text { anchors.left: parent.left; anchors.leftMargin: 12; anchors.verticalCenter: parent.verticalCenter; text: "REGISTERS"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
                    Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
                }
                Text {
                    Layout.leftMargin: 12
                    Layout.topMargin: 12
                    text: "RAX  —\nRBX  —\nRCX  —\nRDX  —\nRSP  —\nRBP  —\nRIP  —"
                    color: Theme.textMuted
                    font.family: Theme.monoFont
                    font.pixelSize: 11
                    lineHeight: 1.45
                }
                Item { Layout.fillHeight: true }
            }
        }
    }
}
