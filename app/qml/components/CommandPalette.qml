import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

Popup {
    id: root
    width: 620
    height: Math.min(430, contentColumn.implicitHeight + 18)
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: 0

    property var commands: [
        { label: "Target: Refresh local targets", section: "Overview", action: "refresh" },
        { label: "View: Overview", section: "Overview", action: "view" },
        { label: "View: Memory", section: "Memory", action: "view" },
        { label: "View: Scanner", section: "Scanner", action: "view" },
        { label: "View: Disassembly", section: "Disassembly", action: "view" },
        { label: "View: Debugger", section: "Debugger", action: "view" },
        { label: "View: MCP", section: "MCP", action: "view" },
        { label: "View: Semantic", section: "Semantic", action: "view" },
        { label: "Safety: Toggle mutation permission", section: "Actions", action: "mutation" }
    ]

    background: Rectangle {
        color: Theme.panel
        border.color: Theme.borderStrong
        radius: 5
    }

    onOpened: {
        query.text = ""
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
            color: Theme.textBright
            placeholderTextColor: Theme.textDisabled
            font.pixelSize: 13
            leftPadding: 12
            rightPadding: 12
            background: Rectangle {
                color: Theme.background
                border.color: query.activeFocus ? Theme.accent : Theme.border
            }
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
            Layout.preferredHeight: Math.min(360, contentHeight)
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

                function runCommand() {
                    if (modelData.action === "refresh") CortexApp.refreshTargets()
                    else if (modelData.action === "mutation") CortexApp.mutationPermission = !CortexApp.mutationPermission
                    else CortexApp.selectSection(modelData.section)
                    root.close()
                }

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.label
                    color: Theme.text
                    font.pixelSize: 12
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
