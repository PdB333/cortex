pragma Singleton
import QtQuick

QtObject {
    readonly property color background: "#1e1e1e"
    readonly property color titleBar: "#181818"
    readonly property color sidebar: "#181818"
    readonly property color panel: "#181818"
    readonly property color surface: "#202020"
    readonly property color surfaceRaised: "#252526"
    readonly property color input: "#1f1f1f"
    readonly property color border: "#303030"
    readonly property color borderStrong: "#444444"
    readonly property color text: "#cccccc"
    readonly property color textBright: "#f0f0f0"
    readonly property color textMuted: "#9a9a9a"
    readonly property color textDisabled: "#666666"
    readonly property color accent: "#3794ff"
    readonly property color accentDark: "#094771"
    readonly property color success: "#89d185"
    readonly property color warning: "#cca700"
    readonly property color mutation: "#d7ba7d"
    readonly property color error: "#f14c4c"
    readonly property color breakpoint: "#e51400"
    readonly property color selection: "#37373d"
    readonly property color hover: "#2a2d2e"
    readonly property color codeKeyword: "#569cd6"
    readonly property color codeNumber: "#b5cea8"
    readonly property color codeString: "#ce9178"
    readonly property color codeComment: "#6a9955"

    readonly property int radius: 3
    readonly property int sidebarWidth: 236
    readonly property int topBarHeight: 44
    readonly property int workspaceBarHeight: 38
    readonly property int bottomBarHeight: 24
    readonly property int navRowHeight: 32
    readonly property int uiSize: 13
    readonly property int smallSize: 11
    readonly property int tinySize: 10
    readonly property string uiFont: "Segoe UI"
    readonly property string monoFont: "Cascadia Mono"
}
