"""Live performance baselines through MCP against the test target.

Measures, on a real process: exact and unknown-value scans (the runtime's
own duration_ms and the round trip), and the MCP round-trip latency of
memory_read, disasm and tools/list (p50/p95 over repeated calls). Writes a
JSON report. Numbers from a shared CI runner are noisy: compare trends.

    python tests/live_baseline.py --cortex stage/Cortex/cortex.exe \\
        --target stage/Cortex/e2e/cortex_test_target_x64.exe --out baseline-live.json [--runner wine]
"""

import argparse
import json
import os
import platform
import shlex
import statistics
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from context_isolation_e2e import start_target, wait_for_runtime  # noqa: E402
from robustness_e2e import Client  # noqa: E402


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(round(fraction * (len(ordered) - 1))))]


def timed(server, name, arguments, repeat):
    samples, errors = [], 0
    for _ in range(repeat):
        reply, elapsed, error, _ = server.call(name, arguments, timeout=120)
        if reply is None or error:
            errors += 1
        samples.append(elapsed * 1000.0)
    return {"calls": repeat, "errors": errors, "p50_ms": round(percentile(samples, 0.5), 2),
            "p95_ms": round(percentile(samples, 0.95), 2), "max_ms": round(max(samples), 2),
            "mean_ms": round(statistics.mean(samples), 2)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cortex", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--runner", default="")
    parser.add_argument("--repeat", type=int, default=50)
    arguments = parser.parse_args()
    runner = shlex.split(arguments.runner)
    cortex = os.path.abspath(arguments.cortex)
    bundle = os.path.dirname(cortex)
    workdir = tempfile.mkdtemp(prefix="cortex-baseline-")

    process, manifest = start_target(runner, os.path.abspath(arguments.target), workdir, "baseline")
    report = {"schema": "cortex.baseline.live.v1", "host": platform.platform(), "runner": arguments.runner or None,
              "target": os.path.basename(arguments.target), "measurements": {}}
    try:
        subprocess.run(runner + [cortex, "inject", str(manifest["pid"])], cwd=bundle, capture_output=True, timeout=120)
        if not wait_for_runtime(bundle, manifest["pid"]):
            print("runtime did not start")
            return 1
        server = Client(runner + [cortex, "mcp", "--tools", "all", "--pid", str(manifest["pid"])], bundle)
        server.initialize()
        m = report["measurements"]
        for label, args in [("scan_exact_i32", {"type": "i32", "value": "100"}),
                            ("scan_unknown_i32", {"type": "i32"})]:
            runs = []
            for _ in range(6):
                reply, elapsed, error, content = server.call("scan_new", args, timeout=300)
                body = content.get("result", content) if isinstance(content, dict) else {}
                runs.append({"round_trip_ms": round(elapsed * 1000.0, 1), "runtime_ms": body.get("duration_ms"),
                             "count": body.get("count"), "truncated": body.get("truncated"), "error": error})
            m[label] = runs
        m["memory_read_u32"] = timed(server, "memory_read", {"address": manifest["u32"], "type": "u32"}, arguments.repeat)
        m["disasm_32"] = timed(server, "disasm", {"_query": {"address": manifest["demo_apply_damage"], "count": 32}},
                               arguments.repeat)
        started = time.time()
        listed = server.request("tools/list", {}, timeout=60)
        m["tools_list"] = {"ms": round((time.time() - started) * 1000.0, 2),
                           "tools": len((listed or {}).get("result", {}).get("tools", []))}
        server.close()
    finally:
        process.kill()
    with open(arguments.out, "w") as handle:
        json.dump(report, handle, indent=1)
    print(json.dumps(report, indent=1))
    failed = [k for k, v in report["measurements"].items() if isinstance(v, dict) and v.get("errors")]
    failed += [k for k, v in report["measurements"].items() if isinstance(v, list) and any(r["error"] for r in v)]
    if failed:
        print("calls failed in: " + ", ".join(failed))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
