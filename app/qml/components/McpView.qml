import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

Rectangle {
    id: root
    property bool semanticOnly: false
    property var selectedTool: null
    color: Theme.background

    ListModel { id: visibleTools }

    function rebuildTools() {
        visibleTools.clear()
        const needle = searchField.text.toLowerCase().trim()
        for (let i = 0; i < CortexRuntime.tools.length; ++i) {
            const tool = CortexRuntime.tools[i]
            if (semanticOnly && !tool.semantic) continue
            if (!semanticOnly && tool.semantic && modeBox.currentIndex === 0) continue
            const haystack = (tool.name + " " + tool.description).toLowerCase()
            if (needle.length > 0 && haystack.indexOf(needle) < 0) continue
            visibleTools.append({
                toolName: tool.name,
                description: tool.description,
                method: tool.method,
                path: tool.path,
                risk: tool.risk,
                mutationRequired: tool.mutationRequired,
                semantic: tool.semantic,
                argumentTemplate: tool.argumentTemplate,
                hint: tool.hint
            })
        }
    }

    onSemanticOnlyChanged: rebuildTools()
    Component.onCompleted: {
        if (CortexPayload.ready && CortexRuntime.tools.length === 0) CortexRuntime.refreshTools()
        rebuildTools()
    }

    Connections {
        target: CortexRuntime
        function onToolsChanged() { root.rebuildTools() }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 44
            color: Theme.background
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 6

                Button {
                    text: CortexPayload.ready ? "Runtime On" : "Enable Runtime"
                    enabled: CortexApp.sessionActive && !CortexPayload.ready
                    font.pixelSize: 11
                    onClicked: {
                        if (CortexPayload.ensureReady()) CortexRuntime.refreshTools()
                    }
                }
                Button {
                    text: "Refresh Catalog"
                    enabled: CortexApp.sessionActive
                    font.pixelSize: 11
                    onClicked: CortexRuntime.refreshTools()
                }
                ComboBox {
                    id: modeBox
                    visible: !root.semanticOnly
                    Layout.preferredWidth: 128
                    Layout.preferredHeight: 30
                    model: ["Primitives", "All tools"]
                    onCurrentIndexChanged: root.rebuildTools()
                }
                TextField {
                    id: searchField
                    Layout.preferredWidth: 240
                    Layout.preferredHeight: 30
                    placeholderText: root.semanticOnly ? "Filter semantic tools" : "Filter MCP tools"
                    onTextChanged: root.rebuildTools()
                }
                Item { Layout.fillWidth: true }
                Label {
                    text: root.semanticOnly
                          ? CortexRuntime.semanticCount + " semantic"
                          : CortexRuntime.primitiveCount + " primitives · " + CortexRuntime.semanticCount + " semantic"
                    color: Theme.textDisabled
                    font.pixelSize: 10
                }
            }
            Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
        }

        CortexSplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Horizontal

            Rectangle {
                SplitView.preferredWidth: 330
                SplitView.minimumWidth: 250
                color: Theme.surface

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 31
                        color: Theme.panel
                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            text: root.semanticOnly ? "SEMANTIC TOOLS" : "MCP CATALOG"
                            color: Theme.textMuted
                            font.pixelSize: 10
                            font.bold: true
                        }
                        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
                    }

                    CortexListView {
                        id: toolList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: visibleTools
                        boundsBehavior: Flickable.StopAtBounds

                        delegate: Rectangle {
                            required property string toolName
                            required property string description
                            required property string method
                            required property string path
                            required property string risk
                            required property bool mutationRequired
                            required property bool semantic
                            required property string argumentTemplate
                            required property string hint

                            width: toolList.width
                            height: 48
                            color: toolName === (root.selectedTool ? root.selectedTool.name : "")
                                   ? Theme.selection
                                   : (hover.containsMouse ? Theme.hover : (index % 2 ? Theme.surface : Theme.background))

                            Column {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.leftMargin: 12
                                anchors.rightMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 2
                                Text {
                                    width: parent.width
                                    text: toolName
                                    color: Theme.text
                                    font.family: Theme.monoFont
                                    font.pixelSize: 11
                                    elide: Text.ElideRight
                                }
                                Text {
                                    width: parent.width
                                    text: semantic ? "semantic" : method + "  " + path
                                    color: mutationRequired ? Theme.mutation : Theme.textDisabled
                                    font.pixelSize: 9
                                    elide: Text.ElideRight
                                }
                            }

                            MouseArea {
                                id: hover
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: {
                                    root.selectedTool = {
                                        name: toolName,
                                        description: description,
                                        method: method,
                                        path: path,
                                        risk: risk,
                                        mutationRequired: mutationRequired,
                                        semantic: semantic,
                                        argumentTemplate: argumentTemplate,
                                        hint: hint
                                    }
                                    argsEdit.text = argumentTemplate.length > 0 ? argumentTemplate : "{}"
                                    CortexRuntime.clearResult()
                                }
                            }
                        }
                    }
                }
            }

            Rectangle {
                SplitView.fillWidth: true
                SplitView.minimumWidth: 460
                color: Theme.background

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 8

                    Text {
                        Layout.fillWidth: true
                        text: root.selectedTool ? root.selectedTool.name : "Select a tool"
                        color: Theme.textBright
                        font.family: Theme.uiFont
                        font.pixelSize: 16
                        font.bold: true
                        elide: Text.ElideRight
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: root.selectedTool !== null
                        text: root.selectedTool
                              ? (root.selectedTool.semantic
                                 ? "semantic · server-side plan/execution"
                                 : root.selectedTool.method + " " + root.selectedTool.path + " · risk: " + root.selectedTool.risk)
                              : ""
                        color: root.selectedTool && root.selectedTool.mutationRequired ? Theme.mutation : Theme.textMuted
                        font.family: Theme.monoFont
                        font.pixelSize: 10
                        elide: Text.ElideRight
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: root.selectedTool !== null
                        text: root.selectedTool ? root.selectedTool.description : ""
                        color: Theme.textMuted
                        font.pixelSize: 11
                        wrapMode: Text.WordWrap
                    }

                    Text { text: "ARGUMENTS"; color: Theme.textDisabled; font.pixelSize: 10; font.bold: true }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 155
                        color: Theme.inputBackground
                        border.color: argsEdit.activeFocus ? Theme.accent : Theme.borderStrong
                        border.width: 1

                        TextArea {
                            id: argsEdit
                            anchors.fill: parent
                            anchors.margins: 7
                            padding: 0
                            text: "{}"
                            color: Theme.text
                            selectionColor: Theme.selection
                            selectedTextColor: Theme.textBright
                            font.family: Theme.monoFont
                            font.pixelSize: 11
                            wrapMode: TextEdit.NoWrap
                            background: Item {}
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Button {
                            text: "Call"
                            enabled: root.selectedTool !== null && CortexPayload.ready &&
                                     (!root.selectedTool.mutationRequired || CortexApp.mutationPermission)
                            onClicked: CortexRuntime.callToolJson(root.selectedTool.name, argsEdit.text)
                        }
                        Label {
                            visible: root.selectedTool && root.selectedTool.mutationRequired && !CortexApp.mutationPermission
                            text: "Enable Mutation to call this tool"
                            color: Theme.mutation
                            font.pixelSize: 10
                        }
                        Item { Layout.fillWidth: true }
                        Label {
                            text: CortexRuntime.lastError
                            color: Theme.error
                            font.pixelSize: 10
                            elide: Text.ElideRight
                            Layout.maximumWidth: 340
                        }
                    }

                    Text {
                        visible: root.selectedTool && root.selectedTool.hint.length > 0
                        Layout.fillWidth: true
                        Layout.maximumHeight: 82
                        text: root.selectedTool ? root.selectedTool.hint : ""
                        color: Theme.textDisabled
                        font.family: Theme.monoFont
                        font.pixelSize: 9
                        wrapMode: Text.WrapAnywhere
                        elide: Text.ElideRight
                    }

                    Text { text: "RESULT"; color: Theme.textDisabled; font.pixelSize: 10; font.bold: true }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: Theme.surface
                        border.color: Theme.border
                        border.width: 1
                        CortexFlickable {
                            id: resultFlick
                            anchors.fill: parent
                            anchors.margins: 8
                            clip: true
                            contentWidth: Math.max(width, resultText.paintedWidth)
                            contentHeight: Math.max(height, resultText.paintedHeight)
                            boundsBehavior: Flickable.StopAtBounds
                            TextEdit {
                                id: resultText
                                width: Math.max(resultFlick.width, paintedWidth)
                                height: Math.max(resultFlick.height, paintedHeight)
                                text: CortexRuntime.lastResult.length > 0 ? CortexRuntime.lastResult : "No result yet."
                                color: CortexRuntime.lastResult.length > 0 ? Theme.text : Theme.textDisabled
                                readOnly: true
                                selectByMouse: true
                                font.family: Theme.monoFont
                                font.pixelSize: 10
                                wrapMode: TextEdit.NoWrap
                            }
                        }
                    }
                }
            }
        }
    }
}
