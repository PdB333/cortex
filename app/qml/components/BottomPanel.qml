import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

Rectangle {
    id: root
    color: Theme.background
    border.color: Theme.border

    property int activeTab: 0
    property var tabs: ["Events", "Console", "Breakpoints", "Watches", "AI Activity", "Diagnostics"]
    onActiveTabChanged: {
        if (activeTab === 0 && CortexPayload.ready) CortexFeatures.refreshRuntimeEvents()
        else if (activeTab === 1 && CortexPayload.ready) CortexFeatures.refreshApiLog()
    }

    Timer {
        interval: CortexSettings.autoRefreshMs
        repeat: true
        running: root.activeTab === 0 && CortexPayload.ready
        onTriggered: CortexFeatures.refreshRuntimeEvents()
    }
    Timer {
        interval: Math.max(500, CortexSettings.autoRefreshMs)
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
                anchors.rightMargin: 8
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

                Text {
                    text: !CortexAi.listening
                          ? "AI activity listener unavailable"
                          : CortexAi.connected
                            ? ("● AI CONNECTED  ·  " + CortexAi.activeTaskCount + " active  ·  " + CortexAi.sessionCount + " session" + (CortexAi.sessionCount === 1 ? "" : "s"))
                            : "AI activity ready"
                    color: !CortexAi.listening ? Theme.warning : (CortexAi.connected ? Theme.success : Theme.textDisabled)
                    font.family: Theme.uiFont
                    font.pixelSize: 10
                    elide: Text.ElideRight
                    Layout.maximumWidth: 330
                }

                ToolButton {
                    visible: root.activeTab === 4 && CortexAi.activities.length > 0
                    text: "Clear"
                    onClicked: CortexAi.clear()
                    Layout.preferredHeight: 28
                    contentItem: Text {
                        text: parent.text
                        color: Theme.textMuted
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        font.family: Theme.uiFont
                        font.pixelSize: 10
                    }
                    background: Rectangle { color: parent.hovered ? Theme.hover : "transparent" }
                }
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

            CortexListView {
                id: aiActivityList
                anchors.fill: parent
                anchors.margins: 8
                visible: root.activeTab === 4
                clip: true
                model: CortexAi.activities
                spacing: 1

                delegate: Rectangle {
                    required property var modelData
                    width: ListView.view.width
                    height: detailText.visible ? 43 : 25
                    color: "transparent"

                    RowLayout {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        height: 24
                        spacing: 8

                        Text {
                            Layout.preferredWidth: 14
                            text: modelData.phase === "started" ? "●"
                                  : modelData.phase === "completed" ? "✓"
                                  : modelData.phase === "failed" ? "×"
                                  : modelData.phase === "ended" ? "◇" : "◆"
                            color: modelData.phase === "failed" ? Theme.error
                                   : modelData.phase === "completed" ? Theme.success
                                   : modelData.phase === "started" ? Theme.accent
                                   : Theme.textDisabled
                            font.family: Theme.uiFont
                            font.pixelSize: 11
                        }
                        Text {
                            Layout.preferredWidth: 78
                            text: modelData.time || ""
                            color: Theme.textDisabled
                            font.family: Theme.monoFont
                            font.pixelSize: 9
                        }
                        Text {
                            Layout.preferredWidth: 165
                            text: modelData.tool || modelData.client || "MCP session"
                            color: Theme.text
                            font.family: Theme.monoFont
                            font.pixelSize: 10
                            elide: Text.ElideRight
                        }
                        Text {
                            Layout.fillWidth: true
                            text: modelData.summary || ""
                            color: Theme.textMuted
                            font.family: Theme.uiFont
                            font.pixelSize: 10
                            elide: Text.ElideRight
                        }
                        Text {
                            Layout.preferredWidth: 150
                            text: modelData.target ? ("target " + modelData.target) : ""
                            color: Theme.textDisabled
                            font.family: Theme.monoFont
                            font.pixelSize: 9
                            elide: Text.ElideMiddle
                        }
                        Text {
                            Layout.preferredWidth: 62
                            text: modelData.duration_ms !== undefined && modelData.duration_ms !== null ? (modelData.duration_ms + " ms") : ""
                            color: Theme.textDisabled
                            font.family: Theme.monoFont
                            font.pixelSize: 9
                            horizontalAlignment: Text.AlignRight
                        }
                    }

                    Text {
                        id: detailText
                        anchors.left: parent.left
                        anchors.leftMargin: 100
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 18
                        visible: modelData.details !== undefined && modelData.details !== null && String(modelData.details).length > 0
                        text: visible ? String(modelData.details) : ""
                        color: Theme.textDisabled
                        font.family: Theme.monoFont
                        font.pixelSize: 9
                        elide: Text.ElideRight
                    }
                }

                Text {
                    anchors.centerIn: parent
                    visible: aiActivityList.count === 0
                    text: CortexAi.listening
                          ? "No AI activity yet. Start cortex.exe mcp and use a Cortex tool."
                          : "Another Cortex UI may already own the AI activity endpoint."
                    color: Theme.textDisabled
                    font.family: Theme.uiFont
                    font.pixelSize: 11
                }
            }

            Column {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: 12
                spacing: 7
                visible: root.activeTab !== 0 && root.activeTab !== 1 && root.activeTab !== 4

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
                          : "No entries."
                    color: root.activeTab === 3 && (CortexFeatures.watches.length > 0 || CortexFeatures.freezes.length > 0) ? Theme.textMuted : Theme.textDisabled
                    font.family: Theme.uiFont
                    font.pixelSize: 11
                }
            }
        }
    }
}
