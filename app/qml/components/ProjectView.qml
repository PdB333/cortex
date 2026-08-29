import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

ColumnLayout {
    id: root
    anchors.fill: parent
    spacing: 0

    property string resolvedPointer: ""

    Component.onCompleted: if (CortexPayload.ready) CortexFeatures.refreshProject()

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 34
        color: Theme.panel
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 7
            Label { text: "Persistent target knowledge"; color: Theme.text; font.pixelSize: 11; font.bold: true }
            Label { text: "Addresses, pointer paths, and notes survive target sessions."; color: Theme.textMuted; font.pixelSize: 10 }
            Item { Layout.fillWidth: true }
            Button { text: "Refresh"; enabled: CortexPayload.ready; onClicked: CortexFeatures.refreshProject() }
            Label {
                text: CortexApp.mutationPermission ? "Mutation enabled" : "Edits require Mutation"
                color: CortexApp.mutationPermission ? Theme.mutation : Theme.textDisabled
                font.pixelSize: 9
            }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    Label {
        Layout.fillWidth: true
        Layout.preferredHeight: visible ? 26 : 0
        visible: CortexFeatures.lastError.length > 0
        leftPadding: 10
        verticalAlignment: Text.AlignVCenter
        text: CortexFeatures.lastError
        color: Theme.error
        font.pixelSize: 10
    }

    SplitView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        orientation: Qt.Vertical

        SplitView {
            SplitView.preferredHeight: 360
            SplitView.minimumHeight: 220
            orientation: Qt.Horizontal

            Rectangle {
                SplitView.fillWidth: true
                color: Theme.background
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 64
                        color: Theme.background
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 7
                            spacing: 4
                            RowLayout {
                                Layout.fillWidth: true
                                Label { text: "NAMED ADDRESSES"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true; Layout.preferredWidth: 120 }
                                TextField { id: addressName; Layout.preferredWidth: 130; placeholderText: "Name" }
                                TextField { id: addressValue; Layout.preferredWidth: 150; placeholderText: "0x..." }
                                TextField { id: addressType; Layout.preferredWidth: 90; placeholderText: "Type" }
                                TextField { id: addressNotes; Layout.fillWidth: true; placeholderText: "Notes" }
                                Button {
                                    text: "Save"
                                    enabled: CortexPayload.ready && CortexApp.mutationPermission && addressName.text.length > 0 && addressValue.text.length > 0
                                    onClicked: CortexFeatures.setProjectAddress(addressName.text, addressValue.text, addressType.text, addressNotes.text)
                                }
                            }
                        }
                        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: CortexFeatures.projectAddresses
                        delegate: Rectangle {
                            required property var modelData
                            width: ListView.view.width
                            height: 32
                            color: index % 2 ? Theme.background : Theme.surface
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 8
                                spacing: 5
                                Text { Layout.preferredWidth: 130; text: modelData.name; color: Theme.text; font.pixelSize: 10; elide: Text.ElideRight }
                                Text { Layout.preferredWidth: 150; text: modelData.address; color: Theme.codeNumber; font.family: Theme.monoFont; font.pixelSize: 10; elide: Text.ElideRight }
                                Text { Layout.preferredWidth: 80; text: modelData.type; color: Theme.textMuted; font.pixelSize: 10 }
                                Text { Layout.fillWidth: true; text: modelData.notes; color: Theme.textMuted; font.pixelSize: 10; elide: Text.ElideRight }
                                Button {
                                    text: "Remove"
                                    enabled: CortexApp.mutationPermission
                                    onClicked: CortexFeatures.deleteProjectAddress(modelData.name)
                                }
                            }
                        }
                        Text {
                            anchors.centerIn: parent
                            visible: parent.count === 0
                            text: "No named addresses saved."
                            color: Theme.textDisabled
                            font.pixelSize: 10
                        }
                    }
                }
            }

            Rectangle {
                SplitView.fillWidth: true
                color: Theme.background
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 92
                        color: Theme.background
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 7
                            spacing: 4
                            RowLayout {
                                Layout.fillWidth: true
                                Label { text: "POINTER PATHS"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true; Layout.preferredWidth: 105 }
                                TextField { id: pointerName; Layout.preferredWidth: 125; placeholderText: "Name" }
                                TextField { id: pointerModule; Layout.preferredWidth: 125; placeholderText: "Module (optional)" }
                                TextField { id: pointerBase; Layout.preferredWidth: 110; placeholderText: "Base offset" }
                                TextField { id: pointerType; Layout.preferredWidth: 85; placeholderText: "Final type" }
                                Item { Layout.fillWidth: true }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                TextField { id: pointerOffsets; Layout.preferredWidth: 270; placeholderText: "Offsets JSON, e.g. [16, -4]"; text: "[]"; font.family: Theme.monoFont; font.pixelSize: 10 }
                                TextField { id: pointerNotes; Layout.fillWidth: true; placeholderText: "Notes" }
                                Button {
                                    text: "Save path"
                                    enabled: CortexPayload.ready && CortexApp.mutationPermission && pointerName.text.length > 0 && pointerBase.text.length > 0
                                    onClicked: CortexFeatures.setProjectPointerPath(pointerName.text, pointerModule.text, pointerBase.text, pointerOffsets.text, pointerType.text, pointerNotes.text)
                                }
                            }
                        }
                        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: CortexFeatures.projectPointerPaths
                        delegate: Rectangle {
                            required property var modelData
                            width: ListView.view.width
                            height: 42
                            color: index % 2 ? Theme.background : Theme.surface
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 8
                                spacing: 5
                                ColumnLayout {
                                    Layout.preferredWidth: 145
                                    spacing: 1
                                    Text { Layout.fillWidth: true; text: modelData.name; color: Theme.text; font.pixelSize: 10; elide: Text.ElideRight }
                                    Text { Layout.fillWidth: true; text: modelData.module.length ? modelData.module : "main module"; color: Theme.textDisabled; font.pixelSize: 9; elide: Text.ElideRight }
                                }
                                Text { Layout.preferredWidth: 100; text: modelData.baseOffset; color: Theme.codeNumber; font.family: Theme.monoFont; font.pixelSize: 9; elide: Text.ElideRight }
                                Text { Layout.fillWidth: true; text: modelData.offsets; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 9; elide: Text.ElideRight }
                                Button {
                                    text: "Resolve"
                                    onClicked: root.resolvedPointer = CortexFeatures.resolveProjectPointerPath(modelData.name)
                                }
                                Button {
                                    text: "Remove"
                                    enabled: CortexApp.mutationPermission
                                    onClicked: CortexFeatures.deleteProjectPointerPath(modelData.name)
                                }
                            }
                        }
                        Text {
                            anchors.centerIn: parent
                            visible: parent.count === 0
                            text: "No pointer paths saved."
                            color: Theme.textDisabled
                            font.pixelSize: 10
                        }
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 25
                        color: Theme.panel
                        visible: root.resolvedPointer.length > 0
                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            text: "Resolved: " + root.resolvedPointer
                            color: Theme.success
                            font.family: Theme.monoFont
                            font.pixelSize: 10
                        }
                    }
                }
            }
        }

        Rectangle {
            SplitView.fillHeight: true
            SplitView.minimumHeight: 140
            color: Theme.background
            ColumnLayout {
                anchors.fill: parent
                spacing: 0
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 44
                    color: Theme.panel
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        spacing: 5
                        Label { text: "NOTES"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true; Layout.preferredWidth: 55 }
                        TextField { id: noteText; Layout.fillWidth: true; placeholderText: "Observation, hypothesis, TODO..." }
                        TextField { id: noteTags; Layout.preferredWidth: 180; placeholderText: "Tags JSON"; text: "[]"; font.family: Theme.monoFont; font.pixelSize: 10 }
                        Button {
                            text: "Add note"
                            enabled: CortexPayload.ready && CortexApp.mutationPermission && noteText.text.length > 0
                            onClicked: CortexFeatures.addProjectNote(noteText.text, noteTags.text)
                        }
                    }
                    Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
                }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: CortexFeatures.projectNotes
                    delegate: Rectangle {
                        required property var modelData
                        width: ListView.view.width
                        height: 34
                        color: index % 2 ? Theme.background : Theme.surface
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 8
                            spacing: 7
                            Text { Layout.preferredWidth: 45; text: "#" + modelData.id; color: Theme.textDisabled; font.family: Theme.monoFont; font.pixelSize: 9 }
                            Text { Layout.fillWidth: true; text: modelData.text; color: Theme.text; font.pixelSize: 10; elide: Text.ElideRight }
                            Text { Layout.preferredWidth: 220; text: modelData.tags; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 9; elide: Text.ElideRight }
                            Button { text: "Remove"; enabled: CortexApp.mutationPermission; onClicked: CortexFeatures.deleteProjectNote(modelData.id) }
                        }
                    }
                    Text {
                        anchors.centerIn: parent
                        visible: parent.count === 0
                        text: "No project notes yet."
                        color: Theme.textDisabled
                        font.pixelSize: 10
                    }
                }
            }
        }
    }
}
