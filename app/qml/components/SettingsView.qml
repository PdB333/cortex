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
            width: Math.min(parent.width - 64, 980)
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
                text: "Defaults that change Cortex runtime, analysis and debugging behavior. Safety permissions are never remembered here."
                color: Theme.textMuted
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: runtimeColumn.implicitHeight + 24
                color: Theme.surface
                border.color: Theme.border
                radius: Theme.radius

                ColumnLayout {
                    id: runtimeColumn
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 10

                    Label { text: "RUNTIME & DIAGNOSTICS"; color: Theme.textDisabled; font.pixelSize: 10; font.bold: true }

                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout { Layout.fillWidth: true; spacing: 1
                            Label { text: "Load runtime automatically after attach"; color: Theme.text; font.pixelSize: 12 }
                            Label { text: "Inject/connect Cortex instrumentation as soon as a target session becomes active."; color: Theme.textMuted; font.pixelSize: 10 }
                        }
                        SettingsToggle { checked: CortexSettings.autoLoadRuntimeOnAttach; onToggled: function(value) { CortexSettings.autoLoadRuntimeOnAttach = value } }
                    }

                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout { Layout.fillWidth: true; spacing: 1
                            Label { text: "Legacy HTTP compatibility API"; color: Theme.text; font.pixelSize: 12 }
                            Label { text: "OFF is the normal mode. Native authenticated Named Pipe remains available."; color: Theme.textMuted; font.pixelSize: 10 }
                        }
                        SettingsToggle { checked: CortexSettings.httpApiEnabled; onToggled: function(value) { CortexSettings.httpApiEnabled = value } }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout { Layout.fillWidth: true; spacing: 1
                            Label { text: "Runtime diagnostics"; color: Theme.text; font.pixelSize: 12 }
                            Label { text: "Enable crash/hang diagnostics and the shared diagnostic channel."; color: Theme.textMuted; font.pixelSize: 10 }
                        }
                        SettingsToggle { checked: CortexSettings.diagnosticsEnabled; onToggled: function(value) { CortexSettings.diagnosticsEnabled = value } }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        enabled: CortexSettings.diagnosticsEnabled
                        opacity: enabled ? 1.0 : 0.45
                        ColumnLayout { Layout.fillWidth: true; spacing: 1
                            Label { text: "Write minidumps"; color: Theme.text; font.pixelSize: 12 }
                            Label { text: "Create a .dmp when Cortex captures a crash or confirmed hang."; color: Theme.textMuted; font.pixelSize: 10 }
                        }
                        SettingsToggle { checked: CortexSettings.diagnosticsWriteMinidump; onToggled: function(value) { CortexSettings.diagnosticsWriteMinidump = value } }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        enabled: CortexSettings.diagnosticsEnabled
                        opacity: enabled ? 1.0 : 0.45
                        ColumnLayout { Layout.fillWidth: true; spacing: 1
                            Label { text: "Crash / dump directory"; color: Theme.text; font.pixelSize: 12 }
                            Label { text: "Leave empty to use the runtime-local cortex_crashes directory."; color: Theme.textMuted; font.pixelSize: 10 }
                        }
                        TextField {
                            Layout.preferredWidth: 390
                            text: CortexSettings.diagnosticsCrashDirectory
                            placeholderText: "Default runtime directory"
                            onEditingFinished: CortexSettings.diagnosticsCrashDirectory = text
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        enabled: CortexSettings.diagnosticsEnabled
                        opacity: enabled ? 1.0 : 0.45
                        ColumnLayout { Layout.fillWidth: true; spacing: 1
                            Label { text: "Symbol search path"; color: Theme.text; font.pixelSize: 12 }
                            Label { text: "PDB/search directory used by runtime diagnostics and symbolization."; color: Theme.textMuted; font.pixelSize: 10 }
                        }
                        TextField {
                            Layout.preferredWidth: 390
                            text: CortexSettings.diagnosticsSymbolPath
                            placeholderText: "Default cortex_symbols directory"
                            onEditingFinished: CortexSettings.diagnosticsSymbolPath = text
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        enabled: CortexSettings.diagnosticsEnabled
                        opacity: enabled ? 1.0 : 0.45
                        ColumnLayout { Layout.fillWidth: true; spacing: 1
                            Label { text: "Maximum diagnostic stack frames"; color: Theme.text; font.pixelSize: 12 }
                            Label { text: "Upper bound used when symbolizing captured stacks."; color: Theme.textMuted; font.pixelSize: 10 }
                        }
                        ComboBox {
                            Layout.preferredWidth: 120
                            property var values: [32, 64, 128, 256]
                            model: ["32", "64", "128", "256"]
                            currentIndex: Math.max(0, values.indexOf(CortexSettings.diagnosticsMaxStackFrames))
                            onActivated: CortexSettings.diagnosticsMaxStackFrames = values[currentIndex]
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: memoryColumn.implicitHeight + 24
                color: Theme.surface
                border.color: Theme.border
                radius: Theme.radius

                ColumnLayout {
                    id: memoryColumn
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 10

                    Label { text: "MEMORY & SCANNER"; color: Theme.textDisabled; font.pixelSize: 10; font.bold: true }

                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout { Layout.fillWidth: true; spacing: 1
                            Label { text: "Memory bytes per row"; color: Theme.text; font.pixelSize: 12 }
                            Label { text: "Width of the hex/ASCII rows in Memory."; color: Theme.textMuted; font.pixelSize: 10 }
                        }
                        ComboBox {
                            Layout.preferredWidth: 120
                            property var values: [8, 16, 32]
                            model: ["8 bytes", "16 bytes", "32 bytes"]
                            currentIndex: Math.max(0, values.indexOf(CortexSettings.memoryBytesPerRow))
                            onActivated: CortexSettings.memoryBytesPerRow = values[currentIndex]
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout { Layout.fillWidth: true; spacing: 1
                            Label { text: "Default memory read size"; color: Theme.text; font.pixelSize: 12 }
                            Label { text: "Amount fetched by Go, Refresh and address navigation."; color: Theme.textMuted; font.pixelSize: 10 }
                        }
                        ComboBox {
                            Layout.preferredWidth: 120
                            property var values: [128, 256, 512, 1024, 2048, 4096]
                            model: ["128 B", "256 B", "512 B", "1 KiB", "2 KiB", "4 KiB"]
                            currentIndex: Math.max(0, values.indexOf(CortexSettings.memoryReadSize))
                            onActivated: CortexSettings.memoryReadSize = values[currentIndex]
                        }
                    }

                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout { Layout.fillWidth: true; spacing: 1
                            Label { text: "Default scan type"; color: Theme.text; font.pixelSize: 12 }
                            Label { text: "Initial value type selected for a new Scanner workspace."; color: Theme.textMuted; font.pixelSize: 10 }
                        }
                        ComboBox {
                            Layout.preferredWidth: 120
                            property var values: ["i32", "i64", "f32", "f64", "string", "bytes"]
                            model: values
                            currentIndex: Math.max(0, values.indexOf(CortexSettings.defaultScanType))
                            onActivated: CortexSettings.defaultScanType = currentText
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout { Layout.fillWidth: true; spacing: 1
                            Label { text: "Maximum scan results"; color: Theme.text; font.pixelSize: 12 }
                            Label { text: "Stops an initial exact scan after this many matches."; color: Theme.textMuted; font.pixelSize: 10 }
                        }
                        ComboBox {
                            Layout.preferredWidth: 130
                            property var values: [1000, 5000, 10000, 25000, 50000]
                            model: ["1,000", "5,000", "10,000", "25,000", "50,000"]
                            currentIndex: Math.max(0, values.indexOf(CortexSettings.maxScanResults))
                            onActivated: CortexSettings.maxScanResults = values[currentIndex]
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: debuggerColumn.implicitHeight + 24
                color: Theme.surface
                border.color: Theme.border
                radius: Theme.radius

                ColumnLayout {
                    id: debuggerColumn
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 10

                    Label { text: "DEBUGGER & TRACE"; color: Theme.textDisabled; font.pixelSize: 10; font.bold: true }

                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout { Layout.fillWidth: true; spacing: 1
                            Label { text: "Default breakpoint action"; color: Theme.text; font.pixelSize: 12 }
                            Label { text: "Log keeps the target moving; Pause is intended for interactive stepping."; color: Theme.textMuted; font.pixelSize: 10 }
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
                            Label { text: "Hardware breakpoints process-global"; color: Theme.text; font.pixelSize: 12 }
                            Label { text: "Apply new hardware breakpoints to current and newly-created threads by default."; color: Theme.textMuted; font.pixelSize: 10 }
                        }
                        SettingsToggle { checked: CortexSettings.hardwareBreakpointsGlobal; onToggled: function(value) { CortexSettings.hardwareBreakpointsGlobal = value } }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout { Layout.fillWidth: true; spacing: 1
                            Label { text: "Default trace step budget"; color: Theme.text; font.pixelSize: 12 }
                            Label { text: "Maximum single-step count prefilled when starting a new trace."; color: Theme.textMuted; font.pixelSize: 10 }
                        }
                        ComboBox {
                            Layout.preferredWidth: 130
                            property var values: [1000, 10000, 50000, 100000, 500000]
                            model: ["1,000", "10,000", "50,000", "100,000", "500,000"]
                            currentIndex: Math.max(0, values.indexOf(CortexSettings.traceMaxSteps))
                            onActivated: CortexSettings.traceMaxSteps = values[currentIndex]
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout { Layout.fillWidth: true; spacing: 1
                            Label { text: "Trace events loaded per request"; color: Theme.text; font.pixelSize: 12 }
                            Label { text: "Controls how many recorded events the Trace viewer loads at once."; color: Theme.textMuted; font.pixelSize: 10 }
                        }
                        ComboBox {
                            Layout.preferredWidth: 120
                            property var values: [100, 250, 500, 1000, 2500]
                            model: ["100", "250", "500", "1,000", "2,500"]
                            currentIndex: Math.max(0, values.indexOf(CortexSettings.traceEventLoadLimit))
                            onActivated: CortexSettings.traceEventLoadLimit = values[currentIndex]
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: projectColumn.implicitHeight + 24
                color: Theme.surface
                border.color: Theme.border
                radius: Theme.radius

                ColumnLayout {
                    id: projectColumn
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 10

                    Label { text: "PROJECTS & SESSIONS"; color: Theme.textDisabled; font.pixelSize: 10; font.bold: true }

                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout { Layout.fillWidth: true; spacing: 1
                            Label { text: "Project storage directory"; color: Theme.text; font.pixelSize: 12 }
                            Label { text: "Per-target knowledge is auto-loaded and saved immediately. Empty uses runtime/cortex_projects."; color: Theme.textMuted; font.pixelSize: 10 }
                        }
                        TextField {
                            Layout.preferredWidth: 390
                            text: CortexSettings.projectDirectory
                            placeholderText: "Default cortex_projects directory"
                            onEditingFinished: CortexSettings.projectDirectory = text
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout { Layout.fillWidth: true; spacing: 1
                            Label { text: "Session export directory"; color: Theme.text; font.pixelSize: 12 }
                            Label { text: "Destination for persistent runtime-state exports and cross-run comparisons."; color: Theme.textMuted; font.pixelSize: 10 }
                        }
                        TextField {
                            Layout.preferredWidth: 390
                            text: CortexSettings.sessionDirectory
                            placeholderText: "Default cortex_sessions directory"
                            onEditingFinished: CortexSettings.sessionDirectory = text
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout { Layout.fillWidth: true; spacing: 1
                            Label { text: "Session history retention"; color: Theme.text; font.pixelSize: 12 }
                            Label { text: "Oldest exported session folders are pruned after a successful new export."; color: Theme.textMuted; font.pixelSize: 10 }
                        }
                        ComboBox {
                            Layout.preferredWidth: 130
                            property var values: [0, 10, 25, 50, 100]
                            model: ["Unlimited", "10", "25", "50", "100"]
                            currentIndex: Math.max(0, values.indexOf(CortexSettings.sessionHistoryLimit))
                            onActivated: CortexSettings.sessionHistoryLimit = values[currentIndex]
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: "Projects always autosave. Patches, active breakpoints and write-state are never restored automatically; Mutation remains an explicit session decision."
                        color: Theme.textMuted
                        font.pixelSize: 10
                        wrapMode: Text.Wrap
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: mcpColumn.implicitHeight + 24
                color: Theme.surface
                border.color: Theme.border
                radius: Theme.radius

                ColumnLayout {
                    id: mcpColumn
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 10

                    Label { text: "MCP & AI ACTIVITY"; color: Theme.textDisabled; font.pixelSize: 10; font.bold: true }

                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout { Layout.fillWidth: true; spacing: 1
                            Label { text: "Default MCP tool profile"; color: Theme.text; font.pixelSize: 12 }
                            Label { text: "Compact starts with primitives; All exposes the complete catalog in the MCP workspace."; color: Theme.textMuted; font.pixelSize: 10 }
                        }
                        ComboBox {
                            Layout.preferredWidth: 120
                            model: ["Compact", "All"]
                            currentIndex: CortexSettings.mcpToolProfile === "all" ? 1 : 0
                            onActivated: CortexSettings.mcpToolProfile = currentIndex === 1 ? "all" : "compact"
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout { Layout.fillWidth: true; spacing: 1
                            Label { text: "AI Activity history limit"; color: Theme.text; font.pixelSize: 12 }
                            Label { text: "Maximum live MCP lifecycle rows retained by the desktop."; color: Theme.textMuted; font.pixelSize: 10 }
                        }
                        ComboBox {
                            Layout.preferredWidth: 120
                            property var values: [100, 300, 500, 1000, 2000]
                            model: ["100", "300", "500", "1,000", "2,000"]
                            currentIndex: Math.max(0, values.indexOf(CortexSettings.aiActivityHistoryLimit))
                            onActivated: CortexSettings.aiActivityHistoryLimit = values[currentIndex]
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout { Layout.fillWidth: true; spacing: 1
                            Label { text: "Show active AI status in title bar"; color: Theme.text; font.pixelSize: 12 }
                            Label { text: "Displays the global AI active-task indicator while an MCP session is connected."; color: Theme.textMuted; font.pixelSize: 10 }
                        }
                        SettingsToggle { checked: CortexSettings.showAiActivityInTitleBar; onToggled: function(value) { CortexSettings.showAiActivityInTitleBar = value } }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: safetyColumn.implicitHeight + 24
                color: Theme.surface
                border.color: Theme.border
                radius: Theme.radius
                ColumnLayout {
                    id: safetyColumn
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 5
                    Label { text: "SAFETY"; color: Theme.textDisabled; font.pixelSize: 10; font.bold: true }
                    Label {
                        Layout.fillWidth: true
                        text: "Mutation permission is intentionally not configurable or persistent. Every target/session returns to Observe mode and write/debug control must be enabled explicitly."
                        color: Theme.mutation
                        font.pixelSize: 10
                        wrapMode: Text.Wrap
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Button { text: "Reset technical defaults"; onClicked: CortexSettings.resetDefaults() }
            }
        }
    }
}
