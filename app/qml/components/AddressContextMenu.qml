import QtQuick
import QtQuick.Controls
import Cortex 1.0

Menu {
    id: menu

    property string address: ""
    property string label: ""
    property string valueType: "i32"
    property bool allowSave: true
    property bool allowRemove: false
    signal removeRequested()

    function normalizedType() {
        const t = String(valueType || "i32").toLowerCase()
        if (t === "f32") return "float"
        if (t === "f64") return "double"
        const supported = ["i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "float", "double"]
        return supported.indexOf(t) >= 0 ? t : "i32"
    }

    function typeSize() {
        const t = normalizedType()
        if (t === "i8" || t === "u8") return 1
        if (t === "i16" || t === "u16") return 2
        if (t === "i64" || t === "u64" || t === "double") return 8
        return 4
    }

    function displayLabel() {
        return label.length > 0 ? label : ("Address " + address)
    }

    function saveToAddresses() {
        if (!CortexApp.mutationPermission || address.length === 0) return
        if (CortexFeatures.setProjectAddress(displayLabel(), address, normalizedType(), "Added from context menu"))
            CortexApp.selectSection("Addresses")
    }

    implicitWidth: 268
    padding: 4
    background: Rectangle {
        color: Theme.surfaceRaised
        border.color: Theme.borderStrong
        border.width: 1
        radius: Theme.radius
    }

    MenuItem {
        text: "Browse memory"
        enabled: menu.address.length > 0
        onTriggered: CortexApp.openAddress("Memory", menu.address)
        contentItem: Text { text: parent.text; color: parent.enabled ? Theme.text : Theme.textDisabled; font.pixelSize: 11; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { color: parent.highlighted ? Theme.hover : "transparent" }
    }
    MenuItem {
        text: "Disassemble"
        enabled: menu.address.length > 0
        onTriggered: CortexApp.openAddress("Disassembly", menu.address)
        contentItem: Text { text: parent.text; color: parent.enabled ? Theme.text : Theme.textDisabled; font.pixelSize: 11; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { color: parent.highlighted ? Theme.hover : "transparent" }
    }
    MenuItem {
        text: "Open in RE"
        enabled: menu.address.length > 0
        onTriggered: CortexApp.openAddress("RE", menu.address)
        contentItem: Text { text: parent.text; color: parent.enabled ? Theme.text : Theme.textDisabled; font.pixelSize: 11; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { color: parent.highlighted ? Theme.hover : "transparent" }
    }

    MenuSeparator { contentItem: Rectangle { implicitHeight: 1; color: Theme.border } }

    MenuItem {
        visible: menu.allowSave
        text: "Add to Addresses"
        enabled: CortexPayload.ready && CortexApp.mutationPermission && menu.address.length > 0
        onTriggered: menu.saveToAddresses()
        contentItem: Text { text: parent.text; color: parent.enabled ? Theme.text : Theme.textDisabled; font.pixelSize: 11; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { color: parent.highlighted ? Theme.hover : "transparent" }
    }
    MenuItem {
        text: "Add software breakpoint    Ctrl+B"
        enabled: CortexPayload.ready && CortexApp.mutationPermission && menu.address.length > 0
        onTriggered: CortexDebugger.addBreakpoint(menu.address, "software", CortexSettings.breakpointDefaultAction, true, 0)
        contentItem: Text { text: parent.text; color: parent.enabled ? Theme.text : Theme.textDisabled; font.pixelSize: 11; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { color: parent.highlighted ? Theme.hover : "transparent" }
    }
    MenuItem {
        text: "Find what writes"
        enabled: CortexPayload.ready && CortexApp.mutationPermission && menu.address.length > 0
        onTriggered: {
            CortexApp.openAddress("RE", menu.address)
            CortexRe.findLastWriter(menu.address, menu.typeSize(), 10000)
        }
        contentItem: Text { text: parent.text; color: parent.enabled ? Theme.text : Theme.textDisabled; font.pixelSize: 11; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { color: parent.highlighted ? Theme.hover : "transparent" }
    }
    MenuItem {
        text: "Find what accesses (page watch)"
        enabled: CortexPayload.ready && CortexApp.mutationPermission && menu.address.length > 0
        onTriggered: {
            if (CortexFeatures.addPageAccessWatch(menu.address, menu.typeSize(), menu.displayLabel()))
                CortexApp.selectSection("Hooks")
        }
        contentItem: Text { text: parent.text; color: parent.enabled ? Theme.text : Theme.textDisabled; font.pixelSize: 11; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { color: parent.highlighted ? Theme.hover : "transparent" }
    }

    MenuSeparator { contentItem: Rectangle { implicitHeight: 1; color: Theme.border } }

    MenuItem {
        text: "Pointer scan"
        enabled: CortexPayload.ready && menu.address.length > 0
        onTriggered: CortexApp.openAddress("Pointers", menu.address)
        contentItem: Text { text: parent.text; color: parent.enabled ? Theme.text : Theme.textDisabled; font.pixelSize: 11; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { color: parent.highlighted ? Theme.hover : "transparent" }
    }
    MenuItem {
        text: "Open in Structures"
        enabled: CortexPayload.ready && menu.address.length > 0
        onTriggered: CortexApp.openAddress("Structures", menu.address)
        contentItem: Text { text: parent.text; color: parent.enabled ? Theme.text : Theme.textDisabled; font.pixelSize: 11; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { color: parent.highlighted ? Theme.hover : "transparent" }
    }
    MenuItem {
        text: "Track object in RE"
        enabled: CortexPayload.ready && CortexApp.mutationPermission && menu.address.length > 0
        onTriggered: {
            CortexApp.openAddress("RE", menu.address)
            CortexRe.trackObject(menu.displayLabel(), menu.address, "", 256, true, "")
        }
        contentItem: Text { text: parent.text; color: parent.enabled ? Theme.text : Theme.textDisabled; font.pixelSize: 11; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { color: parent.highlighted ? Theme.hover : "transparent" }
    }
    MenuItem {
        text: "Detect C++ subobjects"
        enabled: CortexPayload.ready && menu.address.length > 0
        onTriggered: {
            CortexApp.openAddress("RE", menu.address)
            CortexRe.detectSubobjects(menu.address, 256)
        }
        contentItem: Text { text: parent.text; color: parent.enabled ? Theme.text : Theme.textDisabled; font.pixelSize: 11; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { color: parent.highlighted ? Theme.hover : "transparent" }
    }
    MenuItem {
        text: "Snapshot 64 bytes"
        enabled: CortexPayload.ready && menu.address.length > 0
        onTriggered: {
            const ranges = JSON.stringify([{"address": menu.address, "size": 64}])
            if (CortexFeatures.createSnapshot(ranges, menu.displayLabel())) CortexApp.selectSection("Snapshots")
        }
        contentItem: Text { text: parent.text; color: parent.enabled ? Theme.text : Theme.textDisabled; font.pixelSize: 11; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { color: parent.highlighted ? Theme.hover : "transparent" }
    }
    MenuItem {
        text: "Copy address"
        enabled: menu.address.length > 0
        onTriggered: CortexApp.copyText(menu.address)
        contentItem: Text { text: parent.text; color: parent.enabled ? Theme.text : Theme.textDisabled; font.pixelSize: 11; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { color: parent.highlighted ? Theme.hover : "transparent" }
    }
    MenuItem {
        text: "Copy module+offset"
        enabled: CortexPayload.ready && menu.address.length > 0
        onTriggered: {
            if (CortexFeatures.resolveSymbol(menu.address) && CortexFeatures.symbolResult.module && CortexFeatures.symbolResult.rva)
                CortexApp.copyText(String(CortexFeatures.symbolResult.module) + "+" + String(CortexFeatures.symbolResult.rva))
            else
                CortexApp.copyText(menu.address)
        }
        contentItem: Text { text: parent.text; color: parent.enabled ? Theme.text : Theme.textDisabled; font.pixelSize: 11; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { color: parent.highlighted ? Theme.hover : "transparent" }
    }

    MenuSeparator {
        visible: menu.allowRemove
        contentItem: Rectangle { implicitHeight: parent.visible ? 1 : 0; color: Theme.border }
    }
    MenuItem {
        visible: menu.allowRemove
        text: "Remove    Delete"
        enabled: CortexApp.mutationPermission
        onTriggered: menu.removeRequested()
        contentItem: Text { text: parent.text; color: parent.enabled ? Theme.mutation : Theme.textDisabled; font.pixelSize: 11; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { color: parent.highlighted ? Theme.hover : "transparent" }
    }
}