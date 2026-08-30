import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

Rectangle {
    id: root
    color: Theme.background
    property var selectedEntry: null

    function normalizedType(entry) {
        if (!entry) return "i32"
        var t = String(entry.type || "").toLowerCase()
        var supported = ["i8","u8","i16","u16","i32","u32","i64","u64","float","double"]
        return supported.indexOf(t) >= 0 ? t : "i32"
    }
    function typeSize(type) {
        if (type === "i8" || type === "u8") return 1
        if (type === "i16" || type === "u16") return 2
        if (type === "i64" || type === "u64" || type === "double") return 8
        return 4
    }
    function watchFor(entry) {
        if (!entry) return null
        var address = String(entry.address || "").toLowerCase()
        var name = String(entry.name || "")
        for (var i = 0; i < CortexFeatures.watches.length; ++i) {
            var w = CortexFeatures.watches[i]
            if ((name.length && String(w.label || "") === name) || String(w.address || "").toLowerCase() === address) return w
        }
        return null
    }
    function freezeFor(entry) {
        if (!entry) return null
        var address = String(entry.address || "").toLowerCase()
        var name = String(entry.name || "")
        for (var i = 0; i < CortexFeatures.freezes.length; ++i) {
            var f = CortexFeatures.freezes[i]
            if ((name.length && String(f.label || "") === name) || String(f.address || "").toLowerCase() === address) return f
        }
        return null
    }
    function refreshAll() {
        if (!CortexPayload.ready) return
        CortexFeatures.refreshProject()
        CortexFeatures.refreshWatches()
    }
    function addEntry() {
        if (!CortexPayload.ready || !CortexApp.mutationPermission || nameField.text.trim().length === 0 || addressField.text.trim().length === 0) return
        if (CortexFeatures.setProjectAddress(nameField.text.trim(), addressField.text.trim(), typeBox.currentText, notesField.text.trim())) {
            nameField.text = ""
            addressField.text = ""
            notesField.text = ""
        }
    }

    Component.onCompleted: refreshAll()

    Timer {
        interval: CortexSettings.autoRefreshMs
        repeat: true
        running: CortexPayload.ready && CortexApp.selectedSection === "Addresses"
        onTriggered: CortexFeatures.refreshWatches()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: CortexSettings.compactUi ? 66 : 76
            color: Theme.background
            ColumnLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                anchors.topMargin: 6
                anchors.bottomMargin: 6
                spacing: 5

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    Label { text: "Address Table"; color: Theme.textBright; font.pixelSize: 13; font.bold: true; Layout.preferredWidth: 105 }
                    TextField { id: nameField; Layout.preferredWidth: 150; placeholderText: "Description" }
                    TextField { id: addressField; Layout.preferredWidth: 210; placeholderText: "Address / module+RVA"; font.family: Theme.monoFont; onAccepted: root.addEntry() }
                    ComboBox { id: typeBox; Layout.preferredWidth: 105; model: ["i32","float","double","i8","u8","i16","u16","u32","i64","u64"] }
                    TextField { id: notesField; Layout.fillWidth: true; placeholderText: "Notes (optional)"; onAccepted: root.addEntry() }
                    Button { text: "Add"; enabled: CortexPayload.ready && CortexApp.mutationPermission && nameField.text.trim().length > 0 && addressField.text.trim().length > 0; onClicked: root.addEntry() }
                    Button { text: "Refresh"; enabled: CortexPayload.ready; onClicked: root.refreshAll() }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 5
                    Label {
                        Layout.preferredWidth: 105
                        text: root.selectedEntry ? root.selectedEntry.name : "Select a row"
                        color: root.selectedEntry ? Theme.text : Theme.textDisabled
                        elide: Text.ElideRight
                        font.pixelSize: 10
                    }
                    Button { text: "Memory"; enabled: root.selectedEntry !== null; onClicked: CortexApp.openAddress("Memory", root.selectedEntry.address) }
                    Button { text: "Disasm"; enabled: root.selectedEntry !== null; onClicked: CortexApp.openAddress("Disassembly", root.selectedEntry.address) }
                    Button { text: "RE"; enabled: root.selectedEntry !== null; onClicked: CortexApp.openAddress("RE", root.selectedEntry.address) }
                    Button {
                        property var currentWatch: root.watchFor(root.selectedEntry)
                        text: currentWatch ? "Stop live" : "Watch live"
                        enabled: root.selectedEntry !== null && CortexApp.mutationPermission
                        onClicked: {
                            if (currentWatch) CortexFeatures.deleteWatch(currentWatch.id)
                            else CortexFeatures.addWatch(root.selectedEntry.address, root.normalizedType(root.selectedEntry), root.selectedEntry.name)
                        }
                    }
                    Button {
                        property var currentWatch: root.watchFor(root.selectedEntry)
                        property var currentFreeze: root.freezeFor(root.selectedEntry)
                        text: currentFreeze ? "Unfreeze" : "Freeze value"
                        enabled: root.selectedEntry !== null && CortexApp.mutationPermission && (currentFreeze !== null || (currentWatch !== null && currentWatch.hasValue))
                        onClicked: {
                            if (currentFreeze) CortexFeatures.deleteFreeze(currentFreeze.id)
                            else CortexFeatures.addFreeze(root.selectedEntry.address, root.normalizedType(root.selectedEntry), currentWatch.value, root.selectedEntry.name, 0)
                        }
                    }
                    Button {
                        text: "Find writer"
                        enabled: root.selectedEntry !== null && CortexApp.mutationPermission
                        onClicked: {
                            var t = root.normalizedType(root.selectedEntry)
                            CortexApp.openAddress("RE", root.selectedEntry.address)
                            CortexRe.findLastWriter(root.selectedEntry.address, root.typeSize(t), 10000)
                        }
                    }
                    Button {
                        text: "C++ layout"
                        enabled: root.selectedEntry !== null
                        onClicked: {
                            CortexApp.openAddress("RE", root.selectedEntry.address)
                            CortexRe.detectSubobjects(root.selectedEntry.address, 256)
                        }
                    }
                    Item { Layout.fillWidth: true }
                    Button { text: "Remove"; enabled: root.selectedEntry !== null && CortexApp.mutationPermission; onClicked: { CortexFeatures.deleteProjectAddress(root.selectedEntry.name); root.selectedEntry = null } }
                }
            }
            Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 31
            color: Theme.panel
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 0
                Label { Layout.preferredWidth: 205; text: "DESCRIPTION"; color: Theme.textMuted; font.pixelSize: 10 }
                Label { Layout.preferredWidth: 205; text: "ADDRESS"; color: Theme.textMuted; font.pixelSize: 10 }
                Label { Layout.preferredWidth: 90; text: "TYPE"; color: Theme.textMuted; font.pixelSize: 10 }
                Label { Layout.preferredWidth: 150; text: "VALUE"; color: Theme.textMuted; font.pixelSize: 10 }
                Label { Layout.preferredWidth: 95; text: "STATE"; color: Theme.textMuted; font.pixelSize: 10 }
                Label { Layout.fillWidth: true; text: "NOTES"; color: Theme.textMuted; font.pixelSize: 10 }
            }
        }

        CortexListView {
            id: table
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: CortexFeatures.projectAddresses
            clip: true
            currentIndex: -1

            delegate: Rectangle {
                required property var modelData
                required property int index
                width: table.width
                height: CortexSettings.compactUi ? 28 : 34
                property var liveWatch: root.watchFor(modelData)
                property var liveFreeze: root.freezeFor(modelData)
                color: table.currentIndex === index ? Theme.selection : (mouse.containsMouse ? Theme.hover : (index % 2 ? Theme.background : Theme.surface))

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 0
                    Text { Layout.preferredWidth: 205; text: modelData.name; color: Theme.textBright; font.pixelSize: 11; elide: Text.ElideRight }
                    Text { Layout.preferredWidth: 205; text: modelData.address; color: Theme.codeNumber; font.family: Theme.monoFont; font.pixelSize: 10; elide: Text.ElideRight }
                    Text { Layout.preferredWidth: 90; text: root.normalizedType(modelData); color: Theme.text; font.pixelSize: 10 }
                    Text {
                        Layout.preferredWidth: 150
                        text: liveWatch && liveWatch.hasValue ? liveWatch.value : (liveFreeze ? liveFreeze.value : "--")
                        color: liveFreeze ? Theme.mutation : (liveWatch && liveWatch.hasValue ? Theme.success : Theme.textDisabled)
                        font.family: Theme.monoFont
                        font.pixelSize: 10
                        elide: Text.ElideRight
                    }
                    Text {
                        Layout.preferredWidth: 95
                        text: liveFreeze ? "frozen" : (liveWatch ? "live" : "saved")
                        color: liveFreeze ? Theme.mutation : (liveWatch ? Theme.success : Theme.textMuted)
                        font.pixelSize: 10
                    }
                    Text { Layout.fillWidth: true; text: modelData.notes || ""; color: Theme.textMuted; font.pixelSize: 10; elide: Text.ElideRight }
                }

                MouseArea {
                    id: mouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: { table.currentIndex = index; root.selectedEntry = modelData }
                    onDoubleClicked: { table.currentIndex = index; root.selectedEntry = modelData; CortexApp.openAddress("Memory", modelData.address) }
                }
            }

            Text {
                anchors.centerIn: parent
                visible: table.count === 0
                text: "No addresses yet. Add one above or keep using Project / Scanner / RE as before."
                color: Theme.textDisabled
                font.pixelSize: 11
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 24
            color: Theme.panel
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                Label { text: "Double-click: memory viewer"; color: Theme.textDisabled; font.pixelSize: 9 }
                Label { text: "Live values reuse Cortex Watches"; color: Theme.textDisabled; font.pixelSize: 9 }
                Item { Layout.fillWidth: true }
                Label { text: CortexFeatures.projectAddresses.length + " saved"; color: Theme.textMuted; font.pixelSize: 9 }
            }
        }
    }
}