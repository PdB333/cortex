import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import Cortex 1.0

Rectangle {
    id: root
    color: Theme.titleBar

    property var appWindow
    signal openCommandPalette()

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        onPressed: {
            if (Qt.platform.os === "windows" && root.appWindow)
                root.appWindow.startSystemMove()
        }
        onDoubleClicked: {
            if (Qt.platform.os !== "windows" || !root.appWindow) return
            if (root.appWindow.visibility === Window.Maximized)
                root.appWindow.showNormal()
            else
                root.appWindow.showMaximized()
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 11
        spacing: 8

        Text {
            text: "Cortex"
            color: Theme.textBright
            font.family: Theme.uiFont
            font.pixelSize: 14
            font.bold: true
        }

        Rectangle {
            Layout.preferredWidth: 1
            Layout.preferredHeight: 22
            color: Theme.borderStrong
        }

        ToolButton {
            id: targetButton
            Layout.preferredWidth: 350
            Layout.preferredHeight: 32
            onClicked: processPicker.open()

            contentItem: RowLayout {
                spacing: 6
                Text {
                    Layout.fillWidth: true
                    text: CortexApp.currentTargetIndex >= 0 ? CortexApp.currentTargetName : "Select target..."
                    color: CortexApp.currentTargetIndex >= 0 ? Theme.textBright : Theme.textMuted
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                    font.family: Theme.uiFont
                    font.pixelSize: 12
                }
                Text {
                    text: "⌄"
                    color: Theme.textMuted
                    font.pixelSize: 13
                }
            }

            background: Rectangle {
                color: targetButton.hovered || processPicker.opened ? Theme.surfaceRaised : Theme.input
                border.color: processPicker.opened ? Theme.accent : Theme.borderStrong
                border.width: 1
                radius: Theme.radius
            }
        }

        ToolButton {
            id: refreshButton
            text: "Refresh"
            Layout.preferredHeight: 32
            onClicked: CortexApp.refreshTargets()
            contentItem: Text {
                text: parent.text
                color: parent.hovered ? Theme.textBright : Theme.textMuted
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                font.family: Theme.uiFont
                font.pixelSize: 11
            }
            background: Rectangle {
                color: parent.hovered ? Theme.hover : "transparent"
                radius: Theme.radius
            }
        }

        Text {
            Layout.maximumWidth: 430
            text: CortexApp.currentTargetIndex >= 0 ? CortexApp.currentTargetMeta : ""
            color: Theme.textDisabled
            elide: Text.ElideRight
            font.family: Theme.uiFont
            font.pixelSize: 10
        }

        Item { Layout.fillWidth: true }

        ToolButton {
            id: mutationButton
            Layout.preferredHeight: 32
            text: CortexApp.mutationPermission ? "Mutation: on" : "Mutation: off"
            onClicked: CortexApp.mutationPermission = !CortexApp.mutationPermission
            contentItem: Text {
                text: parent.text
                color: CortexApp.mutationPermission ? Theme.mutation : Theme.textMuted
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                font.family: Theme.uiFont
                font.pixelSize: 11
            }
            background: Rectangle {
                color: parent.hovered ? Theme.hover : "transparent"
                radius: Theme.radius
            }
        }

        ToolButton {
            id: commandButton
            Layout.preferredHeight: 32
            text: "Command"
            onClicked: root.openCommandPalette()
            contentItem: Text {
                text: parent.text
                color: parent.hovered ? Theme.textBright : Theme.textMuted
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                font.family: Theme.uiFont
                font.pixelSize: 11
            }
            background: Rectangle {
                color: parent.hovered ? Theme.hover : "transparent"
                radius: Theme.radius
            }
            ToolTip.visible: hovered
            ToolTip.text: "Command Palette (Ctrl+Shift+P)"
        }

        RowLayout {
            visible: Qt.platform.os === "windows"
            Layout.preferredHeight: parent.height
            spacing: 0

            ToolButton {
                Layout.preferredWidth: 44
                Layout.fillHeight: true
                text: "—"
                onClicked: root.appWindow.showMinimized()
                contentItem: Text {
                    text: parent.text
                    color: Theme.textMuted
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.pixelSize: 12
                }
                background: Rectangle { color: parent.hovered ? Theme.hover : "transparent" }
            }

            ToolButton {
                Layout.preferredWidth: 44
                Layout.fillHeight: true
                text: root.appWindow && root.appWindow.visibility === Window.Maximized ? "❐" : "□"
                onClicked: {
                    if (root.appWindow.visibility === Window.Maximized)
                        root.appWindow.showNormal()
                    else
                        root.appWindow.showMaximized()
                }
                contentItem: Text {
                    text: parent.text
                    color: Theme.textMuted
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.pixelSize: 11
                }
                background: Rectangle { color: parent.hovered ? Theme.hover : "transparent" }
            }

            ToolButton {
                Layout.preferredWidth: 48
                Layout.fillHeight: true
                text: "×"
                onClicked: root.appWindow.close()
                contentItem: Text {
                    text: parent.text
                    color: parent.hovered ? "white" : Theme.textMuted
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.pixelSize: 17
                }
                background: Rectangle { color: parent.hovered ? "#c42b1c" : "transparent" }
            }
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: Theme.border
    }

    ProcessPicker {
        id: processPicker
        anchorX: 68
        anchorY: Theme.topBarHeight - 1
    }
}
