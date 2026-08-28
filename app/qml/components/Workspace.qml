import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

Rectangle {
    id: root
    color: Theme.background

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.workspaceBarHeight
            color: Theme.panel

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 9

                Label {
                    text: CortexApp.selectedSection
                    color: Theme.text
                    font.family: Theme.uiFont
                    font.pixelSize: 12
                }
                Rectangle {
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: 16
                    color: Theme.borderStrong
                }
                Label {
                    text: CortexApp.currentTargetIndex >= 0 ? CortexApp.currentTargetName : "No active target"
                    color: Theme.textMuted
                    font.family: Theme.uiFont
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }
                Item { Layout.fillWidth: true }
                Label {
                    visible: CortexApp.currentTargetIndex >= 0
                    text: CortexApp.capabilitySummary()
                    color: Theme.textDisabled
                    font.family: Theme.uiFont
                    font.pixelSize: 10
                    elide: Text.ElideRight
                    Layout.maximumWidth: 420
                }
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: Theme.border
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            Loader {
                anchors.fill: parent
                sourceComponent: CortexApp.selectedSection === "Overview" ? overviewComponent
                               : CortexApp.selectedSection === "Scanner" ? scannerComponent
                               : CortexApp.selectedSection === "Memory" ? memoryComponent
                               : CortexApp.selectedSection === "Debugger" ? debuggerComponent
                               : CortexApp.selectedSection === "Disassembly" ? disassemblyComponent
                               : genericToolComponent
            }
        }
    }

    Component {
        id: overviewComponent

        Flickable {
            contentWidth: width
            contentHeight: overviewColumn.implicitHeight + 56
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            ColumnLayout {
                id: overviewColumn
                width: Math.min(parent.width - 64, 820)
                anchors.left: parent.left
                anchors.leftMargin: 32
                anchors.top: parent.top
                anchors.topMargin: 28
                spacing: 0

                Label {
                    text: CortexApp.currentTargetIndex >= 0 ? CortexApp.currentTargetName : "No target selected"
                    color: Theme.textBright
                    font.family: Theme.uiFont
                    font.pixelSize: 23
                    font.bold: true
                }

                Label {
                    Layout.topMargin: 6
                    text: CortexApp.currentTargetIndex >= 0 ? CortexApp.currentTargetMeta : "Choose a process from the target picker to begin."
                    color: Theme.textMuted
                    font.family: Theme.uiFont
                    font.pixelSize: 12
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.topMargin: 22
                    Layout.bottomMargin: 18
                    Layout.preferredHeight: 1
                    color: Theme.border
                }

                Label {
                    text: "TARGET"
                    color: Theme.textDisabled
                    font.family: Theme.uiFont
                    font.pixelSize: 10
                    font.bold: true
                    font.letterSpacing: 0.55
                }

                GridLayout {
                    Layout.topMargin: 12
                    columns: 2
                    columnSpacing: 26
                    rowSpacing: 10

                    Label { text: "Platform"; color: Theme.textMuted; font.pixelSize: 12; Layout.preferredWidth: 130 }
                    Label { text: CortexApp.currentPlatform; color: Theme.text; font.pixelSize: 12 }
                    Label { text: "Architecture"; color: Theme.textMuted; font.pixelSize: 12 }
                    Label { text: CortexApp.currentArchitecture; color: Theme.text; font.pixelSize: 12; font.family: Theme.monoFont }
                    Label { text: "Processes"; color: Theme.textMuted; font.pixelSize: 12 }
                    Label { text: String(CortexApp.targetCount); color: Theme.text; font.pixelSize: 12 }
                    Label { text: "Mutation"; color: Theme.textMuted; font.pixelSize: 12 }
                    Label {
                        text: CortexApp.mutationPermission ? "Enabled" : "Disabled"
                        color: CortexApp.mutationPermission ? Theme.mutation : Theme.text
                        font.pixelSize: 12
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.topMargin: 22
                    Layout.bottomMargin: 18
                    Layout.preferredHeight: 1
                    color: Theme.border
                }

                Label {
                    text: "ACTIONS"
                    color: Theme.textDisabled
                    font.pixelSize: 10
                    font.bold: true
                    font.letterSpacing: 0.55
                }

                RowLayout {
                    Layout.topMargin: 10
                    spacing: 7

                    Button {
                        text: "Refresh targets"
                        onClicked: CortexApp.refreshTargets()
                        font.pixelSize: 12
                    }
                    Button {
                        text: CortexApp.mutationPermission ? "Disable mutation" : "Enable mutation"
                        enabled: CortexApp.currentTargetIndex >= 0
                        onClicked: CortexApp.mutationPermission = !CortexApp.mutationPermission
                        font.pixelSize: 12
                    }
                }
            }
        }
    }

    Component {
        id: scannerComponent

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 48
                color: Theme.background

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 8

                    TextField {
                        Layout.preferredWidth: 210
                        Layout.preferredHeight: 30
                        placeholderText: "Value"
                        font.pixelSize: 12
                    }
                    ComboBox {
                        Layout.preferredWidth: 110
                        Layout.preferredHeight: 30
                        model: ["i32", "i64", "f32", "f64", "string", "bytes"]
                        font.pixelSize: 11
                    }
                    ComboBox {
                        Layout.preferredWidth: 130
                        Layout.preferredHeight: 30
                        model: ["Exact value", "Changed", "Increased", "Decreased"]
                        font.pixelSize: 11
                    }
                    Button { text: "New Scan"; font.pixelSize: 11; enabled: CortexApp.currentTargetIndex >= 0 }
                    Button { text: "Next Scan"; font.pixelSize: 11; enabled: CortexApp.currentTargetIndex >= 0 }
                    Item { Layout.fillWidth: true }
                    Label {
                        text: "0 results"
                        color: Theme.textMuted
                        font.pixelSize: 11
                    }
                }

                Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 32
                color: Theme.panel

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 0
                    Label { Layout.preferredWidth: 190; text: "Address"; color: Theme.textMuted; font.pixelSize: 11 }
                    Label { Layout.preferredWidth: 160; text: "Value"; color: Theme.textMuted; font.pixelSize: 11 }
                    Label { Layout.preferredWidth: 160; text: "Previous"; color: Theme.textMuted; font.pixelSize: 11 }
                    Label { Layout.fillWidth: true; text: "Region"; color: Theme.textMuted; font.pixelSize: 11 }
                }
                Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Theme.background

                Text {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.leftMargin: 12
                    anchors.topMargin: 14
                    text: CortexApp.currentTargetIndex >= 0 ? "No scan results." : "Select a target to scan memory."
                    color: Theme.textDisabled
                    font.pixelSize: 11
                }
            }
        }
    }

    Component {
        id: memoryComponent

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 46
                color: Theme.background

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 8
                    Label { text: "Address"; color: Theme.textMuted; font.pixelSize: 11 }
                    TextField {
                        Layout.preferredWidth: 240
                        Layout.preferredHeight: 30
                        placeholderText: "0x0000000000000000"
                        font.family: Theme.monoFont
                        font.pixelSize: 11
                    }
                    Button { text: "Go"; font.pixelSize: 11; enabled: CortexApp.currentTargetIndex >= 0 }
                    Button { text: "Refresh"; font.pixelSize: 11; enabled: CortexApp.currentTargetIndex >= 0 }
                    Item { Layout.fillWidth: true }
                    Label { text: "16 bytes / row"; color: Theme.textDisabled; font.pixelSize: 10 }
                }
                Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 32
                color: Theme.panel
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 0
                    Label { Layout.preferredWidth: 190; text: "Address"; color: Theme.textMuted; font.pixelSize: 11 }
                    Label { Layout.fillWidth: true; text: "Hex"; color: Theme.textMuted; font.pixelSize: 11 }
                    Label { Layout.preferredWidth: 190; text: "ASCII"; color: Theme.textMuted; font.pixelSize: 11 }
                }
                Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Theme.background
                Text {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.leftMargin: 12
                    anchors.topMargin: 14
                    text: CortexApp.currentTargetIndex >= 0 ? "Memory view ready." : "Select a target to inspect memory."
                    color: Theme.textDisabled
                    font.pixelSize: 11
                }
            }
        }
    }

    Component {
        id: disassemblyComponent

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 46
                color: Theme.background
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 8
                    TextField {
                        Layout.preferredWidth: 260
                        Layout.preferredHeight: 30
                        placeholderText: "Address or symbol"
                        font.family: Theme.monoFont
                        font.pixelSize: 11
                    }
                    Button { text: "Go"; font.pixelSize: 11; enabled: CortexApp.currentTargetIndex >= 0 }
                    Button { text: "Back"; font.pixelSize: 11 }
                    Button { text: "Forward"; font.pixelSize: 11 }
                    Item { Layout.fillWidth: true }
                }
                Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 32
                color: Theme.panel
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    spacing: 0
                    Label { Layout.preferredWidth: 170; text: "Address"; color: Theme.textMuted; font.pixelSize: 11 }
                    Label { Layout.preferredWidth: 180; text: "Bytes"; color: Theme.textMuted; font.pixelSize: 11 }
                    Label { Layout.fillWidth: true; text: "Instruction"; color: Theme.textMuted; font.pixelSize: 11 }
                }
                Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Theme.background
                Text {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.leftMargin: 12
                    anchors.topMargin: 14
                    text: CortexApp.currentTargetIndex >= 0 ? "No disassembly loaded." : "Select a target to disassemble code."
                    color: Theme.textDisabled
                    font.pixelSize: 11
                }
            }
        }
    }

    Component {
        id: debuggerComponent

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
                    Button { text: "Continue"; font.pixelSize: 11; enabled: CortexApp.currentTargetIndex >= 0 }
                    Button { text: "Pause"; font.pixelSize: 11; enabled: CortexApp.currentTargetIndex >= 0 }
                    Button { text: "Step Into"; font.pixelSize: 11; enabled: CortexApp.currentTargetIndex >= 0 }
                    Button { text: "Step Over"; font.pixelSize: 11; enabled: CortexApp.currentTargetIndex >= 0 }
                    Item { Layout.fillWidth: true }
                    Label { text: "Debugger idle"; color: Theme.textMuted; font.pixelSize: 11 }
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
                                Label { text: "DISASSEMBLY"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
                            }
                            Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
                        }
                        Text {
                            Layout.leftMargin: 12
                            Layout.topMargin: 12
                            text: "No instruction pointer."
                            color: Theme.textDisabled
                            font.family: Theme.monoFont
                            font.pixelSize: 11
                        }
                        Item { Layout.fillHeight: true }
                    }
                }

                Rectangle {
                    SplitView.preferredWidth: 330
                    SplitView.minimumWidth: 250
                    color: Theme.surface
                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 0
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 32
                            color: Theme.panel
                            Text { anchors.left: parent.left; anchors.leftMargin: 12; anchors.verticalCenter: parent.verticalCenter; text: "REGISTERS"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
                            Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
                        }
                        Text {
                            Layout.leftMargin: 12
                            Layout.topMargin: 12
                            text: "RAX  —\nRBX  —\nRCX  —\nRDX  —\nRSP  —\nRBP  —\nRIP  —"
                            color: Theme.textMuted
                            font.family: Theme.monoFont
                            font.pixelSize: 11
                            lineHeight: 1.45
                        }
                        Item { Layout.fillHeight: true }
                    }
                }
            }
        }
    }

    Component {
        id: genericToolComponent

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
                    Button { text: "New"; font.pixelSize: 11; enabled: CortexApp.currentTargetIndex >= 0 }
                    Button { text: "Refresh"; font.pixelSize: 11; onClicked: CortexApp.refreshTargets() }
                    Item { Layout.fillWidth: true }
                    Label {
                        text: CortexApp.currentTargetIndex >= 0 ? CortexApp.currentTargetName : "No target"
                        color: Theme.textDisabled
                        font.pixelSize: 10
                    }
                }
                Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 32
                color: Theme.panel
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    spacing: 0
                    Label { Layout.preferredWidth: 260; text: "Name"; color: Theme.textMuted; font.pixelSize: 11 }
                    Label { Layout.preferredWidth: 160; text: "Status"; color: Theme.textMuted; font.pixelSize: 11 }
                    Label { Layout.fillWidth: true; text: "Details"; color: Theme.textMuted; font.pixelSize: 11 }
                }
                Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Theme.background
                Text {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.leftMargin: 12
                    anchors.topMargin: 14
                    text: CortexApp.currentTargetIndex >= 0 ? "No entries." : "Select a target to continue."
                    color: Theme.textDisabled
                    font.pixelSize: 11
                }
            }
        }
    }
}
