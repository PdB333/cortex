import QtQuick
import Cortex 1.0

Rectangle {
    id: control
    property bool checked: false
    signal toggled(bool checked)

    implicitWidth: 38
    implicitHeight: 20
    radius: height / 2
    color: checked ? Theme.accentDark : Theme.surfaceRaised
    border.width: 1
    border.color: checked ? Theme.accent : Theme.borderStrong
    opacity: enabled ? 1.0 : 0.55

    Rectangle {
        width: 14
        height: 14
        radius: 7
        y: 3
        x: control.checked ? control.width - width - 3 : 3
        color: control.checked ? Theme.textBright : Theme.textMuted
        Behavior on x { NumberAnimation { duration: 90 } }
    }

    MouseArea {
        anchors.fill: parent
        enabled: control.enabled
        cursorShape: Qt.PointingHandCursor
        onClicked: {
            control.checked = !control.checked
            control.toggled(control.checked)
        }
    }
}