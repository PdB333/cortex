import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import Cortex 1.0

ApplicationWindow {
    id: root
    width: 1480
    height: 920
    minimumWidth: 1100
    minimumHeight: 700
    visible: true
    title: "Cortex"
    color: Theme.background

    property bool customFrame: Qt.platform.os === "windows"
    property bool bottomPanelVisible: true

    flags: customFrame ? (Qt.Window | Qt.FramelessWindowHint) : Qt.Window

    palette.window: Theme.background
    palette.windowText: Theme.text
    palette.base: Theme.background
    palette.alternateBase: Theme.panel
    palette.text: Theme.text
    palette.button: Theme.surfaceRaised
    palette.buttonText: Theme.text
    palette.highlight: Theme.selection
    palette.highlightedText: Theme.textBright
    palette.toolTipBase: Theme.surfaceRaised
    palette.toolTipText: Theme.text

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
            appWindow: root
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
                    Layout.preferredHeight: root.bottomPanelVisible ? 184 : 0
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
                anchors.leftMargin: 9
                anchors.rightMargin: 9
                spacing: 14

                Label {
                    text: CortexApp.currentTargetIndex >= 0 ? CortexApp.currentTargetName : "No target"
                    color: Theme.textMuted
                    font.family: Theme.uiFont
                    font.pixelSize: 10
                    elide: Text.ElideRight
                    Layout.maximumWidth: 300
                }
                Label {
                    text: CortexApp.currentPlatform + " / " + CortexApp.currentArchitecture
                    color: Theme.textDisabled
                    font.family: Theme.uiFont
                    font.pixelSize: 10
                }
                Item { Layout.fillWidth: true }
                Label {
                    text: CortexApp.mutationPermission ? "Mutation on" : "Mutation off"
                    color: CortexApp.mutationPermission ? Theme.mutation : Theme.textDisabled
                    font.family: Theme.uiFont
                    font.pixelSize: 10
                }
                Label {
                    text: "MCP preview"
                    color: Theme.textDisabled
                    font.family: Theme.uiFont
                    font.pixelSize: 10
                }
                Label {
                    text: "0.7.0-dev"
                    color: Theme.textDisabled
                    font.family: Theme.uiFont
                    font.pixelSize: 10
                }
            }
        }
    }

    CommandPalette {
        id: commandPalette
        anchors.centerIn: parent
    }

    Rectangle {
        anchors.fill: parent
        color: "transparent"
        border.color: Theme.borderStrong
        border.width: 1
        visible: root.customFrame && root.visibility !== Window.Maximized
        z: 900
    }

    MouseArea {
        visible: root.customFrame && root.visibility !== Window.Maximized
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 5
        z: 1000
        cursorShape: Qt.SizeHorCursor
        onPressed: root.startSystemResize(Qt.LeftEdge)
    }
    MouseArea {
        visible: root.customFrame && root.visibility !== Window.Maximized
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 5
        z: 1000
        cursorShape: Qt.SizeHorCursor
        onPressed: root.startSystemResize(Qt.RightEdge)
    }
    MouseArea {
        visible: root.customFrame && root.visibility !== Window.Maximized
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 5
        z: 1000
        cursorShape: Qt.SizeVerCursor
        onPressed: root.startSystemResize(Qt.TopEdge)
    }
    MouseArea {
        visible: root.customFrame && root.visibility !== Window.Maximized
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 5
        z: 1000
        cursorShape: Qt.SizeVerCursor
        onPressed: root.startSystemResize(Qt.BottomEdge)
    }
    MouseArea {
        visible: root.customFrame && root.visibility !== Window.Maximized
        anchors.left: parent.left
        anchors.top: parent.top
        width: 9
        height: 9
        z: 1001
        cursorShape: Qt.SizeFDiagCursor
        onPressed: root.startSystemResize(Qt.TopEdge | Qt.LeftEdge)
    }
    MouseArea {
        visible: root.customFrame && root.visibility !== Window.Maximized
        anchors.right: parent.right
        anchors.top: parent.top
        width: 9
        height: 9
        z: 1001
        cursorShape: Qt.SizeBDiagCursor
        onPressed: root.startSystemResize(Qt.TopEdge | Qt.RightEdge)
    }
    MouseArea {
        visible: root.customFrame && root.visibility !== Window.Maximized
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        width: 9
        height: 9
        z: 1001
        cursorShape: Qt.SizeBDiagCursor
        onPressed: root.startSystemResize(Qt.BottomEdge | Qt.LeftEdge)
    }
    MouseArea {
        visible: root.customFrame && root.visibility !== Window.Maximized
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: 9
        height: 9
        z: 1001
        cursorShape: Qt.SizeFDiagCursor
        onPressed: root.startSystemResize(Qt.BottomEdge | Qt.RightEdge)
    }
}
