import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

Rectangle {
    id: root
    color: Theme.panel
    border.color: Theme.border

    property int activeTab: 0
    property var tabs: ["Events", "Console", "Breakpoints", "Watches", "MCP Calls", "Diagnostics"]

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 32
            color: Theme.panel

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                spacing: 2

                Repeater {
                    model: root.tabs
                    ToolButton {
                        required property string modelData
                        required property int index
                        text: modelData
                        checked: root.activeTab === index
                        onClicked: root.activeTab = index
                        contentItem: Text {
                            text: parent.text
                            color: parent.checked ? Theme.textBright : Theme.textMuted
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            font.pixelSize: 11
                        }
                        background: Rectangle {
                            color: parent.checked ? Theme.background : (parent.hovered ? Theme.hover : "transparent")
                            border.color: parent.checked ? Theme.borderStrong : "transparent"
                            radius: Theme.radius
                        }
                    }
                }
                Item { Layout.fillWidth: true }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Theme.border
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.background

            Column {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: 12
                spacing: 7

                Text {
                    text: root.tabs[root.activeTab]
                    color: Theme.text
                    font.pixelSize: 11
                    font.bold: true
                }
                Text {
                    text: root.activeTab === 4
                          ? "MCP request tracing will move here when the shared executor is connected to the application runtime."
                          : "No entries in this preview session."
                    color: Theme.textMuted
                    font.pixelSize: 11
                }
            }
        }
    }
}
