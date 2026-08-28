import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

Popup {
    id: root

    property int anchorX: 70
    property int anchorY: Theme.topBarHeight - 1

    x: anchorX
    y: anchorY
    width: Math.min(640, parent ? parent.width - 40 : 640)
    height: 470
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
            Layout.preferredHeight: 48
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
                placeholderTextColor: Theme.textMuted
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

        ListView {
            id: processList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: CortexApp.targets
            boundsBehavior: Flickable.StopAtBounds

            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: Rectangle {
                required property int index
                required property var modelData

                property bool accepted: root.matches(modelData)
                property bool selected: CortexApp.currentTargetIndex === index

                width: ListView.view.width
                height: accepted ? 60 : 0
                visible: accepted
                color: selected ? Theme.selection : (rowMouse.containsMouse ? Theme.hover : "transparent")

                Rectangle {
                    visible: selected
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: 2
                    color: Theme.accent
                }

                Column {
                    anchors.left: parent.left
                    anchors.leftMargin: 14
                    anchors.right: parent.right
                    anchors.rightMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 3

                    RowLayout {
                        width: parent.width
                        spacing: 8

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
                            text: "PID " + modelData.pid + "  ·  " + modelData.architecture
                            color: Theme.textMuted
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
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 30
            color: Theme.panel

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 11
                anchors.rightMargin: 11

                Text {
                    text: CortexApp.targetCount + " processes"
                    color: Theme.textMuted
                    font.family: Theme.uiFont
                    font.pixelSize: 10
                }
                Item { Layout.fillWidth: true }
                Text {
                    text: "Enter to attach  ·  Esc to close"
                    color: Theme.textDisabled
                    font.family: Theme.uiFont
                    font.pixelSize: 10
                }
            }
        }
    }
}
