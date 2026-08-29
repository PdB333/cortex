import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

ColumnLayout {
    anchors.fill: parent
    spacing: 0

    Component.onCompleted: if (CortexApp.sessionActive) CortexFeatures.refreshActions()

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 44
        color: Theme.background
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 5
            Button { text: "Refresh"; enabled: CortexApp.sessionActive; onClicked: CortexFeatures.refreshActions() }
            Button {
                text: "Rollback all"
                enabled: CortexApp.sessionActive && CortexApp.mutationPermission && CortexFeatures.actions.length > 0
                onClicked: CortexFeatures.rollbackAllActions()
            }
            Button {
                text: "Clear history"
                enabled: CortexApp.sessionActive && CortexApp.mutationPermission && CortexFeatures.actions.length > 0
                onClicked: CortexFeatures.clearActions()
            }
            Item { Layout.fillWidth: true }
            Label {
                text: "Checkpoint " + CortexFeatures.actionCheckpoint
                color: Theme.textMuted
                font.family: Theme.monoFont
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
            Label { Layout.preferredWidth: 90; text: "ID"; color: Theme.textMuted; font.pixelSize: 10 }
            Label { Layout.preferredWidth: 150; text: "TIME (MS)"; color: Theme.textMuted; font.pixelSize: 10 }
            Label { Layout.fillWidth: true; text: "REVERSIBLE ACTION"; color: Theme.textMuted; font.pixelSize: 10 }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    CortexListView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        model: CortexFeatures.actions
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
                Text { Layout.preferredWidth: 90; text: modelData.id; color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 11 }
                Text { Layout.preferredWidth: 150; text: modelData.timestamp; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 10 }
                Text { Layout.fillWidth: true; text: modelData.description; color: Theme.text; font.pixelSize: 11; elide: Text.ElideRight }
            }
            Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border; opacity: 0.55 }
        }

        Text {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.margins: 14
            visible: parent.count === 0
            text: CortexApp.sessionActive ? "No reversible actions recorded." : "Select a target to inspect the action journal."
            color: Theme.textDisabled
            font.pixelSize: 11
        }
    }

    Label {
        Layout.fillWidth: true
        Layout.preferredHeight: CortexFeatures.lastError.length ? 28 : 0
        visible: CortexFeatures.lastError.length > 0
        leftPadding: 12
        verticalAlignment: Text.AlignVCenter
        text: CortexFeatures.lastError
        color: Theme.error
        font.pixelSize: 10
    }
}
