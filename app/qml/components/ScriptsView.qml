import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

ColumnLayout {
    id: root
    anchors.fill: parent
    spacing: 0

    property string templateSource: "-- Cortex Lua\nprint('hello from Cortex')\n"

    function newScript() {
        CortexFeatures.clearScriptSelection()
        scriptName.text = ""
        codeEdit.text = root.templateSource
        scriptList.currentIndex = -1
        scriptName.forceActiveFocus()
    }

    function loadScript(name) {
        if (CortexFeatures.loadScript(name)) {
            scriptName.text = CortexFeatures.selectedScriptName
            codeEdit.text = CortexFeatures.selectedScriptSource
        }
    }

    Component.onCompleted: {
        if (CortexApp.sessionActive)
            CortexFeatures.refreshScripts()
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 46
        color: Theme.background

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 6

            Button {
                text: "New"
                onClicked: root.newScript()
            }
            TextField {
                id: scriptName
                Layout.preferredWidth: 190
                placeholderText: "Script name"
                font.family: Theme.monoFont
                font.pixelSize: 11
            }
            TextField {
                id: timeoutField
                Layout.preferredWidth: 90
                text: "5000"
                placeholderText: "Timeout ms"
                font.family: Theme.monoFont
                font.pixelSize: 11
            }
            Button {
                text: "Save"
                enabled: CortexApp.sessionActive && CortexApp.mutationPermission && scriptName.text.length > 0
                onClicked: {
                    if (CortexFeatures.saveScript(scriptName.text, codeEdit.text)) {
                        scriptName.text = CortexFeatures.selectedScriptName
                        codeEdit.text = CortexFeatures.selectedScriptSource
                    }
                }
            }
            Button {
                text: "Run buffer"
                enabled: CortexApp.sessionActive && CortexApp.mutationPermission && codeEdit.text.length > 0
                onClicked: CortexFeatures.runScriptBuffer(codeEdit.text, Math.max(100, parseInt(timeoutField.text) || 5000))
            }
            Button {
                text: "Run saved"
                enabled: CortexApp.sessionActive && CortexApp.mutationPermission && scriptName.text.length > 0
                onClicked: CortexFeatures.runSavedScript(scriptName.text, Math.max(100, parseInt(timeoutField.text) || 5000))
            }
            Button {
                text: "Delete"
                enabled: CortexApp.sessionActive && CortexApp.mutationPermission && scriptName.text.length > 0
                onClicked: {
                    if (CortexFeatures.deleteScript(scriptName.text))
                        root.newScript()
                }
            }
            Button {
                text: "Refresh"
                enabled: CortexApp.sessionActive
                onClicked: CortexFeatures.refreshScripts()
            }
            Item { Layout.fillWidth: true }
            Label {
                text: CortexApp.mutationPermission ? "Mutation on" : "Mutation required to execute/save"
                color: CortexApp.mutationPermission ? Theme.mutation : Theme.textDisabled
                font.pixelSize: 9
            }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    SplitView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        orientation: Qt.Horizontal

        Rectangle {
            SplitView.preferredWidth: 230
            SplitView.minimumWidth: 180
            color: Theme.background

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 31
                    color: Theme.panel
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 11
                        anchors.rightMargin: 9
                        Label { text: "SAVED SCRIPTS"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
                        Item { Layout.fillWidth: true }
                        Label { text: CortexFeatures.scripts.length; color: Theme.textDisabled; font.pixelSize: 10 }
                    }
                    Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
                }

                ListView {
                    id: scriptList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: CortexFeatures.scripts
                    boundsBehavior: Flickable.StopAtBounds

                    delegate: Rectangle {
                        required property var modelData
                        required property int index
                        width: ListView.view.width
                        height: 30
                        color: scriptName.text === modelData.name ? Theme.selection
                              : (mouse.containsMouse ? Theme.hover : (index % 2 ? Theme.background : Theme.surface))

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 12
                            anchors.right: parent.right
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.name
                            color: scriptName.text === modelData.name ? Theme.textBright : Theme.text
                            font.family: Theme.monoFont
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }
                        MouseArea {
                            id: mouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                scriptList.currentIndex = index
                                root.loadScript(modelData.name)
                            }
                        }
                    }

                    Text {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.margins: 12
                        visible: parent.count === 0
                        text: CortexApp.sessionActive ? "No saved Lua scripts." : "Select a target to manage scripts."
                        color: Theme.textDisabled
                        font.pixelSize: 10
                    }
                }
            }

            Rectangle { anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom; width: 1; color: Theme.border }
        }

        Rectangle {
            SplitView.fillWidth: true
            SplitView.minimumWidth: 420
            color: Theme.background

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 31
                    color: Theme.panel
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 11
                        anchors.rightMargin: 10
                        Label {
                            text: scriptName.text.length > 0 ? scriptName.text + ".lua" : "UNTITLED LUA BUFFER"
                            color: Theme.textMuted
                            font.family: Theme.monoFont
                            font.pixelSize: 10
                            font.bold: true
                        }
                        Item { Layout.fillWidth: true }
                        Label { text: codeEdit.text.length + " chars"; color: Theme.textDisabled; font.pixelSize: 9 }
                    }
                    Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: Theme.input
                    border.width: 1
                    border.color: Theme.border

                    Flickable {
                        anchors.fill: parent
                        anchors.margins: 10
                        clip: true
                        contentWidth: Math.max(width, codeEdit.implicitWidth)
                        contentHeight: Math.max(height, codeEdit.implicitHeight)

                        TextEdit {
                            id: codeEdit
                            width: Math.max(parent.width, implicitWidth)
                            height: Math.max(parent.height, implicitHeight)
                            text: root.templateSource
                            color: Theme.text
                            selectionColor: Theme.accentDark
                            selectedTextColor: Theme.textBright
                            font.family: Theme.monoFont
                            font.pixelSize: 11
                            selectByMouse: true
                            wrapMode: TextEdit.NoWrap
                            tabStopDistance: 32
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 30
                    color: Theme.panel
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 11
                        anchors.rightMargin: 10
                        Label { text: "OUTPUT"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
                        Item { Layout.fillWidth: true }
                        Label {
                            text: CortexFeatures.lastError
                            visible: text.length > 0
                            color: Theme.error
                            font.pixelSize: 9
                            elide: Text.ElideRight
                            Layout.maximumWidth: 420
                        }
                    }
                    Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 170
                    color: Theme.input
                    border.width: 1
                    border.color: Theme.border

                    Flickable {
                        anchors.fill: parent
                        anchors.margins: 10
                        clip: true
                        contentWidth: width
                        contentHeight: outputText.implicitHeight
                        TextEdit {
                            id: outputText
                            width: parent.width
                            readOnly: true
                            selectByMouse: true
                            wrapMode: TextEdit.WrapAnywhere
                            text: CortexFeatures.scriptOutput.length > 0
                                  ? CortexFeatures.scriptOutput
                                  : "Run a buffer or saved script to see Lua output and return values."
                            color: CortexFeatures.scriptOutput.length > 0 ? Theme.text : Theme.textDisabled
                            selectionColor: Theme.accentDark
                            selectedTextColor: Theme.textBright
                            font.family: Theme.monoFont
                            font.pixelSize: 11
                        }
                    }
                }
            }
        }
    }
}