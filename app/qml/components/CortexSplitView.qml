import QtQuick
import QtQuick.Controls
import Cortex 1.0

SplitView {
    handle: Rectangle {
        implicitWidth: 5
        implicitHeight: 5
        color: SplitHandle.pressed ? Theme.accentDark : (SplitHandle.hovered ? Theme.borderStrong : Theme.border)
    }
}
