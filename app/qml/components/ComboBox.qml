import QtQuick
import QtQuick.Controls as C
import Cortex 1.0

C.ComboBox {
    id: control

    implicitWidth: 120
    implicitHeight: 30

    leftPadding: 9
    rightPadding: 30
    topPadding: 5
    bottomPadding: 5

    font.family: Theme.uiFont
    font.pixelSize: Theme.smallSize

    contentItem: Text {
        leftPadding: 0
        rightPadding: 0
        text: control.displayText
        color: control.enabled ? Theme.textBright : Theme.textDisabled
        font: control.font
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    indicator: Item {
        x: control.width - width - 6
        y: (control.height - height) / 2
        width: 18
        height: 18

        Rectangle {
            x: 4
            y: 7
            width: 6
            height: 1
            rotation: 45
            transformOrigin: Item.Right
            color: control.enabled ? Theme.textMuted : Theme.textDisabled
            antialiasing: true
        }
        Rectangle {
            x: 8
            y: 7
            width: 6
            height: 1
            rotation: -45
            transformOrigin: Item.Left
            color: control.enabled ? Theme.textMuted : Theme.textDisabled
            antialiasing: true
        }
    }

    background: Rectangle {
        color: control.enabled ? Theme.input : Theme.surface
        border.width: 1
        border.color: control.activeFocus || control.popup.visible
                      ? Theme.accent
                      : control.hovered
                        ? Theme.borderStrong
                        : Theme.borderStrong
        radius: Theme.radius
        opacity: control.enabled ? 1.0 : 0.72
    }

    delegate: C.ItemDelegate {
        id: itemDelegate
        required property int index
        required property var modelData

        width: control.width - 2
        height: 30
        highlighted: control.highlightedIndex === index

        contentItem: Text {
            text: control.textRole && modelData && typeof modelData === "object"
                  ? modelData[control.textRole]
                  : modelData
            color: Theme.text
            font: control.font
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }

        background: Rectangle {
            color: itemDelegate.highlighted
                   ? Theme.selection
                   : itemDelegate.hovered
                     ? Theme.hover
                     : Theme.surfaceRaised
        }
    }

    popup: C.Popup {
        y: control.height + 2
        width: control.width
        padding: 1
        closePolicy: C.Popup.CloseOnEscape | C.Popup.CloseOnPressOutsideParent

        contentItem: ListView {
            clip: true
            implicitHeight: Math.min(contentHeight, 240)
            model: control.delegateModel
            currentIndex: control.highlightedIndex
            boundsBehavior: Flickable.StopAtBounds
        }

        background: Rectangle {
            color: Theme.surfaceRaised
            border.width: 1
            border.color: Theme.borderStrong
            radius: Theme.radius
        }
    }
}
