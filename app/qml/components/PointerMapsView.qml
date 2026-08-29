import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

ColumnLayout {
    id: root
    anchors.fill: parent
    spacing: 0

    Component.onCompleted: if (CortexPayload.ready) CortexFeatures.refreshPointerMaps()

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
                Label { text: "CAPTURE"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true; Layout.preferredWidth: 58 }
                TextField { id: nameField; Layout.preferredWidth: 150; placeholderText: "Stable name" }
                TextField { id: targetField; Layout.preferredWidth: 170; placeholderText: "Target address" }
                TextField { id: depthField; Layout.preferredWidth: 80; placeholderText: "Depth"; text: "5" }
                TextField { id: offsetField; Layout.preferredWidth: 100; placeholderText: "Max offset"; text: "4096" }
                Button {
                    text: "Capture map"
                    enabled: CortexPayload.ready && CortexApp.mutationPermission && nameField.text.length > 0 && targetField.text.length > 0
                    onClicked: CortexFeatures.capturePointerMap(nameField.text, targetField.text, parseInt(depthField.text) || 5, parseInt(offsetField.text) || 4096)
                }
                Button { text: "Refresh"; enabled: CortexPayload.ready; onClicked: CortexFeatures.refreshPointerMaps() }
                Item { Layout.fillWidth: true }
                Label { text: CortexApp.mutationPermission ? "Persistence enabled" : "Capture/Delete require Mutation"; color: CortexApp.mutationPermission ? Theme.mutation : Theme.textDisabled; font.pixelSize: 9 }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Label { text: "INTERSECT"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true; Layout.preferredWidth: 58 }
                TextField { id: namesField; Layout.fillWidth: true; placeholderText: '["session-a","session-b"]'; text: "[]"; font.family: Theme.monoFont; font.pixelSize: 10 }
                Button { text: "Find stable paths"; enabled: CortexPayload.ready && namesField.text.length > 4; onClicked: CortexFeatures.intersectPointerMaps(namesField.text) }
            }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    Label {
        Layout.fillWidth: true
        Layout.preferredHeight: visible ? 26 : 0
        visible: CortexFeatures.lastError.length > 0
        leftPadding: 10
        verticalAlignment: Text.AlignVCenter
        text: CortexFeatures.lastError
        color: Theme.error
        font.pixelSize: 10
    }

    SplitView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        orientation: Qt.Horizontal

        Rectangle {
            SplitView.preferredWidth: 470
            SplitView.minimumWidth: 320
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
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        spacing: 0
                        Label { Layout.preferredWidth: 135; text: "MAP"; color: Theme.textMuted; font.pixelSize: 10 }
                        Label { Layout.preferredWidth: 145; text: "TARGET"; color: Theme.textMuted; font.pixelSize: 10 }
                        Label { Layout.preferredWidth: 70; text: "PATHS"; color: Theme.textMuted; font.pixelSize: 10 }
                        Label { Layout.fillWidth: true; text: "STATE"; color: Theme.textMuted; font.pixelSize: 10 }
                    }
                }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: CortexFeatures.pointerMaps
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
                            Text { Layout.preferredWidth: 135; text: modelData.name; color: Theme.text; font.pixelSize: 10; elide: Text.ElideRight }
                            Text { Layout.preferredWidth: 145; text: modelData.target; color: Theme.codeNumber; font.family: Theme.monoFont; font.pixelSize: 9; elide: Text.ElideRight }
                            Text { Layout.preferredWidth: 70; text: modelData.pathCount; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 9 }
                            Text { Layout.fillWidth: true; text: modelData.truncated ? "truncated" : "complete"; color: modelData.truncated ? Theme.warning : Theme.success; font.pixelSize: 9 }
                            Button { text: "Delete"; enabled: CortexApp.mutationPermission; onClicked: CortexFeatures.deletePointerMap(modelData.name) }
                        }
                    }
                    Text { anchors.centerIn: parent; visible: parent.count === 0; text: CortexPayload.ready ? "No persisted pointer maps." : "Enable instrumentation to use pointer maps."; color: Theme.textDisabled; font.pixelSize: 10 }
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
                        anchors.leftMargin: 10
                        spacing: 0
                        Label { Layout.preferredWidth: 160; text: "MODULE"; color: Theme.textMuted; font.pixelSize: 10 }
                        Label { Layout.preferredWidth: 120; text: "BASE OFFSET"; color: Theme.textMuted; font.pixelSize: 10 }
                        Label { Layout.fillWidth: true; text: "OFFSETS"; color: Theme.textMuted; font.pixelSize: 10 }
                        Label { Layout.preferredWidth: 75; text: "SESSIONS"; color: Theme.textMuted; font.pixelSize: 10 }
                        Label { Layout.preferredWidth: 65; text: "SCORE"; color: Theme.textMuted; font.pixelSize: 10 }
                    }
                }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: CortexFeatures.pointerPaths
                    delegate: Rectangle {
                        required property var modelData
                        required property int index
                        width: ListView.view.width
                        height: 31
                        color: index % 2 ? Theme.background : Theme.surface
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 8
                            spacing: 0
                            Text { Layout.preferredWidth: 160; text: modelData.module; color: Theme.text; font.pixelSize: 9; elide: Text.ElideRight }
                            Text { Layout.preferredWidth: 120; text: modelData.baseOffset; color: Theme.codeNumber; font.family: Theme.monoFont; font.pixelSize: 9 }
                            Text { Layout.fillWidth: true; text: modelData.offsets; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 9; elide: Text.ElideRight }
                            Text { Layout.preferredWidth: 75; text: modelData.sessions; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 9 }
                            Text { Layout.preferredWidth: 65; text: Number(modelData.score).toFixed(1); color: Theme.success; font.family: Theme.monoFont; font.pixelSize: 9 }
                        }
                    }
                    Text { anchors.centerIn: parent; visible: parent.count === 0; text: "Intersect at least two maps to rank stable pointer paths."; color: Theme.textDisabled; font.pixelSize: 10 }
                }
            }
        }
    }
}