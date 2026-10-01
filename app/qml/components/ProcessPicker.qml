import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

Popup {
    id: root

    property int anchorX: 74
    property int anchorY: Theme.topBarHeight - 1

    width: Math.min(720, parent ? parent.width - 32 : 720)
    x: parent ? Math.max(8, Math.min(anchorX, parent.width - width - 8)) : anchorX
    y: anchorY
    height: 510
    padding: 0
    modal: false
    dim: false
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    function matches(target) {
        const q = search.text.trim().toLowerCase()
        if (!q.length) return true
        const haystack = [target.name, target.pid, target.architecture, target.path, target.windowTitle]
                .join(" ").toLowerCase()
        return haystack.indexOf(q) !== -1
    }

    onOpened: {
        search.text = ""
        search.forceActiveFocus()
    }

    background: Rectangle {
        color: Theme.surfaceRaised
        border.color: Theme.borderStrong
        border.width: 1
        radius: Theme.radius
    }

    contentItem: ColumnLayout {
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 50
            color: Theme.surfaceRaised

            TextField {
                id: search
                anchors.fill: parent
                anchors.margins: 7
                leftPadding: 11
                rightPadding: 11
                placeholderText: "Search processes by name, PID or path"
                selectByMouse: true
                color: Theme.textBright
                placeholderTextColor: Theme.textPlaceholder
                font.family: Theme.uiFont
                font.pixelSize: 13

                background: Rectangle {
                    color: Theme.input
                    border.color: search.activeFocus ? Theme.accent : Theme.borderStrong
                    border.width: 1
                    radius: Theme.radius
                }

                Keys.onDownPressed: processList.forceActiveFocus()
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Theme.border
        }

        CortexListView {
            id: processList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: CortexApp.targets
            boundsBehavior: Flickable.StopAtBounds

            ScrollBar.vertical: ScrollBar {
                id: processScroll
                policy: ScrollBar.AsNeeded
                width: 10
                padding: 2
                contentItem: Rectangle {
                    implicitWidth: 6
                    radius: 3
                    color: processScroll.pressed ? Theme.textMuted : Theme.borderStrong
                    opacity: processScroll.active ? 0.95 : 0.65
                }
                background: Item {}
            }

            delegate: Rectangle {
                required property int index
                required property var modelData

                property bool accepted: root.matches(modelData)
                property bool selected: CortexApp.currentTargetIndex === index
                property bool attached: !!modelData.attached

                width: Math.max(0, ListView.view.width - 10)
                height: accepted ? 64 : 0
                visible: accepted
                color: selected ? Theme.selection : (rowMouse.containsMouse ? Theme.hover : "transparent")

                Rectangle {
                    visible: attached
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: 2
                    color: selected ? Theme.accent : Theme.success
                }

                Column {
                    anchors.left: parent.left
                    anchors.leftMargin: 14
                    anchors.right: parent.right
                    anchors.rightMargin: attached ? 42 : 12
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 4

                    RowLayout {
                        width: parent.width
                        spacing: 10

                        Text {
                            Layout.fillWidth: true
                            text: modelData.name
                            color: Theme.textBright
                            elide: Text.ElideRight
                            font.family: Theme.uiFont
                            font.pixelSize: 13
                            font.bold: selected
                        }

                        Text {
                            visible: attached
                            text: selected ? "ACTIVE" : "ATTACHED"
                            color: selected ? Theme.accent : Theme.success
                            font.family: Theme.uiFont
                            font.pixelSize: 9
                            font.bold: true
                        }

                        Text {
                            Layout.preferredWidth: 168
                            Layout.minimumWidth: 168
                            horizontalAlignment: Text.AlignRight
                            text: "PID " + modelData.pid + "  |  " + modelData.architecture
                            color: Theme.textMuted
                            elide: Text.ElideLeft
                            font.family: Theme.monoFont
                            font.pixelSize: 10
                        }
                    }

                    Text {
                        width: parent.width
                        text: modelData.windowTitle && modelData.windowTitle.length
                              ? modelData.windowTitle
                              : (modelData.path && modelData.path.length ? modelData.path : modelData.platform)
                        color: Theme.textMuted
                        elide: Text.ElideMiddle
                        font.family: Theme.uiFont
                        font.pixelSize: 11
                    }
                }

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 1
                    color: Theme.border
                    opacity: 0.7
                }

                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        CortexApp.selectTarget(index)
                        root.close()
                    }
                }

                Rectangle {
                    visible: attached
                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    width: 26
                    height: 26
                    z: 3
                    radius: Theme.radius
                    color: detachMouse.containsMouse ? Theme.hover : "transparent"

                    Text {
                        anchors.centerIn: parent
                        text: "x"
                        color: detachMouse.containsMouse ? Theme.textBright : Theme.textMuted
                        font.pixelSize: 15
                    }

                    MouseArea {
                        id: detachMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            const wasActive = selected
                            CortexApp.detachTargetAt(index)
                            if (wasActive) root.close()
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 31
            color: Theme.panel

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 11
                anchors.rightMargin: 11

                Text {
                    text: CortexApp.attachedTargetCount + " attached  |  " + CortexApp.targetCount + " processes"
                    color: Theme.textMuted
                    font.family: Theme.uiFont
                    font.pixelSize: 10
                }
                Item { Layout.fillWidth: true }
                Text {
                    text: "Click to attach/switch  |  x detach  |  Esc to close"
                    color: Theme.textDisabled
                    font.family: Theme.uiFont
                    font.pixelSize: 10
                }
            }
        }
    }
}
