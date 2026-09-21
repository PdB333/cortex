import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

ColumnLayout {
    id: root
    anchors.fill: parent
    spacing: 0

    function defaultFieldsJson() {
        return "[\n  {\"name\": \"health\", \"offset\": 0, \"type\": \"i32\"}\n]"
    }

    function syncSelectedDefinition() {
        if (CortexFeatures.selectedStructureName.length === 0)
            return
        nameField.text = CortexFeatures.selectedStructureName
        fieldsEdit.text = CortexFeatures.selectedStructureFieldsJson.length > 0
                ? CortexFeatures.selectedStructureFieldsJson : root.defaultFieldsJson()
    }

    function newDefinition() {
        CortexFeatures.clearStructureSelection()
        nameField.text = ""
        fieldsEdit.text = root.defaultFieldsJson()
        valuesField.text = "{}"
    }

    function useInference() {
        const fields = []
        for (let i = 0; i < CortexFeatures.structureInferenceFields.length; ++i) {
            const item = CortexFeatures.structureInferenceFields[i]
            fields.push({"name": item.name, "offset": item.offset, "type": item.type})
        }
        if (fields.length > 0)
            fieldsEdit.text = JSON.stringify(fields, null, 2)
    }

    Connections {
        target: CortexApp
        function onNavigationAddressChanged() {
            if (CortexApp.selectedSection !== "Structures" || CortexApp.navigationAddress.length === 0) return
            addressField.text = CortexApp.navigationAddress
        }
    }
    Component.onCompleted: {
        if (CortexApp.sessionActive)
            CortexFeatures.refreshStructures()
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 44
        color: Theme.background

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 5

            Button {
                text: "Refresh"
                enabled: CortexApp.sessionActive
                onClicked: CortexFeatures.refreshStructures()
            }
            Button {
                text: "New"
                onClicked: root.newDefinition()
            }
            Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 18; color: Theme.borderStrong }
            TextField {
                id: nameField
                Layout.preferredWidth: 190
                placeholderText: "Structure name"
            }
            Button {
                text: "Define"
                enabled: CortexApp.sessionActive && CortexApp.mutationPermission && nameField.text.trim().length > 0
                onClicked: {
                    if (CortexFeatures.defineStructure(nameField.text, fieldsEdit.text))
                        root.syncSelectedDefinition()
                }
            }
            Button {
                text: "Delete"
                enabled: CortexApp.sessionActive && CortexApp.mutationPermission && nameField.text.trim().length > 0
                onClicked: {
                    const deletedName = nameField.text.trim()
                    if (CortexFeatures.deleteStructure(deletedName))
                        root.newDefinition()
                }
            }
            Item { Layout.fillWidth: true }
            Label {
                text: CortexFeatures.lastError.length > 0 ? CortexFeatures.lastError : CortexFeatures.structureStatus
                color: CortexFeatures.lastError.length > 0 ? Theme.error : Theme.textMuted
                font.pixelSize: 10
                elide: Text.ElideRight
                Layout.maximumWidth: 460
            }
            Label {
                text: CortexApp.mutationPermission ? "Mutation enabled" : "Define / Write require Mutation"
                color: CortexApp.mutationPermission ? Theme.mutation : Theme.textDisabled
                font.pixelSize: 9
            }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 46
        color: Theme.panel

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 6

            TextField {
                id: addressField
                Layout.preferredWidth: 190
                placeholderText: "Instance address (0x...)"
                font.family: Theme.monoFont
            }
            Button {
                text: "Read instance"
                enabled: CortexApp.sessionActive && nameField.text.trim().length > 0 && addressField.text.trim().length > 0
                onClicked: CortexFeatures.readStructure(nameField.text, addressField.text)
            }
            Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 18; color: Theme.borderStrong }
            Label { text: "Values"; color: Theme.textMuted; font.pixelSize: 10 }
            TextField {
                id: valuesField
                Layout.fillWidth: true
                text: "{}"
                placeholderText: "{\"health\": 100, \"position\": [0, 1, 2]}"
                font.family: Theme.monoFont
                font.pixelSize: 10
            }
            Button {
                text: "Write fields"
                enabled: CortexApp.sessionActive && CortexApp.mutationPermission &&
                         nameField.text.trim().length > 0 && addressField.text.trim().length > 0 &&
                         valuesField.text.trim().length > 2
                onClicked: CortexFeatures.writeStructure(nameField.text, addressField.text, valuesField.text)
            }
        }
        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
    }

    CortexSplitView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        orientation: Qt.Horizontal

        Rectangle {
            SplitView.preferredWidth: 250
            SplitView.minimumWidth: 190
            color: Theme.background

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 30
                    color: Theme.panel
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 10
                        Label { text: "DEFINED STRUCTURES"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
                        Item { Layout.fillWidth: true }
                        Label { text: CortexFeatures.structureDefinitions.length; color: Theme.textDisabled; font.pixelSize: 10 }
                    }
                    Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
                }

                CortexListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: CortexFeatures.structureDefinitions
                    delegate: Rectangle {
                        required property var modelData
                        required property int index
                        width: ListView.view.width
                        height: 34
                        color: CortexFeatures.selectedStructureName === modelData.name
                               ? Theme.selection : (index % 2 ? Theme.background : Theme.surface)

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 10
                            spacing: 8
                            Text {
                                Layout.fillWidth: true
                                text: modelData.name
                                color: Theme.text
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                            Text {
                                text: modelData.fieldCount + " fields"
                                color: Theme.textDisabled
                                font.pixelSize: 9
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                if (CortexFeatures.selectStructure(modelData.name))
                                    root.syncSelectedDefinition()
                            }
                        }
                        Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border; opacity: 0.55 }
                    }
                    Text {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.margins: 14
                        visible: parent.count === 0
                        text: CortexApp.sessionActive ? "No structure definitions in this project." : "Select a target to load structures."
                        color: Theme.textDisabled
                        font.pixelSize: 11
                    }
                }
            }
        }

        Rectangle {
            SplitView.preferredWidth: 390
            SplitView.minimumWidth: 300
            color: Theme.background

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 30
                    color: Theme.panel
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 10
                        Label { text: "FIELDS JSON"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
                        Item { Layout.fillWidth: true }
                        Button {
                            text: "Use inference"
                            enabled: CortexFeatures.structureInferenceFields.length > 0
                            onClicked: root.useInference()
                        }
                    }
                    Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: Theme.input
                    border.width: 1
                    border.color: Theme.border

                    CortexFlickable {
                        anchors.fill: parent
                        clip: true
                        contentWidth: width
                        contentHeight: Math.max(height, fieldsEdit.implicitHeight + 20)

                        TextEdit {
                            id: fieldsEdit
                            x: 10
                            y: 10
                            width: Math.max(0, parent.width - 20)
                            text: root.defaultFieldsJson()
                            color: Theme.text
                            selectionColor: Theme.accentDark
                            selectedTextColor: Theme.textBright
                            font.family: Theme.monoFont
                            font.pixelSize: 11
                            selectByMouse: true
                            wrapMode: TextEdit.WrapAnywhere
                        }
                    }
                }
            }
        }

        Rectangle {
            SplitView.fillWidth: true
            SplitView.minimumWidth: 390
            color: Theme.background

            CortexSplitView {
                anchors.fill: parent
                orientation: Qt.Vertical

                Rectangle {
                    SplitView.preferredHeight: 265
                    SplitView.minimumHeight: 150
                    color: Theme.background

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 0

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 30
                            color: Theme.panel
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 10
                                Label { text: "INSTANCE FIELDS"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
                                Item { Layout.fillWidth: true }
                                Label { text: CortexFeatures.structureReadFields.length + " rows"; color: Theme.textDisabled; font.pixelSize: 9 }
                            }
                            Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 28
                            color: Theme.background
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                spacing: 0
                                Label { Layout.preferredWidth: 145; text: "FIELD"; color: Theme.textMuted; font.pixelSize: 9 }
                                Label { Layout.fillWidth: true; text: "VALUE"; color: Theme.textMuted; font.pixelSize: 9 }
                                Label { Layout.preferredWidth: 120; text: "ERROR"; color: Theme.textMuted; font.pixelSize: 9 }
                            }
                        }

                        CortexListView {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            model: CortexFeatures.structureReadFields
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
                                    Text { Layout.preferredWidth: 145; text: modelData.name; color: Theme.text; font.pixelSize: 10; elide: Text.ElideRight }
                                    Text { Layout.fillWidth: true; text: modelData.value; color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 10; elide: Text.ElideRight }
                                    Text { Layout.preferredWidth: 120; text: modelData.error; color: Theme.error; font.pixelSize: 9; elide: Text.ElideRight }
                                }
                            }
                            Text {
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.margins: 14
                                visible: parent.count === 0
                                text: "Read a structure instance to populate field values."
                                color: Theme.textDisabled
                                font.pixelSize: 11
                            }
                        }
                    }
                }

                Rectangle {
                    SplitView.fillHeight: true
                    SplitView.minimumHeight: 190
                    color: Theme.background

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 0

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 30
                            color: Theme.panel
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 10
                                Label { text: "STRUCTURE INFERENCE"; color: Theme.textMuted; font.pixelSize: 10; font.bold: true }
                                Item { Layout.fillWidth: true }
                                Label { text: CortexFeatures.structureInferenceFields.length + " guesses"; color: Theme.textDisabled; font.pixelSize: 9 }
                            }
                            Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 44
                            color: Theme.background
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                spacing: 5
                                TextField {
                                    id: instancesField
                                    Layout.fillWidth: true
                                    text: "[]"
                                    placeholderText: "[\"0x140001000\", \"0x140002000\"]"
                                    font.family: Theme.monoFont
                                    font.pixelSize: 10
                                }
                                TextField {
                                    id: inferSizeField
                                    Layout.preferredWidth: 72
                                    text: "64"
                                    placeholderText: "Size"
                                    font.family: Theme.monoFont
                                }
                                Button {
                                    text: "Infer"
                                    enabled: CortexApp.sessionActive && instancesField.text.trim().length > 2
                                    onClicked: CortexFeatures.inferStructure(instancesField.text, parseInt(inferSizeField.text), false, "")
                                }
                                Button {
                                    text: "Infer + Define"
                                    enabled: CortexApp.sessionActive && CortexApp.mutationPermission &&
                                             nameField.text.trim().length > 0 && instancesField.text.trim().length > 2
                                    onClicked: {
                                        if (CortexFeatures.inferStructure(instancesField.text, parseInt(inferSizeField.text), true, nameField.text))
                                            root.syncSelectedDefinition()
                                    }
                                }
                            }
                            Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 28
                            color: Theme.background
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                spacing: 0
                                Label { Layout.preferredWidth: 70; text: "OFFSET"; color: Theme.textMuted; font.pixelSize: 9 }
                                Label { Layout.preferredWidth: 130; text: "NAME"; color: Theme.textMuted; font.pixelSize: 9 }
                                Label { Layout.preferredWidth: 90; text: "TYPE"; color: Theme.textMuted; font.pixelSize: 9 }
                                Label { Layout.preferredWidth: 70; text: "CONF"; color: Theme.textMuted; font.pixelSize: 9 }
                                Label { Layout.fillWidth: true; text: "DETAIL"; color: Theme.textMuted; font.pixelSize: 9 }
                            }
                        }

                        CortexListView {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            model: CortexFeatures.structureInferenceFields
                            delegate: Rectangle {
                                required property var modelData
                                required property int index
                                width: ListView.view.width
                                height: 30
                                color: index % 2 ? Theme.background : Theme.surface
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 10
                                    anchors.rightMargin: 8
                                    spacing: 0
                                    Text { Layout.preferredWidth: 70; text: "0x" + Number(modelData.offset).toString(16).toUpperCase(); color: Theme.textMuted; font.family: Theme.monoFont; font.pixelSize: 10 }
                                    Text { Layout.preferredWidth: 130; text: modelData.name; color: Theme.text; font.pixelSize: 10; elide: Text.ElideRight }
                                    Text { Layout.preferredWidth: 90; text: modelData.type; color: Theme.text; font.family: Theme.monoFont; font.pixelSize: 10 }
                                    Text { Layout.preferredWidth: 70; text: Math.round(Number(modelData.confidence) * 100) + "%"; color: Theme.textMuted; font.pixelSize: 10 }
                                    Text {
                                        Layout.fillWidth: true
                                        text: (modelData.constant ? "constant; " : "varies; ") + modelData.distinctValues + " distinct"
                                        color: Theme.textMuted
                                        font.pixelSize: 9
                                        elide: Text.ElideRight
                                    }
                                }
                                ToolTip.visible: hoverArea.containsMouse
                                ToolTip.text: modelData.reasons + "\nvalues=" + modelData.values
                                MouseArea { id: hoverArea; anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.NoButton }
                            }
                            Text {
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.margins: 14
                                visible: parent.count === 0
                                text: "Supply one or more readable instance addresses to infer a layout."
                                color: Theme.textDisabled
                                font.pixelSize: 11
                            }
                        }
                    }
                }
            }
        }
    }
}
