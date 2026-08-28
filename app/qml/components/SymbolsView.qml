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
            TextField { id: addressField; Layout.preferredWidth: 210; placeholderText: "Address (0x...)" }
            Button {
                text: "Resolve address"
                enabled: CortexApp.sessionActive && addressField.text.length > 0
                onClicked: CortexRuntime.callToolJson("symbols_resolve", JSON.stringify({"_query": {"address": addressField.text}}))
            }
            Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 18; color: Theme.borderStrong }
            TextField { id: nameField; Layout.preferredWidth: 230; placeholderText: "Symbol name" }
            Button {
                text: "Lookup name"
                enabled: CortexApp.sessionActive && nameField.text.length > 0
                onClicked: CortexRuntime.callToolJson("symbols_lookup", JSON.stringify({"_query": {"name": nameField.text}}))
            }
            Item { Layout.fillWidth: true }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 30
        color: Theme.panel
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            Label { text: "SYMBOL RESULT"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
            Item { Layout.fillWidth: true }
            Label { text: CortexRuntime.lastError; visible: text.length > 0; color: Theme.error; font.pixelSize: 10 }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.fillHeight: true
        color: Theme.input
        border.width: 1
        border.color: Theme.border
        Flickable {
            anchors.fill: parent
            anchors.margins: 12
            clip: true
            contentWidth: width
            contentHeight: resultText.implicitHeight
            TextEdit {
                id: resultText
                width: parent.width
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.WrapAnywhere
                text: CortexRuntime.lastResult.length > 0 ? CortexRuntime.lastResult : "Resolve an address or look up a symbol name."
                color: CortexRuntime.lastResult.length > 0 ? Theme.text : Theme.textDisabled
                selectionColor: Theme.accentDark
                selectedTextColor: Theme.textBright
                font.family: Theme.monoFont
                font.pixelSize: 11
            }
        }
    }
}
