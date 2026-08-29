import QtQuick
import QtQuick.Controls
import Cortex 1.0

Rectangle {
    id: root
    color: Theme.sidebar

    ListModel {
        id: navigation
        ListElement { group: "TARGET"; label: "Overview" }
        ListElement { group: "TARGET"; label: "Project" }
        ListElement { group: "TARGET"; label: "RE" }
        ListElement { group: "INSPECT"; label: "Memory" }
        ListElement { group: "INSPECT"; label: "Scanner" }
        ListElement { group: "INSPECT"; label: "Pointers" }
        ListElement { group: "INSPECT"; label: "Disassembly" }
        ListElement { group: "INSPECT"; label: "Structures" }
        ListElement { group: "INSPECT"; label: "Modules" }
        ListElement { group: "INSPECT"; label: "Symbols" }
        ListElement { group: "INSPECT"; label: "Snapshots" }
        ListElement { group: "DEBUG"; label: "Debugger" }
        ListElement { group: "DEBUG"; label: "Breakpoints" }
        ListElement { group: "DEBUG"; label: "Traces" }
        ListElement { group: "DEBUG"; label: "Patches" }
        ListElement { group: "DEBUG"; label: "Watches" }
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

    CortexListView {
        id: list
        anchors.fill: parent
        anchors.topMargin: 5
        anchors.bottomMargin: 5
        clip: true
        model: navigation
        boundsBehavior: Flickable.StopAtBounds
        section.property: "group"
        section.criteria: ViewSection.FullString


        section.delegate: Rectangle {
            width: list.width - 9
            height: 31
            color: Theme.sidebar

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 13
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 6
                text: section
                color: Theme.textDisabled
                font.family: Theme.uiFont
                font.pixelSize: 10
                font.bold: true
                font.letterSpacing: 0.65
            }
        }

        delegate: Rectangle {
            required property string label
            width: list.width - 9
            height: Theme.navRowHeight
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
                anchors.leftMargin: 15
                anchors.verticalCenter: parent.verticalCenter
                text: label
                color: CortexApp.selectedSection === label ? Theme.textBright : Theme.text
                font.family: Theme.uiFont
                font.pixelSize: 12
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

