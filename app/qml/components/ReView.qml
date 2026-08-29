import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

Rectangle {
    id: root
    color: Theme.background

    function editor(parentItem, initialText) { return initialText }

    CortexSplitView {
        anchors.fill: parent
        orientation: Qt.Horizontal

        Rectangle {
            SplitView.preferredWidth: 310
            SplitView.minimumWidth: 250
            color: Theme.panel
            border.color: Theme.border

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 8
                RowLayout {
                    Layout.fillWidth: true
                    Label { text: "RE OBJECTS"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
                    Item { Layout.fillWidth: true }
                    Button { text: "Refresh"; onClicked: { CortexRe.refresh(); CortexRe.refreshSessions() } }
                }
                TextField { id: trackName; Layout.fillWidth: true; placeholderText: "Track name" }
                TextField { id: trackAddress; Layout.fillWidth: true; placeholderText: "Address (0x..., module+RVA)" }
                TextField { id: trackPath; Layout.fillWidth: true; placeholderText: "Pointer path (optional)" }
                RowLayout {
                    Layout.fillWidth: true
                    TextField { id: trackSize; Layout.fillWidth: true; text: "256"; placeholderText: "Size" }
                    Button { text: "Track"; enabled: CortexApp.mutationPermission; onClicked: CortexRe.trackObject(trackName.text, trackAddress.text, trackPath.text, Number(trackSize.text || 256), true) }
                }
                CortexListView {
                    id: tracks
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    model: CortexRe.tracks
                    delegate: Rectangle {
                        required property var modelData
                        width: tracks.width
                        height: 54
                        color: mouse.containsMouse ? Theme.hover : "transparent"
                        Column {
                            anchors.left: parent.left; anchors.leftMargin: 7; anchors.verticalCenter: parent.verticalCenter
                            Text { text: modelData.name + (modelData.alive ? "  ●" : "  ○"); color: modelData.alive ? Theme.textBright : Theme.textMuted; font.pixelSize: 11 }
                            Text { text: modelData.address + "  •  " + modelData.size + " bytes"; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 10 }
                            Text { text: modelData.pointer_path ? ("path: " + modelData.pointer_path) : "direct"; color: Theme.textDisabled; font.pixelSize: 9 }
                        }
                        MouseArea { id: mouse; anchors.fill: parent; hoverEnabled: true; onClicked: CortexRe.selectTrack(modelData.id) }
                    }
                }
                Button { Layout.fillWidth: true; text: "Remove selected"; enabled: CortexApp.mutationPermission && CortexRe.selectedTrack.id > 0; onClicked: CortexRe.deleteTrack(CortexRe.selectedTrack.id) }
            }
        }

        Rectangle {
            SplitView.fillWidth: true
            color: Theme.background
            CortexFlickable {
                anchors.fill: parent
                contentWidth: width
                contentHeight: content.implicitHeight + 24
                ColumnLayout {
                    id: content
                    width: parent.width
                    spacing: 10
                    anchors.margins: 12
                    x: 12

                    Label { text: "Reverse Engineering Session"; color: Theme.textBright; font.pixelSize: 17; font.bold: true }
                    Label { text: "High-level runtime analysis, persistent evidence, game-thread experiments and cross-run comparison."; color: Theme.textMuted; font.pixelSize: 11; wrapMode: Text.Wrap; Layout.fillWidth: true }

                    Rectangle {
                        Layout.fillWidth: true; implicitHeight: quick.implicitHeight + 20; color: Theme.surface; border.color: Theme.border; radius: 3
                        ColumnLayout {
                            id: quick; anchors.fill: parent; anchors.margins: 10; spacing: 7
                            Label { text: "QUICK ANALYSIS"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
                            RowLayout {
                                Layout.fillWidth: true
                                TextField { id: analysisAddress; Layout.fillWidth: true; placeholderText: "Address / object" }
                                TextField { id: analysisSize; Layout.preferredWidth: 72; text: "1"; placeholderText: "Size" }
                                Button { text: "Last writer"; enabled: CortexApp.mutationPermission; onClicked: CortexRe.findLastWriter(analysisAddress.text, Number(analysisSize.text || 1), 10000) }
                                Button { text: "C++ subobjects"; onClicked: CortexRe.detectSubobjects(analysisAddress.text, Math.max(8, Number(analysisSize.text || 256))) }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true; spacing: 10
                        Rectangle {
                            Layout.fillWidth: true; Layout.preferredHeight: 270; color: Theme.surface; border.color: Theme.border; radius: 3
                            ColumnLayout {
                                anchors.fill: parent; anchors.margins: 10; spacing: 6
                                Label { text: "TRANSITION TRACE"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
                                ScrollView {
                                    Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                                    background: Rectangle { color: Theme.input; border.color: transitionJson.activeFocus ? Theme.accent : Theme.borderStrong; radius: 3 }
                                    ScrollBar.vertical: CortexScrollBar {}
                                    ScrollBar.horizontal: CortexScrollBar { orientation: Qt.Horizontal }
                                    TextArea {
                                        id: transitionJson
                                        text: '{\n  "watches": [{"address":"0x0", "size":1, "label":"state"}],\n  "probes": [],\n  "timeout_ms": 10000\n}'
                                        color: Theme.text; selectionColor: Theme.accentDark; font.family: Theme.monoFont; font.pixelSize: 10; wrapMode: TextEdit.NoWrap
                                        background: null
                                    }
                                }
                                Button { text: "Trace transition"; enabled: CortexApp.mutationPermission; onClicked: CortexRe.traceTransition(transitionJson.text) }
                            }
                        }
                        Rectangle {
                            Layout.fillWidth: true; Layout.preferredHeight: 270; color: Theme.surface; border.color: Theme.border; radius: 3
                            ColumnLayout {
                                anchors.fill: parent; anchors.margins: 10; spacing: 6
                                Label { text: "IN-GAME TEST / EXPERIMENT"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
                                ScrollView {
                                    Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                                    background: Rectangle { color: Theme.input; border.color: testJson.activeFocus ? Theme.accent : Theme.borderStrong; radius: 3 }
                                    ScrollBar.vertical: CortexScrollBar {}
                                    ScrollBar.horizontal: CortexScrollBar { orientation: Qt.Horizontal }
                                    TextArea {
                                        id: testJson
                                        text: '{\n  "steps": [\n    {"action":"delay", "ms":100}\n  ],\n  "rollback_ranges": [],\n  "commit": false\n}'
                                        color: Theme.text; selectionColor: Theme.accentDark; font.family: Theme.monoFont; font.pixelSize: 10; wrapMode: TextEdit.NoWrap
                                        background: null
                                    }
                                }
                                RowLayout {
                                    Button { text: "Run test"; enabled: CortexApp.mutationPermission; onClicked: CortexRe.runTest(testJson.text, false) }
                                    Button { text: "Run experiment + rollback"; enabled: CortexApp.mutationPermission; onClicked: CortexRe.runTest(testJson.text, true) }
                                }
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true; implicitHeight: 116; color: Theme.surface; border.color: Theme.border; radius: 3
                        ColumnLayout {
                            anchors.fill: parent; anchors.margins: 10; spacing: 6
                            Label { text: "PERSISTENT RE FACT"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
                            RowLayout {
                                Layout.fillWidth: true
                                TextField { id: factKey; Layout.preferredWidth: 220; placeholderText: "e.g. GameManager.B9" }
                                TextField { id: factValue; Layout.fillWidth: true; placeholderText: 'JSON or text, e.g. {"meaning":"open","offset":"0xB9"}' }
                                Button { text: "Save fact"; enabled: CortexApp.mutationPermission; onClicked: CortexRe.saveFact(factKey.text, factValue.text) }
                                Button { text: "Apply saved BPs"; enabled: CortexApp.mutationPermission; onClicked: CortexRe.applyBreakpointTemplates() }
                            }
                            Label { text: "Saved facts: " + Object.keys(CortexRe.session.facts || {}).length + "   •   suggested breakpoints: " + ((CortexRe.session.suggested_breakpoints || []).length); color: Theme.textMuted; font.pixelSize: 10 }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true; implicitHeight: 124; color: Theme.surface; border.color: Theme.border; radius: 3
                        ColumnLayout {
                            anchors.fill: parent; anchors.margins: 10; spacing: 6
                            Label { text: "RUN DIFF / GHIDRA"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
                            RowLayout {
                                Button { text: "Export run"; onClicked: CortexRe.exportSession() }
                                ComboBox { id: sessionA; Layout.preferredWidth: 205; model: CortexRe.sessions; textRole: "id" }
                                ComboBox { id: sessionB; Layout.preferredWidth: 205; model: CortexRe.sessions; textRole: "id" }
                                Button { text: "Diff runs"; onClicked: CortexRe.diffSessions(sessionA.currentText, sessionB.currentText) }
                                Item { Layout.fillWidth: true }
                                Button { text: "Export Ghidra"; onClicked: CortexRe.ghidraExport("") }
                            }
                            Label { text: "Run exports include executed functions/order, tracked objects/vtables, network call origins and allocation events."; color: Theme.textMuted; font.pixelSize: 10 }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true; Layout.preferredHeight: 300; color: Theme.panel; border.color: Theme.border; radius: 3
                        ColumnLayout {
                            anchors.fill: parent; anchors.margins: 8; spacing: 4
                            Label { text: CortexRe.lastError.length ? ("ERROR  " + CortexRe.lastError) : "RESULT"; color: CortexRe.lastError.length ? Theme.error : Theme.textMuted; font.pixelSize: 10; font.bold: true }
                            TextArea {
                                Layout.fillWidth: true; Layout.fillHeight: true; readOnly: true; text: CortexRe.result
                                color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 10; wrapMode: TextEdit.NoWrap
                                background: Rectangle { color: Theme.input }
                                ScrollBar.vertical: CortexScrollBar {}
                                ScrollBar.horizontal: CortexScrollBar { orientation: Qt.Horizontal }
                            }
                        }
                    }
                }
            }
        }
    }

    Component.onCompleted: { if (CortexPayload.ready) { CortexRe.refresh(); CortexRe.refreshSessions() } }
}

