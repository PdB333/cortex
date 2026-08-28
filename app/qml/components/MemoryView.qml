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

            Label { text: "Address"; color: Theme.textMuted; font.pixelSize: 11 }
            TextField {
                id: addressField
                Layout.preferredWidth: 250
                Layout.preferredHeight: 30
                placeholderText: "0x0000000000000000"
                font.family: Theme.monoFont
                font.pixelSize: 11
                enabled: CortexApp.sessionActive
                Keys.onReturnPressed: CortexApp.readMemory(text, 256)
            }
            Button {
                text: "Go"
                font.pixelSize: 11
                enabled: CortexApp.sessionActive && addressField.text.length > 0
                onClicked: CortexApp.readMemory(addressField.text, 256)
            }
            Button {
                text: "Refresh"
                font.pixelSize: 11
                enabled: CortexApp.sessionActive && addressField.text.length > 0
                onClicked: CortexApp.readMemory(addressField.text, 256)
            }
            Item { Layout.fillWidth: true }
            Label { text: "16 bytes / row"; color: Theme.textDisabled; font.pixelSize: 10 }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 44
        color: Theme.panel

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            spacing: 7

            Label { text: "Write"; color: Theme.mutation; font.pixelSize: 10; font.bold: true }
            TextField {
                id: writeAddress
                Layout.preferredWidth: 220
                placeholderText: "Address (uses current if empty)"
                font.family: Theme.monoFont
                font.pixelSize: 10
            }
            TextField {
                id: writeBytes
                Layout.fillWidth: true
                placeholderText: "Hex bytes, e.g. 90 90 90 90"
                font.family: Theme.monoFont
                font.pixelSize: 10
            }
            Button {
                text: "Write bytes"
                enabled: CortexApp.sessionActive && CortexApp.mutationPermission &&
                         writeBytes.text.length > 0 && (writeAddress.text.length > 0 || addressField.text.length > 0)
                onClicked: {
                    const destination = writeAddress.text.length > 0 ? writeAddress.text : addressField.text
                    if (CortexApp.writeMemoryHex(destination, writeBytes.text)) {
                        addressField.text = destination
                        CortexApp.readMemory(destination, 256)
                    }
                }
            }
            Label {
                text: CortexApp.mutationPermission ? "Mutation enabled" : "Mutation required"
                color: CortexApp.mutationPermission ? Theme.mutation : Theme.textDisabled
                font.pixelSize: 9
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
            anchors.rightMargin: 12
            spacing: 0
            Label { Layout.preferredWidth: 190; text: "Address"; color: Theme.textMuted; font.pixelSize: 11 }
            Label { Layout.fillWidth: true; text: "Hex"; color: Theme.textMuted; font.pixelSize: 11 }
            Label { Layout.preferredWidth: 190; text: "ASCII"; color: Theme.textMuted; font.pixelSize: 11 }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.fillHeight: true
        color: Theme.background

        ListView {
            anchors.fill: parent
            clip: true
            model: CortexApp.memoryRows
            boundsBehavior: Flickable.StopAtBounds

            delegate: Rectangle {
                required property var modelData
                width: ListView.view.width
                height: 28
                color: index % 2 ? Theme.background : Theme.surface

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 0
                    Text { Layout.preferredWidth: 190; text: modelData.address; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 11 }
                    Text { Layout.fillWidth: true; text: modelData.hex; color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 11; elide: Text.ElideRight }
                    Text { Layout.preferredWidth: 190; text: modelData.ascii; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 11; elide: Text.ElideRight }
                }
            }
        }

        Text {
            visible: CortexApp.memoryRows.length === 0
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.leftMargin: 12
            anchors.topMargin: 14
            text: CortexApp.lastError.length ? CortexApp.lastError : (CortexApp.sessionActive ? "Enter an address to read memory." : "Select a target to inspect memory.")
            color: CortexApp.lastError.length ? Theme.error : Theme.textDisabled
            font.family: CortexApp.lastError.length ? Theme.monoFont : Theme.uiFont
            font.pixelSize: 11
        }
    }
}
