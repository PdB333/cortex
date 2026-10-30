#!/usr/bin/env python3
"""E2E: static artifact analysis through the shipped cortex.exe.

No target process is involved. Checks, on the staged bundle:

  1. `cortex.exe inspect <pe>` identifies the PE fixture and its exports;
  2. `cortex.exe mcp` lists the cortex_artifact_* tools as read-only;
  3. without --artifact-root every artifact call is refused;
  4. with --artifact-root, inspect/children/read work inside the root on the
     PE fixture and on an Electron-style ASAR (nested entries with '!');
  5. paths outside the root are refused; reads are bounded;
  6. a root that does not exist stops the host at start-up.

    python tests/artifact_mcp_e2e.py \\
        --cortex stage/Cortex/cortex.exe \\
        --target stage/Cortex/e2e/cortex_test_target_x64.exe [--runner wine]

Exit code 0 = pass.
"""

import argparse
import hashlib
import json
import os
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import threading


class Mcp:
    """Minimal MCP stdio client (newline-delimited JSON-RPC)."""

    def __init__(self, command, cwd):
        self.proc = subprocess.Popen(command, cwd=cwd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True, bufsize=1)
        self.stderr = []
        threading.Thread(target=lambda: [self.stderr.append(line) for line in self.proc.stderr],
                         daemon=True).start()
        self.next_id = 0

    def request(self, method, params=None, timeout=60):
        self.next_id += 1
        message = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            message["params"] = params
        self.proc.stdin.write(json.dumps(message) + "\n")
        self.proc.stdin.flush()
        reply = {}

        def read():
            while True:
                line = self.proc.stdout.readline()
                if not line:
                    return
                try:
                    candidate = json.loads(line)
                except ValueError:
                    continue
                if candidate.get("id") == self.next_id:
                    reply["value"] = candidate
                    return

        reader = threading.Thread(target=read, daemon=True)
        reader.start()
        reader.join(timeout)
        return reply.get("value")

    def initialize(self):
        reply = self.request("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                            "clientInfo": {"name": "artifact-mcp-e2e", "version": "1"}})
        self.proc.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
        self.proc.stdin.flush()
        return reply

    def call(self, name, arguments):
        """Returns (is_error, structuredContent)."""
        reply = self.request("tools/call", {"name": name, "arguments": arguments})
        if reply is None:
            return True, {"_transport": "no_reply"}
        if "error" in reply:
            return True, {"_rpc_error": reply["error"]}
        result = reply.get("result", {})
        return bool(result.get("isError")), result.get("structuredContent", result)

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=15)
        except Exception:
            self.proc.kill()


def asar(files, payload):
    """An Electron archive: pickle header {files} followed by the payload."""
    text = json.dumps({"files": files}).encode()
    padded = (len(text) + 3) & ~3
    header = struct.pack("<IIII", 4, 8 + padded, 4 + padded, len(text)) + text + b"\0" * (padded - len(text))
    return header + payload


def windows_path(runner, path):
    """The path as the Windows process sees it (Wine maps / to Z:)."""
    if runner and os.path.basename(runner[0]).startswith("wine"):
        return "Z:" + os.path.abspath(path).replace("/", "\\")
    return os.path.abspath(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cortex", required=True)
    parser.add_argument("--target", required=True, help="a PE fixture (the e2e test target)")
    parser.add_argument("--runner", default="", help="e.g. wine")
    arguments = parser.parse_args()
    runner = shlex.split(arguments.runner)
    cortex = os.path.abspath(arguments.cortex)
    bundle = os.path.dirname(cortex)
    failures = []

    def check(condition, message):
        print(("PASS " if condition else "FAIL ") + message)
        if not condition:
            failures.append(message)

    workdir = tempfile.mkdtemp(prefix="cortex-artifact-e2e-")
    root = os.path.join(workdir, "root")
    outside = os.path.join(workdir, "outside")
    os.makedirs(os.path.join(root, "app"))
    os.makedirs(outside)
    pe = os.path.join(root, "target.exe")
    shutil.copyfile(arguments.target, pe)
    with open(pe, "rb") as handle:
        pe_sha = hashlib.sha256(handle.read()).hexdigest()
    main_js = b"const { ipcRenderer } = require('electron');\n"
    inner = asar({"index.js": {"size": 7, "offset": "0"}}, b"exports")
    with open(os.path.join(root, "app", "app.asar"), "wb") as handle:
        handle.write(asar({"main.js": {"size": len(main_js), "offset": "0"},
                           "vendor.asar": {"size": len(inner), "offset": str(len(main_js))},
                           "native.node": {"size": 4, "offset": "0", "unpacked": True}},
                          main_js + inner))
    with open(os.path.join(outside, "secret.bin"), "wb") as handle:
        handle.write(b"secret")

    try:
        # 1. CLI
        inspect = subprocess.run(runner + [cortex, "inspect", windows_path(runner, pe)], cwd=bundle,
                                 capture_output=True, text=True, timeout=120)
        try:
            report = json.loads(inspect.stdout)
        except ValueError:
            report = {}
        exports = [row.get("name") for row in report.get("data", {}).get("exports", {}).get("functions", [])]
        check(inspect.returncode == 0 and report.get("format") == "pe", "cortex.exe inspect identifies the PE")
        check(report.get("identity", {}).get("sha256") == pe_sha, "inspect reports the file's SHA-256")
        check("CortexDemoApplyDamage" in exports and "g_cortex_demo_actor" in exports,
              "inspect lists the fixture's exports")
        check(report.get("capabilities", {}).get("decompile", {}).get("status") == "unavailable",
              "decompilation is not claimed")

        # 2-3. MCP without a root
        server = Mcp(runner + [cortex, "mcp", "--tools", "all"], bundle)
        server.initialize()
        tools = {t["name"]: t for t in (server.request("tools/list", {}) or {}).get("result", {}).get("tools", [])}
        names = ("cortex_artifact_inspect", "cortex_artifact_children", "cortex_artifact_read")
        check(all(name in tools for name in names), "tools/list has the artifact tools")
        check(all(tools.get(name, {}).get("_cortex", {}).get("read_only") is True for name in names),
              "artifact tools are marked read-only")
        failed, result = server.call("cortex_artifact_inspect", {"path": windows_path(runner, pe)})
        check(failed and result.get("error") == "artifact_roots_not_configured",
              "no --artifact-root: artifact calls are refused")
        server.close()

        # 4-5. MCP with a root
        server = Mcp(runner + [cortex, "mcp", "--tools", "all", "--artifact-root", windows_path(runner, root)], bundle)
        server.initialize()
        failed, result = server.call("cortex_artifact_inspect", {"path": windows_path(runner, pe), "max_entries": 3})
        check(not failed and result.get("format") == "pe" and result.get("identity", {}).get("sha256") == pe_sha,
              "inspect the PE inside the root")
        check(result.get("data", {}).get("exports_truncated") is True and
              len(result.get("data", {}).get("exports", {}).get("functions", [])) == 3,
              "max_entries bounds the listing")
        asar_path = windows_path(runner, os.path.join(root, "app", "app.asar"))
        failed, result = server.call("cortex_artifact_children", {"path": asar_path})
        children = {row.get("name"): row for row in result.get("children", [])}
        check(not failed and set(children) == {"main.js", "vendor.asar", "native.node"}, "ASAR entries listed")
        check(children.get("native.node", {}).get("readable") is False, "unpacked entry is not read from the archive")
        failed, result = server.call("cortex_artifact_read", {"path": asar_path, "child": "main.js", "encoding": "utf8"})
        check(not failed and result.get("text") == main_js.decode(), "read an ASAR entry")
        failed, result = server.call("cortex_artifact_read",
                                     {"path": asar_path, "child": "vendor.asar!index.js", "encoding": "utf8"})
        check(not failed and result.get("text") == "exports", "read a nested ASAR entry")
        failed, result = server.call("cortex_artifact_read", {"path": windows_path(runner, pe), "length": 1 << 20})
        check(not failed and result.get("length") == 65536 and result.get("hex", "")[:4] == "4d5a",
              "reads are bounded to 64 KiB")
        failed, result = server.call("cortex_artifact_read",
                                     {"path": windows_path(runner, os.path.join(outside, "secret.bin"))})
        check(failed and result.get("error") == "path_outside_artifact_roots", "path outside the root refused")
        failed, result = server.call("cortex_artifact_read",
                                     {"path": windows_path(runner, os.path.join(root, "..", "outside", "secret.bin"))})
        check(failed and result.get("error") == "path_outside_artifact_roots", "'..' escape refused")
        server.close()

        # 6. invalid root
        bad = subprocess.run(runner + [cortex, "mcp", "--artifact-root", windows_path(runner, os.path.join(workdir, "missing"))],
                             cwd=bundle, input="", capture_output=True, text=True, timeout=120)
        check(bad.returncode == 2, "a missing --artifact-root stops the host (exit 2)")
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    if failures:
        print("%d artifact E2E check(s) failed" % len(failures))
        return 1
    print("artifact E2E passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
