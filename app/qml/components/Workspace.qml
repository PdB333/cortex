import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

Rectangle {
    id: root
    color: Theme.background

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 38
            color: Theme.panel

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 10
                spacing: 10

                Label {
                    text: CortexApp.selectedSection
                    color: Theme.textBright
                    font.family: Theme.uiFont
                    font.pixelSize: 13
                    font.bold: true
                }
                Rectangle {
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: 18
                    color: Theme.borderStrong
                }
                Label {
                    text: CortexApp.currentTargetIndex >= 0 ? CortexApp.currentTargetName : "No active target"
                    color: Theme.textMuted
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }
                Item { Layout.fillWidth: true }
                Label {
                    visible: CortexApp.currentTargetIndex >= 0
                    text: CortexApp.capabilitySummary()
                    color: Theme.textDisabled
                    font.pixelSize: 10
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

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            Loader {
                anchors.fill: parent
                sourceComponent: CortexApp.selectedSection === "Overview" ? overviewComponent : toolComponent
            }
        }
    }

    Component {
        id: overviewComponent

        Flickable {
            contentWidth: width
            contentHeight: overviewColumn.implicitHeight + 48
            clip: true

            ColumnLayout {
                id: overviewColumn
                width: Math.min(parent.width - 48, 980)
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                anchors.topMargin: 28
                spacing: 16

                Label {
                    text: CortexApp.currentTargetIndex >= 0 ? CortexApp.currentTargetName : "Select a target"
                    color: Theme.textBright
                    font.pixelSize: 24
                    font.bold: true
                }
                Label {
                    text: CortexApp.currentTargetMeta
                    color: Theme.textMuted
                    font.pixelSize: 13
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    Repeater {
                        model: [
                            { title: "PLATFORM", value: CortexApp.currentPlatform },
                            { title: "ARCHITECTURE", value: CortexApp.currentArchitecture },
                            { title: "TARGETS FOUND", value: String(CortexApp.targetCount) },
                            { title: "MUTATIONS", value: CortexApp.mutationPermission ? "ENABLED" : "OFF" }
                        ]

                        Rectangle {
                            required property var modelData
                            Layout.fillWidth: true
                            Layout.preferredHeight: 82
                            color: Theme.panel
                            border.color: Theme.border
                            radius: Theme.radius

                            Column {
                                anchors.left: parent.left
                                anchors.leftMargin: 12
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 7
                                Text {
                                    text: modelData.title
                                    color: Theme.textDisabled
                                    font.pixelSize: 10
                                    font.bold: true
                                    font.letterSpacing: 0.5
                                }
                                Text {
                                    text: modelData.value
                                    color: modelData.title === "MUTATIONS" && CortexApp.mutationPermission ? "#ffd580" : Theme.textBright
                                    font.pixelSize: 15
                                    font.family: modelData.title === "ARCHITECTURE" ? Theme.monoFont : Theme.uiFont
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 190
                    color: Theme.panel
                    border.color: Theme.border
                    radius: Theme.radius

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: 8

                        Label {
                            text: "Unified Cortex application"
                            color: Theme.textBright
                            font.pixelSize: 14
                            font.bold: true
                        }
                        Label {
                            Layout.fillWidth: true
                            text: "This branch moves the human interface out of the injected runtime. Qt/QML becomes the application UI; MCP, CLI and the UI will converge on the same target/capability services. The current preview only selects targets and exercises the new shell -- no mutation or attachment is performed from this screen yet."
                            color: Theme.textMuted
                            wrapMode: Text.WordWrap
                            lineHeight: 1.25
                            font.pixelSize: 12
                        }
                        Item { Layout.fillHeight: true }
                        RowLayout {
                            spacing: 8
                            Button {
                                text: "Refresh targets"
                                onClicked: CortexApp.refreshTargets()
                            }
                            Button {
                                text: CortexApp.mutationPermission ? "Disable mutation permission" : "Enable mutation permission"
                                enabled: CortexApp.currentTargetIndex >= 0
                                onClicked: CortexApp.mutationPermission = !CortexApp.mutationPermission
                            }
                        }
                    }
                }
            }
        }
    }

    Component {
        id: toolComponent

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 10

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 38
                color: Theme.panel
                border.color: Theme.border
                radius: Theme.radius

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    spacing: 8
                    Button { text: "New"; enabled: CortexApp.currentTargetIndex >= 0 }
                    Button { text: "Refresh"; onClicked: CortexApp.refreshTargets() }
                    Item { Layout.fillWidth: true }
                    Label {
                        text: CortexApp.currentTargetIndex >= 0 ? "Target selected; backend wiring pending" : "Select a target first"
                        color: Theme.textMuted
                        font.pixelSize: 11
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Theme.panel
                border.color: Theme.border
                radius: Theme.radius

                Column {
                    anchors.centerIn: parent
                    spacing: 9
                    width: Math.min(parent.width - 80, 620)

                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: CortexApp.selectedSection
                        color: Theme.textBright
                        font.pixelSize: 20
                        font.bold: true
                    }
                    Label {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        text: "The workspace is ready. This panel will be connected to the existing Cortex service during the migration, before the corresponding ImGui control is removed."
                        color: Theme.textMuted
                        wrapMode: Text.WordWrap
                        font.pixelSize: 12
                    }
                }
            }
        }
    }
}
