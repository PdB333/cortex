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
            spacing: 5
            Button {
                text: "Export active runtime state"
                enabled: CortexApp.sessionActive
                onClicked: CortexFeatures.exportSession()
            }
            Button {
                text: "Detach active"
                enabled: CortexApp.sessionActive
                onClicked: CortexApp.detachTarget()
            }
            Button {
                text: "Detach all"
                visible: CortexApp.attachedTargetCount > 1
                enabled: CortexApp.attachedTargetCount > 0
                onClicked: CortexApp.detachAllTargets()
            }
            Item { Layout.fillWidth: true }
            Label {
                text: CortexApp.attachedTargetCount + (CortexApp.attachedTargetCount === 1 ? " target attached" : " targets attached")
                color: CortexApp.attachedTargetCount > 0 ? Theme.text : Theme.textMuted
                font.pixelSize: 10
            }
            Rectangle {
                width: 1
                height: 16
                color: Theme.border
                visible: CortexApp.sessionActive
            }
            Label {
                visible: CortexApp.sessionActive
                text: CortexPayload.ready ? "Instrumentation connected" : "External session"
                color: CortexPayload.ready ? Theme.success : Theme.textMuted
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
            anchors.rightMargin: 12
            spacing: 0
            Label { Layout.preferredWidth: 220; text: "TARGET"; color: Theme.textMuted; font.pixelSize: 10 }
            Label { Layout.preferredWidth: 100; text: "STATE"; color: Theme.textMuted; font.pixelSize: 10 }
            Label { Layout.fillWidth: true; text: "DETAILS"; color: Theme.textMuted; font.pixelSize: 10 }
            Item { Layout.preferredWidth: 152 }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    CortexListView {
        id: sessionList
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(contentHeight, 220)
        Layout.minimumHeight: CortexApp.attachedTargetCount > 0 ? Math.min(contentHeight, 42) : 0
        visible: CortexApp.attachedTargetCount > 0
        model: CortexApp.targets
        spacing: 0

        delegate: Rectangle {
            required property int index
            required property var modelData

            readonly property bool attachedTarget: modelData && modelData.attached === true
            readonly property bool activeTarget: attachedTarget && modelData.active === true

            width: sessionList.width
            height: attachedTarget ? 42 : 0
            visible: attachedTarget
            color: activeTarget ? Theme.selection : Theme.background
            clip: true

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 0

                ColumnLayout {
                    Layout.preferredWidth: 220
                    Layout.fillHeight: true
                    spacing: 1
                    Item { Layout.fillHeight: true }
                    Text {
                        Layout.fillWidth: true
                        text: modelData.name || "Unknown target"
                        color: Theme.text
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }
                    Text {
                        Layout.fillWidth: true
                        text: "PID " + modelData.pid
                        color: Theme.textMuted
                        font.family: Theme.monoFont
                        font.pixelSize: 9
                        elide: Text.ElideRight
                    }
                    Item { Layout.fillHeight: true }
                }

                Text {
                    Layout.preferredWidth: 100
                    text: activeTarget ? "ACTIVE" : "ATTACHED"
                    color: activeTarget ? Theme.success : Theme.textMuted
                    font.pixelSize: 10
                    font.bold: activeTarget
                }

                Text {
                    Layout.fillWidth: true
                    text: (modelData.platform || "Unknown") + "  |  " + (modelData.architecture || "unknown") +
                          (modelData.windowTitle && modelData.windowTitle.length > 0 ? "  |  " + modelData.windowTitle : "")
                    color: Theme.textMuted
                    font.pixelSize: 10
                    elide: Text.ElideRight
                }

                RowLayout {
                    Layout.preferredWidth: 152
                    spacing: 4
                    Button {
                        text: activeTarget ? "Active" : "Switch"
                        enabled: !activeTarget
                        Layout.preferredWidth: 72
                        onClicked: CortexApp.selectTarget(index)
                    }
                    Button {
                        text: "Detach"
                        Layout.preferredWidth: 72
                        onClicked: CortexApp.detachTargetAt(index)
                    }
                }
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: attachedTarget ? 1 : 0
                color: Theme.border
            }
        }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: CortexApp.attachedTargetCount === 0 ? 58 : 0
        visible: CortexApp.attachedTargetCount === 0
        color: Theme.background
        Text {
            anchors.centerIn: parent
            text: "No attached targets. Select one from the top bar to create a Cortex session."
            color: Theme.textMuted
            font.pixelSize: 10
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    ColumnLayout {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.margins: 14
        spacing: 10

        Label {
            text: CortexApp.sessionActive ? "Active target capabilities" : "No active session"
            color: Theme.textBright
            font.pixelSize: 12
            font.bold: true
        }
        Text {
            Layout.fillWidth: true
            text: CortexApp.sessionActive ? CortexApp.capabilitySummary() : "Attach or switch to a target above to inspect its capabilities."
            color: Theme.textMuted
            font.pixelSize: 11
            wrapMode: Text.Wrap
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

        Label { text: "Last exported runtime state"; color: Theme.textBright; font.pixelSize: 12; font.bold: true }
        Text {
            Layout.fillWidth: true
            text: CortexFeatures.sessionExportPath.length > 0 ? CortexFeatures.sessionExportPath : "No export created in this application session."
            color: CortexFeatures.sessionExportPath.length > 0 ? Theme.text : Theme.textDisabled
            font.family: CortexFeatures.sessionExportPath.length > 0 ? Theme.monoFont : Theme.uiFont
            font.pixelSize: 10
            wrapMode: Text.WrapAnywhere
        }

        Label {
            text: CortexFeatures.lastError
            visible: text.length > 0
            color: Theme.error
            font.pixelSize: 10
        }
        Item { Layout.fillHeight: true }
    }
}