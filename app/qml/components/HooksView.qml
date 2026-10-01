import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

ColumnLayout {
    id: root
    anchors.fill: parent
    spacing: 0

    property bool showAllocationEvents: false

    Component.onCompleted: {
        if (CortexApp.sessionActive) {
            CortexFeatures.refreshInstrumentationState()
            CortexFeatures.refreshInstrumentationEvents()
        }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 44
        color: Theme.background
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 6
            Label { text: "Allocation watch"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
            TextField {
                id: minSizeField
                Layout.preferredWidth: 120
                text: CortexFeatures.allocationWatchMinSize.toString()
                placeholderText: "Min bytes"
                font.family: Theme.monoFont
                font.pixelSize: 10
            }
            Button {
                text: CortexFeatures.allocationWatchEnabled ? "Disable" : "Enable"
                enabled: CortexApp.sessionActive && CortexApp.mutationPermission
                onClicked: CortexFeatures.setAllocationWatch(!CortexFeatures.allocationWatchEnabled,
                                                              Math.max(0, parseInt(minSizeField.text) || 0))
            }
            Button {
                text: "Refresh state"
                enabled: CortexApp.sessionActive
                onClicked: CortexFeatures.refreshInstrumentationState()
            }
            Button {
                text: "Refresh events"
                enabled: CortexApp.sessionActive
                onClicked: CortexFeatures.refreshInstrumentationEvents()
            }
            Item { Layout.fillWidth: true }
            Label {
                text: CortexFeatures.allocationWatchEnabled
                      ? ("ON >= " + CortexFeatures.allocationWatchMinSize + " bytes")
                      : "OFF"
                color: CortexFeatures.allocationWatchEnabled ? Theme.mutation : Theme.textDisabled
                font.family: Theme.monoFont
                font.pixelSize: 9
            }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 48
        color: Theme.panel
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 6
            Label { text: "Page guard"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
            TextField {
                id: addressField
                Layout.preferredWidth: 190
                placeholderText: "Address"
                font.family: Theme.monoFont
                font.pixelSize: 10
            }
            TextField {
                id: sizeField
                Layout.preferredWidth: 85
                text: "4"
                placeholderText: "Size"
                font.family: Theme.monoFont
                font.pixelSize: 10
            }
            TextField {
                id: labelField
                Layout.preferredWidth: 180
                placeholderText: "Label"
                font.pixelSize: 10
            }
            Button {
                text: "Add watch"
                enabled: CortexApp.sessionActive && CortexApp.mutationPermission && addressField.text.length > 0
                         && parseInt(sizeField.text) > 0
                onClicked: {
                    if (CortexFeatures.addPageAccessWatch(addressField.text, parseInt(sizeField.text), labelField.text)) {
                        addressField.text = ""
                        labelField.text = ""
                    }
                }
            }
            Item { Layout.fillWidth: true }
            Label {
                text: CortexFeatures.pageAccessWatches.length + " active"
                color: Theme.textDisabled
                font.pixelSize: 9
            }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 126
        color: Theme.background

        ColumnLayout {
            anchors.fill: parent
            spacing: 0
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                color: Theme.panel
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 0
                    Label { Layout.preferredWidth: 54; text: "ID"; color: Theme.textMuted; font.pixelSize: 9 }
                    Label { Layout.preferredWidth: 190; text: "ADDRESS"; color: Theme.textMuted; font.pixelSize: 9 }
                    Label { Layout.preferredWidth: 90; text: "SIZE"; color: Theme.textMuted; font.pixelSize: 9 }
                    Label { Layout.fillWidth: true; text: "LABEL"; color: Theme.textMuted; font.pixelSize: 9 }
                    Item { Layout.preferredWidth: 76 }
                }
                Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
            }
            CortexListView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: CortexFeatures.pageAccessWatches
                boundsBehavior: Flickable.StopAtBounds
                delegate: Rectangle {
                    required property var modelData
                    required property int index
                    width: ListView.view.width
                    height: 30
                    color: index % 2 ? Theme.background : Theme.surface
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 8
                        spacing: 0
                        Text { Layout.preferredWidth: 54; text: modelData.id; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 10 }
                        Text { Layout.preferredWidth: 190; text: modelData.address; color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 10 }
                        Text { Layout.preferredWidth: 90; text: modelData.size; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 10 }
                        Text { Layout.fillWidth: true; text: modelData.label; color: Theme.textMuted; font.pixelSize: 10; elide: Text.ElideRight }
                        Button {
                            Layout.preferredWidth: 72
                            text: "Remove"
                            enabled: CortexApp.mutationPermission
                            onClicked: CortexFeatures.deletePageAccessWatch(modelData.id)
                        }
                    }
                }
            }
        }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 34
        color: Theme.panel
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 6
            Button {
                text: "Page access (" + CortexFeatures.pageAccessEvents.length + ")"
                onClicked: root.showAllocationEvents = false
            }
            Button {
                text: "Allocations (" + CortexFeatures.allocationEvents.length + ")"
                onClicked: root.showAllocationEvents = true
            }
            Label { text: "NON-DESTRUCTIVE SNAPSHOT"; color: Theme.textDisabled; font.pixelSize: 9 }
            Item { Layout.fillWidth: true }
            Label {
                text: CortexFeatures.lastError
                visible: text.length > 0
                color: Theme.error
                font.pixelSize: 9
                elide: Text.ElideRight
                Layout.maximumWidth: 420
            }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.fillHeight: true
        color: Theme.background

        CortexListView {
            id: pageEventList
            anchors.fill: parent
            visible: !root.showAllocationEvents
            clip: true
            model: CortexFeatures.pageAccessEvents
            boundsBehavior: Flickable.StopAtBounds

            delegate: Rectangle {
                required property var modelData
                required property int index
                width: ListView.view.width
                height: 48
                color: mouse.containsMouse ? Theme.hover : (index % 2 ? Theme.background : Theme.surface)
                ColumnLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    anchors.topMargin: 5
                    anchors.bottomMargin: 4
                    spacing: 2
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        Text { text: modelData.access.toUpperCase(); color: Theme.mutation; font.family: Theme.monoFont; font.pixelSize: 10; font.bold: true }
                        Text { text: modelData.address; color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 10 }
                        Text { text: "IP " + modelData.instruction; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 10 }
                        Text { text: "T" + modelData.threadId; color: Theme.textDisabled; font.family: Theme.monoFont; font.pixelSize: 9 }
                        Text { text: modelData.size + " B"; color: Theme.textDisabled; font.pixelSize: 9 }
                        Item { Layout.fillWidth: true }
                        Text { text: modelData.label; color: Theme.textMuted; font.pixelSize: 9; elide: Text.ElideRight; Layout.maximumWidth: 220 }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Text { text: modelData.before + " -> " + modelData.after; color: Theme.textDisabled; font.family: Theme.monoFont; font.pixelSize: 9; elide: Text.ElideRight; Layout.fillWidth: true }
                        Text { text: modelData.timestamp; color: Theme.textDisabled; font.family: Theme.monoFont; font.pixelSize: 9 }
                    }
                }
                MouseArea {
                    id: mouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onDoubleClicked: {
                        if (modelData.instruction.length > 0) {
                            CortexDisasm.disassemble(modelData.instruction, 160)
                            CortexApp.selectSection("Disassembly")
                        }
                    }
                }
            }

            Text {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: 12
                visible: parent.count === 0
                text: "No page-access events in the current ring snapshot."
                color: Theme.textDisabled
                font.pixelSize: 10
            }
        }

        CortexListView {
            id: allocationEventList
            anchors.fill: parent
            visible: root.showAllocationEvents
            clip: true
            model: CortexFeatures.allocationEvents
            boundsBehavior: Flickable.StopAtBounds

            delegate: Rectangle {
                required property var modelData
                required property int index
                width: ListView.view.width
                height: 32
                color: index % 2 ? Theme.background : Theme.surface
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 0
                    Text { Layout.preferredWidth: 120; text: modelData.api; color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 10 }
                    Text { Layout.preferredWidth: 190; text: modelData.address; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 10 }
                    Text { Layout.preferredWidth: 120; text: modelData.size + " B"; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 10 }
                    Text { Layout.preferredWidth: 120; text: "0x" + Number(modelData.flags).toString(16).toUpperCase(); color: Theme.textDisabled; font.family: Theme.monoFont; font.pixelSize: 10 }
                    Item { Layout.fillWidth: true }
                    Text { text: modelData.timestamp; color: Theme.textDisabled; font.family: Theme.monoFont; font.pixelSize: 9 }
                }
            }

            Text {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: 12
                visible: parent.count === 0
                text: "No allocation events in the current ring snapshot."
                color: Theme.textDisabled
                font.pixelSize: 10
            }
        }
    }
}