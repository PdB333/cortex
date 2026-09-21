import QtQuick
import QtQuick.Templates as T
import Cortex 1.0

T.ToolButton {
    id: control
    implicitWidth: Math.max(CortexSettings.compactUi ? 28 : 32, implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: CortexSettings.compactUi ? 26 : 30
    leftPadding: CortexSettings.compactUi ? 6 : 8
    rightPadding: CortexSettings.compactUi ? 6 : 8
    topPadding: 4
    bottomPadding: 4
    hoverEnabled: true

    font.family: Theme.uiFont
    font.pixelSize: Theme.smallSize

    contentItem: Text {
        text: control.text
        font: control.font
        color: control.enabled ? (control.checked ? Theme.textBright : Theme.text) : Theme.textDisabled
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        color: !control.enabled ? "transparent" : control.down ? Theme.selection : control.hovered ? Theme.hover : "transparent"
        border.width: control.activeFocus ? 1 : 0
        border.color: Theme.accent
        radius: Theme.radius
    }
}
