import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

Popup {
    id: root
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: 0

    property string errorText: ""

    function show() {
        errorText = ""
        input.text = CortexApp.navigationAddress
        open()
        input.forceActiveFocus()
        input.selectAll()
    }

    function resolveInput() {
        const query = input.text.trim()
        if (query.length === 0) return ""
        const direct = CortexApp.resolveAddressExpression(query)
        if (direct.length > 0) return direct
        if (CortexPayload.ready && CortexFeatures.lookupSymbol(query) && CortexFeatures.symbolResult.found)
            return String(CortexFeatures.symbolResult.address || "")
        return ""
    }

    function go(section) {
        const resolved = resolveInput()
        if (resolved.length === 0) {
            errorText = "Address, module+offset, or symbol not found"
            return
        }
        CortexApp.openAddress(section, resolved)
        close()
    }

    width: Math.min(620, parent ? parent.width - 40 : 620)
    height: panel.implicitHeight + 24
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.max(30, Math.round(parent.height * 0.16)) : 0

    background: Rectangle {
        color: Theme.surfaceRaised
        border.color: Theme.borderStrong
        border.width: 1
        radius: 4
    }

    ColumnLayout {
        id: panel
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            Label { text: "GO TO"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
            Item { Layout.fillWidth: true }
            Label { text: "Ctrl+G"; color: Theme.textDisabled; font.pixelSize: 9 }
        }

        TextField {
            id: input
            Layout.fillWidth: true
            placeholderText: "0x..., game.exe+0x1234, or symbol"
            font.family: Theme.monoFont
            onAccepted: root.go("Memory")
        }

        RowLayout {
            Layout.fillWidth: true
            Button { text: "Memory"; onClicked: root.go("Memory") }
            Button { text: "Disassembly"; onClicked: root.go("Disassembly") }
            Button { text: "RE"; onClicked: root.go("RE") }
            Button { text: "Addresses"; onClicked: root.go("Addresses") }
            Item { Layout.fillWidth: true }
        }

        Label {
            Layout.fillWidth: true
            visible: root.errorText.length > 0
            text: root.errorText
            color: Theme.error
            font.pixelSize: 10
            wrapMode: Text.Wrap
        }
    }
}