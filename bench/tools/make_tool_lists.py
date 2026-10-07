#!/usr/bin/env python3
"""Write the exact tool list of every Cortex configuration from a real server.

The lists are generated, not typed, so they cannot drift from the build that is
pinned for the benchmark. The output records the Cortex commit and the SHA-256
of cortex.exe it was generated from.

  make_tool_lists.py --cortex bundle/cortex.exe --commit <sha> [--runner wine]
"""
import argparse
import hashlib
import json
import os
import shlex
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "harness"))
from mcpclient import McpClient  # noqa: E402

# C keeps what Cheat Engine-style raw access has, and leaves out
#   input_*    replaced by bench_trigger for games
#   prompt_*   asks the person at the screen
#   re_*       Cortex's reverse-engineering helpers (that is D0')
#   project_*, session_*, actions_*   persistence and rollback (memory is a separate axis)
C_EXCLUDED_PREFIXES = ("input_", "prompt_", "re_", "project_", "session_", "actions_")


def list_tools(runner, cortex, profile, pid):
    client = McpClient(runner + [cortex, "mcp", "--pid", str(pid), "--tools", profile], cwd=os.path.dirname(cortex))
    try:
        client.initialize()
        return client.list_tools()
    finally:
        client.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cortex", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--pid", type=int, required=True,
                        help="a running bench_target whose runtime is already loaded (cortex.exe inject <pid>)")
    parser.add_argument("--runner", default="")
    parser.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                      "..", "configs", "cortex_tool_lists.json"))
    arguments = parser.parse_args()
    runner = shlex.split(arguments.runner)
    cortex = os.path.abspath(arguments.cortex)

    full = list_tools(runner, cortex, "all", arguments.pid)
    compact = list_tools(runner, cortex, "compact", arguments.pid)

    host = sorted(t["name"] for t in full if t["name"].startswith("cortex_"))
    semantic = sorted(t["name"] for t in full if t.get("_semantic"))
    primitives = sorted(t["name"] for t in full if t.get("_http"))
    compact_names = sorted(t["name"] for t in compact)
    risks = {t["name"]: (t.get("_cortex") or {}).get("risk") for t in full if t.get("_http")}

    assert sorted(host + semantic + primitives) == sorted(t["name"] for t in full), "unclassified listing entry"
    assert compact_names == sorted(host + semantic) or set(compact_names) <= set(host + semantic), \
        "compact profile is not host + semantic tools"

    c_tools = host + [n for n in primitives if not n.startswith(C_EXCLUDED_PREFIXES)]
    d0_tools = compact_names
    d0p_tools = sorted(set(compact_names) | {n for n in primitives if n.startswith("re_")} | {"actions_rollback"})

    digest = hashlib.sha256(open(cortex, "rb").read()).hexdigest()
    result = {
        "cortex_commit": arguments.commit,
        "cortex_exe_sha256": digest,
        "counts": {"all": len(full), "host": len(host), "semantic": len(semantic),
                   "primitives": len(primitives), "compact": len(compact_names),
                   "C": len(c_tools), "D0": len(d0_tools), "D0prime": len(d0p_tools)},
        "excluded_from_C_prefixes": list(C_EXCLUDED_PREFIXES),
        "C": {"profile": "all", "tools": sorted(c_tools)},
        "D0": {"profile": "compact", "tools": sorted(d0_tools)},
        "D0prime": {"profile": "all", "tools": d0p_tools},
        "primitive_risks": risks,
    }
    with open(arguments.out, "w") as handle:
        json.dump(result, handle, indent=1, sort_keys=True)
    print(json.dumps(result["counts"]))


if __name__ == "__main__":
    main()
