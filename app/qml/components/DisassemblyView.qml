import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

ColumnLayout {
    anchors.fill: parent
    spacing: 0

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 46
        color: Theme.background
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            spacing: 8
            TextField {
                Layout.preferredWidth: 260
                Layout.preferredHeight: 30
                placeholderText: "Address or symbol"
                font.family: Theme.monoFont
                font.pixelSize: 11
                enabled: CortexApp.sessionActive
            }
            Button { text: "Go"; font.pixelSize: 11; enabled: false }
            Button { text: "Back"; font.pixelSize: 11; enabled: false }
            Button { text: "Forward"; font.pixelSize: 11; enabled: false }
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
            Label { Layout.preferredWidth: 170; text: "Address"; color: Theme.textMuted; font.pixelSize: 11 }
            Label { Layout.preferredWidth: 180; text: "Bytes"; color: Theme.textMuted; font.pixelSize: 11 }
            Label { Layout.fillWidth: true; text: "Instruction"; color: Theme.textMuted; font.pixelSize: 11 }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.fillHeight: true
        color: Theme.background
        Text {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.leftMargin: 12
            anchors.topMargin: 14
            text: CortexApp.sessionActive ? "Disassembly service is the next backend migration." : "Select a target to disassemble code."
            color: Theme.textDisabled
            font.pixelSize: 11
        }
    }
}
