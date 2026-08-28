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
            spacing: 4
            Button { text: "Refresh"; font.pixelSize: 11; enabled: CortexApp.sessionActive; onClicked: CortexDebugger.refreshThreads() }
            Button { text: "Continue"; font.pixelSize: 11; enabled: false }
            Button { text: "Pause"; font.pixelSize: 11; enabled: false }
            Button { text: "Step Into"; font.pixelSize: 11; enabled: false }
            Button { text: "Step Over"; font.pixelSize: 11; enabled: false }
            Item { Layout.fillWidth: true }
            Label {
                text: CortexDebugger.lastError.length > 0
                    ? CortexDebugger.lastError
                    : (CortexApp.sessionActive ? "Inspect mode" : "No active session")
                color: CortexDebugger.lastError.length > 0 ? Theme.mutation : Theme.textMuted
                font.pixelSize: 11
            }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    SplitView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        orientation: Qt.Horizontal

        Rectangle {
            SplitView.fillWidth: true
            SplitView.minimumWidth: 420
            color: Theme.background
            ColumnLayout {
                anchors.fill: parent
                spacing: 0
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 32
                    color: Theme.panel
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        Text { text: "DISASSEMBLY"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: CortexDebugger.instructionPointer
                            color: Theme.textDisabled
                            font.family: Theme.monoFont
                            font.pixelSize: 10
                        }
                    }
                    Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
                }

                ListView {
                    id: disasmList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: CortexDisasm.rows
                    boundsBehavior: Flickable.StopAtBounds

                    delegate: Rectangle {
                        required property var modelData
                        width: disasmList.width
                        height: 25
                        color: index % 2 ? Theme.background : Theme.surface
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 8
                            spacing: 0
                            Text { Layout.preferredWidth: 155; text: modelData.address; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 10 }
                            Text { Layout.preferredWidth: 185; text: modelData.bytes; color: Theme.textDisabled; font.family: Theme.monoFont; font.pixelSize: 10; elide: Text.ElideRight }
                            Text { Layout.fillWidth: true; text: modelData.text; color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 10; elide: Text.ElideRight }
                        }
                    }
                }

                Text {
                    visible: CortexDisasm.rows.length === 0
                    Layout.leftMargin: 12
                    Layout.topMargin: 12
                    text: CortexDebugger.currentThreadId > 0 ? "No code loaded for this thread." : "Select a thread to inspect its instruction pointer."
                    color: Theme.textDisabled
                    font.family: Theme.monoFont
                    font.pixelSize: 11
                }
            }
        }

        Rectangle {
            SplitView.preferredWidth: 350
            SplitView.minimumWidth: 280
            color: Theme.surface
            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 32
                    color: Theme.panel
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 8
                        Text { text: "THREADS"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
                        Item { Layout.fillWidth: true }
                        Text { text: String(CortexDebugger.threads.length); color: Theme.textDisabled; font.pixelSize: 10 }
                    }
                    Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
                }

                ListView {
                    id: threadList
                    Layout.fillWidth: true
                    Layout.preferredHeight: 150
                    clip: true
                    model: CortexDebugger.threads
                    boundsBehavior: Flickable.StopAtBounds

                    delegate: Rectangle {
                        required property var modelData
                        width: threadList.width
                        height: 26
                        color: Number(modelData.id) === Number(CortexDebugger.currentThreadId) ? Theme.selection : (index % 2 ? Theme.surface : Theme.background)
                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.label
                            color: Theme.text
                            font.family: Theme.monoFont
                            font.pixelSize: 11
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                if (CortexDebugger.selectThread(modelData.id) && CortexDebugger.instructionPointer.length > 0)
                                    CortexDisasm.disassemble(CortexDebugger.instructionPointer, 64)
                            }
                        }
                    }
                }

                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 32
                    color: Theme.panel
                    Text { anchors.left: parent.left; anchors.leftMargin: 12; anchors.verticalCenter: parent.verticalCenter; text: "REGISTERS"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
                    Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
                }

                ListView {
                    id: registerList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: CortexDebugger.registers
                    boundsBehavior: Flickable.StopAtBounds

                    delegate: Rectangle {
                        required property var modelData
                        width: registerList.width
                        height: 24
                        color: index % 2 ? Theme.surface : Theme.background
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 10
                            Text { Layout.preferredWidth: 70; text: modelData.name; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 10 }
                            Text { Layout.fillWidth: true; text: modelData.value; color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 10; horizontalAlignment: Text.AlignRight }
                        }
                    }
                }
            }
        }
    }
}
