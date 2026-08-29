import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

ColumnLayout {
    anchors.fill: parent
    spacing: 0

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
            anchors.fill: parent
            clip: true
            model: CortexApp.scanResults
            boundsBehavior: Flickable.StopAtBounds

            delegate: Rectangle {
                id: resultRow
                required property var modelData
                required property int index
                width: ListView.view.width
                height: 28
                color: resultMouse.containsMouse ? Theme.hover : (index % 2 ? Theme.background : Theme.surface)

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
                    onDoubleClicked: {
                        if (CortexApp.readMemory(resultRow.modelData.address, 256))
                            CortexApp.selectSection("Memory")
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
}
