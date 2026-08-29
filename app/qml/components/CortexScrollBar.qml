import QtQuick
import QtQuick.Controls
import Cortex 1.0

ScrollBar {
    id: control
    policy: size < 0.999 ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
    interactive: true
    hoverEnabled: true
    implicitWidth: 11
    padding: 2

    contentItem: Rectangle {
        implicitWidth: 7
        radius: 3
        color: control.pressed ? Theme.textMuted : (control.hovered ? Theme.borderStrong : Theme.borderStrong)
        opacity: control.pressed ? 1.0 : (control.hovered ? 0.9 : 0.68)
    }
    background: Rectangle {
        color: Theme.background
        opacity: control.size < 0.999 ? 0.35 : 0
    }
}
