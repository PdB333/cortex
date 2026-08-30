import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

ColumnLayout {
    id: root
    anchors.fill: parent
    spacing: 0
    property var selectedResult: null

    function focusSearch() {
        valueField.forceActiveFocus()
        valueField.selectAll()
    }

    Component.onCompleted: Qt.callLater(root.focusSearch)

    function addressType() {
        const t = valueType.currentText
        if (t === "f32") return "float"
        if (t === "f64") return "double"
        if (t === "i64") return "i64"
        return "i32"
    }

    function addResultToAddresses(result) {
        if (!result) return
        const address = String(result.address || "")
        if (address.length === 0) return
        if (!CortexApp.mutationPermission) {
            CortexApp.openAddress("Addresses", address)
            return
        }
        const name = "Scan " + address
        const notes = "Scanner value: " + String(result.value || "")
        if (CortexFeatures.setProjectAddress(name, address, root.addressType(), notes))
            CortexApp.selectSection("Addresses")
    }

    function openContextFor(item, sourceItem, x, y) {
        selectedResult = item
        contextMenu.address = String(item.address || "")
        contextMenu.label = "Scan " + contextMenu.address
        contextMenu.valueType = root.addressType()
        const p = sourceItem.mapToItem(root, x, y)
        contextMenu.x = Math.max(0, Math.min(root.width - contextMenu.implicitWidth, p.x))
        contextMenu.y = Math.max(0, Math.min(root.height - 320, p.y))
        contextMenu.open()
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 48
        color: Theme.background

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            spacing: 8

            TextField {
                id: valueField
                Layout.preferredWidth: 220
                Layout.preferredHeight: 30
                placeholderText: scanMode.currentIndex === 0 ? "Value" : "Previous scan values"
                font.pixelSize: 12
                enabled: CortexApp.sessionActive && !CortexApp.scanBusy && scanMode.currentIndex === 0
                Keys.onReturnPressed: {
                    if (scanMode.currentIndex === 0 && text.length) {
                        if (CortexApp.scanResults.length > 0)
                            CortexApp.startScan(text, valueType.currentText, "Exact value", true)
                        else
                            CortexApp.startScan(text, valueType.currentText, "Exact value", false)
                    }
                }
            }
            ComboBox {
                id: valueType
                Layout.preferredWidth: 110
                Layout.preferredHeight: 30
                model: ["i32", "i64", "f32", "f64", "string", "bytes"]
                font.pixelSize: 11
                enabled: CortexApp.sessionActive && !CortexApp.scanBusy && CortexApp.scanResults.length === 0
            }
            ComboBox {
                id: scanMode
                Layout.preferredWidth: 130
                Layout.preferredHeight: 30
                model: ["Exact value", "Changed", "Unchanged", "Increased", "Decreased"]
                font.pixelSize: 11
                enabled: CortexApp.sessionActive && !CortexApp.scanBusy
                onCurrentIndexChanged: {
                    if ((currentIndex === 3 || currentIndex === 4) &&
                        (valueType.currentText === "string" || valueType.currentText === "bytes"))
                        currentIndex = 1
                }
            }
            Button {
                text: CortexApp.scanBusy ? "Scanning..." : "New Scan"
                font.pixelSize: 11
                enabled: CortexApp.sessionActive && !CortexApp.scanBusy && valueField.text.length > 0
                onClicked: {
                    CortexApp.clearScanResults()
                    scanMode.currentIndex = 0
                    CortexApp.startScan(valueField.text, valueType.currentText, "Exact value", false)
                }
            }
            Button {
                text: "Next Scan"
                font.pixelSize: 11
                enabled: CortexApp.sessionActive && !CortexApp.scanBusy && CortexApp.scanResults.length > 0 &&
                         (scanMode.currentIndex !== 0 || valueField.text.length > 0)
                onClicked: CortexApp.startScan(scanMode.currentIndex === 0 ? valueField.text : "",
                                               valueType.currentText, scanMode.currentText, true)
            }
            Button {
                text: "Cancel"
                visible: CortexApp.scanBusy
                font.pixelSize: 11
                onClicked: CortexApp.cancelScan()
            }
            Item { Layout.fillWidth: true }
            Label {
                text: CortexApp.scanBusy ? CortexApp.scanStatus : CortexApp.scanResults.length + " results"
                color: CortexApp.scanBusy ? Theme.accent : Theme.textMuted
                font.pixelSize: 11
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
            Label { Layout.preferredWidth: 190; text: "Address"; color: Theme.textMuted; font.pixelSize: 11 }
            Label { Layout.preferredWidth: 180; text: "Value"; color: Theme.textMuted; font.pixelSize: 11 }
            Label { Layout.fillWidth: true; text: "Bytes"; color: Theme.textMuted; font.pixelSize: 11 }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.fillHeight: true
        color: Theme.background

        CortexListView {
            id: resultList
            anchors.fill: parent
            clip: true
            model: CortexApp.scanResults
            boundsBehavior: Flickable.StopAtBounds
            currentIndex: -1

            delegate: Rectangle {
                id: resultRow
                required property var modelData
                required property int index
                width: ListView.view.width
                height: 28
                color: resultList.currentIndex === index ? Theme.selection : (resultMouse.containsMouse ? Theme.hover : (index % 2 ? Theme.background : Theme.surface))

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 0
                    Text { Layout.preferredWidth: 190; text: modelData.address; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 11 }
                    Text { Layout.preferredWidth: 180; text: modelData.value; color: Theme.text; font.pixelSize: 11; elide: Text.ElideRight }
                    Text { Layout.fillWidth: true; text: modelData.bytes; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 11; elide: Text.ElideRight }
                }

                MouseArea {
                    id: resultMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onPressed: function(event) {
                        resultList.currentIndex = index
                        root.selectedResult = modelData
                        if (event.button === Qt.RightButton) root.openContextFor(modelData, resultMouse, event.x, event.y)
                    }
                    onDoubleClicked: function(event) {
                        if (event.button === Qt.LeftButton) root.addResultToAddresses(modelData)
                    }
                }
            }
        }

        Text {
            visible: CortexApp.scanResults.length === 0 && !CortexApp.scanBusy
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.leftMargin: 12
            anchors.topMargin: 14
            text: CortexApp.lastError.length ? CortexApp.lastError : (CortexApp.sessionActive ? "Run an exact-value scan." : "Select a target to scan memory.")
            color: CortexApp.lastError.length ? Theme.error : Theme.textDisabled
            font.family: CortexApp.lastError.length ? Theme.monoFont : Theme.uiFont
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
            Label { text: "Double-click: add to Addresses | Right-click: address actions"; color: Theme.textDisabled; font.pixelSize: 9 }
            Item { Layout.fillWidth: true }
        }
    }

    AddressContextMenu {
        id: contextMenu
        allowSave: true
    }
}