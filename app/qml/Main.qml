import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

ApplicationWindow {
    id: root
    width: 1440
    height: 900
    minimumWidth: 1060
    minimumHeight: 680
    visible: true
    title: "Cortex"
    color: Theme.background

    palette.window: Theme.background
    palette.windowText: Theme.text
    palette.base: Theme.background
    palette.alternateBase: Theme.panel
    palette.text: Theme.text
    palette.button: Theme.panelRaised
    palette.buttonText: Theme.text
    palette.highlight: Theme.selection
    palette.highlightedText: Theme.textBright
    palette.toolTipBase: Theme.panel
    palette.toolTipText: Theme.text

    property bool bottomPanelVisible: true

    Shortcut {
        sequences: ["Ctrl+Shift+P", "Ctrl+K"]
        onActivated: commandPalette.open()
    }
    Shortcut {
        sequence: "Ctrl+J"
        onActivated: root.bottomPanelVisible = !root.bottomPanelVisible
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        TopBar {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.topBarHeight
            onOpenCommandPalette: commandPalette.open()
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Sidebar {
                Layout.preferredWidth: Theme.sidebarWidth
                Layout.fillHeight: true
            }

            Rectangle {
                Layout.preferredWidth: 1
                Layout.fillHeight: true
                color: Theme.border
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                Workspace {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                }

                BottomPanel {
                    Layout.fillWidth: true
                    Layout.preferredHeight: root.bottomPanelVisible ? 188 : 0
                    visible: root.bottomPanelVisible
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.bottomBarHeight
            color: Theme.accentDark

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 16

                Label {
                    text: CortexApp.currentTargetIndex >= 0 ? "Target selected" : "No target"
                    color: "white"
                    font.pixelSize: 12
                }
                Label {
                    text: CortexApp.currentPlatform + " / " + CortexApp.currentArchitecture
                    color: "white"
                    opacity: 0.9
                    font.pixelSize: 12
                }
                Item { Layout.fillWidth: true }
                Label {
                    text: CortexApp.mutationPermission ? "MUTATION ENABLED" : "Mutation permission: OFF"
                    color: CortexApp.mutationPermission ? "#ffd580" : "white"
                    font.bold: CortexApp.mutationPermission
                    font.pixelSize: 12
                }
                Label {
                    text: "MCP: preview"
                    color: "white"
                    opacity: 0.9
                    font.pixelSize: 12
                }
                Label {
                    text: "0.7.0-dev"
                    color: "white"
                    opacity: 0.8
                    font.pixelSize: 12
                }
            }
        }
    }

    CommandPalette {
        id: commandPalette
        anchors.centerIn: parent
    }
}
