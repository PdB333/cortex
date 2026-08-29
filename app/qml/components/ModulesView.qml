import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

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
                text: "Refresh"
                font.pixelSize: 11
                enabled: CortexApp.sessionActive
                onClicked: CortexApp.refreshModules()
            }
            Label { text: "Double-click a module to disassemble from its base"; color: Theme.textDisabled; font.pixelSize: 9 }
            Item { Layout.fillWidth: true }
            Label {
                text: CortexApp.modules.length + " modules"
                color: Theme.textMuted
                font.pixelSize: 11
            }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 32
        color: Theme.panel
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            spacing: 0
            Label { Layout.preferredWidth: 220; text: "Name"; color: Theme.textMuted; font.pixelSize: 11 }
            Label { Layout.preferredWidth: 190; text: "Base"; color: Theme.textMuted; font.pixelSize: 11 }
            Label { Layout.preferredWidth: 100; text: "Size"; color: Theme.textMuted; font.pixelSize: 11 }
            Label { Layout.fillWidth: true; text: "Path"; color: Theme.textMuted; font.pixelSize: 11 }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.fillHeight: true
        color: Theme.background

        CortexListView {
            anchors.fill: parent
            clip: true
            model: CortexApp.modules
            boundsBehavior: Flickable.StopAtBounds

            delegate: Rectangle {
                id: moduleRow
                required property var modelData
                required property int index
                width: ListView.view.width
                height: 30
                color: moduleMouse.containsMouse ? Theme.hover : (index % 2 ? Theme.background : Theme.surface)

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 0
                    Text { Layout.preferredWidth: 220; text: modelData.name; color: Theme.text; font.pixelSize: 11; elide: Text.ElideRight }
                    Text { Layout.preferredWidth: 190; text: modelData.base; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 11 }
                    Text { Layout.preferredWidth: 100; text: modelData.size; color: Theme.textMuted; font.pixelSize: 11 }
                    Text { Layout.fillWidth: true; text: modelData.path; color: Theme.textDisabled; font.pixelSize: 10; elide: Text.ElideMiddle }
                }

                MouseArea {
                    id: moduleMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton
                    onDoubleClicked: {
                        if (CortexDisasm.disassemble(moduleRow.modelData.base, 192))
                            CortexApp.selectSection("Disassembly")
                    }
                }
            }
        }

        Text {
            visible: CortexApp.modules.length === 0
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.leftMargin: 12
            anchors.topMargin: 14
            text: CortexApp.lastError.length ? CortexApp.lastError : (CortexApp.sessionActive ? "No modules found." : "Select a target to list modules.")
            color: CortexApp.lastError.length ? Theme.error : Theme.textDisabled
            font.pixelSize: 11
        }
    }
}
