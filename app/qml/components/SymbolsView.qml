import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

ColumnLayout {
    id: root
    anchors.fill: parent
    spacing: 0

    function field(name) {
        const value = CortexFeatures.symbolResult[name]
        return value === undefined || value === null ? "" : String(value)
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 46
        color: Theme.background
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 6
            TextField {
                id: addressField
                Layout.preferredWidth: 230
                placeholderText: "Address (0x...)"
                font.family: Theme.monoFont
                font.pixelSize: 10
                onAccepted: if (text.length > 0) CortexFeatures.resolveSymbol(text)
            }
            Button {
                text: "Resolve address"
                enabled: CortexApp.sessionActive && addressField.text.length > 0
                onClicked: CortexFeatures.resolveSymbol(addressField.text)
            }
            Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 18; color: Theme.borderStrong }
            TextField {
                id: nameField
                Layout.preferredWidth: 260
                placeholderText: "Symbol name"
                font.family: Theme.monoFont
                font.pixelSize: 10
                onAccepted: if (text.length > 0) CortexFeatures.lookupSymbol(text)
            }
            Button {
                text: "Lookup name"
                enabled: CortexApp.sessionActive && nameField.text.length > 0
                onClicked: CortexFeatures.lookupSymbol(nameField.text)
            }
            Button {
                text: "Clear"
                onClicked: CortexFeatures.clearSymbolResult()
            }
            Item { Layout.fillWidth: true }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 34
        color: Theme.panel
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            spacing: 8
            Label { text: "SYMBOL RESULT"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
            Label {
                visible: root.field("query").length > 0
                text: root.field("found") === "true" ? "RESOLVED" : "NO SYMBOL"
                color: root.field("found") === "true" ? Theme.success : Theme.textDisabled
                font.pixelSize: 9
                font.bold: true
            }
            Item { Layout.fillWidth: true }
            Button {
                text: "Disassembly"
                enabled: CortexApp.sessionActive && root.field("address").length > 0
                onClicked: {
                    if (CortexDisasm.disassemble(root.field("address"), 160))
                        CortexApp.selectSection("Disassembly")
                }
            }
            Button {
                text: "Memory"
                enabled: CortexApp.sessionActive && root.field("address").length > 0
                onClicked: {
                    if (CortexApp.readMemory(root.field("address"), CortexSettings.memoryReadSize, CortexSettings.memoryBytesPerRow))
                        CortexApp.selectSection("Memory")
                }
            }
            Label {
                text: CortexFeatures.lastError
                visible: text.length > 0
                color: Theme.error
                font.pixelSize: 9
                elide: Text.ElideRight
                Layout.maximumWidth: 360
            }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.fillHeight: true
        color: Theme.background

        CortexFlickable {
            id: symbolScroll
            anchors.fill: parent
            contentWidth: width
            contentHeight: symbolGrid.implicitHeight + 32

            GridLayout {
                id: symbolGrid
                x: 16
                y: 16
                width: Math.max(0, parent.width - 32)
                columns: 2
                columnSpacing: 18
                rowSpacing: 9

                Label { text: "Query"; color: Theme.textMuted; font.pixelSize: 10 }
                Text { Layout.fillWidth: true; text: root.field("query"); color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 10; wrapMode: Text.WrapAnywhere }

                Label { text: "Address"; color: Theme.textMuted; font.pixelSize: 10 }
                Text { Layout.fillWidth: true; text: root.field("address"); color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 10 }

                Label { text: "Module"; color: Theme.textMuted; font.pixelSize: 10 }
                Text { Layout.fillWidth: true; text: root.field("module"); color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 10; wrapMode: Text.WrapAnywhere }

                Label { text: "Module base / RVA"; color: Theme.textMuted; font.pixelSize: 10 }
                Text { Layout.fillWidth: true; text: root.field("moduleBase") + (root.field("rva").length ? "  +  " + root.field("rva") : ""); color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 10 }

                Label { text: "Symbol"; color: Theme.textMuted; font.pixelSize: 10 }
                Text { Layout.fillWidth: true; text: root.field("symbol"); color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 10; wrapMode: Text.WrapAnywhere }

                Label { text: "Symbol address"; color: Theme.textMuted; font.pixelSize: 10 }
                Text { Layout.fillWidth: true; text: root.field("symbolAddress"); color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 10 }

                Label { text: "Displacement"; color: Theme.textMuted; font.pixelSize: 10 }
                Text { Layout.fillWidth: true; text: root.field("displacement"); color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 10 }

                Label { text: "Source"; color: Theme.textMuted; font.pixelSize: 10 }
                Text {
                    Layout.fillWidth: true
                    text: root.field("file") + (root.field("line").length && root.field("line") !== "0" ? ":" + root.field("line") : "")
                    color: Theme.text
                    font.family: Theme.monoFont
                    font.pixelSize: 10
                    wrapMode: Text.WrapAnywhere
                }

                Label { text: "Build ID"; color: Theme.textMuted; font.pixelSize: 10 }
                Text { Layout.fillWidth: true; text: root.field("buildId"); color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 10; wrapMode: Text.WrapAnywhere }

                Label { text: "Loaded PDB"; color: Theme.textMuted; font.pixelSize: 10 }
                Text { Layout.fillWidth: true; text: root.field("loadedPdb"); color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 10; wrapMode: Text.WrapAnywhere }

                Label { text: "Symbol type"; color: Theme.textMuted; font.pixelSize: 10 }
                Text { Layout.fillWidth: true; text: root.field("symbolType"); color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 10 }

                Label { text: "Verification"; color: Theme.textMuted; font.pixelSize: 10 }
                Text { Layout.fillWidth: true; text: root.field("verification"); color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 10; wrapMode: Text.WrapAnywhere }

                Label { text: "Exact symbols"; color: Theme.textMuted; font.pixelSize: 10 }
                Text { Layout.fillWidth: true; text: root.field("exactSymbols"); color: Theme.textDisabled; font.family: Theme.monoFont; font.pixelSize: 9; wrapMode: Text.WrapAnywhere }

                Label { text: "Module path"; color: Theme.textMuted; font.pixelSize: 10 }
                Text { Layout.fillWidth: true; text: root.field("modulePath"); color: Theme.textDisabled; font.family: Theme.monoFont; font.pixelSize: 9; wrapMode: Text.WrapAnywhere }

                Label { text: "Result"; color: Theme.textMuted; font.pixelSize: 10 }
                Text {
                    Layout.fillWidth: true
                    text: root.field("error").length > 0 ? root.field("error")
                          : (root.field("query").length > 0 ? "ok" : "Resolve an address or look up a symbol name.")
                    color: root.field("error").length > 0 ? Theme.textDisabled : Theme.success
                    font.family: Theme.monoFont
                    font.pixelSize: 10
                    wrapMode: Text.WrapAnywhere
                }
            }
        }
    }
}