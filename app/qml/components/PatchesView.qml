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
                        if (root.mode === 0) CortexFeatures.applyPatchBytes(addressField.text, valueField.text, labelField.text)
                        else if (root.mode === 1) CortexFeatures.applyPatchNop(addressField.text, parseInt(valueField.text), labelField.text)
                        else if (root.mode === 2) CortexFeatures.applyPatchAssembly(addressField.text, valueField.text, labelField.text)
                        else if (root.mode === 3) CortexFeatures.applyPatchDetour(addressField.text, valueField.text, parseInt(extraField.text) || 5)
                        else if (root.mode === 4) CortexFeatures.applyPatchTrampoline(addressField.text, valueField.text, parseInt(extraField.text) || 5)
                        else CortexFeatures.allocatePatchCave(addressField.text, parseInt(valueField.text))
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

    CortexSplitView {
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
                CortexListView {
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
                    text: CortexFeatures.lastError.length ? CortexFeatures.lastError : (CortexFeatures.patchOperationResult.length ? CortexFeatures.patchOperationResult : "No patch operation yet.")
                    color: CortexFeatures.lastError.length ? Theme.error : (CortexFeatures.patchOperationResult.length ? Theme.text : Theme.textDisabled)
                    font.family: Theme.monoFont
                    font.pixelSize: 10
                    leftPadding: 10
                    topPadding: 8
                }
            }
        }
    }
}