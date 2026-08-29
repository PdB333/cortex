import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

ColumnLayout {
    id: root
    anchors.fill: parent
    spacing: 0

    function refresh() {
        if (CortexApp.sessionActive)
            CortexFeatures.refreshDiagnostics()
    }

    Component.onCompleted: refresh()

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 44
        color: Theme.background
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 7
            Button { text: "Refresh"; enabled: CortexApp.sessionActive; onClicked: root.refresh() }
            Label {
                text: CortexFeatures.diagnosticSummary.length > 0
                      ? CortexFeatures.diagnosticSummary
                      : "Runtime diagnostics"
                color: Theme.textMuted
                font.pixelSize: 10
            }
            Item { Layout.fillWidth: true }
            Rectangle {
                Layout.preferredWidth: 7
                Layout.preferredHeight: 7
                radius: 4
                color: CortexPayload.ready && CortexFeatures.diagnosticHealth.ok ? Theme.success : Theme.textDisabled
            }
            Label { text: CortexPayload.status; color: Theme.textMuted; font.pixelSize: 10 }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    Flickable {
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        contentWidth: width
        contentHeight: content.implicitHeight + 24

        ColumnLayout {
            id: content
            width: parent.width
            spacing: 10
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: 12

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 128
                    color: Theme.panel
                    border.width: 1
                    border.color: Theme.border
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 10
                        spacing: 5
                        Label { text: "RUNTIME"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
                        Label { text: CortexFeatures.diagnosticStatus.process || "—"; color: Theme.text; font.pixelSize: 12; font.bold: true }
                        Label { text: "PID  " + (CortexFeatures.diagnosticStatus.pid || 0); color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 10 }
                        Label { text: "Bitness  " + (CortexFeatures.diagnosticHealth.bitness || 0); color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 10 }
                        Label { text: "Uptime  " + Math.round((CortexFeatures.diagnosticStatus.uptimeMs || 0) / 1000) + " s"; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 10 }
                        Item { Layout.fillHeight: true }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 128
                    color: Theme.panel
                    border.width: 1
                    border.color: Theme.border
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 10
                        spacing: 5
                        Label { text: "API / TRANSPORT"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
                        Label {
                            text: CortexFeatures.diagnosticHealth.apiRunning ? "Running" : "Unavailable"
                            color: CortexFeatures.diagnosticHealth.apiRunning ? Theme.success : Theme.error
                            font.pixelSize: 12
                            font.bold: true
                        }
                        Label { text: "Auth  " + (CortexFeatures.diagnosticHealth.authentication || "—"); color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 10 }
                        Label { text: "Port  " + (CortexFeatures.diagnosticStatus.port || 0); color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 10 }
                        Label {
                            Layout.fillWidth: true
                            text: CortexFeatures.diagnosticHealth.lastError ? "Last error: " + CortexFeatures.diagnosticHealth.lastError : "No runtime error"
                            color: CortexFeatures.diagnosticHealth.lastError ? Theme.error : Theme.textMuted
                            font.pixelSize: 10
                            elide: Text.ElideRight
                        }
                        Item { Layout.fillHeight: true }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 128
                    color: Theme.panel
                    border.width: 1
                    border.color: Theme.border
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 10
                        spacing: 5
                        Label { text: "TOOL CATALOG"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
                        Label { text: (CortexFeatures.diagnosticToolStats.total || 0) + " operations"; color: Theme.text; font.pixelSize: 12; font.bold: true }
                        Label { text: "GET     " + (CortexFeatures.diagnosticToolStats["get"] || 0); color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 10 }
                        Label { text: "POST    " + (CortexFeatures.diagnosticToolStats["post"] || 0); color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 10 }
                        Label { text: "DELETE  " + (CortexFeatures.diagnosticToolStats["delete"] || 0) + "    public  " + (CortexFeatures.diagnosticToolStats["public"] || 0); color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 10 }
                        Item { Layout.fillHeight: true }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.max(150, hooksColumn.implicitHeight + 42)
                color: Theme.panel
                border.width: 1
                border.color: Theme.border

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 30
                        color: Theme.background
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            Label { text: "RENDER / INSTRUMENTATION HOOKS"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
                            Item { Layout.fillWidth: true }
                            Label { text: CortexFeatures.diagnosticHooks.length + " backend(s)"; color: Theme.textDisabled; font.pixelSize: 9 }
                        }
                    }
                    ColumnLayout {
                        id: hooksColumn
                        Layout.fillWidth: true
                        spacing: 0
                        Repeater {
                            model: CortexFeatures.diagnosticHooks
                            delegate: Rectangle {
                                required property var modelData
                                required property int index
                                Layout.fillWidth: true
                                Layout.preferredHeight: 34
                                color: index % 2 === 0 ? Theme.background : Theme.panel
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 10
                                    anchors.rightMargin: 10
                                    spacing: 10
                                    Rectangle {
                                        Layout.preferredWidth: 7
                                        Layout.preferredHeight: 7
                                        radius: 4
                                        color: modelData.installed ? Theme.success : Theme.textDisabled
                                    }
                                    Label { Layout.preferredWidth: 110; text: modelData.name || "hook"; color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 10 }
                                    Label { Layout.preferredWidth: 90; text: modelData.installed ? "installed" : "idle"; color: modelData.installed ? Theme.success : Theme.textMuted; font.pixelSize: 10 }
                                    Label { Layout.fillWidth: true; text: modelData.backend || "—"; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 10 }
                                }
                            }
                        }
                        Label {
                            Layout.leftMargin: 10
                            Layout.topMargin: 12
                            Layout.bottomMargin: 12
                            visible: CortexFeatures.diagnosticHooks.length === 0
                            text: "No render hook backend reported by this runtime build."
                            color: Theme.textDisabled
                            font.pixelSize: 10
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 72
                color: Theme.input
                border.width: 1
                border.color: Theme.border
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 3
                    Label { text: "AUTH TOKEN"; color: Theme.textMuted; font.pixelSize: 9; font.bold: true }
                    Label {
                        Layout.fillWidth: true
                        text: CortexFeatures.diagnosticHealth.tokenFile || "—"
                        color: Theme.textMuted
                        font.family: Theme.monoFont
                        font.pixelSize: 10
                        elide: Text.ElideMiddle
                    }
                    Label {
                        visible: CortexFeatures.lastError.length > 0
                        text: CortexFeatures.lastError
                        color: Theme.error
                        font.pixelSize: 10
                    }
                }
            }

            Item { Layout.preferredHeight: 2 }
        }
    }
}
