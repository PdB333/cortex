import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

Rectangle {
    id: root
    color: Theme.background

    property int activeTab: 0
    property var tabs: ["Events", "Console", "Breakpoints", "Watches", "MCP Calls", "Diagnostics"]

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 30
            color: Theme.panel

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                spacing: 1

                Repeater {
                    model: root.tabs
                    ToolButton {
                        required property string modelData
                        required property int index
                        Layout.preferredHeight: 30
                        text: modelData
                        checked: root.activeTab === index
                        onClicked: root.activeTab = index

                        contentItem: Text {
                            text: parent.text
                            color: parent.checked ? Theme.textBright : Theme.textMuted
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            font.pixelSize: 10
                        }
                        background: Rectangle {
                            color: parent.hovered ? Theme.hover : "transparent"

                            Rectangle {
                                visible: parent.parent.checked
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                height: 1
                                color: Theme.accent
                            }
                        }
                    }
                }
                Item { Layout.fillWidth: true }
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

            Column {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.leftMargin: 10
                anchors.topMargin: 9
                spacing: 4

                Text {
                    text: root.tabs[root.activeTab]
                    color: Theme.text
                    font.pixelSize: 10
                }
                Text {
                    text: root.activeTab === 4 ? "No MCP calls in this session." : "No entries."
                    color: Theme.textDisabled
                    font.pixelSize: 10
                }
            }
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 1
        color: Theme.border
    }
}
