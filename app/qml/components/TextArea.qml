import QtQuick
import QtQuick.Controls as C
import Cortex 1.0

C.TextArea {
    id: control

    implicitWidth: 240
    implicitHeight: 96
    leftPadding: CortexSettings.compactUi ? 7 : 9
    rightPadding: CortexSettings.compactUi ? 7 : 9
    topPadding: CortexSettings.compactUi ? 5 : 7
    bottomPadding: CortexSettings.compactUi ? 5 : 7

    color: control.enabled ? Theme.textBright : Theme.textDisabled
    selectionColor: Theme.accentDark
    selectedTextColor: Theme.textBright
    placeholderTextColor: Theme.textPlaceholder
    font.family: Theme.uiFont
    font.pixelSize: Theme.smallSize
    selectByMouse: true

    background: Rectangle {
        color: control.readOnly ? Theme.surface : Theme.input
        border.width: 1
        border.color: control.activeFocus ? Theme.accent : Theme.borderStrong
        radius: Theme.radius
        opacity: control.enabled ? 1.0 : 0.72
    }
}
