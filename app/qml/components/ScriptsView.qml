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
            TextField { id: scriptName; Layout.preferredWidth: 170; placeholderText: "Script name" }
            Button {
                text: "Run buffer"
                enabled: CortexApp.sessionActive && CortexApp.mutationPermission
                onClicked: CortexRuntime.callToolJson("lua_exec", JSON.stringify({"code": codeEdit.text, "timeout_ms": 5000}))
            }
            Button {
                text: "Save"
                enabled: CortexApp.sessionActive && CortexApp.mutationPermission && scriptName.text.length > 0
                onClicked: CortexRuntime.callToolJson("lua_scripts_save", JSON.stringify({"name": scriptName.text, "code": codeEdit.text}))
            }
            Button {
                text: "Run saved"
                enabled: CortexApp.sessionActive && CortexApp.mutationPermission && scriptName.text.length > 0
                onClicked: CortexRuntime.callToolJson("lua_scripts_run", JSON.stringify({"_path": {"name": scriptName.text}, "timeout_ms": 5000}))
            }
            Button { text: "List"; enabled: CortexApp.sessionActive; onClicked: CortexRuntime.callToolJson("lua_scripts", "{}") }
            Item { Layout.fillWidth: true }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    SplitView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        orientation: Qt.Horizontal

        Rectangle {
            SplitView.fillWidth: true
            SplitView.minimumWidth: 360
            color: Theme.input
            border.width: 1
            border.color: Theme.border
            TextEdit {
                id: codeEdit
                anchors.fill: parent
                anchors.margins: 12
                text: "-- Cortex Lua\nprint('hello from Cortex')\n"
                color: Theme.text
                selectionColor: Theme.accentDark
                selectedTextColor: Theme.textBright
                font.family: Theme.monoFont
                font.pixelSize: 11
                selectByMouse: true
                wrapMode: TextEdit.NoWrap
            }
        }

        Rectangle {
            SplitView.preferredWidth: 430
            SplitView.minimumWidth: 280
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
                        Label { text: "OUTPUT"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
                        Item { Layout.fillWidth: true }
                        Label { text: CortexRuntime.lastError; visible: text.length > 0; color: Theme.error; font.pixelSize: 10; elide: Text.ElideRight; Layout.maximumWidth: 260 }
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
                        contentHeight: outputText.implicitHeight
                        TextEdit {
                            id: outputText
                            width: parent.width
                            readOnly: true
                            selectByMouse: true
                            wrapMode: TextEdit.WrapAnywhere
                            text: CortexRuntime.lastResult.length > 0 ? CortexRuntime.lastResult : "Script output and persisted script operations appear here."
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
