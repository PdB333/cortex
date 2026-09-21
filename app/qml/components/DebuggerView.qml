import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

ColumnLayout {
    id: root
    anchors.fill: parent
    spacing: 0

    function reloadInstructionPointer() {
        if (CortexDebugger.instructionPointer.length > 0)
            CortexDisasm.disassemble(CortexDebugger.instructionPointer, 64)
    }

    function openAddressContext(address, sourceItem, x, y) {
        debuggerContext.address = address
        debuggerContext.label = "Debugger " + address
        debuggerContext.valueType = "i32"
        const p = sourceItem.mapToItem(root, x, y)
        debuggerContext.x = Math.max(0, Math.min(root.width - debuggerContext.implicitWidth, p.x))
        debuggerContext.y = Math.max(0, Math.min(root.height - 320, p.y))
        debuggerContext.open()
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 44
        color: Theme.background
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 4

            ComboBox {
                Layout.preferredWidth: 112
                model: ["Windows", "VEH"]
                currentIndex: CortexSettings.debuggerBackend === "veh" ? 1 : 0
                enabled: CortexApp.sessionActive
                onActivated: {
                    CortexSettings.debuggerBackend = currentIndex === 1 ? "veh" : "windows"
                    if (CortexDebugger.ready) CortexDebugger.clear()
                    CortexDebugger.refreshThreads()
                }
                ToolTip.visible: hovered
                ToolTip.text: currentIndex === 0
                    ? "External Windows debugger (recommended)"
                    : "In-process VEH debugger (loads Cortex runtime)"
            }
            Button {
                text: CortexDebugger.ready
                    ? (CortexDebugger.backend.toUpperCase() + " On")
                    : ("Enable " + (CortexSettings.debuggerBackend === "veh" ? "VEH" : "Windows"))
                font.pixelSize: 11
                enabled: CortexApp.sessionActive && !CortexDebugger.ready
                onClicked: {
                    if (CortexDebugger.enableRuntime()) {
                        CortexDebugger.refreshThreads()
                        root.reloadInstructionPointer()
                    }
                }
            }
            Button {
                text: "Refresh"
                font.pixelSize: 11
                enabled: CortexApp.sessionActive
                onClicked: {
                    CortexDebugger.refreshThreads()
                    if (CortexDebugger.ready) CortexDebugger.refreshRuntime()
                    root.reloadInstructionPointer()
                }
            }
            Button {
                text: "Continue"
                font.pixelSize: 11
                enabled: CortexDebugger.ready && CortexApp.mutationPermission && CortexDebugger.currentThreadId > 0
                onClicked: {
                    CortexDebugger.continueCurrent()
                    root.reloadInstructionPointer()
                }
            }
            Button {
                text: "Pause"
                font.pixelSize: 11
                enabled: CortexDebugger.ready && CortexApp.mutationPermission && CortexDebugger.currentThreadId > 0
                onClicked: {
                    if (CortexDebugger.pauseCurrent()) root.reloadInstructionPointer()
                }
            }
            Button {
                text: "Step Into"
                font.pixelSize: 11
                enabled: CortexDebugger.ready && CortexApp.mutationPermission && CortexDebugger.currentThreadId > 0
                onClicked: {
                    if (CortexDebugger.stepCurrent(2000)) root.reloadInstructionPointer()
                }
            }
            Button {
                text: "Step Over"
                font.pixelSize: 11
                enabled: CortexDebugger.ready && CortexApp.mutationPermission && CortexDebugger.currentThreadId > 0
                onClicked: {
                    if (CortexDebugger.stepOverCurrent(5000)) root.reloadInstructionPointer()
                }
            }
            Button {
                text: "Breakpoint @ IP"
                font.pixelSize: 11
                enabled: CortexDebugger.ready && CortexApp.mutationPermission && CortexDebugger.instructionPointer.length > 0
                onClicked: CortexDebugger.addBreakpoint(CortexDebugger.instructionPointer, "software", "pause")
            }
            Item { Layout.fillWidth: true }
            Label {
                text: CortexDebugger.lastError.length > 0
                    ? CortexDebugger.lastError
                    : (CortexDebugger.ready
                       ? (CortexDebugger.pausedThreads.length > 0
                          ? CortexDebugger.pausedThreads.length + " paused"
                          : CortexDebugger.backend.toUpperCase() + " debugger connected")
                       : (CortexApp.sessionActive ? "Inspect mode" : "No active session"))
                color: CortexDebugger.lastError.length > 0 ? Theme.mutation : Theme.textMuted
                font.pixelSize: 11
                elide: Text.ElideRight
                Layout.maximumWidth: 260
            }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    CortexSplitView {
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

                CortexListView {
                    id: disasmList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: CortexDisasm.rows
                    boundsBehavior: Flickable.StopAtBounds

                    delegate: Rectangle {
                        required property var modelData
                        required property int index
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
                        MouseArea {
                            id: debuggerDisasmMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            onPressed: function(event) {
                                if (event.button === Qt.RightButton)
                                    root.openAddressContext(modelData.address, debuggerDisasmMouse, event.x, event.y)
                            }
                            onDoubleClicked: function(event) {
                                if (event.button === Qt.LeftButton && CortexDebugger.ready && CortexApp.mutationPermission)
                                    CortexDebugger.addBreakpoint(modelData.address, "software", CortexSettings.breakpointDefaultAction)
                            }
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
            SplitView.preferredWidth: 360
            SplitView.minimumWidth: 290
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

                CortexListView {
                    id: threadList
                    Layout.fillWidth: true
                    Layout.preferredHeight: 135
                    clip: true
                    model: CortexDebugger.threads
                    boundsBehavior: Flickable.StopAtBounds

                    delegate: Rectangle {
                        required property var modelData
                        required property int index
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
                                if (CortexDebugger.selectThread(modelData.id)) root.reloadInstructionPointer()
                            }
                        }
                    }
                }

                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 32
                    color: Theme.panel
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 8
                        Text { text: "BREAKPOINTS"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
                        Item { Layout.fillWidth: true }
                        Text { text: String(CortexDebugger.breakpoints.length); color: Theme.textDisabled; font.pixelSize: 10 }
                    }
                    Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
                }

                CortexListView {
                    id: breakpointList
                    Layout.fillWidth: true
                    Layout.preferredHeight: CortexDebugger.ready ? 118 : 42
                    clip: true
                    model: CortexDebugger.breakpoints
                    boundsBehavior: Flickable.StopAtBounds

                    delegate: Rectangle {
                        required property var modelData
                        required property int index
                        width: breakpointList.width
                        height: 26
                        color: index % 2 ? Theme.surface : Theme.background
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 8
                            spacing: 6
                            Text { Layout.preferredWidth: 106; text: modelData.address; color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 10; elide: Text.ElideRight }
                            Text { Layout.fillWidth: true; text: modelData.kind + " · " + modelData.hitCount; color: Theme.textDisabled; font.pixelSize: 10; elide: Text.ElideRight }
                            Text {
                                text: "×"
                                visible: CortexApp.mutationPermission
                                color: removeArea.containsMouse ? Theme.textBright : Theme.textMuted
                                font.pixelSize: 14
                                MouseArea {
                                    id: removeArea
                                    anchors.fill: parent
                                    anchors.margins: -6
                                    hoverEnabled: true
                                    onClicked: CortexDebugger.removeBreakpoint(modelData.id)
                                }
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

                CortexListView {
                    id: registerList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: CortexDebugger.registers
                    boundsBehavior: Flickable.StopAtBounds

                    delegate: Rectangle {
                        required property var modelData
                        required property int index
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

    AddressContextMenu {
        id: debuggerContext
        allowSave: true
    }
}
