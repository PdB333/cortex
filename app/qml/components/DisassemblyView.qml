import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

ColumnLayout {
    id: root
    anchors.fill: parent
    spacing: 0
    property string selectedAddress: ""

    function analysisAddress() {
        return selectedAddress.length > 0 ? selectedAddress : addressField.text
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 78
        color: Theme.background
        ColumnLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            anchors.topMargin: 7
            anchors.bottomMargin: 7
            spacing: 5
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                TextField {
                    id: addressField
                    Layout.preferredWidth: 260
                    Layout.preferredHeight: 30
                    placeholderText: "Address, e.g. 0x7FF612340000"
                    font.family: Theme.monoFont
                    font.pixelSize: 11
                    enabled: CortexApp.sessionActive
                    onAccepted: if (enabled && text.length > 0 && CortexDisasm.disassemble(text, 160)) root.selectedAddress = CortexDisasm.currentAddress
                }
                Button {
                    text: "Go"
                    font.pixelSize: 11
                    enabled: CortexApp.sessionActive && addressField.text.length > 0
                    onClicked: if (CortexDisasm.disassemble(addressField.text, 160)) root.selectedAddress = CortexDisasm.currentAddress
                }
                Button {
                    text: "Back"
                    font.pixelSize: 11
                    enabled: CortexDisasm.canGoBack
                    onClicked: if (CortexDisasm.goBack()) { addressField.text = CortexDisasm.currentAddress; root.selectedAddress = CortexDisasm.currentAddress }
                }
                Button {
                    text: "Forward"
                    font.pixelSize: 11
                    enabled: CortexDisasm.canGoForward
                    onClicked: if (CortexDisasm.goForward()) { addressField.text = CortexDisasm.currentAddress; root.selectedAddress = CortexDisasm.currentAddress }
                }
                Item { Layout.fillWidth: true }
                Label {
                    text: CortexDisasm.lastError.length > 0
                        ? CortexDisasm.lastError
                        : (CortexDisasm.rows.length > 0 ? CortexDisasm.rows.length + " instructions" : "")
                    color: CortexDisasm.lastError.length > 0 ? Theme.mutation : Theme.textDisabled
                    font.pixelSize: 10
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Label { text: "ANALYSIS"; color: Theme.textMuted; font.pixelSize: 9; font.bold: true }
                Button { text: "CFG"; enabled: root.analysisAddress().length > 0 && CortexPayload.ready; onClicked: CortexRuntime.callToolJson("analysis_cfg", JSON.stringify({address: root.analysisAddress()})) }
                Button { text: "Xrefs"; enabled: root.analysisAddress().length > 0 && CortexPayload.ready; onClicked: CortexRuntime.callToolJson("analysis_xrefs", JSON.stringify({target: root.analysisAddress(), include_data: true})) }
                Button { text: "Structured CFG"; enabled: root.analysisAddress().length > 0 && CortexPayload.ready; onClicked: CortexRuntime.callToolJson("analysis_structure", JSON.stringify({address: root.analysisAddress()})) }
                Label { text: root.selectedAddress.length > 0 ? "Selected " + root.selectedAddress : "Select an instruction or enter an address"; color: Theme.textDisabled; font.family: Theme.monoFont; font.pixelSize: 9 }
                Item { Layout.fillWidth: true }
                Button { text: "Clear analysis"; enabled: CortexRuntime.lastResult.length > 0 || CortexRuntime.lastError.length > 0; onClicked: CortexRuntime.clearResult() }
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
            spacing: 0
            Label { Layout.preferredWidth: 170; text: "Address"; color: Theme.textMuted; font.pixelSize: 11 }
            Label { Layout.preferredWidth: 220; text: "Bytes"; color: Theme.textMuted; font.pixelSize: 11 }
            Label { Layout.fillWidth: true; text: "Instruction"; color: Theme.textMuted; font.pixelSize: 11 }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    SplitView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        orientation: Qt.Vertical

        Rectangle {
            SplitView.fillHeight: true
            SplitView.minimumHeight: 180
            color: Theme.background
            ListView {
                id: list
                anchors.fill: parent
                clip: true
                model: CortexDisasm.rows
                boundsBehavior: Flickable.StopAtBounds

                delegate: Rectangle {
                    required property var modelData
                    required property int index
                    width: list.width
                    height: 26
                    color: ListView.isCurrentItem ? Theme.selection : (index % 2 ? Theme.background : Theme.surface)
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 0
                        Text { Layout.preferredWidth: 170; text: modelData.address; color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 11 }
                        Text { Layout.preferredWidth: 220; text: modelData.bytes; color: Theme.textDisabled; font.family: Theme.monoFont; font.pixelSize: 11; elide: Text.ElideRight }
                        Text { Layout.fillWidth: true; text: modelData.text; color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 11; elide: Text.ElideRight }
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            list.currentIndex = index
                            root.selectedAddress = modelData.address
                        }
                        onDoubleClicked: {
                            addressField.text = modelData.address
                            if (CortexDisasm.disassemble(modelData.address, 160)) root.selectedAddress = CortexDisasm.currentAddress
                        }
                    }
                }
            }
            Text {
                visible: CortexDisasm.rows.length === 0
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.leftMargin: 12
                anchors.topMargin: 14
                text: CortexApp.sessionActive ? "Enter a readable code address to disassemble." : "Select a target to disassemble code."
                color: Theme.textDisabled
                font.pixelSize: 11
            }
        }

        Rectangle {
            SplitView.preferredHeight: 175
            SplitView.minimumHeight: 90
            color: Theme.input
            border.color: Theme.border
            ColumnLayout {
                anchors.fill: parent
                spacing: 0
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 28
                    color: Theme.panel
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        Label { text: "ANALYSIS RESULT"; color: Theme.textMuted; font.pixelSize: 9; font.bold: true }
                        Item { Layout.fillWidth: true }
                        Label { text: CortexRuntime.lastError; visible: text.length > 0; color: Theme.error; font.pixelSize: 9 }
                    }
                }
                Flickable {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    contentWidth: width
                    contentHeight: analysisResult.implicitHeight + 18
                    TextEdit {
                        id: analysisResult
                        width: parent.width
                        readOnly: true
                        selectByMouse: true
                        wrapMode: TextEdit.WrapAnywhere
                        text: CortexRuntime.lastResult.length > 0 ? CortexRuntime.lastResult : "Run CFG, Xrefs, or Structured CFG for the selected address."
                        color: CortexRuntime.lastResult.length > 0 ? Theme.text : Theme.textDisabled
                        font.family: Theme.monoFont
                        font.pixelSize: 10
                    }
                }
            }
        }
    }
}