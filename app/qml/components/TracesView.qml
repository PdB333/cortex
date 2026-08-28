import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

ColumnLayout {
    anchors.fill: parent
    spacing: 0

    Component.onCompleted: if (CortexApp.sessionActive) CortexFeatures.refreshTraces()

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 44
        color: Theme.background
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 5
            TextField {
                id: threadField
                Layout.preferredWidth: 120
                text: CortexDebugger.currentThreadId > 0 ? CortexDebugger.currentThreadId.toString() : ""
                placeholderText: "Thread ID"
            }
            TextField { id: stepsField; Layout.preferredWidth: 100; text: "10000"; placeholderText: "Max steps" }
            Button {
                text: "Start trace"
                enabled: CortexApp.sessionActive && CortexApp.mutationPermission && threadField.text.length > 0
                onClicked: CortexFeatures.startTrace(parseInt(threadField.text), parseInt(stepsField.text))
            }
            Button { text: "Refresh"; enabled: CortexApp.sessionActive; onClicked: CortexFeatures.refreshTraces() }
            Button {
                text: "Load events"
                enabled: CortexFeatures.selectedTraceId >= 0
                onClicked: CortexFeatures.loadTraceEvents(CortexFeatures.selectedTraceId, 250)
            }
            Item { Layout.fillWidth: true }
            Label {
                text: CortexFeatures.lastError
                visible: text.length > 0
                color: Theme.error
                font.pixelSize: 10
                elide: Text.ElideRight
                Layout.maximumWidth: 320
            }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    SplitView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        orientation: Qt.Vertical

        Rectangle {
            SplitView.preferredHeight: 220
            SplitView.minimumHeight: 120
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
                        Label { Layout.preferredWidth: 60; text: "ID"; color: Theme.textMuted; font.pixelSize: 10 }
                        Label { Layout.preferredWidth: 100; text: "THREAD"; color: Theme.textMuted; font.pixelSize: 10 }
                        Label { Layout.preferredWidth: 90; text: "STATE"; color: Theme.textMuted; font.pixelSize: 10 }
                        Label { Layout.preferredWidth: 100; text: "STEPS"; color: Theme.textMuted; font.pixelSize: 10 }
                        Label { Layout.preferredWidth: 100; text: "EVENTS"; color: Theme.textMuted; font.pixelSize: 10 }
                        Label { Layout.fillWidth: true; text: "STOP REASON"; color: Theme.textMuted; font.pixelSize: 10 }
                    }
                }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: CortexFeatures.traces
                    delegate: Rectangle {
                        required property var modelData
                        width: ListView.view.width
                        height: 31
                        color: CortexFeatures.selectedTraceId === modelData.id ? Theme.selection : (mouse.containsMouse ? Theme.hover : Theme.background)
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            spacing: 0
                            Text { Layout.preferredWidth: 60; text: modelData.id; color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 10 }
                            Text { Layout.preferredWidth: 100; text: modelData.threadId; color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 10 }
                            Text { Layout.preferredWidth: 90; text: modelData.active ? "running" : "stopped"; color: modelData.active ? Theme.mutation : Theme.textMuted; font.pixelSize: 10 }
                            Text { Layout.preferredWidth: 100; text: modelData.steps; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 10 }
                            Text { Layout.preferredWidth: 100; text: modelData.eventCount; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 10 }
                            Text { Layout.fillWidth: true; text: modelData.reason; color: Theme.textMuted; font.pixelSize: 10; elide: Text.ElideRight }
                        }
                        MouseArea { id: mouse; anchors.fill: parent; hoverEnabled: true; onClicked: CortexFeatures.selectTrace(modelData.id) }
                    }
                }
            }
        }

        Rectangle {
            SplitView.fillHeight: true
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
                        Label { Layout.preferredWidth: 70; text: "SEQ"; color: Theme.textMuted; font.pixelSize: 10 }
                        Label { Layout.preferredWidth: 150; text: "INSTRUCTION"; color: Theme.textMuted; font.pixelSize: 10 }
                        Label { Layout.preferredWidth: 180; text: "BYTES"; color: Theme.textMuted; font.pixelSize: 10 }
                        Label { Layout.fillWidth: true; text: "REGISTERS"; color: Theme.textMuted; font.pixelSize: 10 }
                    }
                }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: CortexFeatures.traceEvents
                    delegate: Rectangle {
                        required property var modelData
                        width: ListView.view.width
                        height: 31
                        color: index % 2 ? Theme.background : Theme.surface
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            spacing: 0
                            Text { Layout.preferredWidth: 70; text: modelData.seq; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 10 }
                            Text { Layout.preferredWidth: 150; text: modelData.instruction; color: Theme.codeNumber; font.family: Theme.monoFont; font.pixelSize: 10 }
                            Text { Layout.preferredWidth: 180; text: modelData.bytes; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 10; elide: Text.ElideRight }
                            Text { Layout.fillWidth: true; text: modelData.registers; color: Theme.textDisabled; font.family: Theme.monoFont; font.pixelSize: 9; elide: Text.ElideRight }
                        }
                    }
                    Text {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.margins: 12
                        visible: parent.count === 0
                        text: CortexFeatures.selectedTraceId >= 0 ? "No events loaded." : "Select a trace to inspect its execution events."
                        color: Theme.textDisabled
                        font.pixelSize: 11
                    }
                }
            }
        }
    }
}
