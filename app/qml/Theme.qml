pragma Singleton
import QtQuick

QtObject {
    readonly property color background: "#1e1e1e"
    readonly property color sidebar: "#181818"
    readonly property color panel: "#181818"
    readonly property color panelRaised: "#252526"
    readonly property color border: "#2b2b2b"
    readonly property color borderStrong: "#3a3a3a"
    readonly property color text: "#cccccc"
    readonly property color textBright: "#e8e8e8"
    readonly property color textMuted: "#8f8f8f"
    readonly property color textDisabled: "#676767"
    readonly property color accent: "#2f8bd6"
    readonly property color accentDark: "#1f5f8b"
    readonly property color success: "#89d185"
    readonly property color warning: "#cca700"
    readonly property color mutation: "#ce9178"
    readonly property color error: "#f14c4c"
    readonly property color breakpoint: "#e51400"
    readonly property color selection: "#37373d"
    readonly property color hover: "#2a2d2e"
    readonly property color codeKeyword: "#569cd6"
    readonly property color codeNumber: "#b5cea8"
    readonly property color codeString: "#ce9178"
    readonly property color codeComment: "#6a9955"

    readonly property int radius: 2
    readonly property int sidebarWidth: 208
    readonly property int topBarHeight: 36
    readonly property int bottomBarHeight: 22
    readonly property string uiFont: "Segoe UI"
    readonly property string monoFont: "Cascadia Mono"
}
