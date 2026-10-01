// Unit tests for the desktop application models, run against a scripted
// runtime transport so they need no target process, pipe or Windows API.

#include "application/actions_model.h"
#include "application/cheat_table.h"
#include "application/hotkeys.h"
#include "application/patches_model.h"
#include "application/prompt_model.h"
#include "application/runtime_events_model.h"
#include "application/snapshots_model.h"
#include "application/symbols_model.h"
#include "application/watches_model.h"
#include "services/runtime_transport.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

using json = nlohmann::json;

struct RecordedCall {
    std::string name;
    json arguments;
};

struct RecordedRoute {
    std::string method;
    std::string path;
    json body;
};

// Stands in for the runtime. `loaded` means the runtime is present in the
// target (TryConnectExisting succeeds); `connected` means Ready().
class FakeTransport final : public cortex::services::RuntimeTransport {
public:
    bool connected = false;
    bool loaded = false;
    bool injectable = true;
    int injections = 0;
    std::vector<RecordedCall> calls;
    std::vector<RecordedRoute> routes;
    std::map<std::string, json> toolResults;   // tool -> route result
    std::function<json(const std::string&, const std::string&, const json&)> routeHandler;

    bool Ready() const override { return connected; }

    bool TryConnectExisting(std::string* error) override {
        if (error) error->clear();
        if (!loaded) {
            if (error) *error = "payload_token_unavailable";
            return false;
        }
        connected = true;
        return true;
    }

    bool EnsureReady(std::string* error) override {
        if (TryConnectExisting(nullptr)) return true;
        if (!injectable) {
            if (error) *error = "payload_injection_failed";
            return false;
        }
        ++injections;
        loaded = connected = true;
        return true;
    }

    bool CallTool(const std::string& name, const json& arguments, json& output,
                  std::string* error) override {
        if (error) error->clear();
        calls.push_back({name, arguments});
        const auto found = toolResults.find(name);
        output = {{"status", 200},
                  {"result", found != toolResults.end() ? found->second : json{{"ok", true}}}};
        return true;
    }

    bool CallRouteExisting(const std::string& method, const std::string& path,
                           const json& body, json& output, std::string* error) override {
        if (error) error->clear();
        if (!connected && !TryConnectExisting(error)) return false;
        routes.push_back({method, path, body});
        output = {{"status", 200},
                  {"result", routeHandler ? routeHandler(method, path, body) : json{{"ok", true}}}};
        return true;
    }

    bool Called(const std::string& name) const {
        for (const auto& call : calls) if (call.name == name) return true;
        return false;
    }
    const RecordedCall* Last(const std::string& name) const {
        for (auto it = calls.rbegin(); it != calls.rend(); ++it)
            if (it->name == name) return &*it;
        return nullptr;
    }
};

}  // namespace

int main() {
    int failures = 0;
    auto check = [&](bool value, const char* message) {
        if (!value) {
            std::cerr << "FAIL: " << message << '\n';
            ++failures;
        }
    };

    using namespace cortex::application;

    // Cheat Engine tables: entries, groups, pointers (last offset listed
    // first), strings and byte arrays; scripts are counted, not imported.
    {
        const std::string xml = R"(<?xml version="1.0" encoding="utf-8"?>
<CheatTable CheatEngineTableVersion="45">
  <CheatEntries>
    <CheatEntry>
      <ID>0</ID>
      <Description>"Player"</Description>
      <Options moHideChildren="1" moActivateChildrenAsWell='1'/>
      <Color>0000FF</Color>
      <GroupHeader>1</GroupHeader>
      <CheatEntries>
        <CheatEntry>
          <ID>1</ID>
          <Description>"Health &amp; armor"</Description>
          <ShowAsHex>1</ShowAsHex>
          <DropDownList DescriptionOnly="1" DisplayValueAsItem="1">0:Dead
64:Full &amp; healthy
</DropDownList>
          <VariableType>4 Bytes</VariableType>
          <Address>"Tutorial-i386.exe"+001FD660</Address>
          <Hotkeys>
            <Hotkey>
              <Action>Set Value</Action>
              <Keys>
                <Key>17</Key>
                <Key>112</Key>
              </Keys>
              <Value>999</Value>
              <ID>0</ID>
            </Hotkey>
            <Hotkey>
              <Action>Toggle Activation</Action>
              <Keys>
                <Key>113</Key>
              </Keys>
              <ID>1</ID>
            </Hotkey>
          </Hotkeys>
          <Offsets>
            <Offset>18</Offset>
            <Offset>0</Offset>
            <Offset>14</Offset>
            <Offset>C</Offset>
          </Offsets>
        </CheatEntry>
        <CheatEntry>
          <ID>2</ID>
          <Description>"Name"</Description>
          <VariableType>String</VariableType>
          <Length>12</Length>
          <Unicode>1</Unicode>
          <Address>00401000</Address>
        </CheatEntry>
      </CheatEntries>
    </CheatEntry>
    <CheatEntry>
      <ID>3</ID>
      <Description>"Infinite ammo"</Description>
      <VariableType>Auto Assembler Script</VariableType>
      <AssemblerScript><![CDATA[[ENABLE]
nop 5
[DISABLE]]]></AssemblerScript>
    </CheatEntry>
    <CheatEntry>
      <ID>4</ID>
      <Description>"Code"</Description>
      <VariableType>Array of byte</VariableType>
      <Length>4</Length>
      <ShowAsSigned>0</ShowAsSigned>
      <Address>game.exe+10</Address>
    </CheatEntry>
  </CheatEntries>
  <UserdefinedSymbols>
    <SymbolEntry>
      <Name>player</Name>
      <Address>"Tutorial-i386.exe"+1FD660</Address>
    </SymbolEntry>
  </UserdefinedSymbols>
  <LuaScript>print("hi")</LuaScript>
</CheatTable>)";
        CheatTable table;
        std::string error;
        check(ParseCheatTable(xml, table, &error), "a Cheat Engine table parses");
        check(table.entries.size() == 5 && table.scripts == 1, "entries are read, scripts included");
        {
            const auto script = std::find_if(table.entries.begin(), table.entries.end(),
                                             [](const CheatTableEntry& item) { return item.script; });
            check(script != table.entries.end(), "the Auto Assembler entry is kept");
            if (script != table.entries.end()) {
                check(script->description == "Infinite ammo", "script entries keep their description");
                check(script->assemblerScript.find("[ENABLE]") != std::string::npos &&
                          script->assemblerScript.find("nop 5") != std::string::npos,
                      "the script source is kept verbatim");
                CheatTable again2;
                std::string error2;
                check(ParseCheatTable(WriteCheatTable(table), again2, &error2), "a table with a script rewrites");
                const auto back = std::find_if(again2.entries.begin(), again2.entries.end(),
                                               [](const CheatTableEntry& item) { return item.script; });
                check(back != again2.entries.end() && back->assemblerScript == script->assemblerScript,
                      "scripts survive a round trip");
            }
        }
        if (table.entries.size() == 5) {
            const auto& group = table.entries[0];
            const auto& health = table.entries[1];
            check(group.groupHeader && group.description == "Player" && group.depth == 0, "group header");
            check(health.depth == 1 && health.description == "Health & armor" && health.showAsHex,
                  "entities, nesting and hex display");
            check(health.offsets == std::vector<uint32_t>({0xC, 0x14, 0x0, 0x18}), "offsets are applied base first");
            check(table.entries[2].variableType == "String" && table.entries[2].unicode &&
                  table.entries[2].length == 12, "UTF-16 strings keep their length");
            check(table.entries[4].depth == 0 && !table.entries[4].showAsSigned, "entries after a group");
            check(group.collapsed && group.color == 0xFF0000, "collapsed groups and BGR colors");
            check(health.color == -1, "entries without a color keep the default");
            check(health.dropDown.size() == 2 && health.dropDown[1].value == "64" &&
                  health.dropDown[1].label == "Full & healthy" && health.dropDownDescriptionOnly,
                  "dropdown lists and their attributes");
            check(health.hotkeys.size() == 2 && health.hotkeys[0].action == "Set Value" &&
                  health.hotkeys[0].keys == std::vector<unsigned>({17, 112}) && health.hotkeys[0].value == "999" &&
                  health.hotkeys[1].action == "Toggle Activation", "entry hotkeys");
        }
        check(table.luaScript == "print(\"hi\")", "the table's Lua script is kept");
        check(table.userSymbols.size() == 1 && table.userSymbols[0].first == "player" &&
                  table.userSymbols[0].second == "\"Tutorial-i386.exe\"+1FD660",
              "user-defined symbols are read");

        std::string module;
        uint64_t offset = 0;
        check(ParseCheatAddress("\"Tutorial-i386.exe\"+001FD660", module, offset) &&
              module == "Tutorial-i386.exe" && offset == 0x1FD660, "quoted module addresses");
        check(ParseCheatAddress("game.exe+10", module, offset) && module == "game.exe" && offset == 0x10,
              "plain module addresses");
        check(ParseCheatAddress("7FF6A1B20010", module, offset) && module.empty() && offset == 0x7FF6A1B20010ull,
              "absolute addresses");
        check(!ParseCheatAddress("player_base", module, offset), "symbols are not addresses");
        check(!ParseCheatAddress("[game.exe+10]+8", module, offset), "pointer expressions are not module+offset");
        check(!ParseCheatAddress("game.exe+10+8", module, offset), "sums are expressions");
        check(FormatCheatAddress("game.exe", 0x10) == "\"game.exe\"+00000010", "module addresses format like CE");

        CheatTable again;
        check(ParseCheatTable(WriteCheatTable(table), again, &error) && again.entries.size() == 5,
              "a written table reads back");
        if (again.entries.size() == 5) {
            check(again.entries[1].offsets == table.entries[1].offsets && again.entries[1].depth == 1,
                  "pointers and groups survive a round trip");
            check(again.entries[1].description == "Health & armor", "descriptions are escaped");
            check(again.entries[0].collapsed && again.entries[0].color == 0xFF0000, "colors survive a round trip");
            check(again.userSymbols == table.userSymbols && again.luaScript == table.luaScript,
                  "symbols and the Lua script survive a round trip");
            check(again.entries[1].hotkeys.size() == 2 && again.entries[1].hotkeys[0].keys == table.entries[1].hotkeys[0].keys &&
                  again.entries[1].hotkeys[0].value == "999", "hotkeys survive a round trip");
            check(again.entries[1].dropDown.size() == 2 && again.entries[1].dropDown[1].label == "Full & healthy",
                  "dropdown lists survive a round trip");
        }
        // A user-defined type is written as Cheat Engine's "Custom" with the
        // type's name beside it, and comes back naming the same type.
        {
            CheatTable custom;
            CheatTableEntry entry;
            entry.description = "Health x10";
            entry.variableType = "Custom";
            entry.customType = "health x10";
            entry.address = "game.exe+20";
            custom.entries.push_back(entry);
            const std::string written = WriteCheatTable(custom);
            check(written.find("<CustomType>health x10</CustomType>") != std::string::npos,
                  "custom types are written beside the variable type");
            CheatTable read;
            std::string customError;
            check(ParseCheatTable(written, read, &customError) && read.entries.size() == 1 &&
                      read.unsupported == 0,
                  "a table using a custom type reads back");
            if (read.entries.size() == 1)
                check(read.entries[0].variableType == "Custom" && read.entries[0].customType == "health x10",
                      "custom types survive a round trip");
        }

        check(!ParseCheatTable("<Other/>", again, &error), "other XML is refused");
        check(!ParseCheatTable("<CheatTable><CheatEntries>", again, &error), "truncated XML is refused");
    }

    // Global hotkey chords round-trip and reject what RegisterHotKey cannot take.
    {
        HotkeyChord chord;
        check(ParseHotkeyChord("Ctrl+Alt+F5", chord) && chord.modifiers == 3 && chord.virtualKey == 0x74,
              "Ctrl+Alt+F5 parses");
        check(FormatHotkeyChord(chord) == "Ctrl+Alt+F5", "Ctrl+Alt+F5 formats back");
        check(ParseHotkeyChord("shift + keypad1", chord) && chord.modifiers == 4 && chord.virtualKey == 0x61,
              "chords ignore case and spaces");
        check(FormatHotkeyChord(chord) == "Shift+Keypad1", "keypad keys keep their ImGui name");
        check(ParseHotkeyChord("Win+Ctrl+Z", chord) && FormatHotkeyChord(chord) == "Ctrl+Win+Z",
              "modifiers format in a fixed order");
        check(!ParseHotkeyChord("Ctrl+Alt", chord), "a chord needs a key");
        check(!ParseHotkeyChord("Ctrl+A+B", chord), "a chord has one key");
        check(!ParseHotkeyChord("Ctrl+Nope", chord), "unknown keys are rejected");
        check(!ParseHotkeyChord("", chord), "empty chords are rejected");
        bool unique = true;
        for (size_t i = 0; i < HotkeyActions().size(); ++i)
            for (size_t j = i + 1; j < HotkeyActions().size(); ++j)
                unique &= std::string(HotkeyActions()[i].id) != HotkeyActions()[j].id;
        check(unique, "hotkey action ids are unique");

        check(HotkeyChordFromKeys({17, 112}, chord) && FormatHotkeyChord(chord) == "Ctrl+F1",
              "cheat table keys become a chord");
        check(HotkeyChordFromKeys({0xA1, 0xA4, 0x41}, chord) && FormatHotkeyChord(chord) == "Alt+Shift+A",
              "left/right modifiers count as modifiers");
        check(!HotkeyChordFromKeys({65, 66}, chord), "two plain keys do not fit RegisterHotKey");
        check(!HotkeyChordFromKeys({17}, chord), "a modifier alone is not a hotkey");
        check(ParseHotkeyChord("Ctrl+Shift+F9", chord) &&
              HotkeyChordKeys(chord) == std::vector<unsigned>({0x11, 0x10, 0x78}), "a chord becomes table keys");

        HotkeyRegistrar registrar;
        const auto failures = registrar.Apply(nullptr, {{"pause_target", "Ctrl+Bogus"}});
        check(failures.size() == 1 && failures.front() == "pause_target", "invalid bindings are reported");
        void* window = nullptr;
#if defined(_WIN32)
        window = CreateWindowExW(0, L"STATIC", L"hotkeys", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
#endif
        const auto refused = registrar.Apply(window, {{"entry 1 0", "Ctrl+Alt+Shift+F21"},
                                                      {"entry 2 0", "ctrl + alt + shift + f21"},
                                                      {"scan_next", "Ctrl+Alt+Shift+F22"}});
        check(refused.empty(), "free chords register");
        bool shared = false;
        for (int id = 0x4C00; id < 0x4C10; ++id) {
            const auto actions = registrar.ActionsFor(id);
            shared |= actions.size() == 2 && actions[0] == "entry 1 0" && actions[1] == "entry 2 0";
        }
        check(shared, "actions on the same chord share one registration");
        registrar.Clear();
#if defined(_WIN32)
        if (window) DestroyWindow(static_cast<HWND>(window));
#endif
    }

    // Mutation gate: no runtime present and writes not allowed -> nothing is
    // injected and no tool is called.
    {
        FakeTransport transport;
        WatchesModel watches(transport);
        std::string error;
        check(!watches.AddWatch("0x1000", "i32", "hp", false, &error),
              "AddWatch without write permission fails");
        check(error == "mutation_permission_required", "write gate reports mutation_permission_required");
        check(transport.injections == 0, "write gate never injects the runtime");
        check(transport.calls.empty(), "write gate sends no tool call");
    }

    // Read-only refresh never injects: it only connects to a loaded runtime.
    {
        FakeTransport transport;
        WatchesModel watches(transport);
        std::string error;
        check(!watches.Refresh(&error), "Refresh without a runtime fails");
        check(error == "payload_token_unavailable", "Refresh reports the connection error");
        check(transport.injections == 0, "Refresh never injects");
    }

    // Allowed write: runtime is loaded on demand and the call carries the
    // explicit mutation_permission flag; the model then refreshes and parses.
    {
        FakeTransport transport;
        transport.toolResults["watch_list"] = {
            {"watches", json::array({{{"id", 7}, {"address", "0x1000"}, {"type", "i32"},
                                      {"label", "hp"}, {"value", "100"}, {"has_value", true}}})}};
        transport.toolResults["freeze_list"] = {{"freezes", json::array()}};
        WatchesModel watches(transport);
        std::string error;
        check(watches.AddWatch("  0x1000 ", "I32", " hp ", true, &error), "AddWatch with permission succeeds");
        check(transport.injections == 1, "allowed write loads the runtime once");
        const RecordedCall* add = transport.Last("watch_add");
        check(add != nullptr, "watch_add was called");
        if (add) {
            check(add->arguments.value("mutation_permission", false), "mutating call sets mutation_permission");
            check(add->arguments.value("address", "") == "0x1000", "address is trimmed");
            check(add->arguments.value("type", "") == "i32", "type is lower-cased");
            check(add->arguments.value("label", "") == "hp", "label is trimmed");
        }
        check(watches.Watches().size() == 1 && watches.Watches()[0].id == 7 &&
                  watches.Watches()[0].value == "100",
              "watch list is parsed after the write");
        const RecordedCall* list = transport.Last("watch_list");
        check(list && !list->arguments.contains("mutation_permission"),
              "read-only calls do not carry mutation_permission");
    }

    // Freeze values are validated before anything is sent.
    {
        FakeTransport transport;
        transport.loaded = true;
        WatchesModel watches(transport);
        std::string error;
        check(!watches.AddFreeze("0x10", "float", "not-a-number", "", 0, true, &error),
              "invalid freeze value is rejected");
        check(error == "invalid_freeze_value", "invalid freeze reports invalid_freeze_value");
        check(!transport.Called("freeze_add"), "invalid freeze sends nothing");
        check(!watches.AddFreeze("0x10", "i32", "5", "", -1, true, &error),
              "negative TTL is rejected");
    }

    // Patches: input validation and gate.
    {
        FakeTransport transport;
        transport.loaded = true;
        PatchesModel patches(transport);
        std::string error;
        check(!patches.ApplyBytes("", "90", "", true, &error), "patch requires an address");
        check(error == "patch_address_and_bytes_required", "patch reports missing input");
        check(!patches.ApplyBytes("0x10", "90", "", false, &error), "patch requires write permission");
        check(error == "mutation_permission_required", "patch gate error");
        check(patches.ApplyBytes("0x10", "90 90", "nop", true, &error), "allowed patch succeeds");
        const RecordedCall* write = transport.Last("patch_write");
        check(write && write->arguments.value("bytes", "") == "90 90", "patch bytes are forwarded");
        check(transport.injections == 0, "an already loaded runtime is reused");
    }

    // Symbols are read-only: they never load the runtime even when writes
    // are allowed elsewhere.
    {
        FakeTransport transport;
        SymbolsModel symbols(transport);
        std::string error;
        check(!symbols.Resolve("0x1000", &error), "symbol resolve without runtime fails");
        check(transport.injections == 0, "symbols never inject");
        transport.loaded = true;
        transport.toolResults["symbols_resolve"] = {{"ok", true}, {"symbol", "main"}};
        check(symbols.Resolve("0x1000", &error), "symbol resolve with runtime succeeds");
        const RecordedCall* call = transport.Last("symbols_resolve");
        check(call && call->arguments["_query"].value("address", "") == "0x1000",
              "resolve passes the address as a query parameter");
    }

    // Prompt: private routes, connection errors surface, answers are trimmed.
    {
        FakeTransport transport;
        PromptModel prompt(transport);
        std::string error;
        check(!prompt.Refresh(&error), "prompt refresh without runtime fails");
        check(!error.empty(), "prompt refresh reports why it could not connect");

        transport.loaded = true;
        bool answered = false;
        transport.routeHandler = [&](const std::string& method, const std::string& path, const json& body) -> json {
            if (method == "GET" && path == "/prompt/active") {
                if (answered) return {{"ok", true}, {"prompt", nullptr}};
                return {{"ok", true},
                        {"prompt", {{"id", 4}, {"kind", "value_change"}, {"label", "HP"},
                                    {"current_value", "10"}, {"target_value", "20"}}}};
            }
            if (method == "POST" && path == "/prompt/4/answer") {
                answered = body.value("value", "") == "done";
                return {{"ok", true}};
            }
            return {{"ok", false}};
        };
        check(prompt.Refresh(&error) && prompt.Active() && prompt.Id() == 4, "active prompt is observed");
        check(prompt.Answer("  done  ", &error), "prompt answer succeeds");
        check(answered, "answer is trimmed and posted to the prompt route");
        check(!prompt.Active(), "prompt is cleared after it is answered");
    }

    // Actions rollback is a mutation; clearing respects the gate too.
    {
        FakeTransport transport;
        transport.loaded = true;
        ActionsModel actions(transport);
        std::string error;
        check(!actions.RollbackAll(false, &error), "rollback requires write permission");
        check(!actions.Clear(false, &error), "clear requires write permission");
        check(!transport.Called("actions_rollback") && !transport.Called("actions_clear"),
              "denied rollback and clear send nothing");
    }

    if (failures) {
        std::cerr << failures << " application model check(s) failed\n";
        return 1;
    }
    std::cout << "PASS: application models (runtime gate, arguments, parsing, prompt routes)\n";
    return 0;
}
