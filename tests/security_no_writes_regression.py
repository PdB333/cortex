#!/usr/bin/env python3
"""Regression test: without --allow-writes, no MCP call can change the target.

It starts the test target, loads the runtime into it the way a person would
(`cortex.exe inject`), then starts `cortex.exe mcp --tools all` WITHOUT
--allow-writes and calls every tool the server lists with plausible arguments,
always including mutation_permission=true. The target publishes a hash of its
code and of the globals nothing in it writes; the hash must be identical before
and after. Controls show that the test can see a change when one is allowed.

    python tests/security_no_writes_regression.py \
        --cortex stage/Cortex/cortex.exe \
        --target stage/Cortex/e2e/cortex_test_target_x64.exe

Exit code 0 = pass. Anything else prints what went wrong.
"""

import argparse
import json
import os
import shlex
import subprocess
import sys
import tempfile
import threading
import time

DENIAL_REASONS_PREFIXES = (
    "mutation_permission_required",
    "write_authority_required",
    "batch_op_",
    "batch_ops_required",
    "method_not_allowed",
)


class Mcp:
    """Minimal MCP stdio client (newline-delimited JSON-RPC)."""

    def __init__(self, command, cwd):
        self.proc = subprocess.Popen(
            command, cwd=cwd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, bufsize=1)
        self.stderr = []
        threading.Thread(target=lambda: [self.stderr.append(l) for l in self.proc.stderr], daemon=True).start()
        self.next_id = 0

    def request(self, method, params=None, timeout=60):
        self.next_id += 1
        message = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            message["params"] = params
        self.proc.stdin.write(json.dumps(message) + "\n")
        self.proc.stdin.flush()
        result = {}

        def read():
            while True:
                line = self.proc.stdout.readline()
                if not line:
                    return
                try:
                    reply = json.loads(line)
                except ValueError:
                    continue
                if reply.get("id") == message["id"]:
                    result["reply"] = reply
                    return

        reader = threading.Thread(target=read, daemon=True)
        reader.start()
        reader.join(timeout)
        return result.get("reply")

    def notify(self, method):
        self.proc.stdin.write(json.dumps({"jsonrpc": "2.0", "method": method}) + "\n")
        self.proc.stdin.flush()

    def initialize(self):
        reply = self.request("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                            "clientInfo": {"name": "security-regression", "version": "1"}})
        self.notify("notifications/initialized")
        return reply

    def call(self, name, arguments, timeout=60):
        reply = self.request("tools/call", {"name": name, "arguments": arguments}, timeout)
        if reply is None:
            return {"_transport": "no_reply"}
        if "error" in reply:
            return {"_rpc_error": reply["error"]}
        return reply.get("result", {}).get("structuredContent", reply.get("result", {}))

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=15)
        except Exception:
            self.proc.kill()


def error_code(result):
    """Best-effort extraction of the stable error code of a tool result."""
    if not isinstance(result, dict):
        return None
    if "_rpc_error" in result:
        data = result["_rpc_error"]
        return (data.get("data") or {}).get("code") or data.get("message")
    for container in (result, result.get("result") if isinstance(result.get("result"), dict) else {}):
        if isinstance(container.get("error"), str):
            return container["error"]
    return None


def is_denial(code):
    return bool(code) and str(code).startswith(DENIAL_REASONS_PREFIXES)


class Context:
    def __init__(self, manifest):
        self.pid = manifest["pid"]
        self.address = manifest["u32"]
        self.object = manifest["fake_object"]
        self.native_add = manifest["native_add"]
        self.thread = manifest.get("main_thread_id", 1)
        self.health = manifest["health"]


ADDRESS_NAMES = ("address", "addr", "start", "begin", "target", "base", "from", "ptr", "pointer",
                 "object", "site", "location")


def plausible(schema, name, ctx, depth=0):
    if depth > 6 or not isinstance(schema, dict):
        return 1
    if schema.get("oneOf"):
        options = schema["oneOf"]
        wanted = "string" if (name.lower() in ADDRESS_NAMES or name.lower().endswith("address")) else None
        for option in options:
            if option.get("type") == wanted:
                return plausible(option, name, ctx, depth)
        return plausible(options[0], name, ctx, depth)
    kind = schema.get("type")
    lowered = name.lower()
    if kind == "object":
        out = {}
        required = schema.get("required", [])
        for key, sub in (schema.get("properties") or {}).items():
            if key.startswith("_cortex"):
                continue
            if key in required or key in ("mutation_permission",):
                out[key] = plausible(sub, key, ctx, depth + 1)
        return out
    if kind == "array":
        if lowered == "ops":
            return [{"op": "memory_write", "address": ctx.address, "type": "u32", "value": 7}]
        if lowered == "steps":
            return []
        if lowered in ("bytes", "values"):
            return [144, 144]
        return []
    if kind == "boolean":
        return True
    if kind in ("integer", "number"):
        if lowered in ("pid", "process_id"):
            return int(ctx.pid)
        if lowered in ("thread_id", "tid"):
            return int(ctx.thread)
        if lowered in ("size", "count", "length", "bytes", "limit", "width", "len"):
            return 4
        if "timeout" in lowered or lowered.endswith("_ms"):
            return 1000
        if lowered in ("id", "scan_id", "snapshot_id", "a", "b"):
            return 1
        return 1
    if kind == "string" or kind is None:
        if lowered in ADDRESS_NAMES or lowered.endswith("address"):
            return ctx.address
        if lowered == "end":
            return hex(int(ctx.address, 16) + 0x1000)
        if lowered in ("type", "value_type", "data_type", "scan_type"):
            return "u32"
        if lowered == "kind":
            return "hw_write"
        if lowered in ("value", "new_value"):
            return "1"
        if lowered in ("code", "script", "source", "lua", "expression", "text"):
            return "writeInteger(%s, 1)" % ctx.address
        if lowered in ("asm", "assembly", "instructions"):
            return "nop"
        if lowered in ("pattern", "signature", "hex", "data"):
            return "90 90"
        if lowered in ("module", "module_name"):
            return "cortex_test_target_x64.exe"
        if lowered in ("backend",):
            return "veh"
        if lowered in ("objective",):
            return "write a value into the target"
        if lowered in ("method",):
            return "POST"
        if lowered in ("path",):
            return "/memory/write"
        if lowered in ("process", "name", "label"):
            return "cortex_test_target_x64.exe" if lowered == "process" else "x"
        return "x"
    return 1


def build_arguments(tool, ctx):
    """Plausible arguments for one tool, always claiming mutation intent."""
    arguments = plausible(tool.get("inputSchema", {}), tool["name"], ctx)
    if not isinstance(arguments, dict):
        arguments = {}
    props = (tool.get("inputSchema") or {}).get("properties") or {}
    if "mutation_permission" in props:
        arguments["mutation_permission"] = True
    if tool["name"] == "cortex_attach":
        arguments = {"pid": int(ctx.pid)}
    if tool["name"] == "cortex_debugger_backend":
        arguments = {"backend": "veh"}
    return arguments


def semantic_calls(tool, primitives_by_name, ctx):
    """For a semantic tool: one execution per allowed primitive, mutating or not."""
    calls = []
    for primitive in tool.get("_primitives", []):
        manifest = primitives_by_name.get(primitive)
        if not manifest:
            continue  # nested semantic tool: rejected by design
        step_arguments = build_arguments(manifest, ctx)
        calls.append((primitive, {
            "objective": "write a value into the target",
            "execute": True,
            "mutation_permission": True,
            "steps": [{"tool": primitive, "arguments": step_arguments}],
        }))
    return calls


def read_hash(path, timeout=10.0):
    """The target rewrites the hash file every 200 ms; wait for a fresh one."""
    deadline = time.time() + timeout
    last_mtime = os.path.getmtime(path) if os.path.exists(path) else 0
    while time.time() < deadline:
        time.sleep(0.45)
        if os.path.exists(path) and os.path.getmtime(path) > last_mtime:
            with open(path) as handle:
                return handle.read().strip()
    raise RuntimeError("the target did not publish a state hash: " + path)


def start_target(runner, target, workdir, tag):
    manifest_path = os.path.join(workdir, "manifest-%s.json" % tag)
    process = subprocess.Popen(runner + [target, "--e2e-manifest", manifest_path],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + 30
    while time.time() < deadline:
        if os.path.exists(manifest_path):
            try:
                with open(manifest_path) as handle:
                    manifest = json.load(handle)
                if manifest.get("hash_file"):
                    return process, manifest, manifest_path
            except ValueError:
                pass
        time.sleep(0.2)
    process.kill()
    raise RuntimeError("target manifest never appeared")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cortex", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--runner", default="", help="command prefix, e.g. wine")
    parser.add_argument("--skip-tool", action="append", default=[],
                        help="do not call this tool (for hosts whose debugger backend is broken, e.g. Wine)")
    parser.add_argument("--expect-tools", type=int, default=0,
                        help="fail unless tools/list has exactly this many tools (0 = just report)")
    parser.add_argument("--dump-tools", default="",
                        help="write name/method/path/risk of every listed tool to this JSON file, then continue")
    arguments = parser.parse_args()

    runner = shlex.split(arguments.runner)
    cortex = os.path.abspath(arguments.cortex)
    target = os.path.abspath(arguments.target)
    bundle = os.path.dirname(cortex)
    failures = []
    workdir = tempfile.mkdtemp(prefix="cortex-sec-")

    def check(condition, message):
        print(("ok    " if condition else "FAIL  ") + message)
        if not condition:
            failures.append(message)

    # Denial logs from earlier runs would hide a missing log.
    for root, _, files in os.walk(bundle):
        for name in files:
            if name in ("cortex_denied_mutations.jsonl", "cortex_debugger_events.jsonl"):
                os.remove(os.path.join(root, name))

    # ------------------------------------------------------------ scenario 1
    process, manifest, manifest_path = start_target(runner, target, workdir, "sweep")
    ctx = Context(manifest)
    hash_file = manifest["hash_file"]
    server = None
    try:
        inject = subprocess.run(runner + [cortex, "inject", str(ctx.pid)], cwd=bundle,
                                capture_output=True, text=True, timeout=120)
        check(inject.returncode == 0, "the person loads the runtime beforehand (cortex.exe inject): "
              + (inject.stdout + inject.stderr).strip()[-200:])

        server = Mcp(runner + [cortex, "mcp", "--pid", str(ctx.pid), "--tools", "all"], bundle)
        init = server.initialize()
        check(bool(init and init.get("result")), "server initializes without --allow-writes")
        listing = server.request("tools/list", {})
        tools = (listing or {}).get("result", {}).get("tools", [])
        check(len(tools) > 0, "tools/list returns tools (%d)" % len(tools))
        if arguments.dump_tools:
            with open(arguments.dump_tools, "w") as dump:
                json.dump([{"name": t.get("name"), "risk": (t.get("_cortex") or {}).get("risk"),
                            "method": (t.get("_http") or {}).get("method"),
                            "path": (t.get("_http") or {}).get("path")} for t in tools], dump, indent=1)
        if arguments.expect_tools:
            check(len(tools) == arguments.expect_tools,
                  "tools/list has exactly %d tools (found %d)" % (arguments.expect_tools, len(tools)))

        # Every listed tool must carry a risk class. A tool nobody classified is
        # refused by the runtime, so it must never appear as a usable tool.
        known_risks = ("observe", "analyze", "control", "mutate", "native_call")
        unclassified = []
        for tool in tools:
            meta = tool.get("_cortex") or {}
            if tool.get("_http"):
                ok = meta.get("risk") in known_risks
            elif tool.get("_semantic"):
                ok = True  # its steps are classified one by one as primitives
            else:
                ok = bool(meta.get("host_control") or meta.get("read_only"))
            if not ok:
                unclassified.append("%s (%s)" % (tool.get("name"), meta.get("risk")))
        check(not unclassified, "every listed tool has a risk classification"
              + ("" if not unclassified else ": " + ", ".join(unclassified)))
        tools = [t for t in tools if (t.get("_cortex") or {}).get("risk") != "unclassified"]

        before = read_hash(hash_file)
        print("state hash before:", before)

        died = []
        progress = {"mtime": os.path.getmtime(hash_file), "at": time.time(), "recent": []}

        def alive(label):
            """Name the call during which the target stopped or froze, if one did."""
            if died:
                return False
            progress["recent"] = (progress["recent"] + [label])[-4:]
            reason = None
            if process.poll() is not None:
                reason = "exited"
            else:
                modified = os.path.getmtime(hash_file)
                if modified > progress["mtime"]:
                    progress["mtime"], progress["at"] = modified, time.time()
                elif time.time() - progress["at"] > 1.0:
                    # A stall is not a change of state; only a target that does not come back is a problem.
                    deadline = time.time() + 6.0
                    while time.time() < deadline and os.path.getmtime(hash_file) <= progress["mtime"]:
                        time.sleep(0.2)
                    if os.path.getmtime(hash_file) > progress["mtime"]:
                        progress["mtime"], progress["at"] = os.path.getmtime(hash_file), time.time()
                        print("warn  the target stalled for over a second near: " + " / ".join(progress["recent"]))
                    else:
                        reason = "stopped responding"
            if reason:
                died.append(label)
                shown = " / ".join(progress["recent"])
                print("FAIL  the target %s during: %s" % (reason, shown))
                failures.append("the target %s during: %s" % (reason, shown))
            return not died

        semantic = [t for t in tools if t.get("_semantic")]
        primitives = {t["name"]: t for t in tools if not t.get("_semantic") and not t["name"].startswith("cortex_")}
        calls = 0
        denied = 0
        not_denied_but_mutating = []
        for tool in tools:
            name = tool["name"]
            if name in ("cortex_detach",):
                continue  # last, below
            if name in arguments.skip_tool:
                print("skip  " + name)
                continue
            if tool.get("_semantic"):
                for primitive, call_arguments in semantic_calls(tool, primitives, ctx):
                    if primitive in arguments.skip_tool:
                        continue
                    result = server.call(name, call_arguments, timeout=60)
                    calls += 1
                    if not alive("%s (step %s)" % (name, primitive)):
                        break
                    code = result.get("error") if isinstance(result, dict) else None
                    risk = (primitives[primitive].get("_cortex") or {}).get("risk")
                    mutating = risk in ("control", "mutate", "native_call")
                    if is_denial(code) or (isinstance(code, str) and code.startswith("primitive_lacks_safe_rollback")):
                        denied += 1
                    elif mutating:
                        not_denied_but_mutating.append("%s via %s -> %s" % (primitive, name, code or result.get("status")))
                continue
            result = server.call(name, build_arguments(tool, ctx), timeout=60)
            calls += 1
            if not alive(name):
                break
            code = error_code(result)
            meta = tool.get("_cortex") or {}
            if is_denial(code):
                denied += 1
            elif meta.get("mutation_permission_required") and not name.startswith("cortex_"):
                not_denied_but_mutating.append("%s -> %s" % (name, code or "succeeded"))

        # The two classic bypasses, spelled out.
        for label, call in (
            ("batch_run with a write", ("batch_run", {"ops": [{"op": "memory_write", "address": ctx.address,
                                                            "type": "u32", "value": 305419896}],
                                                    "mutation_permission": True})),
            ("batch_run with an unknown op", ("batch_run", {"ops": [{"op": "format_disk"}],
                                                          "mutation_permission": True})),
            ("batch_run with a computed op name", ("batch_run", {"ops": [{"op": {"$ref": 0}}],
                                                               "mutation_permission": True})),
            ("semantic batch_run write", ("test_candidate_causality", {
                "objective": "x", "execute": True, "mutation_permission": True,
                "steps": [{"tool": "batch_run", "arguments": {"transactional": True, "ops": [
                    {"op": "memory_write", "address": ctx.address, "type": "u32", "value": 305419896}]}}]})),
        ):
            result = server.call(*call)
            calls += 1
            alive(label)
            code = error_code(result)
            check(is_denial(code), "%s is refused (%s)" % (label, code))

        # A raw client message on Cortex's private channel is not forwarded.
        raw = server.request("cortex/private/route", {"method": "POST", "path": "/memory/write",
                                                      "body": {"address": ctx.address, "type": "u32", "value": 1}})
        alive("cortex/private/route")
        check(bool(raw) and "error" in raw and raw["error"].get("code") == -32601,
              "cortex/private/route from an MCP client is refused")

        check(not not_denied_but_mutating,
              "every tool that needs authority was refused: " + "; ".join(not_denied_but_mutating[:8]))
        print("calls made: %d, refused: %d" % (calls, denied))

        # The hash is read BEFORE the last tool: detaching a debugger is not a
        # change to the target's code or data, and on some hosts (Wine) it
        # leaves the target stopped.
        check(process.poll() is None, "the target is still running")
        if process.poll() is None:
            after = read_hash(hash_file)
            print("state hash after: ", after)
            check(before == after, "state hash unchanged after the sweep (%s -> %s)" % (before, after))

        server.call("cortex_detach", {"_cortex_target": str(ctx.pid)}, timeout=30)
        time.sleep(2.5)
        check(process.poll() is None, "the target survives cortex_detach")
        modified = os.path.getmtime(hash_file)
        time.sleep(1.0)
        if os.path.getmtime(hash_file) == modified and process.poll() is None:
            print("warn  the target stopped publishing after cortex_detach (a debugger detach that leaves "
                  "the target stopped; seen under Wine, not a change to its state)")
    finally:
        if server:
            server.close()

    # Refusals were logged.
    logs = []
    for root, _, files in os.walk(bundle):
        for name in files:
            if name == "cortex_denied_mutations.jsonl":
                logs.append(os.path.join(root, name))
    entries = []
    for path in logs:
        with open(path) as handle:
            entries += [json.loads(line) for line in handle if line.strip()]
    check(len(entries) >= 20, "refusals are logged (%d entries in %d file(s))" % (len(entries), len(logs)))
    check(all({"ts_ms", "tool", "reason", "source"} <= set(entry) for entry in entries),
          "every log entry has tool, reason, source and a timestamp")
    check(any(entry["source"] == "runtime" for entry in entries) and
          any(entry["source"] == "host" for entry in entries),
          "both the runtime and the host record refusals")

    # The debug_* read tools attach a debugger; that is journaled.
    phases = []
    for root, _, files in os.walk(bundle):
        for name in files:
            if name == "cortex_debugger_events.jsonl":
                with open(os.path.join(root, name)) as handle:
                    phases += [json.loads(line).get("phase") for line in handle if line.strip()]
    check("attached" in phases and "detached" in phases,
          "debugger attach and detach are journaled (%s)" % ", ".join(sorted(set(p for p in phases if p))))

    # ------------------------------------------------------------ control A: --allow-writes works, and still needs intent
    # A fresh target, so nothing left over from the sweep can matter.
    process, manifest, manifest_path = start_target(runner, target, workdir, "writes")
    try:
        ctx = Context(manifest)
        hash_file = manifest["hash_file"]
        subprocess.run(runner + [cortex, "inject", str(ctx.pid)], cwd=bundle, capture_output=True,
                       text=True, timeout=120)
        server = Mcp(runner + [cortex, "mcp", "--pid", str(ctx.pid), "--tools", "all", "--allow-writes"], bundle)
        try:
            server.initialize()
            baseline = read_hash(hash_file)
            result = server.call("memory_write", {"address": ctx.address, "type": "u32", "value": 305419896})
            check(error_code(result) == "mutation_permission_required",
                  "with --allow-writes a call without mutation_permission is still refused")
            check(read_hash(hash_file) == baseline, "a refused write leaves the hash unchanged")
            result = server.call("memory_write", {"address": ctx.address, "type": "u32", "value": 305419896,
                                                  "mutation_permission": True})
            check(error_code(result) is None, "with --allow-writes and mutation_permission the write is applied")
            check(read_hash(hash_file) != baseline,
                  "control: the state hash does change when a write is allowed (the test can see a change)")
            batch = server.call("batch_run", {"ops": [{"op": "memory_write", "address": ctx.address,
                                                      "type": "u32", "value": 7}], "mutation_permission": True})
            check(error_code(batch) is None, "with --allow-writes batch_run applies an allowlisted write")
        finally:
            server.close()
    finally:
        process.kill()

    # ------------------------------------------------------------ control B: attaching without authority does not inject
    process, manifest, manifest_path = start_target(runner, target, workdir, "attach")
    try:
        ctx = Context(manifest)
        server = Mcp(runner + [cortex, "mcp", "--tools", "all"], bundle)
        try:
            server.initialize()
            result = server.call("cortex_attach", {"pid": int(ctx.pid)}, timeout=60)
            code = error_code(result)
            text = json.dumps(result)
            check(code == "write_authority_required" or "write_authority_required" in text,
                  "cortex_attach without --allow-writes refuses to load the runtime (%s)" % (code,))
        finally:
            server.close()
        tokens = [n for n in os.listdir(bundle) if n.startswith("cortex.mcp.%s" % ctx.pid)]
        runtime_dirs = []
        for root, _, files in os.walk(bundle):
            runtime_dirs += [f for f in files if f.startswith("cortex.mcp.%s." % ctx.pid)]
        check(not tokens and not runtime_dirs, "no runtime was loaded into the target (no token file)")
    finally:
        process.kill()

    if failures:
        print("\n%d check(s) failed" % len(failures))
        return 1
    print("\nsecurity regression passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
