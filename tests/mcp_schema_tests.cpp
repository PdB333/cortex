#include "../core/api/mcp_contract.h"

#include <iostream>
#include <string>

using json = nlohmann::json;

int main() {
    int failures = 0;
    auto check = [&](bool condition, const char* message) {
        if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
    };

    const auto rendered = api::mcp_contract::RenderPath(
        "/objects/{name}",
        {{"_path", {{"name", "player/main #1"}}},
         {"_query", {{"filter", "hp&armor=1"}, {"limit", 25}}}});
    check(static_cast<bool>(rendered), "valid path renders");
    check(rendered.path == "/objects/player%2Fmain%20%231?filter=hp%26armor%3D1&limit=25",
          "URI components are percent encoded");

    const auto missing = api::mcp_contract::RenderPath("/objects/{id}", json::object());
    check(!missing && missing.error == "missing_path_parameters", "missing path values are rejected");

    const auto tooLong = api::mcp_contract::RenderPath(
        "/objects", {{"_query", {{"q", std::string(100, 'a')}}}}, 32);
    check(!tooLong && tooLong.error == "rendered_path_too_long", "rendered URI is bounded");

    const auto booleanSchema = api::mcp_contract::SchemaForProperty(
        "pause_process", "optional bool, default false");
    check(booleanSchema.value("type", std::string()) == "boolean", "boolean property is typed");

    const auto integerSchema = api::mcp_contract::SchemaForProperty(
        "limit", "optional, default 100, max 1000");
    check(integerSchema.value("type", std::string()) == "integer", "integer property is typed");

    const auto addressSchema = api::mcp_contract::SchemaForProperty("address", "required address");
    check(addressSchema.contains("oneOf") && addressSchema["oneOf"].size() == 2,
          "address accepts integer or string");

    const auto arraySchema = api::mcp_contract::SchemaForProperty("items", "array of values");
    check(arraySchema.value("type", std::string()) == "array", "array property is typed");

    check(api::mcp_contract::IsRequiredSpec("required: value"), "required legacy property detected");
    check(!api::mcp_contract::IsRequiredSpec("optional: value"), "optional legacy property detected");

    const auto traceStopRisk = api::mcp_contract::ClassifyTool("trace_stop", "POST", "/trace/{id}/stop");
    check(traceStopRisk == api::mcp_contract::ToolRisk::Control,
          "trace_stop is classified as a control operation");
    check(api::mcp_contract::RequiresMutationPermission(traceStopRisk),
          "trace_stop requires explicit mutation permission");

    const auto debugPauseRisk = api::mcp_contract::ClassifyTool("debug_pause", "POST", "/debug/pause");
    check(debugPauseRisk == api::mcp_contract::ToolRisk::Control,
          "debug_pause is classified as a control operation");
    check(api::mcp_contract::RequiresMutationPermission(debugPauseRisk),
          "debug_pause requires explicit mutation permission");
    const auto debugStepOverRisk = api::mcp_contract::ClassifyTool("debug_step_over", "POST", "/debug/step_over");
    check(debugStepOverRisk == api::mcp_contract::ToolRisk::Control,
          "debug_step_over is classified as a control operation");
    check(api::mcp_contract::RequiresMutationPermission(debugStepOverRisk),
          "debug_step_over requires explicit mutation permission");
    const auto traceDeleteRisk = api::mcp_contract::ClassifyTool("trace_delete", "DELETE", "/trace/{id}");
    check(traceDeleteRisk == api::mcp_contract::ToolRisk::Control,
          "trace_delete is classified as a control operation");
    check(api::mcp_contract::RequiresMutationPermission(traceDeleteRisk),
          "trace_delete requires explicit mutation permission");
    const auto snapshotDeleteRisk = api::mcp_contract::ClassifyTool("snapshot_delete", "DELETE", "/snapshot/{id}");
    check(snapshotDeleteRisk == api::mcp_contract::ToolRisk::Control,
          "snapshot_delete is classified as a control operation");
    check(api::mcp_contract::RequiresMutationPermission(snapshotDeleteRisk),
          "snapshot_delete requires explicit mutation permission");
    const auto pointerMapIntersectRisk = api::mcp_contract::ClassifyTool("pointermap_intersect", "POST", "/pointermap/intersect");
    check(pointerMapIntersectRisk == api::mcp_contract::ToolRisk::Analyze,
          "pointermap_intersect stays available without mutation permission");
    const auto pointerMapDeleteRisk = api::mcp_contract::ClassifyTool("pointermap_delete", "DELETE", "/pointermap/{name}");
    check(pointerMapDeleteRisk == api::mcp_contract::ToolRisk::Control,
          "pointermap_delete is a control operation");
    const auto structInferRisk = api::mcp_contract::ClassifyTool(
        "struct_infer", "POST", "/struct/infer");
    check(structInferRisk == api::mcp_contract::ToolRisk::Analyze,
          "struct_infer is observational unless define=true is requested");
    check(!api::mcp_contract::RequiresMutationPermission(structInferRisk),
          "plain struct inference does not require mutation permission");
    const auto gameThreadRisk = api::mcp_contract::ClassifyTool(
        "call_on_game_thread", "POST", "/call/game-thread");
    check(gameThreadRisk == api::mcp_contract::ToolRisk::NativeCall,
          "game-thread native calls are classified as native-call risk");
    check(api::mcp_contract::RequiresMutationPermission(gameThreadRisk),
          "game-thread native calls require explicit mutation permission");
    const auto callStatusRisk = api::mcp_contract::ClassifyTool(
        "call_game_thread_status", "GET", "/call/game-thread/status");
    check(callStatusRisk == api::mcp_contract::ToolRisk::Observe,
          "game-thread status remains observational");
    const auto processGlobalSchema = api::mcp_contract::SchemaForProperty(
        "process_global", "optional bool");
    check(processGlobalSchema.value("type", std::string()) == "boolean",
          "process_global is a boolean schema field");
    const json typedRequired = {{"type","integer"},{"required",true},{"minimum",1}};
    const auto typedRequiredSchema = api::mcp_contract::SchemaForProperty("thread_id", typedRequired);
    check(typedRequiredSchema.value("type", std::string()) == "integer" &&
          !typedRequiredSchema.contains("required"),
          "typed manifest required marker is consumed instead of leaking into JSON Schema");
    check(api::mcp_contract::IsRequiredSpec(typedRequired),
          "typed manifest required marker contributes to the parent required array");
    const auto reWriterRisk = api::mcp_contract::ClassifyTool(
        "re_find_last_writer", "POST", "/re/last-writer");
    check(reWriterRisk == api::mcp_contract::ToolRisk::Control,
          "RE last-writer instrumentation is classified as control");
    check(api::mcp_contract::RequiresMutationPermission(reWriterRisk),
          "RE last-writer requires explicit mutation permission");
    const auto reCheckpointRollbackRisk = api::mcp_contract::ClassifyTool(
        "re_checkpoint_rollback", "POST", "/re/checkpoint/{id}/rollback");
    check(reCheckpointRollbackRisk == api::mcp_contract::ToolRisk::Control,
          "RE checkpoint rollback is classified as control");
    check(api::mcp_contract::RequiresMutationPermission(reCheckpointRollbackRisk),
          "RE checkpoint rollback requires explicit mutation permission");
    const auto reCompareRisk = api::mcp_contract::ClassifyTool(
        "re_object_compare", "POST", "/re/object/compare");
    check(reCompareRisk == api::mcp_contract::ToolRisk::Analyze,
          "RE object comparison remains an analysis operation");
    check(!api::mcp_contract::RequiresMutationPermission(reCompareRisk),
          "RE object comparison does not require mutation permission");
    const auto reSubobjectRisk = api::mcp_contract::ClassifyTool(
        "re_cpp_subobjects", "POST", "/re/cpp/subobjects");
    check(reSubobjectRisk == api::mcp_contract::ToolRisk::Analyze,
          "C++ subobject detection remains an analysis operation");
    const auto allocationSnapshotRisk = api::mcp_contract::ClassifyTool(
        "watch_allocations_events_snapshot", "GET", "/watch/allocations/events_snapshot");
    check(allocationSnapshotRisk == api::mcp_contract::ToolRisk::Observe,
          "allocation snapshot remains observational");
    check(!api::mcp_contract::RequiresMutationPermission(allocationSnapshotRisk),
          "allocation snapshot does not require mutation permission");
    const auto pageSnapshotRisk = api::mcp_contract::ClassifyTool(
        "watch_page_access_events_snapshot", "GET", "/watch/page_access/events_snapshot");
    check(pageSnapshotRisk == api::mcp_contract::ToolRisk::Observe,
          "page-access snapshot remains observational");
    check(!api::mcp_contract::RequiresMutationPermission(pageSnapshotRisk),
          "page-access snapshot does not require mutation permission");
    const auto allocationControlRisk = api::mcp_contract::ClassifyTool(
        "watch_allocations", "POST", "/watch/allocations");
    check(allocationControlRisk == api::mcp_contract::ToolRisk::Control,
          "allocation watch toggle remains a control operation");
    check(api::mcp_contract::RequiresMutationPermission(allocationControlRisk),
          "allocation watch toggle requires mutation permission");
    if (failures) return 1;
    std::cout << "PASS: MCP URI and schema contract\n";
    return 0;
}

