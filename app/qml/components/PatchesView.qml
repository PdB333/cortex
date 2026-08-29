import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

ColumnLayout {
    id: root
    anchors.fill: parent
    spacing: 0

    property int mode: 0
    property var modes: ["Raw bytes", "NOP", "Assemble", "Detour", "Trampoline", "Code cave"]

    Component.onCompleted: if (CortexPayload.ready) CortexFeatures.refreshPatches()

    function runMutation(tool, args) {
        if (!CortexRuntime.callToolJson(tool, JSON.stringify(args))) return false
        CortexFeatures.refreshPatches()
        return true
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 78
        color: Theme.background
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 7
            spacing: 5
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Label { text: "PATCH"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true; Layout.preferredWidth: 46 }
                ComboBox { id: modeBox; Layout.preferredWidth: 125; model: root.modes; onCurrentIndexChanged: root.mode = currentIndex }
                TextField { id: addressField; Layout.preferredWidth: 155; placeholderText: root.mode === 5 ? "Near address" : "Address (0x...)" }
                TextField { id: valueField; Layout.fillWidth: true; placeholderText: root.mode === 0 ? "Bytes hex, e.g. 909090" : root.mode === 1 ? "Size" : root.mode === 2 ? "Assembly; separate instructions with ;" : root.mode === 5 ? "Allocation size" : "Target address" }
                TextField { id: labelField; Layout.preferredWidth: 145; placeholderText: "Label (optional)"; visible: root.mode <= 2 }
                Button {
                    text: root.mode === 2 ? "Assemble / write" : root.mode === 5 ? "Allocate" : "Apply"
                    enabled: CortexPayload.ready && CortexApp.mutationPermission && addressField.text.length > 0 && valueField.text.length > 0
                    onClicked: {
                        if (root.mode === 0) root.runMutation("patch_write", {address: addressField.text, bytes: valueField.text, label: labelField.text})
                        else if (root.mode === 1) root.runMutation("patch_nop", {address: addressField.text, size: parseInt(valueField.text), label: labelField.text})
                        else if (root.mode === 2) root.runMutation("patch_assemble", {address: addressField.text, lines: valueField.text.split(";").map(function(v){return v.trim()}).filter(function(v){return v.length > 0}), write: true, label: labelField.text})
                        else if (root.mode === 3) root.runMutation("patch_detour", {address: addressField.text, target: valueField.text, jmp_size: parseInt(extraField.text) || 5})
                        else if (root.mode === 4) root.runMutation("patch_trampoline", {address: addressField.text, target: valueField.text, minimum_overwrite: parseInt(extraField.text) || 5})
                        else CortexRuntime.callToolJson("patch_alloc_cave", JSON.stringify({near_address: addressField.text, size: parseInt(valueField.text)}))
                    }
                }
                Button { text: "Refresh"; enabled: CortexPayload.ready; onClicked: CortexFeatures.refreshPatches() }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Item { Layout.preferredWidth: 52 }
                TextField { id: extraField; Layout.preferredWidth: 125; visible: root.mode === 3 || root.mode === 4; placeholderText: root.mode === 3 ? "JMP size (5)" : "Min overwrite (5)" }
                Label {
                    Layout.fillWidth: true
                    text: root.mode === 5 ? "Allocation returns its address in the result pane below." : "All applied patches are tracked and revertible."
                    color: Theme.textDisabled
                    font.pixelSize: 9
                }
                Label { text: CortexApp.mutationPermission ? "Mutation enabled" : "Mutation required"; color: CortexApp.mutationPermission ? Theme.mutation : Theme.textDisabled; font.pixelSize: 9 }
            }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    SplitView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        orientation: Qt.Vertical

        Rectangle {
            SplitView.fillHeight: true
            SplitView.minimumHeight: 180
            color: Theme.background
            ColumnLayout {
                anchors.fill: parent
                spacing: 0
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 31
                    color: Theme.panel
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        spacing: 0
                        Label { Layout.preferredWidth: 50; text: "ID"; color: Theme.textMuted; font.pixelSize: 10 }
                        Label { Layout.preferredWidth: 145; text: "ADDRESS"; color: Theme.textMuted; font.pixelSize: 10 }
                        Label { Layout.preferredWidth: 180; text: "ORIGINAL"; color: Theme.textMuted; font.pixelSize: 10 }
                        Label { Layout.preferredWidth: 180; text: "CURRENT"; color: Theme.textMuted; font.pixelSize: 10 }
                        Label { Layout.fillWidth: true; text: "LABEL / GATEWAY"; color: Theme.textMuted; font.pixelSize: 10 }
                    }
                }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: CortexFeatures.patches
                    delegate: Rectangle {
                        required property var modelData
                        required property int index
                        width: ListView.view.width
                        height: 34
                        color: index % 2 ? Theme.background : Theme.surface
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 8
                            spacing: 0
                            Text { Layout.preferredWidth: 50; text: modelData.id; color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 10 }
                            Text { Layout.preferredWidth: 145; text: modelData.address; color: Theme.codeNumber; font.family: Theme.monoFont; font.pixelSize: 10; elide: Text.ElideRight }
                            Text { Layout.preferredWidth: 180; text: modelData.original; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 9; elide: Text.ElideRight }
                            Text { Layout.preferredWidth: 180; text: modelData.current; color: Theme.mutation; font.family: Theme.monoFont; font.pixelSize: 9; elide: Text.ElideRight }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0
                                Text { Layout.fillWidth: true; text: modelData.label.length ? modelData.label : "(unlabeled)"; color: Theme.text; font.pixelSize: 9; elide: Text.ElideRight }
                                Text { Layout.fillWidth: true; visible: modelData.gateway.length > 0; text: modelData.gateway; color: Theme.textDisabled; font.family: Theme.monoFont; font.pixelSize: 8; elide: Text.ElideRight }
                            }
                            Button { text: "Revert"; enabled: CortexApp.mutationPermission; onClicked: CortexFeatures.revertPatch(modelData.id) }
                        }
                    }
                    Text { anchors.centerIn: parent; visible: parent.count === 0; text: CortexPayload.ready ? "No active patches." : "Enable instrumentation to inspect patches."; color: Theme.textDisabled; font.pixelSize: 10 }
                }
            }
        }

        Rectangle {
            SplitView.preferredHeight: 120
            SplitView.minimumHeight: 80
            color: Theme.input
            border.color: Theme.border
            ColumnLayout {
                anchors.fill: parent
                spacing: 0
                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 28; color: Theme.panel; Label { anchors.left: parent.left; anchors.leftMargin: 10; anchors.verticalCenter: parent.verticalCenter; text: "LAST PATCH OPERATION"; color: Theme.textMuted; font.pixelSize: 9; font.bold: true } }
                TextEdit {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.WrapAnywhere
                    text: CortexRuntime.lastError.length ? CortexRuntime.lastError : (CortexRuntime.lastResult.length ? CortexRuntime.lastResult : "No patch operation yet.")
                    color: CortexRuntime.lastError.length ? Theme.error : (CortexRuntime.lastResult.length ? Theme.text : Theme.textDisabled)
                    font.family: Theme.monoFont
                    font.pixelSize: 10
                    leftPadding: 10
                    topPadding: 8
                }
            }
        }
    }
}