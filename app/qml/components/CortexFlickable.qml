import QtQuick
import QtQuick.Controls
import Cortex 1.0

Flickable {
    id: control
    clip: true
    boundsBehavior: Flickable.StopAtBounds

    ScrollBar.vertical: CortexScrollBar {}

    WheelHandler {
        acceptedDevices: PointerDevice.Mouse
        onWheel: function(event) {
            const maxY = Math.max(0, control.contentHeight - control.height)
            if (maxY <= 0 || Math.abs(event.angleDelta.y) < 1) {
                event.accepted = false
                return
            }
            const step = Math.max(60, control.height * 0.10) * CortexSettings.scrollSpeed
            const notches = event.angleDelta.y / 120.0
            control.contentY = Math.max(0, Math.min(maxY, control.contentY - notches * step))
            event.accepted = true
        }
    }
}
