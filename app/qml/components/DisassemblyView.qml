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
                id: addressField
                Layout.preferredWidth: 260
                Layout.preferredHeight: 30
                placeholderText: "Address, e.g. 0x7FF612340000"
                font.family: Theme.monoFont
                font.pixelSize: 11
                enabled: CortexApp.sessionActive
                onAccepted: if (enabled && text.length > 0) CortexDisasm.disassemble(text, 160)
            }
            Button {
                text: "Go"
                font.pixelSize: 11
                enabled: CortexApp.sessionActive && addressField.text.length > 0
                onClicked: CortexDisasm.disassemble(addressField.text, 160)
            }
            Button { text: "Back"; font.pixelSize: 11; enabled: false }
            Button { text: "Forward"; font.pixelSize: 11; enabled: false }
            Item { Layout.fillWidth: true }
            Label {
                text: CortexDisasm.lastError.length > 0
                    ? CortexDisasm.lastError
                    : (CortexDisasm.rows.length > 0 ? CortexDisasm.rows.length + " instructions" : "")
                color: CortexDisasm.lastError.length > 0 ? Theme.mutation : Theme.textDisabled
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
            Label { Layout.preferredWidth: 170; text: "Address"; color: Theme.textMuted; font.pixelSize: 11 }
            Label { Layout.preferredWidth: 220; text: "Bytes"; color: Theme.textMuted; font.pixelSize: 11 }
            Label { Layout.fillWidth: true; text: "Instruction"; color: Theme.textMuted; font.pixelSize: 11 }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.fillHeight: true
        color: Theme.background

        ListView {
            id: list
            anchors.fill: parent
            clip: true
            model: CortexDisasm.rows
            boundsBehavior: Flickable.StopAtBounds

            delegate: Rectangle {
                required property var modelData
                width: list.width
                height: 26
                color: ListView.isCurrentItem ? Theme.selection : (index % 2 ? Theme.background : Theme.surface)

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 0
                    Text {
                        Layout.preferredWidth: 170
                        text: modelData.address
                        color: Theme.textMuted
                        font.family: Theme.monoFont
                        font.pixelSize: 11
                    }
                    Text {
                        Layout.preferredWidth: 220
                        text: modelData.bytes
                        color: Theme.textDisabled
                        font.family: Theme.monoFont
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }
                    Text {
                        Layout.fillWidth: true
                        text: modelData.text
                        color: Theme.text
                        font.family: Theme.monoFont
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: list.currentIndex = index
                }
            }
        }

        Text {
            visible: CortexDisasm.rows.length === 0
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.leftMargin: 12
            anchors.topMargin: 14
            text: CortexApp.sessionActive ? "Enter a readable code address to disassemble." : "Select a target to disassemble code."
            color: Theme.textDisabled
            font.pixelSize: 11
        }
    }
}
