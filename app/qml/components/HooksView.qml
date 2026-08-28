import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

ColumnLayout {
    id: root
    anchors.fill: parent
    spacing: 0
    property bool allocationsEnabled: false

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 44
        color: Theme.background
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 5
            Button { text: "Hook health"; enabled: CortexApp.sessionActive; onClicked: CortexRuntime.callToolJson("health", "{}") }
            Button {
                text: CortexFeatures.networkCaptureEnabled ? "Network On" : "Network Off"
                enabled: CortexApp.sessionActive && CortexApp.mutationPermission
                onClicked: CortexFeatures.setNetworkCapture(!CortexFeatures.networkCaptureEnabled)
            }
            Button {
                text: root.allocationsEnabled ? "Alloc watch On" : "Alloc watch Off"
                enabled: CortexApp.sessionActive && CortexApp.mutationPermission
                onClicked: {
                    const next = !root.allocationsEnabled
                    if (CortexRuntime.callToolJson("watch_allocations", JSON.stringify({"enabled": next, "min_size": 0})))
                        root.allocationsEnabled = next
                }
            }
            Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 18; color: Theme.borderStrong }
            Button { text: "Freezes"; enabled: CortexApp.sessionActive; onClicked: CortexRuntime.callToolJson("freeze_list", "{}") }
            Button { text: "Watches"; enabled: CortexApp.sessionActive; onClicked: CortexRuntime.callToolJson("watch_list", "{}") }
            Button { text: "Watch events"; enabled: CortexApp.sessionActive; onClicked: CortexRuntime.callToolJson("watch_events", "{}") }
            Item { Layout.fillWidth: true }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 54
        color: Theme.panel
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 5
            TextField { id: addressField; Layout.preferredWidth: 180; placeholderText: "Page watch address" }
            TextField { id: sizeField; Layout.preferredWidth: 80; text: "4"; placeholderText: "Size" }
            Button {
                text: "Watch page"
                enabled: CortexApp.sessionActive && CortexApp.mutationPermission && addressField.text.length > 0
                onClicked: CortexRuntime.callToolJson("watch_page_access", JSON.stringify({"address": addressField.text, "size": parseInt(sizeField.text), "label": "Cortex UI"}))
            }
            Button { text: "Page watches"; enabled: CortexApp.sessionActive; onClicked: CortexRuntime.callToolJson("watch_page_access_list", "{}") }
            Button { text: "Page events"; enabled: CortexApp.sessionActive; onClicked: CortexRuntime.callToolJson("watch_page_access_events", "{}") }
            Button { text: "Alloc events"; enabled: CortexApp.sessionActive; onClicked: CortexRuntime.callToolJson("watch_allocations_events", "{}") }
            Item { Layout.fillWidth: true }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 30
        color: Theme.background
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            Label { text: "INSTRUMENTATION / FREEZES / WATCHES"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
            Item { Layout.fillWidth: true }
            Label { text: CortexRuntime.lastError; visible: text.length > 0; color: Theme.error; font.pixelSize: 10 }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.fillHeight: true
        color: Theme.input
        border.width: 1
        border.color: Theme.border
        Flickable {
            anchors.fill: parent
            anchors.margins: 12
            clip: true
            contentWidth: width
            contentHeight: hookText.implicitHeight
            TextEdit {
                id: hookText
                width: parent.width
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.WrapAnywhere
                text: CortexRuntime.lastResult.length > 0 ? CortexRuntime.lastResult : "Renderer, network, allocation, page-access, freeze and watch state can be inspected here."
                color: CortexRuntime.lastResult.length > 0 ? Theme.text : Theme.textDisabled
                selectionColor: Theme.accentDark
                selectedTextColor: Theme.textBright
                font.family: Theme.monoFont
                font.pixelSize: 11
            }
        }
    }
}
