import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

Rectangle {
    id: root
    color: Theme.panel

    signal openCommandPalette()

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 8
        spacing: 8

        Label {
            text: "Cortex"
            color: Theme.textBright
            font.family: Theme.uiFont
            font.pixelSize: 13
            font.bold: true
        }

        Rectangle {
            Layout.preferredWidth: 1
            Layout.preferredHeight: 16
            color: Theme.borderStrong
        }

        ComboBox {
            id: targetPicker
            Layout.preferredWidth: 300
            Layout.preferredHeight: 26
            model: CortexApp.targets
            textRole: "name"
            currentIndex: CortexApp.currentTargetIndex
            displayText: currentIndex >= 0 ? currentText : "Select target"
            onActivated: CortexApp.selectTarget(currentIndex)

            indicator: null
            background: Rectangle {
                color: targetPicker.activeFocus ? "#20252a" : Theme.background
                border.color: targetPicker.activeFocus ? Theme.accent : Theme.borderStrong
                radius: Theme.radius
            }
            contentItem: Text {
                leftPadding: 8
                rightPadding: 8
                text: targetPicker.displayText
                color: Theme.text
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                font.family: Theme.uiFont
                font.pixelSize: 11
            }
        }

        ToolButton {
            Layout.preferredHeight: 26
            text: "Refresh"
            onClicked: CortexApp.refreshTargets()
            contentItem: Text {
                text: parent.text
                color: parent.hovered ? Theme.textBright : Theme.textMuted
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                font.pixelSize: 11
            }
            background: Rectangle {
                color: parent.hovered ? Theme.hover : "transparent"
                radius: Theme.radius
            }
        }

        Label {
            Layout.maximumWidth: 340
            text: CortexApp.currentTargetMeta
            color: Theme.textDisabled
            elide: Text.ElideRight
            font.family: Theme.uiFont
            font.pixelSize: 10
        }

        Item { Layout.fillWidth: true }

        ToolButton {
            Layout.preferredHeight: 26
            text: CortexApp.mutationPermission ? "Mutation: on" : "Mutation: off"
            onClicked: CortexApp.mutationPermission = !CortexApp.mutationPermission
            contentItem: Text {
                text: parent.text
                color: CortexApp.mutationPermission ? "#d7b36a" : Theme.textMuted
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                font.pixelSize: 10
            }
            background: Rectangle {
                color: parent.hovered ? Theme.hover : "transparent"
                radius: Theme.radius
            }
            ToolTip.visible: hovered
            ToolTip.text: CortexApp.mutationPermission ? "Disable mutation permission" : "Enable mutation permission"
        }

        ToolButton {
            Layout.preferredHeight: 26
            text: "Command"
            onClicked: root.openCommandPalette()
            contentItem: Text {
                text: parent.text
                color: parent.hovered ? Theme.textBright : Theme.textMuted
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                font.pixelSize: 11
            }
            background: Rectangle {
                color: parent.hovered ? Theme.hover : "transparent"
                radius: Theme.radius
            }
            ToolTip.visible: hovered
            ToolTip.text: "Command Palette (Ctrl+Shift+P / Ctrl+K)"
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: Theme.border
    }
}
