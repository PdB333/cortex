import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cortex 1.0

CortexFlickable {
    contentWidth: width
    contentHeight: column.implicitHeight + 56
    clip: true
    boundsBehavior: Flickable.StopAtBounds

    ColumnLayout {
        id: column
        width: Math.min(parent.width - 64, 820)
        anchors.left: parent.left
        anchors.leftMargin: 32
        anchors.top: parent.top
        anchors.topMargin: 28
        spacing: 0

        Label {
            text: CortexApp.currentTargetIndex >= 0 ? CortexApp.currentTargetName : "No target selected"
            color: Theme.textBright
            font.family: Theme.uiFont
            font.pixelSize: 23
            font.bold: true
        }

        Label {
            Layout.topMargin: 6
            text: CortexApp.currentTargetIndex >= 0 ? CortexApp.currentTargetMeta : "Choose a process from the target picker to begin."
            color: Theme.textMuted
            font.family: Theme.uiFont
            font.pixelSize: 12
        }

        Label {
            visible: CortexApp.lastError.length > 0
            Layout.topMargin: 7
            text: CortexApp.lastError
            color: Theme.error
            font.family: Theme.monoFont
            font.pixelSize: 11
        }

        Rectangle { Layout.fillWidth: true; Layout.topMargin: 22; Layout.bottomMargin: 18; Layout.preferredHeight: 1; color: Theme.border }

        Label {
            text: "TARGET"
            color: Theme.textDisabled
            font.pixelSize: 10
            font.bold: true
            font.letterSpacing: 0.55
        }

        GridLayout {
            Layout.topMargin: 12
            columns: 2
            columnSpacing: 26
            rowSpacing: 10

            Label { text: "Platform"; color: Theme.textMuted; font.pixelSize: 12; Layout.preferredWidth: 130 }
            Label { text: CortexApp.currentPlatform; color: Theme.text; font.pixelSize: 12 }
            Label { text: "Architecture"; color: Theme.textMuted; font.pixelSize: 12 }
            Label { text: CortexApp.currentArchitecture; color: Theme.text; font.pixelSize: 12; font.family: Theme.monoFont }
            Label { text: "Session"; color: Theme.textMuted; font.pixelSize: 12 }
            Label { text: CortexApp.sessionStatus; color: CortexApp.sessionActive ? Theme.success : Theme.text; font.pixelSize: 12 }
            Label { text: "Processes"; color: Theme.textMuted; font.pixelSize: 12 }
            Label { text: String(CortexApp.targetCount); color: Theme.text; font.pixelSize: 12 }
            Label { text: "Attached"; color: Theme.textMuted; font.pixelSize: 12 }
            Label { text: String(CortexApp.attachedTargetCount); color: CortexApp.attachedTargetCount > 0 ? Theme.success : Theme.text; font.pixelSize: 12 }
            Label { text: "Mutation"; color: Theme.textMuted; font.pixelSize: 12 }
            Label { text: CortexApp.mutationPermission ? "Enabled" : "Disabled"; color: CortexApp.mutationPermission ? Theme.mutation : Theme.text; font.pixelSize: 12 }
        }

        Rectangle { Layout.fillWidth: true; Layout.topMargin: 22; Layout.bottomMargin: 18; Layout.preferredHeight: 1; color: Theme.border }

        Label {
            text: "ACTIONS"
            color: Theme.textDisabled
            font.pixelSize: 10
            font.bold: true
            font.letterSpacing: 0.55
        }

        RowLayout {
            Layout.topMargin: 10
            spacing: 7

            Button { text: "Refresh targets"; onClicked: CortexApp.refreshTargets(); font.pixelSize: 12 }
            Button { text: "Detach"; visible: CortexApp.sessionActive; onClicked: CortexApp.detachTarget(); font.pixelSize: 12 }
            Button { text: "Detach all"; visible: CortexApp.attachedTargetCount > 1; onClicked: CortexApp.detachAllTargets(); font.pixelSize: 12 }
            Button {
                text: CortexApp.mutationPermission ? "Disable mutation" : "Enable mutation"
                enabled: CortexApp.sessionActive
                onClicked: CortexApp.mutationPermission = !CortexApp.mutationPermission
                font.pixelSize: 12
            }
        }
    }
}
