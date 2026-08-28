import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

Popup {
    id: root
    width: 640
    height: Math.min(500, contentColumn.implicitHeight + 18)
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: 0

    property var commands: [
        { label: "Target: Refresh local targets", action: "refreshTargets" },
        { label: "Target: Detach active session", action: "detach" },
        { label: "Runtime: Enable instrumentation", action: "runtime" },
        { label: "Session: Export runtime state", action: "exportSession" },
        { label: "Safety: Toggle mutation permission", action: "mutation" },
        { label: "View: Overview", section: "Overview", action: "view" },
        { label: "View: Memory", section: "Memory", action: "view" },
        { label: "View: Scanner", section: "Scanner", action: "view" },
        { label: "View: Disassembly", section: "Disassembly", action: "view" },
        { label: "View: Structures", section: "Structures", action: "view" },
        { label: "View: Modules", section: "Modules", action: "view" },
        { label: "View: Symbols", section: "Symbols", action: "view" },
        { label: "View: Debugger", section: "Debugger", action: "view" },
        { label: "View: Breakpoints", section: "Breakpoints", action: "view" },
        { label: "View: Traces", section: "Traces", action: "view" },
        { label: "View: Hooks", section: "Hooks", action: "view" },
        { label: "View: Network", section: "Network", action: "view" },
        { label: "View: Screenshots", section: "Screenshots", action: "view" },
        { label: "View: Diagnostics", section: "Diagnostics", action: "view" },
        { label: "View: Scripts", section: "Scripts", action: "view" },
        { label: "View: Input", section: "Input", action: "view" },
        { label: "View: Actions", section: "Actions", action: "view" },
        { label: "View: MCP", section: "MCP", action: "view" },
        { label: "View: Semantic", section: "Semantic", action: "view" },
        { label: "View: Sessions", section: "Sessions", action: "view" }
    ]

    function execute(command) {
        switch (command.action) {
        case "refreshTargets":
            CortexApp.refreshTargets()
            break
        case "detach":
            if (CortexApp.sessionActive) CortexApp.detachTarget()
            break
        case "runtime":
            if (CortexApp.sessionActive) CortexPayload.ensureReady()
            break
        case "exportSession":
            if (CortexApp.sessionActive) {
                CortexFeatures.exportSession()
                CortexApp.selectSection("Sessions")
            }
            break
        case "mutation":
            if (CortexApp.sessionActive)
                CortexApp.mutationPermission = !CortexApp.mutationPermission
            break
        default:
            if (command.section) CortexApp.selectSection(command.section)
            break
        }
        root.close()
    }

    background: Rectangle {
        color: Theme.panel
        border.color: Theme.borderStrong
        radius: 5
    }

    onOpened: {
        query.text = ""
        commandList.currentIndex = 0
        query.forceActiveFocus()
    }

    ColumnLayout {
        id: contentColumn
        anchors.fill: parent
        spacing: 0

        TextField {
            id: query
            Layout.fillWidth: true
            Layout.preferredHeight: 44
            placeholderText: "Type a Cortex command..."
            font.pixelSize: 13
            leftPadding: 12
            rightPadding: 12
            Keys.onDownPressed: commandList.incrementCurrentIndex()
            Keys.onUpPressed: commandList.decrementCurrentIndex()
            Keys.onReturnPressed: {
                if (commandList.currentIndex >= 0 && commandList.currentItem)
                    commandList.currentItem.runCommand()
            }
        }

        ListView {
            id: commandList
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(430, contentHeight)
            clip: true
            currentIndex: 0

            model: root.commands.filter(function(command) {
                return query.text.length === 0 || command.label.toLowerCase().indexOf(query.text.toLowerCase()) !== -1
            })

            delegate: Rectangle {
                id: commandDelegate
                required property var modelData
                required property int index
                width: commandList.width
                height: 38
                color: commandList.currentIndex === index ? Theme.selection : (hover.containsMouse ? Theme.hover : "transparent")

                function runCommand() { root.execute(modelData) }

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.right: statusText.left
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.label
                    color: Theme.text
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }

                Text {
                    id: statusText
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.action === "mutation"
                          ? (CortexApp.mutationPermission ? "ON" : "OFF")
                          : (modelData.action === "runtime" && CortexPayload.ready ? "ON" : "")
                    color: modelData.action === "mutation" && CortexApp.mutationPermission ? Theme.mutation : Theme.textDisabled
                    font.family: Theme.monoFont
                    font.pixelSize: 10
                }

                MouseArea {
                    id: hover
                    anchors.fill: parent
                    hoverEnabled: true
                    onEntered: commandList.currentIndex = index
                    onClicked: commandDelegate.runCommand()
                }
            }
        }
    }
}
