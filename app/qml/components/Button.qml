import QtQuick
import QtQuick.Templates as T
import Cortex 1.0

T.Button {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(28, implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    leftPadding: 9
    rightPadding: 9
    topPadding: 5
    bottomPadding: 5

    font.family: Theme.uiFont
    font.pixelSize: Theme.smallSize

    contentItem: Text {
        text: control.text
        font: control.font
        color: control.enabled ? Theme.text : Theme.textDisabled
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        implicitWidth: 72
        implicitHeight: 28
        radius: 2
        color: !control.enabled
               ? Theme.surface
               : control.down
                 ? Theme.selection
                 : control.hovered
                   ? Theme.hover
                   : Theme.surfaceRaised
        border.width: 1
        border.color: control.activeFocus
                      ? Theme.accent
                      : control.hovered
                        ? Theme.borderStrong
                        : Theme.border
    }
}
