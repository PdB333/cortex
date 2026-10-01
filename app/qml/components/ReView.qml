import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

Rectangle {
    id: root
    color: Theme.background

    property int activePage: 0
    property bool resultVisible: false
    property string resultState: CortexRe.result + "\n" + CortexRe.lastError
    property var pages: [
        { "title": "Objects", "hint": "Tracked runtime objects" },
        { "title": "Analysis", "hint": "Writer and layout analysis" },
        { "title": "Trace", "hint": "State / write transitions" },
        { "title": "Experiments", "hint": "Controlled runtime tests" },
        { "title": "Sessions", "hint": "Facts, checkpoints, diffs" },
        { "title": "Interop", "hint": "Ghidra and BP templates" }
    ]

    onResultStateChanged: {
        if (resultState.trim().length > 0)
            resultVisible = true
    }

    Connections {
        target: CortexApp
        function onNavigationAddressChanged() {
            if (CortexApp.selectedSection !== "RE" || CortexApp.navigationAddress.length === 0) return
            analysisAddress.text = CortexApp.navigationAddress
            if (trackAddress.text.length === 0) trackAddress.text = CortexApp.navigationAddress
            if (traceQuickAddress.text.length === 0) traceQuickAddress.text = CortexApp.navigationAddress
            root.activePage = 1
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 50
            color: Theme.panel

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 10
                spacing: 10

                ColumnLayout {
                    spacing: 0
                    Label {
                        text: "Reverse Engineering"
                        color: Theme.textBright
                        font.pixelSize: 15
                        font.bold: true
                    }
                    Label {
                        text: root.pages[root.activePage].hint
                        color: Theme.textMuted
                        font.pixelSize: 9
                    }
                }

                Item { Layout.fillWidth: true }

                Button {
                    text: root.resultVisible ? "Hide result" : "Result"
                    onClicked: root.resultVisible = !root.resultVisible
                }
                Button {
                    text: "Refresh"
                    onClicked: {
                        CortexRe.refresh()
                        CortexRe.refreshSessions()
                        CortexRe.refreshCheckpoints()
                    }
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

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Rectangle {
                Layout.preferredWidth: 190
                Layout.minimumWidth: 170
                Layout.fillHeight: true
                color: Theme.panel

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 3

                    Label {
                        Layout.leftMargin: 7
                        Layout.topMargin: 4
                        Layout.bottomMargin: 4
                        text: "RE WORKSPACE"
                        color: Theme.textDisabled
                        font.pixelSize: 9
                        font.bold: true
                    }

                    Repeater {
                        model: root.pages
                        delegate: Rectangle {
                            required property var modelData
                            required property int index
                            Layout.fillWidth: true
                            Layout.preferredHeight: 48
                            radius: Theme.radius
                            color: root.activePage === index ? Theme.selection : (pageMouse.containsMouse ? Theme.hover : "transparent")

                            Rectangle {
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                width: 2
                                visible: root.activePage === parent.index
                                color: Theme.accent
                            }

                            Column {
                                anchors.left: parent.left
                                anchors.leftMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width - 16
                                spacing: 2
                                Text {
                                    width: parent.width
                                    text: modelData.title
                                    color: root.activePage === index ? Theme.textBright : Theme.text
                                    font.family: Theme.uiFont
                                    font.pixelSize: 11
                                    font.bold: root.activePage === index
                                    elide: Text.ElideRight
                                }
                                Text {
                                    width: parent.width
                                    text: modelData.hint
                                    color: Theme.textDisabled
                                    font.family: Theme.uiFont
                                    font.pixelSize: 8
                                    elide: Text.ElideRight
                                }
                            }

                            MouseArea {
                                id: pageMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: root.activePage = index
                            }
                        }
                    }

                    Item { Layout.fillHeight: true }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 58
                        color: Theme.background
                        border.color: Theme.border
                        radius: Theme.radius
                        Column {
                            anchors.fill: parent
                            anchors.margins: 7
                            spacing: 3
                            Text {
                                text: CortexApp.mutationPermission ? "Mutation enabled" : "Read-oriented mode"
                                color: CortexApp.mutationPermission ? Theme.mutation : Theme.textMuted
                                font.pixelSize: 9
                                font.bold: true
                            }
                            Text {
                                width: parent.width
                                text: CortexRe.tracks.length + " tracked object" + (CortexRe.tracks.length === 1 ? "" : "s")
                                color: Theme.textDisabled
                                font.pixelSize: 8
                                elide: Text.ElideRight
                            }
                        }
                    }
                }

                Rectangle {
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: 1
                    color: Theme.border
                }
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: root.activePage

                // -----------------------------------------------------------------
                // Objects
                // -----------------------------------------------------------------
                Item {
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 10

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label { text: "Tracked Objects"; color: Theme.textBright; font.pixelSize: 16; font.bold: true }
                            Label { text: "Keep important runtime objects in one place and carry them across RE sessions."; color: Theme.textMuted; font.pixelSize: 10 }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 150
                            color: Theme.surface
                            border.color: Theme.border
                            radius: Theme.radius

                            GridLayout {
                                anchors.fill: parent
                                anchors.margins: 10
                                columns: 4
                                columnSpacing: 8
                                rowSpacing: 7

                                Label { text: "TRACK OBJECT"; color: Theme.textMuted; font.pixelSize: 9; font.bold: true; Layout.columnSpan: 4 }
                                TextField { id: trackName; Layout.columnSpan: 2; Layout.fillWidth: true; placeholderText: "Name (optional)" }
                                TextField { id: trackAddress; Layout.columnSpan: 2; Layout.fillWidth: true; placeholderText: "Address (0x..., module+RVA)" }
                                TextField { id: trackPath; Layout.columnSpan: 2; Layout.fillWidth: true; placeholderText: "Pointer path (optional)" }
                                TextField { id: trackStruct; Layout.fillWidth: true; placeholderText: "Struct definition" }
                                TextField { id: trackSize; Layout.preferredWidth: 90; text: "256"; placeholderText: "Size" }
                                Item { Layout.columnSpan: 3; Layout.fillWidth: true }
                                Button {
                                    text: "Track"
                                    enabled: CortexApp.mutationPermission && trackAddress.text.length > 0
                                    onClicked: CortexRe.trackObject(
                                                   trackName.text.length ? trackName.text : ("Object " + trackAddress.text),
                                                   trackAddress.text,
                                                   trackPath.text,
                                                   Number(trackSize.text || 256),
                                                   true,
                                                   trackStruct.text)
                                }
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            color: Theme.surface
                            border.color: Theme.border
                            radius: Theme.radius

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 0
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 34
                                    color: Theme.background
                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 10
                                        anchors.rightMargin: 8
                                        Label { text: "OBJECTS"; color: Theme.textMuted; font.pixelSize: 9; font.bold: true }
                                        Item { Layout.fillWidth: true }
                                        Label { text: CortexRe.tracks.length + " total"; color: Theme.textDisabled; font.pixelSize: 9 }
                                        Button {
                                            text: "Remove selected"
                                            enabled: CortexApp.mutationPermission && CortexRe.selectedTrack.id > 0
                                            onClicked: CortexRe.deleteTrack(CortexRe.selectedTrack.id)
                                        }
                                    }
                                }
                                CortexListView {
                                    id: tracks
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    clip: true
                                    model: CortexRe.tracks
                                    spacing: 1
                                    delegate: Rectangle {
                                        required property var modelData
                                        width: ListView.view.width
                                        height: 58
                                        color: objectMouse.containsMouse ? Theme.hover : "transparent"
                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.leftMargin: 10
                                            anchors.rightMargin: 10
                                            spacing: 10
                                            Rectangle {
                                                Layout.preferredWidth: 7
                                                Layout.preferredHeight: 7
                                                radius: 4
                                                color: modelData.alive ? Theme.success : Theme.textDisabled
                                            }
                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: 2
                                                Text {
                                                    Layout.fillWidth: true
                                                    text: modelData.name
                                                    color: Theme.textBright
                                                    font.pixelSize: 11
                                                    elide: Text.ElideRight
                                                }
                                                Text {
                                                    Layout.fillWidth: true
                                                    text: modelData.address + "  |  " + modelData.size + " bytes" + (modelData.struct_name ? ("  |  " + modelData.struct_name) : "")
                                                    color: Theme.textMuted
                                                    font.family: Theme.monoFont
                                                    font.pixelSize: 9
                                                    elide: Text.ElideRight
                                                }
                                                Text {
                                                    Layout.fillWidth: true
                                                    text: modelData.pointer_path ? ("path: " + modelData.pointer_path) : "direct address"
                                                    color: Theme.textDisabled
                                                    font.pixelSize: 8
                                                    elide: Text.ElideRight
                                                }
                                            }
                                            Button {
                                                text: "Analyze"
                                                onClicked: {
                                                    CortexRe.selectTrack(modelData.id)
                                                    analysisAddress.text = modelData.address
                                                    traceQuickAddress.text = modelData.address
                                                    root.activePage = 1
                                                }
                                            }
                                        }
                                        MouseArea {
                                            id: objectMouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            acceptedButtons: Qt.LeftButton
                                            propagateComposedEvents: true
                                            onClicked: CortexRe.selectTrack(modelData.id)
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // -----------------------------------------------------------------
                // Analysis
                // -----------------------------------------------------------------
                Item {
                    CortexFlickable {
                        anchors.fill: parent
                        contentWidth: width
                        contentHeight: analysisContent.implicitHeight + 24

                        ColumnLayout {
                            id: analysisContent
                            width: Math.max(0, parent.width - 24)
                            x: 12
                            spacing: 10

                            Label { text: "Runtime Analysis"; color: Theme.textBright; font.pixelSize: 16; font.bold: true }
                            Label { text: "Start from one address, then ask Cortex who changes it or what object layout surrounds it."; color: Theme.textMuted; font.pixelSize: 10; wrapMode: Text.Wrap; Layout.fillWidth: true }

                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: analysisCard.implicitHeight + 20
                                color: Theme.surface
                                border.color: Theme.border
                                radius: Theme.radius

                                ColumnLayout {
                                    id: analysisCard
                                    anchors.fill: parent
                                    anchors.margins: 10
                                    spacing: 8
                                    Label { text: "ADDRESS / OBJECT"; color: Theme.textMuted; font.pixelSize: 9; font.bold: true }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        TextField { id: analysisAddress; Layout.fillWidth: true; placeholderText: "Address, module+RVA or symbol" }
                                        TextField { id: analysisSize; Layout.preferredWidth: 90; text: "1"; placeholderText: "Size" }
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Button {
                                            text: "Last writer"
                                            enabled: CortexApp.mutationPermission && analysisAddress.text.length > 0
                                            onClicked: CortexRe.findLastWriter(analysisAddress.text, Number(analysisSize.text || 1), 10000)
                                        }
                                        Button {
                                            text: "C++ subobjects"
                                            enabled: analysisAddress.text.length > 0
                                            onClicked: CortexRe.detectSubobjects(analysisAddress.text, Math.max(8, Number(analysisSize.text || 256)))
                                        }
                                        Button {
                                            text: "Prepare write trace"
                                            enabled: analysisAddress.text.length > 0
                                            onClicked: {
                                                traceQuickAddress.text = analysisAddress.text
                                                transitionJson.text = JSON.stringify({
                                                    "watches": [{
                                                        "address": analysisAddress.text,
                                                        "size": Math.max(1, Number(analysisSize.text || 1)),
                                                        "label": "state"
                                                    }],
                                                    "probes": [],
                                                    "timeout_ms": 10000
                                                }, null, 2)
                                                root.activePage = 2
                                            }
                                        }
                                        Item { Layout.fillWidth: true }
                                    }
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: 96
                                color: Theme.panel
                                border.color: Theme.border
                                radius: Theme.radius
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.margins: 12
                                    spacing: 12
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 3
                                        Label { text: "SELECTED OBJECT"; color: Theme.textMuted; font.pixelSize: 9; font.bold: true }
                                        Label {
                                            text: CortexRe.selectedTrack.name || "No tracked object selected"
                                            color: CortexRe.selectedTrack.id > 0 ? Theme.textBright : Theme.textDisabled
                                            font.pixelSize: 11
                                            font.bold: CortexRe.selectedTrack.id > 0
                                        }
                                        Label {
                                            text: CortexRe.selectedTrack.address || "Choose one from Objects or paste an address above."
                                            color: Theme.textMuted
                                            font.family: Theme.monoFont
                                            font.pixelSize: 9
                                        }
                                    }
                                    Button {
                                        text: "Use selected"
                                        enabled: CortexRe.selectedTrack.id > 0
                                        onClicked: analysisAddress.text = CortexRe.selectedTrack.address || ""
                                    }
                                }
                            }
                        }
                    }
                }

                // -----------------------------------------------------------------
                // Trace
                // -----------------------------------------------------------------
                Item {
                    CortexFlickable {
                        anchors.fill: parent
                        contentWidth: width
                        contentHeight: traceContent.implicitHeight + 24

                        ColumnLayout {
                            id: traceContent
                            width: Math.max(0, parent.width - 24)
                            x: 12
                            spacing: 10

                            Label { text: "Transition Trace"; color: Theme.textBright; font.pixelSize: 16; font.bold: true }
                            Label { text: "Observe a bounded state transition without mixing trace setup with the rest of the RE workspace."; color: Theme.textMuted; font.pixelSize: 10; wrapMode: Text.Wrap; Layout.fillWidth: true }

                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: traceQuick.implicitHeight + 20
                                color: Theme.surface
                                border.color: Theme.border
                                radius: Theme.radius
                                ColumnLayout {
                                    id: traceQuick
                                    anchors.fill: parent
                                    anchors.margins: 10
                                    spacing: 7
                                    Label { text: "QUICK WRITE TRACE"; color: Theme.textMuted; font.pixelSize: 9; font.bold: true }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        TextField { id: traceQuickAddress; Layout.fillWidth: true; placeholderText: "Address / object" }
                                        TextField { id: traceQuickSize; Layout.preferredWidth: 90; text: "1"; placeholderText: "Size" }
                                        Button {
                                            text: "Build"
                                            enabled: traceQuickAddress.text.length > 0
                                            onClicked: transitionJson.text = JSON.stringify({
                                                "watches": [{
                                                    "address": traceQuickAddress.text,
                                                    "size": Math.max(1, Number(traceQuickSize.text || 1)),
                                                    "label": "state"
                                                }],
                                                "probes": [],
                                                "timeout_ms": 10000
                                            }, null, 2)
                                        }
                                    }
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 330
                                color: Theme.surface
                                border.color: Theme.border
                                radius: Theme.radius
                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 10
                                    spacing: 7
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label { text: "TRACE DEFINITION"; color: Theme.textMuted; font.pixelSize: 9; font.bold: true }
                                        Item { Layout.fillWidth: true }
                                        Label { text: "JSON"; color: Theme.textDisabled; font.pixelSize: 8 }
                                    }
                                    ScrollView {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        clip: true
                                        background: Rectangle { color: Theme.input; border.color: transitionJson.activeFocus ? Theme.accent : Theme.borderStrong; radius: Theme.radius }
                                        ScrollBar.vertical: CortexScrollBar {}
                                        ScrollBar.horizontal: CortexScrollBar { orientation: Qt.Horizontal }
                                        TextArea {
                                            id: transitionJson
                                            text: '{\n  "watches": [{"address":"0x0", "size":1, "label":"state"}],\n  "probes": [],\n  "timeout_ms": 10000\n}'
                                            color: Theme.text
                                            selectionColor: Theme.accentDark
                                            font.family: Theme.monoFont
                                            font.pixelSize: 10
                                            wrapMode: TextEdit.NoWrap
                                            background: null
                                        }
                                    }
                                    Button {
                                        text: "Trace transition"
                                        enabled: CortexApp.mutationPermission
                                        onClicked: CortexRe.traceTransition(transitionJson.text)
                                    }
                                }
                            }
                        }
                    }
                }

                // -----------------------------------------------------------------
                // Experiments
                // -----------------------------------------------------------------
                Item {
                    CortexFlickable {
                        anchors.fill: parent
                        contentWidth: width
                        contentHeight: experimentContent.implicitHeight + 24

                        ColumnLayout {
                            id: experimentContent
                            width: Math.max(0, parent.width - 24)
                            x: 12
                            spacing: 10

                            Label { text: "Controlled Experiments"; color: Theme.textBright; font.pixelSize: 16; font.bold: true }
                            Label { text: "Run bounded in-target experiments with an explicit rollback path when you do not want state to persist."; color: Theme.textMuted; font.pixelSize: 10; wrapMode: Text.Wrap; Layout.fillWidth: true }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 390
                                color: Theme.surface
                                border.color: Theme.border
                                radius: Theme.radius
                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 10
                                    spacing: 7
                                    Label { text: "EXPERIMENT DEFINITION"; color: Theme.textMuted; font.pixelSize: 9; font.bold: true }
                                    ScrollView {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        clip: true
                                        background: Rectangle { color: Theme.input; border.color: testJson.activeFocus ? Theme.accent : Theme.borderStrong; radius: Theme.radius }
                                        ScrollBar.vertical: CortexScrollBar {}
                                        ScrollBar.horizontal: CortexScrollBar { orientation: Qt.Horizontal }
                                        TextArea {
                                            id: testJson
                                            text: '{\n  "steps": [\n    {"action":"delay", "ms":100}\n  ],\n  "rollback_ranges": [],\n  "commit": false\n}'
                                            color: Theme.text
                                            selectionColor: Theme.accentDark
                                            font.family: Theme.monoFont
                                            font.pixelSize: 10
                                            wrapMode: TextEdit.NoWrap
                                            background: null
                                        }
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Button {
                                            text: "Run test"
                                            enabled: CortexApp.mutationPermission
                                            onClicked: CortexRe.runTest(testJson.text, false)
                                        }
                                        Button {
                                            text: "Run + rollback"
                                            enabled: CortexApp.mutationPermission
                                            onClicked: CortexRe.runTest(testJson.text, true)
                                        }
                                        Item { Layout.fillWidth: true }
                                        Label { text: CortexApp.mutationPermission ? "Mutation explicitly enabled" : "Enable Mutation to execute"; color: CortexApp.mutationPermission ? Theme.mutation : Theme.textDisabled; font.pixelSize: 9 }
                                    }
                                }
                            }
                        }
                    }
                }

                // -----------------------------------------------------------------
                // Sessions
                // -----------------------------------------------------------------
                Item {
                    CortexFlickable {
                        anchors.fill: parent
                        contentWidth: width
                        contentHeight: sessionContent.implicitHeight + 24

                        ColumnLayout {
                            id: sessionContent
                            width: Math.max(0, parent.width - 24)
                            x: 12
                            spacing: 10

                            Label { text: "RE Sessions"; color: Theme.textBright; font.pixelSize: 16; font.bold: true }
                            Label { text: "Persist knowledge, checkpoint reversible state and compare observations across runs."; color: Theme.textMuted; font.pixelSize: 10; wrapMode: Text.Wrap; Layout.fillWidth: true }

                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: factCard.implicitHeight + 20
                                color: Theme.surface
                                border.color: Theme.border
                                radius: Theme.radius
                                ColumnLayout {
                                    id: factCard
                                    anchors.fill: parent
                                    anchors.margins: 10
                                    spacing: 7
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label { text: "PERSISTENT FACT"; color: Theme.textMuted; font.pixelSize: 9; font.bold: true }
                                        Item { Layout.fillWidth: true }
                                        Label { text: Object.keys(CortexRe.session.facts || {}).length + " saved"; color: Theme.textDisabled; font.pixelSize: 9 }
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        TextField { id: factKey; Layout.preferredWidth: 220; Layout.minimumWidth: 150; placeholderText: "e.g. GameManager.B9" }
                                        TextField { id: factValue; Layout.fillWidth: true; placeholderText: 'JSON or text, e.g. {"meaning":"open","offset":"0xB9"}' }
                                        Button {
                                            text: "Save fact"
                                            enabled: CortexApp.mutationPermission
                                            onClicked: CortexRe.saveFact(factKey.text, factValue.text)
                                        }
                                    }
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: checkpointCard.implicitHeight + 20
                                color: Theme.surface
                                border.color: Theme.border
                                radius: Theme.radius
                                ColumnLayout {
                                    id: checkpointCard
                                    anchors.fill: parent
                                    anchors.margins: 10
                                    spacing: 7
                                    Label { text: "CHECKPOINT / ROLLBACK"; color: Theme.textMuted; font.pixelSize: 9; font.bold: true }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        TextField { id: checkpointLabel; Layout.preferredWidth: 210; placeholderText: "Checkpoint label" }
                                        TextField { id: checkpointRanges; Layout.fillWidth: true; text: "[]"; placeholderText: '[{"address":"0x...","size":64}]' }
                                        Button {
                                            text: "Checkpoint"
                                            enabled: CortexApp.mutationPermission
                                            onClicked: CortexRe.createCheckpoint(checkpointLabel.text, checkpointRanges.text)
                                        }
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        ComboBox { id: checkpointBox; Layout.fillWidth: true; model: CortexRe.checkpoints; textRole: "label"; valueRole: "id" }
                                        Button { text: "Rollback"; enabled: CortexApp.mutationPermission && checkpointBox.currentIndex >= 0; onClicked: CortexRe.rollbackCheckpoint(Number(checkpointBox.currentValue), false) }
                                        Button { text: "Rollback + keep"; enabled: CortexApp.mutationPermission && checkpointBox.currentIndex >= 0; onClicked: CortexRe.rollbackCheckpoint(Number(checkpointBox.currentValue), true) }
                                        Button { text: "Delete"; enabled: CortexApp.mutationPermission && checkpointBox.currentIndex >= 0; onClicked: CortexRe.deleteCheckpoint(Number(checkpointBox.currentValue)) }
                                    }
                                    Label { text: "Checkpoints restore the reversible Actions journal plus the explicit memory ranges captured here."; color: Theme.textDisabled; font.pixelSize: 9; wrapMode: Text.Wrap; Layout.fillWidth: true }
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: historyCard.implicitHeight + 20
                                color: Theme.surface
                                border.color: Theme.border
                                radius: Theme.radius
                                ColumnLayout {
                                    id: historyCard
                                    anchors.fill: parent
                                    anchors.margins: 10
                                    spacing: 7
                                    Label { text: "RUN HISTORY"; color: Theme.textMuted; font.pixelSize: 9; font.bold: true }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Button { text: "Export run"; onClicked: CortexRe.exportSession() }
                                        ComboBox { id: sessionA; Layout.fillWidth: true; model: CortexRe.sessions; textRole: "id" }
                                        ComboBox { id: sessionB; Layout.fillWidth: true; model: CortexRe.sessions; textRole: "id" }
                                        Button { text: "Diff runs"; onClicked: CortexRe.diffSessions(sessionA.currentText, sessionB.currentText) }
                                    }
                                    Label { text: "Exports include executed functions/order, tracked objects/vtables, network call origins and allocation events."; color: Theme.textDisabled; font.pixelSize: 9; wrapMode: Text.Wrap; Layout.fillWidth: true }
                                }
                            }
                        }
                    }
                }

                // -----------------------------------------------------------------
                // Interop
                // -----------------------------------------------------------------
                Item {
                    CortexFlickable {
                        anchors.fill: parent
                        contentWidth: width
                        contentHeight: interopContent.implicitHeight + 24

                        ColumnLayout {
                            id: interopContent
                            width: Math.max(0, parent.width - 24)
                            x: 12
                            spacing: 10

                            Label { text: "RE Interop"; color: Theme.textBright; font.pixelSize: 16; font.bold: true }
                            Label { text: "Keep external-tool integration separate from day-to-day runtime analysis."; color: Theme.textMuted; font.pixelSize: 10; wrapMode: Text.Wrap; Layout.fillWidth: true }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 300
                                color: Theme.surface
                                border.color: Theme.border
                                radius: Theme.radius
                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 10
                                    spacing: 7
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label { text: "GHIDRA"; color: Theme.textMuted; font.pixelSize: 9; font.bold: true }
                                        Item { Layout.fillWidth: true }
                                        Button { text: "Export Cortex -> Ghidra"; onClicked: CortexRe.ghidraExport("") }
                                    }
                                    Label { text: "Import symbols, structures and xrefs from a Ghidra-side export."; color: Theme.textDisabled; font.pixelSize: 9 }
                                    ScrollView {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        clip: true
                                        background: Rectangle { color: Theme.input; border.color: Theme.borderStrong; radius: Theme.radius }
                                        ScrollBar.vertical: CortexScrollBar {}
                                        ScrollBar.horizontal: CortexScrollBar { orientation: Qt.Horizontal }
                                        TextArea {
                                            id: ghidraImportJson
                                            text: '{"symbols":[]}'
                                            color: Theme.text
                                            font.family: Theme.monoFont
                                            font.pixelSize: 10
                                            wrapMode: TextEdit.NoWrap
                                            background: null
                                        }
                                    }
                                    Button {
                                        text: "Import Ghidra data"
                                        enabled: CortexApp.mutationPermission
                                        onClicked: CortexRe.ghidraImport(ghidraImportJson.text)
                                    }
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 260
                                color: Theme.surface
                                border.color: Theme.border
                                radius: Theme.radius
                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 10
                                    spacing: 7
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label { text: "BREAKPOINT TEMPLATES"; color: Theme.textMuted; font.pixelSize: 9; font.bold: true }
                                        Item { Layout.fillWidth: true }
                                        Label { text: (CortexRe.session.suggested_breakpoints || []).length + " suggested"; color: Theme.textDisabled; font.pixelSize: 9 }
                                    }
                                    ScrollView {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        clip: true
                                        background: Rectangle { color: Theme.input; border.color: Theme.borderStrong; radius: Theme.radius }
                                        ScrollBar.vertical: CortexScrollBar {}
                                        ScrollBar.horizontal: CortexScrollBar { orientation: Qt.Horizontal }
                                        TextArea {
                                            id: breakpointTemplatesJson
                                            text: "[]"
                                            color: Theme.text
                                            font.family: Theme.monoFont
                                            font.pixelSize: 10
                                            wrapMode: TextEdit.NoWrap
                                            background: null
                                        }
                                    }
                                    RowLayout {
                                        Button { text: "Save templates"; enabled: CortexApp.mutationPermission; onClicked: CortexRe.saveBreakpointTemplates(breakpointTemplatesJson.text) }
                                        Button { text: "Arm templates"; enabled: CortexApp.mutationPermission; onClicked: CortexRe.applyBreakpointTemplates() }
                                        Item { Layout.fillWidth: true }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: root.resultVisible ? 190 : 30
            color: Theme.panel
            border.color: Theme.border
            clip: true

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
                        anchors.rightMargin: 8
                        spacing: 8
                        Label {
                            text: CortexRe.lastError.length ? "RE ERROR" : "RE RESULT"
                            color: CortexRe.lastError.length ? Theme.error : Theme.textMuted
                            font.pixelSize: 9
                            font.bold: true
                        }
                        Label {
                            Layout.fillWidth: true
                            text: CortexRe.lastError.length ? CortexRe.lastError : (CortexRe.result.length ? "Latest operation output" : "No result yet")
                            color: CortexRe.lastError.length ? Theme.error : Theme.textDisabled
                            font.pixelSize: 9
                            elide: Text.ElideRight
                        }
                        ToolButton {
                            text: root.resultVisible ? "Hide" : "Show"
                            onClicked: root.resultVisible = !root.resultVisible
                        }
                    }
                }

                TextArea {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.margins: 8
                    visible: root.resultVisible
                    readOnly: true
                    text: CortexRe.result
                    color: Theme.text
                    font.family: Theme.monoFont
                    font.pixelSize: 10
                    wrapMode: TextEdit.NoWrap
                    background: Rectangle { color: Theme.input }
                    ScrollBar.vertical: CortexScrollBar {}
                    ScrollBar.horizontal: CortexScrollBar { orientation: Qt.Horizontal }
                }
            }
        }
    }

    Component.onCompleted: {
        if (CortexPayload.ready) {
            CortexRe.refresh()
            CortexRe.refreshSessions()
            CortexRe.refreshCheckpoints()
        }
    }
}
