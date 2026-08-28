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
                placeholderText: "Value"
                font.pixelSize: 12
                enabled: CortexApp.sessionActive
                Keys.onReturnPressed: if (scanMode.currentIndex === 0) CortexApp.scanExact(text, valueType.currentText)
            }
            ComboBox {
                id: valueType
                Layout.preferredWidth: 110
                Layout.preferredHeight: 30
                model: ["i32", "i64", "f32", "f64", "string", "bytes"]
                font.pixelSize: 11
                enabled: CortexApp.sessionActive
            }
            ComboBox {
                id: scanMode
                Layout.preferredWidth: 130
                Layout.preferredHeight: 30
                model: ["Exact value", "Changed", "Increased", "Decreased"]
                font.pixelSize: 11
                enabled: CortexApp.sessionActive
            }
            Button {
                text: "New Scan"
                font.pixelSize: 11
                enabled: CortexApp.sessionActive && valueField.text.length > 0 && scanMode.currentIndex === 0
                onClicked: {
                    CortexApp.clearScanResults()
                    CortexApp.scanExact(valueField.text, valueType.currentText)
                }
            }
            Button {
                text: "Next Scan"
                font.pixelSize: 11
                enabled: false
                ToolTip.visible: hovered
                ToolTip.text: "Refinement scans will be enabled after the first exact-scan backend is validated."
            }
            Item { Layout.fillWidth: true }
            Label { text: CortexApp.scanResults.length + " results"; color: Theme.textMuted; font.pixelSize: 11 }
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

        ListView {
            anchors.fill: parent
            clip: true
            model: CortexApp.scanResults
            boundsBehavior: Flickable.StopAtBounds

            delegate: Rectangle {
                required property var modelData
                width: ListView.view.width
                height: 28
                color: index % 2 ? Theme.background : Theme.surface

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 0
                    Text { Layout.preferredWidth: 190; text: modelData.address; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 11 }
                    Text { Layout.preferredWidth: 180; text: modelData.value; color: Theme.text; font.pixelSize: 11; elide: Text.ElideRight }
                    Text { Layout.fillWidth: true; text: modelData.bytes; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 11; elide: Text.ElideRight }
                }
            }
        }

        Text {
            visible: CortexApp.scanResults.length === 0
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
