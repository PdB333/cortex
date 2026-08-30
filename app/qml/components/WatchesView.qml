import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

ColumnLayout {
    anchors.fill: parent
    spacing: 0

    Component.onCompleted: if (CortexPayload.ready) CortexFeatures.refreshWatches()

    Timer {
        interval: CortexSettings.autoRefreshMs
        repeat: true
        running: CortexPayload.ready && CortexApp.selectedSection === "Watches"
        onTriggered: CortexFeatures.refreshWatches()
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 76
        color: Theme.background

        ColumnLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            anchors.topMargin: 5
            anchors.bottomMargin: 5
            spacing: 4

            RowLayout {
                Layout.fillWidth: true
                spacing: 5
                Label { text: "Watch"; color: Theme.textMuted; font.pixelSize: 10; Layout.preferredWidth: 48 }
                TextField { id: watchAddress; Layout.preferredWidth: 150; placeholderText: "Address (0x...)" }
                ComboBox { id: watchType; Layout.preferredWidth: 92; model: ["i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "float", "double"] }
                TextField { id: watchLabel; Layout.preferredWidth: 150; placeholderText: "Label (optional)" }
                Button {
                    text: "Add watch"
                    enabled: CortexPayload.ready && CortexApp.mutationPermission && watchAddress.text.length > 0
                    onClicked: CortexFeatures.addWatch(watchAddress.text, watchType.currentText, watchLabel.text)
                }
                Item { Layout.fillWidth: true }
                Button { text: "Refresh"; enabled: CortexPayload.ready; onClicked: CortexFeatures.refreshWatches() }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 5
                Label { text: "Freeze"; color: Theme.textMuted; font.pixelSize: 10; Layout.preferredWidth: 48 }
                TextField { id: freezeAddress; Layout.preferredWidth: 150; placeholderText: "Address (0x...)" }
                ComboBox { id: freezeType; Layout.preferredWidth: 92; model: ["i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "float", "double", "bytes"] }
                TextField { id: freezeValue; Layout.preferredWidth: 120; placeholderText: "Value" }
                TextField { id: freezeLabel; Layout.preferredWidth: 130; placeholderText: "Label" }
                TextField { id: freezeTtl; Layout.preferredWidth: 78; placeholderText: "TTL ms"; text: "0" }
                Button {
                    text: "Add freeze"
                    enabled: CortexPayload.ready && CortexApp.mutationPermission && freezeAddress.text.length > 0 && freezeValue.text.length > 0
                    onClicked: CortexFeatures.addFreeze(freezeAddress.text, freezeType.currentText, freezeValue.text, freezeLabel.text, parseInt(freezeTtl.text) || 0)
                }
                Item { Layout.fillWidth: true }
            }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    Label {
        Layout.fillWidth: true
        Layout.leftMargin: 10
        Layout.rightMargin: 10
        Layout.preferredHeight: visible ? 24 : 0
        verticalAlignment: Text.AlignVCenter
        visible: CortexFeatures.lastError.length > 0
        text: CortexFeatures.lastError
        color: Theme.error
        font.pixelSize: 10
        elide: Text.ElideRight
    }

    CortexSplitView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        orientation: Qt.Horizontal

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
                        spacing: 0
                        Label { Layout.preferredWidth: 48; text: "ID"; color: Theme.textMuted; font.pixelSize: 10 }
                        Label { Layout.preferredWidth: 150; text: "ADDRESS"; color: Theme.textMuted; font.pixelSize: 10 }
                        Label { Layout.preferredWidth: 70; text: "TYPE"; color: Theme.textMuted; font.pixelSize: 10 }
                        Label { Layout.fillWidth: true; text: "WATCH"; color: Theme.textMuted; font.pixelSize: 10 }
                    }
                }
                CortexListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: CortexFeatures.watches
                    delegate: Rectangle {
                        required property var modelData
                        width: ListView.view.width
                        height: 31
                        color: index % 2 ? Theme.background : Theme.surface
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            spacing: 0
                            Text { Layout.preferredWidth: 48; text: modelData.id; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 10 }
                            Text { Layout.preferredWidth: 150; text: modelData.address; color: Theme.codeNumber; font.family: Theme.monoFont; font.pixelSize: 10 }
                            Text { Layout.preferredWidth: 70; text: modelData.type; color: Theme.text; font.pixelSize: 10 }
                            Text { Layout.fillWidth: true; text: modelData.label; color: Theme.textMuted; font.pixelSize: 10; elide: Text.ElideRight }
                            Button { text: "Remove"; enabled: CortexApp.mutationPermission; onClicked: CortexFeatures.deleteWatch(modelData.id) }
                        }
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
                        spacing: 0
                        Label { Layout.preferredWidth: 48; text: "ID"; color: Theme.textMuted; font.pixelSize: 10 }
                        Label { Layout.preferredWidth: 140; text: "ADDRESS"; color: Theme.textMuted; font.pixelSize: 10 }
                        Label { Layout.preferredWidth: 65; text: "TYPE"; color: Theme.textMuted; font.pixelSize: 10 }
                        Label { Layout.preferredWidth: 120; text: "VALUE"; color: Theme.textMuted; font.pixelSize: 10 }
                        Label { Layout.fillWidth: true; text: "FREEZE"; color: Theme.textMuted; font.pixelSize: 10 }
                    }
                }
                CortexListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: CortexFeatures.freezes
                    delegate: Rectangle {
                        required property var modelData
                        width: ListView.view.width
                        height: 31
                        color: index % 2 ? Theme.background : Theme.surface
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            spacing: 0
                            Text { Layout.preferredWidth: 48; text: modelData.id; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 10 }
                            Text { Layout.preferredWidth: 140; text: modelData.address; color: Theme.codeNumber; font.family: Theme.monoFont; font.pixelSize: 10 }
                            Text { Layout.preferredWidth: 65; text: modelData.type; color: Theme.text; font.pixelSize: 10 }
                            Text { Layout.preferredWidth: 120; text: modelData.value; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 9; elide: Text.ElideRight }
                            Text { Layout.fillWidth: true; text: modelData.label + (modelData.ttl > 0 ? "  (" + modelData.ttl + " ms)" : ""); color: Theme.textMuted; font.pixelSize: 10; elide: Text.ElideRight }
                            Button { text: "Remove"; enabled: CortexApp.mutationPermission; onClicked: CortexFeatures.deleteFreeze(modelData.id) }
                        }
                    }
                }
            }
        }
    }
}
