import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

ColumnLayout {
    id: root
    anchors.fill: parent
    spacing: 0
    property string selectedAddress: ""

    Connections {
        target: CortexApp
        function onNavigationAddressChanged() {
            if (CortexApp.selectedSection !== "Memory" || CortexApp.navigationAddress.length === 0) return
            addressField.text = CortexApp.navigationAddress
            CortexApp.readMemory(CortexApp.navigationAddress, CortexSettings.memoryReadSize, CortexSettings.memoryBytesPerRow)
            root.selectedAddress = CortexApp.navigationAddress
        }
    }

    function openContext(address, sourceItem, x, y) {
        root.selectedAddress = address
        contextMenu.address = address
        contextMenu.label = "Memory " + address
        contextMenu.valueType = "i32"
        const p = sourceItem.mapToItem(root, x, y)
        contextMenu.x = Math.max(0, Math.min(root.width - contextMenu.implicitWidth, p.x))
        contextMenu.y = Math.max(0, Math.min(root.height - 320, p.y))
        contextMenu.open()
    }

    Shortcut {
        sequence: "Ctrl+B"
        enabled: root.selectedAddress.length > 0 && CortexPayload.ready && CortexApp.mutationPermission && !addressField.activeFocus && !writeAddress.activeFocus && !writeBytes.activeFocus
        onActivated: CortexDebugger.addBreakpoint(root.selectedAddress, "software", CortexSettings.breakpointDefaultAction, true, 0)
    }

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
                Keys.onReturnPressed: {
                    if (CortexApp.readMemory(text, CortexSettings.memoryReadSize, CortexSettings.memoryBytesPerRow)) root.selectedAddress = text
                }
            }
            Button {
                text: "Go"
                font.pixelSize: 11
                enabled: CortexApp.sessionActive && addressField.text.length > 0
                onClicked: if (CortexApp.readMemory(addressField.text, CortexSettings.memoryReadSize, CortexSettings.memoryBytesPerRow)) root.selectedAddress = addressField.text
            }
            Button {
                text: "Refresh"
                font.pixelSize: 11
                enabled: CortexApp.sessionActive && addressField.text.length > 0
                onClicked: CortexApp.readMemory(addressField.text, CortexSettings.memoryReadSize, CortexSettings.memoryBytesPerRow)
            }
            Item { Layout.fillWidth: true }
            Label { text: CortexSettings.memoryBytesPerRow + " bytes / row | Right-click for address actions"; color: Theme.textDisabled; font.pixelSize: 10 }
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
                        root.selectedAddress = destination
                        CortexApp.readMemory(destination, CortexSettings.memoryReadSize, CortexSettings.memoryBytesPerRow)
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

        CortexListView {
            id: memoryList
            anchors.fill: parent
            clip: true
            model: CortexApp.memoryRows
            boundsBehavior: Flickable.StopAtBounds
            currentIndex: -1

            delegate: Rectangle {
                id: memoryRow
                required property var modelData
                required property int index
                width: ListView.view.width
                height: 28
                color: memoryList.currentIndex === index ? Theme.selection : (mouse.containsMouse ? Theme.hover : (index % 2 ? Theme.background : Theme.surface))

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 0
                    Text { Layout.preferredWidth: 190; text: modelData.address; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 11 }
                    Text { Layout.fillWidth: true; text: modelData.hex; color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 11; elide: Text.ElideRight }
                    Text { Layout.preferredWidth: 190; text: modelData.ascii; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 11; elide: Text.ElideRight }
                }

                MouseArea {
                    id: mouse
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onPressed: function(event) {
                        memoryList.currentIndex = index
                        root.selectedAddress = String(modelData.address || "")
                        if (event.button === Qt.RightButton) root.openContext(root.selectedAddress, mouse, event.x, event.y)
                    }
                    onDoubleClicked: function(event) {
                        if (event.button === Qt.LeftButton) CortexApp.openAddress("Disassembly", modelData.address)
                    }
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

    AddressContextMenu {
        id: contextMenu
        allowSave: true
    }
}