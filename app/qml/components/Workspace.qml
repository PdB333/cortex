import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

Rectangle {
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
                    text: CortexApp.sessionActive ? CortexApp.capabilitySummary() : CortexApp.sessionStatus
                    color: CortexApp.sessionActive ? Theme.textDisabled : (CortexApp.lastError.length ? Theme.error : Theme.textDisabled)
                    font.family: Theme.uiFont
                    font.pixelSize: 10
                    elide: Text.ElideRight
                    Layout.maximumWidth: 480
                }
            }

            Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
        }

        Loader {
            Layout.fillWidth: true
            Layout.fillHeight: true
            sourceComponent: CortexApp.selectedSection === "Overview" ? overviewComponent
                           : CortexApp.selectedSection === "Memory" ? memoryComponent
                           : CortexApp.selectedSection === "Scanner" ? scannerComponent
                           : CortexApp.selectedSection === "Debugger" ? debuggerComponent
                           : CortexApp.selectedSection === "Disassembly" ? disassemblyComponent
                           : genericComponent
        }
    }

    Component { id: overviewComponent; OverviewView {} }
    Component { id: memoryComponent; MemoryView {} }
    Component { id: scannerComponent; ScannerView {} }
    Component { id: debuggerComponent; DebuggerView {} }
    Component { id: disassemblyComponent; DisassemblyView {} }
    Component { id: genericComponent; GenericToolView {} }
}
