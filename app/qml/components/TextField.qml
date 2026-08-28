import QtQuick
import QtQuick.Controls as C
import Cortex 1.0

C.TextField {
    id: control

    implicitWidth: 180
    implicitHeight: 30

    leftPadding: 9
    rightPadding: 9
    topPadding: 5
    bottomPadding: 5

    color: Theme.textBright
    selectionColor: Theme.accentDark
    selectedTextColor: Theme.textBright
    placeholderTextColor: Theme.textPlaceholder

    font.family: Theme.uiFont
    font.pixelSize: Theme.smallSize
    selectByMouse: true

    background: Rectangle {
        color: control.enabled ? Theme.input : Theme.surface
        border.width: 1
        border.color: control.activeFocus
                      ? Theme.accent
                      : control.hovered
                        ? Theme.borderStrong
                        : Theme.borderStrong
        radius: Theme.radius
        opacity: control.enabled ? 1.0 : 0.72
    }
}
