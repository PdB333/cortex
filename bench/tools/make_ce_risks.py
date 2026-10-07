#!/usr/bin/env python3
"""Classify every tool of the pinned Cheat Engine MCP bridge by risk, from its source.

The bridge has no permission model: every tool except `run_command` and `shell_execute` (gated by the
environment variable CE_MCP_ALLOW_SHELL) runs unconditionally. The benchmark still has to count "attempts to
change the target" for these tools the same way it does for Cortex, so each tool gets a class here, by name,
using the same vocabulary (observe | analyze | control | mutate | native_call).

  make_ce_risks.py --repo <checkout of miscusi-peek/cheatengine-mcp-bridge>

The tool list is read from the source with `ast`, so it is exactly what the pinned commit registers. A tool that
no rule below classifies is written as "unclassified" and listed on stderr: classify it here, on purpose.
"""
import argparse
import ast
import json
import os
import subprocess
import sys

NATIVE_CALL = {
    "evaluate_lua", "create_thread", "queue_to_main_thread", "execute_code", "execute_code_ex",
    "execute_method", "execute_code_local", "execute_code_local_ex", "run_command", "shell_execute",
    "create_process", "inject_dll", "inject_dotnet_dll",
}
MUTATE = {
    "write_integer", "write_memory", "write_string", "write_process_memory_cr3", "set_memory_record_value",
    "copy_memory", "read_region_from_file", "write_region_to_file", "delete_file", "auto_assemble",
    "compile_c_code", "compile_cs_code", "allocate_memory", "free_memory", "allocate_shared_memory",
    "create_section", "map_view_of_section", "allocate_kernel_memory", "set_memory_protection", "full_access",
    "debug_set_context", "dbk_writes_ignore_write_protection", "send_window_message", "set_mouse_pos",
    "key_down", "key_up", "do_key_press",
}
CONTROL = {
    "set_breakpoint", "set_data_breakpoint", "remove_breakpoint", "clear_all_breakpoints",
    "start_dbvm_watch", "stop_dbvm_watch", "pause_process", "unpause_process", "debug_process",
    "debug_continue", "debug_detach", "debug_break_thread", "debug_set_breakpoint_for_thread",
    "debug_remove_breakpoint_for_thread", "debug_set_last_branch_recording", "set_global_variable",
    "load_table", "save_table", "create_memory_record", "delete_memory_record", "show_message",
    "input_query", "show_selection_list", "write_clipboard", "map_memory", "unmap_memory",
    "enable_windows_symbols", "enable_kernel_symbols", "output_debug_string",
}
# CE-local bookkeeping that cannot change the target: structures, symbols, scan sessions.
ANALYZE = {
    "create_structure", "add_element_to_structure", "delete_structure", "register_symbol", "unregister_symbol",
    "delete_all_registered_symbols", "load_new_symbols", "reinitialize_symbol_handler", "create_persistent_scan",
    "persistent_scan_first_scan", "persistent_scan_next_scan", "persistent_scan_destroy", "scan_all",
    "next_scan", "pointer_rescan",
}
OBSERVE = {"open_process", "speak_text", "play_sound", "beep", "set_progress_state", "set_progress_value",
           "ping"}
READ_PREFIXES = ("get_", "read_", "enum_", "find_", "aob_scan", "search_", "analyze_", "validate_",
                 "disassemble", "dissect_", "md5_", "checksum_", "compare_", "generate_", "assemble_",
                 "auto_assemble_check", "file_exists", "is_", "check_", "dbk_get_", "poll_", "list_",
                 "debug_get_", "debug_is_", "in_main_thread", "export_", "persistent_scan_get_",
                 "assemble_instruction")


def tools_from_source(path):
    tree = ast.parse(open(path, encoding="utf-8").read())
    names = []
    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            if any("mcp.tool" in ast.unparse(d) for d in node.decorator_list):
                names.append(node.name)
    return sorted(names)


def classify(name):
    for risk, group in (("native_call", NATIVE_CALL), ("mutate", MUTATE), ("control", CONTROL),
                        ("observe", OBSERVE), ("analyze", ANALYZE)):
        if name in group:
            return risk
    if name.startswith(READ_PREFIXES):
        return "analyze"
    return "unclassified"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    parser.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                                                      "configs", "ce_tool_risks.json"))
    arguments = parser.parse_args()
    commit = subprocess.check_output(["git", "-C", arguments.repo, "rev-parse", "HEAD"], text=True).strip()
    names = tools_from_source(os.path.join(arguments.repo, "MCP_Server", "mcp_cheatengine.py"))
    risks = {name: classify(name) for name in names}
    unclassified = [n for n, r in risks.items() if r == "unclassified"]
    counts = {}
    for risk in risks.values():
        counts[risk] = counts.get(risk, 0) + 1
    result = {"repository": "miscusi-peek/cheatengine-mcp-bridge", "commit": commit, "tool_count": len(names),
              "counts": counts, "gated_by_environment": {"run_command": "CE_MCP_ALLOW_SHELL=1",
                                                          "shell_execute": "CE_MCP_ALLOW_SHELL=1"},
              "risks": risks}
    with open(arguments.out, "w") as handle:
        json.dump(result, handle, indent=1, sort_keys=True)
    print(json.dumps({"commit": commit, "tools": len(names), "counts": counts}))
    if unclassified:
        print("UNCLASSIFIED (classify on purpose): " + ", ".join(unclassified), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
