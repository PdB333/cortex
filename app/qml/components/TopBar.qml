import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

Rectangle {
    id: root
    color: Theme.panel
    border.color: Theme.border
    border.width: 0

    signal openCommandPalette()

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 10
        spacing: 10

        Label {
            text: "Cortex"
            color: Theme.textBright
            font.family: Theme.uiFont
            font.pixelSize: 15
            font.bold: true
        }

        Rectangle {
            Layout.preferredWidth: 1
            Layout.preferredHeight: 20
            color: Theme.borderStrong
        }

        ComboBox {
            id: targetPicker
            Layout.preferredWidth: 330
            model: CortexApp.targets
            textRole: "name"
            currentIndex: CortexApp.currentTargetIndex
            displayText: currentIndex >= 0 ? currentText : "Select target..."

            onActivated: CortexApp.selectTarget(currentIndex)

            background: Rectangle {
                color: targetPicker.hovered ? Theme.hover : Theme.background
                border.color: targetPicker.activeFocus ? Theme.accent : Theme.borderStrong
                radius: Theme.radius
            }
            contentItem: Text {
                leftPadding: 9
                text: targetPicker.displayText
                color: Theme.text
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                font.family: Theme.uiFont
                font.pixelSize: 12
            }
        }

        ToolButton {
            text: "Refresh"
            onClicked: CortexApp.refreshTargets()
            contentItem: Text {
                text: parent.text
                color: Theme.text
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                font.pixelSize: 12
            }
            background: Rectangle {
                color: parent.hovered ? Theme.hover : "transparent"
                radius: Theme.radius
            }
        }

        Label {
            Layout.maximumWidth: 380
            text: CortexApp.currentTargetMeta
            color: Theme.textMuted
            elide: Text.ElideRight
            font.family: Theme.uiFont
            font.pixelSize: 12
        }

        Item { Layout.fillWidth: true }

        Rectangle {
            implicitWidth: mutationText.implicitWidth + 18
            implicitHeight: 26
            color: CortexApp.mutationPermission ? "#3a2e20" : Theme.background
            border.color: CortexApp.mutationPermission ? Theme.warning : Theme.borderStrong
            radius: Theme.radius

            Text {
                id: mutationText
                anchors.centerIn: parent
                text: CortexApp.mutationPermission ? "Mutation enabled" : "Safe mode"
                color: CortexApp.mutationPermission ? "#ffd580" : Theme.textMuted
                font.pixelSize: 11
                font.bold: CortexApp.mutationPermission
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: CortexApp.mutationPermission = !CortexApp.mutationPermission
            }
        }

        ToolButton {
            text: "Command"
            onClicked: root.openCommandPalette()
            contentItem: Text {
                text: parent.text
                color: Theme.text
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                font.pixelSize: 12
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
