import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

ColumnLayout {
    id: root
    anchors.fill: parent
    spacing: 0

    function parseJson(text, fallback) {
        try { return JSON.parse(text) } catch (e) { return fallback }
    }

    function defineStruct() {
        const fields = parseJson(fieldsEdit.text, null)
        if (fields === null) { fieldsEdit.forceActiveFocus(); return }
        CortexRuntime.callToolJson("struct_define", JSON.stringify({"name": nameField.text, "fields": fields}))
    }

    function writeStruct() {
        const values = parseJson(valuesField.text, null)
        if (values === null) { valuesField.forceActiveFocus(); return }
        CortexRuntime.callToolJson("struct_write", JSON.stringify({"name": nameField.text, "address": addressField.text, "values": values}))
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 44
        color: Theme.background
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 5
            Button { text: "List"; enabled: CortexApp.sessionActive; onClicked: CortexRuntime.callToolJson("struct_list", "{}") }
            TextField { id: nameField; Layout.preferredWidth: 170; placeholderText: "Structure name" }
            TextField { id: addressField; Layout.preferredWidth: 180; placeholderText: "Instance address" }
            Button {
                text: "Read"
                enabled: CortexApp.sessionActive && nameField.text.length > 0 && addressField.text.length > 0
                onClicked: CortexRuntime.callToolJson("struct_read", JSON.stringify({"name": nameField.text, "address": addressField.text}))
            }
            Button {
                text: "Define"
                enabled: CortexApp.sessionActive && CortexApp.mutationPermission && nameField.text.length > 0
                onClicked: root.defineStruct()
            }
            Item { Layout.fillWidth: true }
            Label {
                text: CortexApp.mutationPermission ? "Mutation enabled" : "Define/Write require Mutation"
                color: CortexApp.mutationPermission ? Theme.mutation : Theme.textDisabled
                font.pixelSize: 9
            }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 44
        color: Theme.panel
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 6
            Label { text: "Values"; color: Theme.textMuted; font.pixelSize: 10 }
            TextField {
                id: valuesField
                Layout.fillWidth: true
                placeholderText: "{\"health\": 100, \"ammo\": 30}"
                font.family: Theme.monoFont
                font.pixelSize: 10
            }
            Button {
                text: "Write fields"
                enabled: CortexApp.sessionActive && CortexApp.mutationPermission &&
                         nameField.text.length > 0 && addressField.text.length > 0 && valuesField.text.length > 0
                onClicked: root.writeStruct()
            }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    SplitView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        orientation: Qt.Horizontal

        Rectangle {
            SplitView.preferredWidth: 390
            SplitView.minimumWidth: 260
            color: Theme.background
            ColumnLayout {
                anchors.fill: parent
                spacing: 0
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 30
                    color: Theme.panel
                    Label { anchors.left: parent.left; anchors.leftMargin: 12; anchors.verticalCenter: parent.verticalCenter; text: "FIELDS JSON"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: Theme.input
                    border.width: 1
                    border.color: Theme.border
                    TextEdit {
                        id: fieldsEdit
                        anchors.fill: parent
                        anchors.margins: 10
                        text: "[\n  {\"name\": \"health\", \"offset\": 0, \"type\": \"i32\"}\n]"
                        color: Theme.text
                        selectionColor: Theme.accentDark
                        selectedTextColor: Theme.textBright
                        font.family: Theme.monoFont
                        font.pixelSize: 11
                        selectByMouse: true
                        wrapMode: TextEdit.WrapAnywhere
                    }
                }
            }
        }

        Rectangle {
            SplitView.fillWidth: true
            color: Theme.background
            ColumnLayout {
                anchors.fill: parent
                spacing: 0
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 30
                    color: Theme.panel
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        Label { text: "RESULT"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
                        Item { Layout.fillWidth: true }
                        Label { text: CortexRuntime.lastError; visible: text.length > 0; color: Theme.error; font.pixelSize: 10 }
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: Theme.input
                    border.width: 1
                    border.color: Theme.border
                    Flickable {
                        anchors.fill: parent
                        anchors.margins: 10
                        clip: true
                        contentWidth: width
                        contentHeight: resultText.implicitHeight
                        TextEdit {
                            id: resultText
                            width: parent.width
                            readOnly: true
                            selectByMouse: true
                            wrapMode: TextEdit.WrapAnywhere
                            text: CortexRuntime.lastResult.length > 0 ? CortexRuntime.lastResult : "List, define, read, or write a structure."
                            color: CortexRuntime.lastResult.length > 0 ? Theme.text : Theme.textDisabled
                            selectionColor: Theme.accentDark
                            selectedTextColor: Theme.textBright
                            font.family: Theme.monoFont
                            font.pixelSize: 11
                        }
                    }
                }
            }
        }
    }
}
