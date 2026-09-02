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
        anchors.leftMargin: 9
        spacing: 8

        Item {
            Layout.preferredWidth: 27
            Layout.preferredHeight: 27

            Canvas {
                id: cortexMark
                anchors.fill: parent
                property color markColor: "#74777a"

                onMarkColorChanged: requestPaint()
                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
                Component.onCompleted: requestPaint()

                onPaint: {
                    var ctx = getContext("2d")
                    ctx.reset()
                    ctx.clearRect(0, 0, width, height)

                    var cx = width * 0.53
                    var cy = height * 0.5
                    var radius = Math.min(width, height) * 0.31

                    ctx.strokeStyle = markColor.toString()
                    ctx.lineWidth = Math.max(3.5, width * 0.16)
                    ctx.lineCap = "butt"
                    ctx.beginPath()
                    ctx.arc(cx, cy, radius, Math.PI * 0.24, Math.PI * 1.76, false)
                    ctx.stroke()

                    ctx.strokeStyle = markColor.toString()
                    ctx.lineWidth = Math.max(1.5, width * 0.075)
                    ctx.lineCap = "round"
                    ctx.beginPath()
                    ctx.moveTo(width * 0.05, cy)
                    ctx.lineTo(cx - width * 0.08, cy)
                    ctx.stroke()

                    var square = Math.max(3.5, width * 0.16)
                    ctx.fillStyle = markColor.toString()
                    ctx.fillRect(cx - square * 0.5, cy - square * 0.5, square, square)
                }
            }
        }

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
            Layout.preferredWidth: 330
            Layout.minimumWidth: 180
            Layout.maximumWidth: 330
            Layout.preferredHeight: 32
            onClicked: processPicker.open()

            contentItem: RowLayout {
                spacing: 6
                Text {
                    Layout.fillWidth: true
                    text: CortexApp.currentTargetIndex >= 0
                          ? CortexApp.currentTargetName + (CortexApp.attachedTargetCount > 1 ? "  (+" + (CortexApp.attachedTargetCount - 1) + ")" : "")
                          : (CortexApp.attachedTargetCount > 0 ? "Select target...  (" + CortexApp.attachedTargetCount + " attached)" : "Select target...")
                    color: CortexApp.currentTargetIndex >= 0 ? Theme.textBright : Theme.textMuted
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                    font.family: Theme.uiFont
                    font.pixelSize: 12
                }
                Text {
                    text: "v"
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
                text: refreshButton.text
                color: refreshButton.hovered ? Theme.textBright : Theme.textMuted
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                font.family: Theme.uiFont
                font.pixelSize: 11
            }
            background: Rectangle {
                color: refreshButton.hovered ? Theme.hover : "transparent"
                radius: Theme.radius
            }
        }

        Text {
            visible: root.width >= 1180
            Layout.maximumWidth: 260
            text: CortexApp.currentTargetIndex >= 0 ? CortexApp.currentTargetMeta : ""
            color: Theme.textDisabled
            elide: Text.ElideRight
            font.family: Theme.uiFont
            font.pixelSize: 10
        }

        Item { Layout.fillWidth: true }

        Rectangle {
            visible: CortexSettings.showAiActivityInTitleBar && CortexAi.connected
            Layout.preferredHeight: 24
            Layout.preferredWidth: aiStatusText.implicitWidth + 16
            color: Theme.surfaceRaised
            border.width: 1
            border.color: Theme.success
            radius: Theme.radius

            Text {
                id: aiStatusText
                anchors.centerIn: parent
                text: "● AI  " + CortexAi.activeTaskCount + " active"
                color: Theme.success
                font.family: Theme.uiFont
                font.pixelSize: 10
                font.bold: true
            }

            ToolTip.visible: aiStatusMouse.containsMouse
            ToolTip.text: CortexAi.sessionCount + " MCP session" + (CortexAi.sessionCount === 1 ? "" : "s") + " connected"
            MouseArea {
                id: aiStatusMouse
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.NoButton
            }
        }

        ToolButton {
            id: pausedButton
            visible: CortexDebugger.pausedThreads.length > 0
            Layout.preferredHeight: 32
            text: "Paused: " + CortexDebugger.pausedThreads.length
            onClicked: CortexApp.selectSection("Debugger")
            contentItem: Text {
                text: pausedButton.text
                color: Theme.mutation
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                font.family: Theme.uiFont
                font.pixelSize: 11
                font.bold: true
            }
            background: Rectangle {
                color: pausedButton.hovered ? Theme.hover : Theme.surfaceRaised
                border.width: 1
                border.color: Theme.mutation
                radius: Theme.radius
            }
            ToolTip.visible: hovered
            ToolTip.text: "Execution paused - open Debugger"
        }

        ToolButton {
            id: mutationButton
            Layout.preferredHeight: 32
            text: CortexApp.mutationPermission ? "Mutation: on" : "Mutation: off"
            onClicked: CortexApp.mutationPermission = !CortexApp.mutationPermission
            contentItem: Text {
                text: mutationButton.text
                color: CortexApp.mutationPermission ? Theme.mutation : Theme.textMuted
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                font.family: Theme.uiFont
                font.pixelSize: 11
            }
            background: Rectangle {
                color: mutationButton.hovered ? Theme.hover : "transparent"
                radius: Theme.radius
            }
        }

        ToolButton {
            id: settingsButton
            Layout.preferredHeight: 32
            text: "Settings"
            onClicked: CortexApp.selectSection("Settings")
            contentItem: Text {
                text: settingsButton.text
                color: settingsButton.hovered ? Theme.textBright : Theme.textMuted
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                font.family: Theme.uiFont
                font.pixelSize: 11
            }
            background: Rectangle {
                color: settingsButton.hovered ? Theme.hover : "transparent"
                radius: Theme.radius
            }
        }
        ToolButton {
            id: commandButton
            Layout.preferredHeight: 32
            text: "Command"
            onClicked: root.openCommandPalette()
            contentItem: Text {
                text: commandButton.text
                color: commandButton.hovered ? Theme.textBright : Theme.textMuted
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                font.family: Theme.uiFont
                font.pixelSize: 11
            }
            background: Rectangle {
                color: commandButton.hovered ? Theme.hover : "transparent"
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
                id: minimizeButton
                Layout.preferredWidth: 44
                Layout.fillHeight: true
                onClicked: root.appWindow.showMinimized()
                contentItem: Item {
                    Rectangle {
                        anchors.centerIn: parent
                        width: 10
                        height: 1
                        color: minimizeButton.hovered ? Theme.textBright : Theme.textMuted
                    }
                }
                background: Rectangle { color: minimizeButton.hovered ? Theme.hover : "transparent" }
            }

            ToolButton {
                id: maximizeButton
                Layout.preferredWidth: 44
                Layout.fillHeight: true
                onClicked: {
                    if (root.appWindow.visibility === Window.Maximized)
                        root.appWindow.showNormal()
                    else
                        root.appWindow.showMaximized()
                }
                contentItem: Item {
                    Item {
                        anchors.centerIn: parent
                        width: 11
                        height: 11

                        Rectangle {
                            visible: !root.appWindow || root.appWindow.visibility !== Window.Maximized
                            anchors.centerIn: parent
                            width: 9
                            height: 9
                            color: "transparent"
                            border.width: 1
                            border.color: maximizeButton.hovered ? Theme.textBright : Theme.textMuted
                        }

                        Rectangle {
                            visible: root.appWindow && root.appWindow.visibility === Window.Maximized
                            x: 2
                            y: 0
                            width: 8
                            height: 8
                            color: Theme.titleBar
                            border.width: 1
                            border.color: maximizeButton.hovered ? Theme.textBright : Theme.textMuted
                        }
                        Rectangle {
                            visible: root.appWindow && root.appWindow.visibility === Window.Maximized
                            x: 0
                            y: 3
                            width: 8
                            height: 8
                            color: Theme.titleBar
                            border.width: 1
                            border.color: maximizeButton.hovered ? Theme.textBright : Theme.textMuted
                        }
                    }
                }
                background: Rectangle { color: maximizeButton.hovered ? Theme.hover : "transparent" }
            }

            ToolButton {
                id: closeButton
                Layout.preferredWidth: 48
                Layout.fillHeight: true
                onClicked: root.appWindow.close()
                contentItem: Item {
                    Rectangle {
                        anchors.centerIn: parent
                        width: 12
                        height: 1
                        rotation: 45
                        color: closeButton.hovered ? "white" : Theme.textMuted
                        antialiasing: true
                    }
                    Rectangle {
                        anchors.centerIn: parent
                        width: 12
                        height: 1
                        rotation: -45
                        color: closeButton.hovered ? "white" : Theme.textMuted
                        antialiasing: true
                    }
                }
                background: Rectangle { color: closeButton.hovered ? "#c42b1c" : "transparent" }
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
        anchorX: 105
        anchorY: Theme.topBarHeight - 1
    }
}
