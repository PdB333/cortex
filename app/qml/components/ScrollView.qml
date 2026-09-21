import QtQuick
import QtQuick.Controls as C
import Cortex 1.0

C.ScrollView {
    id: control
    clip: true

    background: Rectangle {
        color: "transparent"
    }

    C.ScrollBar.vertical: CortexScrollBar {}
    C.ScrollBar.horizontal: CortexScrollBar { orientation: Qt.Horizontal }
}
