pragma Singleton
import QtQuick

QtObject {
    readonly property color background: "#1e1e1e"
    readonly property color sidebar: "#181818"
    readonly property color panel: "#252526"
    readonly property color panelRaised: "#2a2d2e"
    readonly property color border: "#303031"
    readonly property color borderStrong: "#3f3f46"
    readonly property color text: "#d4d4d4"
    readonly property color textBright: "#f1f1f1"
    readonly property color textMuted: "#969696"
    readonly property color textDisabled: "#666666"
    readonly property color accent: "#3794ff"
    readonly property color accentDark: "#094771"
    readonly property color success: "#89d185"
    readonly property color warning: "#cca700"
    readonly property color mutation: "#ce9178"
    readonly property color error: "#f14c4c"
    readonly property color breakpoint: "#e51400"
    readonly property color selection: "#264f78"
    readonly property color hover: "#2a2d2e"
    readonly property color codeKeyword: "#569cd6"
    readonly property color codeNumber: "#b5cea8"
    readonly property color codeString: "#ce9178"
    readonly property color codeComment: "#6a9955"

    readonly property int radius: 3
    readonly property int sidebarWidth: 218
    readonly property int topBarHeight: 46
    readonly property int bottomBarHeight: 28
    readonly property string uiFont: "Segoe UI"
    readonly property string monoFont: "Cascadia Mono"
}
