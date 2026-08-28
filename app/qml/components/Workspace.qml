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
            Layout.preferredHeight: 34
            color: Theme.panel

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 8

                Label {
                    text: CortexApp.selectedSection
                    color: Theme.text
                    font.family: Theme.uiFont
                    font.pixelSize: 11
                }
                Rectangle {
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: 14
                    color: Theme.borderStrong
                }
                Label {
                    text: CortexApp.currentTargetIndex >= 0 ? CortexApp.currentTargetName : "No active target"
                    color: Theme.textMuted
                    font.pixelSize: 10
                    elide: Text.ElideRight
                }
                Item { Layout.fillWidth: true }
                Label {
                    visible: CortexApp.currentTargetIndex >= 0
                    text: CortexApp.capabilitySummary()
                    color: Theme.textDisabled
                    font.pixelSize: 9
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
                width: Math.min(parent.width - 48, 760)
                anchors.left: parent.left
                anchors.leftMargin: 28
                anchors.top: parent.top
                anchors.topMargin: 24
                spacing: 0

                Label {
                    text: CortexApp.currentTargetIndex >= 0 ? CortexApp.currentTargetName : "No target selected"
                    color: Theme.textBright
                    font.pixelSize: 20
                    font.bold: true
                }

                Label {
                    Layout.topMargin: 5
                    text: CortexApp.currentTargetIndex >= 0 ? CortexApp.currentTargetMeta : "Choose a process from the target picker to begin."
                    color: Theme.textMuted
                    font.pixelSize: 11
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.topMargin: 20
                    Layout.bottomMargin: 16
                    Layout.preferredHeight: 1
                    color: Theme.border
                }

                Label {
                    text: "TARGET"
                    color: Theme.textDisabled
                    font.pixelSize: 9
                    font.bold: true
                    font.letterSpacing: 0.5
                }

                ColumnLayout {
                    Layout.topMargin: 10
                    Layout.fillWidth: true
                    spacing: 7

                    RowLayout {
                        Layout.fillWidth: true
                        Label { Layout.preferredWidth: 120; text: "Platform"; color: Theme.textMuted; font.pixelSize: 11 }
                        Label { text: CortexApp.currentPlatform; color: Theme.text; font.pixelSize: 11 }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Label { Layout.preferredWidth: 120; text: "Architecture"; color: Theme.textMuted; font.pixelSize: 11 }
                        Label { text: CortexApp.currentArchitecture; color: Theme.text; font.pixelSize: 11; font.family: Theme.monoFont }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Label { Layout.preferredWidth: 120; text: "Processes"; color: Theme.textMuted; font.pixelSize: 11 }
                        Label { text: String(CortexApp.targetCount); color: Theme.text; font.pixelSize: 11 }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Label { Layout.preferredWidth: 120; text: "Mutation"; color: Theme.textMuted; font.pixelSize: 11 }
                        Label {
                            text: CortexApp.mutationPermission ? "Enabled" : "Disabled"
                            color: CortexApp.mutationPermission ? "#d7b36a" : Theme.text
                            font.pixelSize: 11
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.topMargin: 20
                    Layout.bottomMargin: 16
                    Layout.preferredHeight: 1
                    color: Theme.border
                }

                Label {
                    text: "ACTIONS"
                    color: Theme.textDisabled
                    font.pixelSize: 9
                    font.bold: true
                    font.letterSpacing: 0.5
                }

                RowLayout {
                    Layout.topMargin: 8
                    spacing: 4

                    ToolButton {
                        text: "Refresh targets"
                        onClicked: CortexApp.refreshTargets()
                        contentItem: Text {
                            text: parent.text
                            color: parent.hovered ? Theme.textBright : Theme.text
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            font.pixelSize: 11
                        }
                        background: Rectangle {
                            color: parent.hovered ? Theme.hover : "transparent"
                            border.color: parent.hovered ? Theme.borderStrong : Theme.border
                            radius: Theme.radius
                        }
                    }

                    ToolButton {
                        text: CortexApp.mutationPermission ? "Disable mutation" : "Enable mutation"
                        enabled: CortexApp.currentTargetIndex >= 0
                        onClicked: CortexApp.mutationPermission = !CortexApp.mutationPermission
                        contentItem: Text {
                            text: parent.text
                            color: parent.enabled ? (parent.hovered ? Theme.textBright : Theme.text) : Theme.textDisabled
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            font.pixelSize: 11
                        }
                        background: Rectangle {
                            color: parent.hovered ? Theme.hover : "transparent"
                            border.color: parent.hovered ? Theme.borderStrong : Theme.border
                            radius: Theme.radius
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
            spacing: 0

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 32
                color: Theme.background

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    spacing: 3

                    ToolButton {
                        text: "New"
                        enabled: CortexApp.currentTargetIndex >= 0
                        contentItem: Text {
                            text: parent.text
                            color: parent.enabled ? Theme.text : Theme.textDisabled
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            font.pixelSize: 10
                        }
                        background: Rectangle { color: parent.hovered ? Theme.hover : "transparent"; radius: Theme.radius }
                    }
                    ToolButton {
                        text: "Refresh"
                        onClicked: CortexApp.refreshTargets()
                        contentItem: Text {
                            text: parent.text
                            color: Theme.text
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            font.pixelSize: 10
                        }
                        background: Rectangle { color: parent.hovered ? Theme.hover : "transparent"; radius: Theme.radius }
                    }
                    Item { Layout.fillWidth: true }
                    Label {
                        text: CortexApp.currentTargetIndex >= 0 ? CortexApp.currentTargetName : "No target"
                        color: Theme.textDisabled
                        font.pixelSize: 9
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

                Column {
                    anchors.centerIn: parent
                    spacing: 5

                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: CortexApp.selectedSection
                        color: Theme.text
                        font.pixelSize: 14
                    }
                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: CortexApp.currentTargetIndex >= 0 ? "No data to display." : "Select a target to continue."
                        color: Theme.textMuted
                        font.pixelSize: 11
                    }
                }
            }
        }
    }
}
