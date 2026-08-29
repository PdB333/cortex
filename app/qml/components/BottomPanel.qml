import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

Rectangle {
    id: root
    color: Theme.background
    border.color: Theme.border

    property int activeTab: 0
    property var tabs: ["Events", "Console", "Breakpoints", "Watches", "MCP Calls", "Diagnostics"]
    onActiveTabChanged: {
        if (!CortexPayload.ready) return
        if (activeTab === 0) CortexFeatures.refreshRuntimeEvents()
        else if (activeTab === 1) CortexFeatures.refreshApiLog()
    }

    Timer {
        interval: 750
        repeat: true
        running: root.activeTab === 0 && CortexPayload.ready
        onTriggered: CortexFeatures.refreshRuntimeEvents()
    }
    Timer {
        interval: 1000
        repeat: true
        running: root.activeTab === 1 && CortexPayload.ready
        onTriggered: CortexFeatures.refreshApiLog()
    }
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            color: Theme.panel

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                spacing: 1

                Repeater {
                    model: root.tabs
                    ToolButton {
                        required property string modelData
                        required property int index
                        text: modelData
                        checked: root.activeTab === index
                        onClicked: root.activeTab = index
                        Layout.preferredHeight: 33

                        contentItem: Text {
                            text: parent.text
                            color: parent.checked ? Theme.textBright : Theme.textMuted
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            font.family: Theme.uiFont
                            font.pixelSize: 11
                        }

                        background: Rectangle {
                            color: parent.hovered ? Theme.hover : "transparent"
                            Rectangle {
                                visible: parent.parent.checked
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                height: 2
                                color: Theme.accent
                            }
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

            CortexListView {
                anchors.fill: parent
                anchors.margins: 10
                visible: root.activeTab === 0
                clip: true
                model: CortexFeatures.runtimeEvents
                spacing: 2
                delegate: RowLayout {
                    required property var modelData
                    width: ListView.view.width
                    spacing: 8
                    Text { Layout.preferredWidth: 82; text: modelData.id; color: Theme.textDisabled; font.family: Theme.monoFont; font.pixelSize: 9 }
                    Text { Layout.preferredWidth: 145; text: modelData.type; color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 10; elide: Text.ElideRight }
                    Text { Layout.fillWidth: true; text: modelData.data; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 9; elide: Text.ElideRight }
                }
            }
            CortexListView {
                anchors.fill: parent
                anchors.margins: 10
                visible: root.activeTab === 1
                clip: true
                model: CortexFeatures.apiLog
                spacing: 2
                delegate: Text {
                    required property var modelData
                    width: ListView.view.width
                    text: modelData
                    color: Theme.textMuted
                    font.family: Theme.monoFont
                    font.pixelSize: 10
                    elide: Text.ElideRight
                }
            }
            Column {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: 12
                spacing: 7
                visible: root.activeTab !== 0 && root.activeTab !== 1

                Text {
                    text: root.tabs[root.activeTab]
                    color: Theme.text
                    font.family: Theme.uiFont
                    font.pixelSize: 11
                    font.bold: true
                }
                Text {
                    text: root.activeTab === 3
                          ? (CortexFeatures.watches.length + " active watches | " + CortexFeatures.freezes.length + " active freezes")
                          : root.activeTab === 4
                            ? "No MCP calls in this session."
                            : "No entries."
                    color: root.activeTab === 3 && (CortexFeatures.watches.length > 0 || CortexFeatures.freezes.length > 0) ? Theme.textMuted : Theme.textDisabled
                    font.family: Theme.uiFont
                    font.pixelSize: 11
                }
            }
        }
    }
}
