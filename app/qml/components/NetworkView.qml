import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

ColumnLayout {
    anchors.fill: parent
    spacing: 0

    Component.onCompleted: if (CortexApp.sessionActive) CortexFeatures.refreshNetwork()

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 44
        color: Theme.background
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 5
            Button {
                text: CortexFeatures.networkCaptureEnabled ? "Stop capture" : "Start capture"
                enabled: CortexApp.sessionActive && CortexApp.mutationPermission
                onClicked: {
                    if (CortexFeatures.setNetworkCapture(!CortexFeatures.networkCaptureEnabled))
                        CortexFeatures.refreshNetwork()
                }
            }
            Button { text: "Refresh"; enabled: CortexApp.sessionActive; onClicked: CortexFeatures.refreshNetwork() }
            Item { Layout.fillWidth: true }
            Rectangle {
                Layout.preferredWidth: 7
                Layout.preferredHeight: 7
                radius: 4
                color: CortexFeatures.networkCaptureEnabled ? Theme.success : Theme.textDisabled
            }
            Label {
                text: CortexFeatures.networkCaptureEnabled ? "Capture active" : "Capture stopped"
                color: Theme.textMuted
                font.pixelSize: 10
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
            spacing: 0
            Label { Layout.preferredWidth: 70; text: "ID"; color: Theme.textMuted; font.pixelSize: 10 }
            Label { Layout.preferredWidth: 70; text: "DIR"; color: Theme.textMuted; font.pixelSize: 10 }
            Label { Layout.preferredWidth: 130; text: "SOCKET"; color: Theme.textMuted; font.pixelSize: 10 }
            Label { Layout.preferredWidth: 90; text: "SIZE"; color: Theme.textMuted; font.pixelSize: 10 }
            Label { Layout.fillWidth: true; text: "PREVIEW"; color: Theme.textMuted; font.pixelSize: 10 }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    ListView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        model: CortexFeatures.networkEvents
        delegate: Rectangle {
            required property var modelData
            width: ListView.view.width
            height: 32
            color: index % 2 ? Theme.background : Theme.surface
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 10
                spacing: 0
                Text { Layout.preferredWidth: 70; text: modelData.id; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 10 }
                Text { Layout.preferredWidth: 70; text: modelData.direction; color: modelData.direction === "send" ? Theme.mutation : Theme.success; font.pixelSize: 10 }
                Text { Layout.preferredWidth: 130; text: modelData.socket; color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 10 }
                Text { Layout.preferredWidth: 90; text: modelData.size; color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 10 }
                Text { Layout.fillWidth: true; text: modelData.preview; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 10; elide: Text.ElideRight }
            }
            Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border; opacity: 0.55 }
        }
        Text {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.margins: 14
            visible: parent.count === 0
            text: CortexApp.sessionActive ? "No captured network events." : "Select a target to inspect network activity."
            color: Theme.textDisabled
            font.pixelSize: 11
        }
    }
}
