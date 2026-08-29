import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

ColumnLayout {
    id: root
    anchors.fill: parent
    spacing: 0

    Component.onCompleted: if (CortexPayload.ready) CortexFeatures.refreshSnapshots()

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 82
        color: Theme.background
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 7
            spacing: 5
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Label { text: "CAPTURE"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true; Layout.preferredWidth: 58 }
                TextField { id: rangesField; Layout.fillWidth: true; placeholderText: '[{"address":"0x...","size":16}]'; text: "[]"; font.family: Theme.monoFont; font.pixelSize: 10 }
                TextField { id: labelField; Layout.preferredWidth: 160; placeholderText: "Label (optional)" }
                Button { text: "Capture"; enabled: CortexPayload.ready && rangesField.text.length > 2; onClicked: CortexFeatures.createSnapshot(rangesField.text, labelField.text) }
                Button { text: "Refresh"; enabled: CortexPayload.ready; onClicked: CortexFeatures.refreshSnapshots() }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Label { text: "ANALYZE"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true; Layout.preferredWidth: 58 }
                TextField { id: fromField; Layout.preferredWidth: 78; placeholderText: "From ID" }
                TextField { id: toField; Layout.preferredWidth: 78; placeholderText: "To ID" }
                Button { text: "Diff"; enabled: fromField.text.length > 0 && toField.text.length > 0; onClicked: CortexFeatures.diffSnapshots(parseInt(fromField.text), parseInt(toField.text)) }
                Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 18; color: Theme.borderStrong }
                TextField { id: changeAddress; Layout.preferredWidth: 150; placeholderText: "Address" }
                TextField { id: changeSize; Layout.preferredWidth: 80; placeholderText: "Size"; text: "4" }
                Button { text: "Last change"; enabled: changeAddress.text.length > 0; onClicked: CortexFeatures.lastSnapshotChange(changeAddress.text, parseInt(changeSize.text) || 4) }
                Item { Layout.fillWidth: true }
                Label { text: CortexApp.mutationPermission ? "Rewind/Delete enabled" : "Rewind/Delete require Mutation"; color: CortexApp.mutationPermission ? Theme.mutation : Theme.textDisabled; font.pixelSize: 9 }
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
                        Label { Layout.preferredWidth: 55; text: "ID"; color: Theme.textMuted; font.pixelSize: 10 }
                        Label { Layout.preferredWidth: 150; text: "TIME (MS)"; color: Theme.textMuted; font.pixelSize: 10 }
                        Label { Layout.fillWidth: true; text: "LABEL"; color: Theme.textMuted; font.pixelSize: 10 }
                        Label { Layout.preferredWidth: 85; text: "RANGES"; color: Theme.textMuted; font.pixelSize: 10 }
                        Label { Layout.preferredWidth: 100; text: "BYTES"; color: Theme.textMuted; font.pixelSize: 10 }
                        Item { Layout.preferredWidth: 160 }
                    }
                }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: CortexFeatures.snapshots
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
                            Text { Layout.preferredWidth: 55; text: modelData.id; color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 10 }
                            Text { Layout.preferredWidth: 150; text: modelData.timestamp; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 9 }
                            Text { Layout.fillWidth: true; text: modelData.label.length ? modelData.label : "(unlabeled)"; color: Theme.text; font.pixelSize: 10; elide: Text.ElideRight }
                            Text { Layout.preferredWidth: 85; text: modelData.rangeCount; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 9 }
                            Text { Layout.preferredWidth: 100; text: modelData.totalBytes; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 9 }
                            Button { text: "Rewind"; enabled: CortexApp.mutationPermission; onClicked: CortexFeatures.rewindSnapshot(modelData.id) }
                            Button { text: "Delete"; enabled: CortexApp.mutationPermission; onClicked: CortexFeatures.deleteSnapshot(modelData.id) }
                        }
                    }
                    Text { anchors.centerIn: parent; visible: parent.count === 0; text: CortexPayload.ready ? "No snapshots captured." : "Enable instrumentation to use target snapshots."; color: Theme.textDisabled; font.pixelSize: 10 }
                }
            }
        }

        Rectangle {
            SplitView.preferredHeight: 145
            SplitView.minimumHeight: 90
            color: Theme.input
            border.color: Theme.border
            ColumnLayout {
                anchors.fill: parent
                spacing: 0
                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 28; color: Theme.panel; Label { anchors.left: parent.left; anchors.leftMargin: 10; anchors.verticalCenter: parent.verticalCenter; text: "SNAPSHOT RESULT"; color: Theme.textMuted; font.pixelSize: 9; font.bold: true } }
                Flickable {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    contentWidth: width
                    contentHeight: resultText.implicitHeight + 16
                    TextEdit {
                        id: resultText
                        width: parent.width
                        readOnly: true
                        selectByMouse: true
                        wrapMode: TextEdit.WrapAnywhere
                        text: CortexFeatures.lastError.length ? CortexFeatures.lastError : (CortexFeatures.snapshotResult.length ? CortexFeatures.snapshotResult : "Capture or compare checkpoints to inspect changes.")
                        color: CortexFeatures.lastError.length ? Theme.error : (CortexFeatures.snapshotResult.length ? Theme.text : Theme.textDisabled)
                        font.family: Theme.monoFont
                        font.pixelSize: 10
                    }
                }
            }
        }
    }
}