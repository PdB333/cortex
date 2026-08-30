import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

Rectangle {
    color: Theme.background

    CortexFlickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: content.implicitHeight + 56

        ColumnLayout {
            id: content
            width: Math.min(parent.width - 64, 900)
            x: 32
            y: 26
            spacing: 12

            Label {
                text: "Settings"
                color: Theme.textBright
                font.family: Theme.uiFont
                font.pixelSize: 22
                font.bold: true
            }
            Label {
                Layout.fillWidth: true
                text: "Cortex keeps all advanced tools available. These options only change how quickly the interface gets out of your way."
                color: Theme.textMuted
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: interfaceColumn.implicitHeight + 22
                color: Theme.surface
                border.color: Theme.border
                radius: Theme.radius
                ColumnLayout {
                    id: interfaceColumn
                    anchors.fill: parent
                    anchors.margins: 11
                    spacing: 9
                    Label { text: "INTERFACE"; color: Theme.textDisabled; font.pixelSize: 10; font.bold: true }

                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout { Layout.fillWidth: true; spacing: 1
                            Label { text: "Compact density"; color: Theme.text; font.pixelSize: 12 }
                            Label { text: "Tighter rows and controls, closer to a classic RE tool."; color: Theme.textMuted; font.pixelSize: 10 }
                        }
                        SettingsToggle { checked: CortexSettings.compactUi; onToggled: function(value) { CortexSettings.compactUi = value } }
                    }
                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }
                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout { Layout.fillWidth: true; spacing: 1
                            Label { text: "Mouse wheel speed"; color: Theme.text; font.pixelSize: 12 }
                            Label { text: "Applies to lists, memory views and long workspaces."; color: Theme.textMuted; font.pixelSize: 10 }
                        }
                        ComboBox {
                            Layout.preferredWidth: 110
                            model: ["1x", "2x", "3x", "4x", "5x"]
                            currentIndex: CortexSettings.scrollSpeed - 1
                            onActivated: CortexSettings.scrollSpeed = currentIndex + 1
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout { Layout.fillWidth: true; spacing: 1
                            Label { text: "Persistent scrollbars"; color: Theme.text; font.pixelSize: 12 }
                            Label { text: "Keep a draggable scrollbar visible whenever content can scroll."; color: Theme.textMuted; font.pixelSize: 10 }
                        }
                        SettingsToggle { checked: CortexSettings.persistentScrollbars; onToggled: function(value) { CortexSettings.persistentScrollbars = value } }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: workspaceColumn.implicitHeight + 22
                color: Theme.surface
                border.color: Theme.border
                radius: Theme.radius
                ColumnLayout {
                    id: workspaceColumn
                    anchors.fill: parent
                    anchors.margins: 11
                    spacing: 9
                    Label { text: "WORKSPACE"; color: Theme.textDisabled; font.pixelSize: 10; font.bold: true }
                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout { Layout.fillWidth: true; spacing: 1
                            Label { text: "Restore last section"; color: Theme.text; font.pixelSize: 12 }
                            Label { text: "Open Cortex where you left it on the next launch."; color: Theme.textMuted; font.pixelSize: 10 }
                        }
                        SettingsToggle { checked: CortexSettings.restoreLastSection; onToggled: function(value) { CortexSettings.restoreLastSection = value } }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout { Layout.fillWidth: true; spacing: 1
                            Label { text: "Remember window layout"; color: Theme.text; font.pixelSize: 12 }
                            Label { text: "Persist window geometry and bottom panel state."; color: Theme.textMuted; font.pixelSize: 10 }
                        }
                        SettingsToggle { checked: CortexSettings.rememberWindowLayout; onToggled: function(value) { CortexSettings.rememberWindowLayout = value } }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout { Layout.fillWidth: true; spacing: 1
                            Label { text: "Show advanced RE tools by default"; color: Theme.text; font.pixelSize: 12 }
                            Label { text: "JSON editors, Ghidra payloads and breakpoint templates stay available either way."; color: Theme.textMuted; font.pixelSize: 10 }
                        }
                        SettingsToggle { checked: CortexSettings.showAdvancedByDefault; onToggled: function(value) { CortexSettings.showAdvancedByDefault = value } }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: liveColumn.implicitHeight + 22
                color: Theme.surface
                border.color: Theme.border
                radius: Theme.radius
                ColumnLayout {
                    id: liveColumn
                    anchors.fill: parent
                    anchors.margins: 11
                    spacing: 9
                    Label { text: "LIVE DATA"; color: Theme.textDisabled; font.pixelSize: 10; font.bold: true }
                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout { Layout.fillWidth: true; spacing: 1
                            Label { text: "Auto-refresh interval"; color: Theme.text; font.pixelSize: 12 }
                            Label { text: "Used by watches, runtime events and API console polling."; color: Theme.textMuted; font.pixelSize: 10 }
                        }
                        ComboBox {
                            id: refreshBox
                            Layout.preferredWidth: 130
                            property var values: [250, 500, 750, 1000, 2000, 5000]
                            model: ["250 ms", "500 ms", "750 ms", "1 s", "2 s", "5 s"]
                            currentIndex: {
                                var found = values.indexOf(CortexSettings.autoRefreshMs)
                                return found >= 0 ? found : 2
                            }
                            onActivated: CortexSettings.autoRefreshMs = values[currentIndex]
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: debuggerColumn.implicitHeight + 22
                color: Theme.surface
                border.color: Theme.border
                radius: Theme.radius
                ColumnLayout {
                    id: debuggerColumn
                    anchors.fill: parent
                    anchors.margins: 11
                    spacing: 9
                    Label { text: "DEBUGGER"; color: Theme.textDisabled; font.pixelSize: 10; font.bold: true }
                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout { Layout.fillWidth: true; spacing: 1
                            Label { text: "New breakpoint action"; color: Theme.text; font.pixelSize: 12 }
                            Label { text: "Log is safer for exploratory RE; Pause is useful for interactive stepping."; color: Theme.textMuted; font.pixelSize: 10 }
                        }
                        ComboBox {
                            Layout.preferredWidth: 120
                            model: ["log", "pause"]
                            currentIndex: CortexSettings.breakpointDefaultAction === "pause" ? 1 : 0
                            onActivated: CortexSettings.breakpointDefaultAction = currentText
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout { Layout.fillWidth: true; spacing: 1
                            Label { text: "Hardware breakpoints process-global by default"; color: Theme.text; font.pixelSize: 12 }
                            Label { text: "New threads are automatically covered; you can still target one TID per breakpoint."; color: Theme.textMuted; font.pixelSize: 10 }
                        }
                        SettingsToggle { checked: CortexSettings.hardwareBreakpointsGlobal; onToggled: function(value) { CortexSettings.hardwareBreakpointsGlobal = value } }
                    }
                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }
                    Label {
                        Layout.fillWidth: true
                        text: "Safety: mutation permission always starts disabled after attach. This is intentionally not configurable."
                        color: Theme.mutation
                        font.pixelSize: 10
                        wrapMode: Text.Wrap
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Button { text: "Reset settings"; onClicked: CortexSettings.resetDefaults() }
            }
        }
    }
}