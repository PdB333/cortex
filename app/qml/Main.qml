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
                    Layout.preferredHeight: root.bottomPanelVisible ? 168 : 0
                    visible: root.bottomPanelVisible
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.bottomBarHeight
            color: Theme.panel

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: 1
                color: Theme.border
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 12

                Label {
                    text: CortexApp.currentTargetIndex >= 0 ? CortexApp.currentTargetName : "No target"
                    color: Theme.textMuted
                    font.pixelSize: 9
                    elide: Text.ElideRight
                    Layout.maximumWidth: 260
                }
                Label {
                    text: CortexApp.currentPlatform + " / " + CortexApp.currentArchitecture
                    color: Theme.textDisabled
                    font.pixelSize: 9
                }
                Item { Layout.fillWidth: true }
                Label {
                    text: CortexApp.mutationPermission ? "Mutation on" : "Mutation off"
                    color: CortexApp.mutationPermission ? "#d7b36a" : Theme.textDisabled
                    font.pixelSize: 9
                }
                Label {
                    text: "MCP preview"
                    color: Theme.textDisabled
                    font.pixelSize: 9
                }
                Label {
                    text: "0.7.0-dev"
                    color: Theme.textDisabled
                    font.pixelSize: 9
                }
            }
        }
    }

    CommandPalette {
        id: commandPalette
        anchors.centerIn: parent
    }
}
