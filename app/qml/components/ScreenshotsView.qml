import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

ColumnLayout {
    anchors.fill: parent
    spacing: 0

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 44
        color: Theme.background
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 5
            ComboBox {
                id: modeBox
                Layout.preferredWidth: 120
                model: ["auto", "render", "window", "last"]
            }
            Button {
                text: "Capture"
                enabled: CortexApp.sessionActive
                onClicked: CortexFeatures.captureScreenshot(modeBox.currentText)
            }
            Item { Layout.fillWidth: true }
            Label {
                text: CortexFeatures.screenshotMeta.length > 0 ? CortexFeatures.screenshotMeta : CortexFeatures.lastError
                color: CortexFeatures.lastError.length > 0 ? Theme.error : Theme.textMuted
                font.pixelSize: 10
                elide: Text.ElideRight
                Layout.maximumWidth: 420
            }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.margins: 10
        color: Theme.surface
        border.width: 1
        border.color: Theme.border

        Image {
            anchors.fill: parent
            anchors.margins: 8
            source: CortexFeatures.screenshotSource
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            cache: false
        }

        Text {
            anchors.centerIn: parent
            visible: CortexFeatures.screenshotSource.length === 0
            text: CortexApp.sessionActive ? "Capture a live or cached target frame." : "Select a target to capture its visual state."
            color: Theme.textDisabled
            font.pixelSize: 11
        }
    }
}
