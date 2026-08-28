import QtQuick
import QtQuick.Controls
import Cortex 1.0

Rectangle {
    id: root
    color: Theme.sidebar

    ListModel {
        id: navigation
        ListElement { group: "TARGET"; label: "Overview" }
        ListElement { group: "INSPECT"; label: "Memory" }
        ListElement { group: "INSPECT"; label: "Scanner" }
        ListElement { group: "INSPECT"; label: "Disassembly" }
        ListElement { group: "INSPECT"; label: "Structures" }
        ListElement { group: "INSPECT"; label: "Modules" }
        ListElement { group: "INSPECT"; label: "Symbols" }
        ListElement { group: "DEBUG"; label: "Debugger" }
        ListElement { group: "DEBUG"; label: "Breakpoints" }
        ListElement { group: "DEBUG"; label: "Traces" }
        ListElement { group: "DEBUG"; label: "Hooks" }
        ListElement { group: "OBSERVE"; label: "Network" }
        ListElement { group: "OBSERVE"; label: "Screenshots" }
        ListElement { group: "OBSERVE"; label: "Diagnostics" }
        ListElement { group: "AUTOMATE"; label: "Scripts" }
        ListElement { group: "AUTOMATE"; label: "Input" }
        ListElement { group: "AUTOMATE"; label: "Actions" }
        ListElement { group: "AI"; label: "MCP" }
        ListElement { group: "AI"; label: "Semantic" }
        ListElement { group: "AI"; label: "Sessions" }
    }

    Text {
        id: heading
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.leftMargin: 12
        anchors.topMargin: 8
        text: "CORTEX"
        color: Theme.textMuted
        font.family: Theme.uiFont
        font.pixelSize: 10
        font.bold: true
        font.letterSpacing: 0.6
    }

    ListView {
        id: list
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: heading.bottom
        anchors.bottom: parent.bottom
        anchors.topMargin: 5
        anchors.bottomMargin: 4
        clip: true
        model: navigation
        section.property: "group"
        section.criteria: ViewSection.FullString

        section.delegate: Rectangle {
            width: list.width
            height: 21
            color: Theme.sidebar

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: section
                color: Theme.textDisabled
                font.family: Theme.uiFont
                font.pixelSize: 9
                font.bold: true
                font.letterSpacing: 0.4
            }
        }

        delegate: Rectangle {
            required property string label
            width: list.width
            height: 25
            color: CortexApp.selectedSection === label ? Theme.selection : (mouse.containsMouse ? Theme.hover : "transparent")

            Rectangle {
                visible: CortexApp.selectedSection === label
                width: 2
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                color: Theme.accent
            }

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 13
                anchors.verticalCenter: parent.verticalCenter
                text: label
                color: CortexApp.selectedSection === label ? Theme.textBright : Theme.text
                font.family: Theme.uiFont
                font.pixelSize: 11
            }

            MouseArea {
                id: mouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: CortexApp.selectSection(label)
            }
        }
    }

    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: Theme.border
    }
}
