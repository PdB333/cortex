"""Robustness end-to-end tests: what happens when things go away.

- The target is killed while the MCP server is attached (idle, and in the
  middle of a long scan): every call must return within a bound, with an
  error, and the server must keep serving (tools/list, attach a new target).
- An external engine is absent, or exits with an error: the job reports why,
  and the job service keeps working.

    python tests/robustness_e2e.py --cortex stage/Cortex/cortex.exe \\
        --target stage/Cortex/e2e/cortex_test_target_x64.exe [--runner wine]

Exit code 0 = pass.
"""

import argparse
import json
import os
import queue
import shlex
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from context_isolation_e2e import start_target, wait_for_runtime  # noqa: E402


class Client:
    """MCP stdio client with one reader thread: a reply that comes after its
    caller gave up waiting is dropped, never mistaken for a later reply."""

    def __init__(self, command, cwd):
        self.proc = subprocess.Popen(command, cwd=cwd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.lock = threading.Lock()
        self.waiting = {}
        self.next_id = 0
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.proc.stdout:
            try:
                message = json.loads(line)
            except ValueError:
                continue
            with self.lock:
                slot = self.waiting.pop(message.get("id"), None)
            if slot is not None:
                slot.put(message)

    def send(self, method, params=None):
        with self.lock:
            self.next_id += 1
            request_id = self.next_id
            slot = queue.Queue()
            self.waiting[request_id] = slot
        message = {"jsonrpc": "2.0", "id": request_id, "method": method}
        if params is not None:
            message["params"] = params
        self.proc.stdin.write(json.dumps(message) + "\n")
        self.proc.stdin.flush()
        return slot

    def request(self, method, params=None, timeout=60):
        try:
            return self.send(method, params).get(timeout=timeout)
        except queue.Empty:
            return None

    def call(self, name, arguments, timeout=60):
        """(reply or None, seconds, is_error, content)."""
        started = time.time()
        reply = self.request("tools/call", {"name": name, "arguments": arguments}, timeout)
        elapsed = time.time() - started
        if reply is None:
            return None, elapsed, True, {"_transport": "no_reply"}
        if "error" in reply:
            return reply, elapsed, True, {"_rpc_error": reply["error"]}
        result = reply.get("result", {})
        return reply, elapsed, bool(result.get("isError")), result.get("structuredContent", result)

    def initialize(self):
        reply = self.request("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                            "clientInfo": {"name": "robustness-e2e", "version": "1"}})
        self.proc.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
        self.proc.stdin.flush()
        return reply

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=15)
        except Exception:
            self.proc.kill()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cortex", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--runner", default="")
    arguments = parser.parse_args()
    runner = shlex.split(arguments.runner)
    cortex = os.path.abspath(arguments.cortex)
    target = os.path.abspath(arguments.target)
    bundle = os.path.dirname(cortex)
    workdir = tempfile.mkdtemp(prefix="cortex-robust-")
    failures = []
    bound = 30.0  # seconds: no call may take longer once the target is gone

    def check(condition, message, detail=None):
        print(("ok    " if condition else "FAIL  ") + message)
        if not condition:
            if detail is not None:
                print("      " + json.dumps(detail, default=str)[:600])
            failures.append(message)
        return condition

    def launch(tag):
        process, manifest = start_target(runner, target, workdir, tag)
        # A token left by an earlier process with the same PID would make
        # wait_for_runtime return before this runtime is listening.
        for folder in (bundle, os.path.join(bundle, "runtime", "x64"), os.path.join(bundle, "runtime", "x86")):
            stale = os.path.join(folder, "cortex.mcp.%d.token" % manifest["pid"])
            if os.path.exists(stale):
                os.remove(stale)
        subprocess.run(runner + [cortex, "inject", str(manifest["pid"])], cwd=bundle, capture_output=True, timeout=120)
        check(wait_for_runtime(bundle, manifest["pid"]), "runtime started in target %s" % tag)
        return process, manifest

    def alive(server, label):
        started = time.time()
        tools = server.request("tools/list", {}, timeout=bound)
        check(tools is not None and len(tools.get("result", {}).get("tools", [])) > 0 and time.time() - started < bound,
              "the server still answers tools/list " + label)

    processes = []
    server = None
    try:
        # Stale runtime tokens: a killed runtime cannot delete its
        # cortex.mcp.<pid>.token. The server removes those of processes that
        # are gone, or that were written before the process with that PID
        # started; a live runtime's token stays.
        ghost = os.path.join(bundle, "cortex.mcp.4000000000.token")
        with open(ghost, "w") as handle:
            handle.write("0" * 64 + "\n")
        process, manifest = start_target(runner, target, workdir, "stale")
        processes.append(process)
        old = os.path.join(bundle, "cortex.mcp.%d.token" % manifest["pid"])
        with open(old, "w") as handle:
            handle.write("1" * 64 + "\n")
        an_hour_ago = time.time() - 3600
        os.utime(old, (an_hour_ago, an_hour_ago))
        sweeper = Client(runner + [cortex, "mcp", "--tools", "all"], bundle)
        sweeper.initialize()
        check(not os.path.exists(ghost), "the token of a process that no longer exists is removed")
        check(not os.path.exists(old), "a token older than the live process with that PID is removed")
        _, _, error, content = sweeper.call("cortex_attach", {"pid": manifest["pid"]})
        check(error or not content.get("ok"), "attach without a runtime still fails cleanly", content)
        sweeper.close()
        subprocess.run(runner + [cortex, "inject", str(manifest["pid"])], cwd=bundle, capture_output=True, timeout=120)
        check(wait_for_runtime(bundle, manifest["pid"]), "the runtime starts and writes a fresh token")
        tokens = [os.path.join(folder, "cortex.mcp.%d.token" % manifest["pid"])
                  for folder in (bundle, os.path.join(bundle, "runtime", "x64"), os.path.join(bundle, "runtime", "x86"))]
        live = [path for path in tokens if os.path.exists(path)]
        sweeper = Client(runner + [cortex, "mcp", "--tools", "all"], bundle)
        sweeper.initialize()
        check(live and all(os.path.exists(path) for path in live), "a live runtime's token is kept")
        _, _, error, content = sweeper.call("cortex_attach", {"pid": manifest["pid"]})
        check(not error and content.get("ok"), "attach after the fresh token", content)
        sweeper.close()
        process.kill()

        # Target a: malformed requests, then killed during a request.
        process, manifest = launch("a")
        processes.append(process)
        server = Client(runner + [cortex, "mcp", "--tools", "all", "--allow-writes"], bundle)
        server.initialize()
        _, _, error, content = server.call("cortex_attach", {"pid": manifest["pid"]})
        if not check(not error and content.get("ok"), "attach target a", content):
            raise SystemExit("cannot test a kill without an attached target")
        # 0. Malformed requests: wrong argument types for every tool (the
        # target's runtime and the server's own tools), and malformed
        # JSON-RPC. Each gets an answer; neither the server nor the target dies.
        listed = server.request("tools/list", {}, timeout=bound)
        names = [tool["name"] for tool in (listed or {}).get("result", {}).get("tools", [])]
        skip = {"cortex_launch", "cortex_restart", "cortex_stop", "cortex_detach"}
        unanswered = []
        killer = None
        for name in names:
            if name in skip:
                continue
            for bad in (None, "x", {}, -1):
                args = {key: bad for key in ("address", "type", "value", "id", "job_id", "scan_id", "session_id",
                                             "count", "limit", "size", "path", "name", "pid", "kind", "provider")}
                args["_query"] = {"address": bad, "count": bad}
                reply, _, _, _ = server.call(name, args, timeout=bound)
                if reply is None:
                    unanswered.append((name, bad))
                if server.proc.poll() is not None or process.poll() is not None:
                    killer = {"tool": name, "argument": bad, "server_exit": server.proc.poll(),
                              "target_exit": process.poll()}
                    break
            if server.proc.poll() is not None or process.poll() is not None:
                break
        check(len(names) > 50, "tools listed for the malformed-argument sweep (%d)" % len(names))
        check(not unanswered, "every malformed tool call is answered", unanswered[:10])
        check(server.proc.poll() is None, "the server survives wrongly typed arguments to every tool", killer)
        check(process.poll() is None, "the target survives wrongly typed arguments to every tool", killer)
        for raw in ('{"jsonrpc":"2.0","method":5}', '{"jsonrpc":"2.0","id":901,"method":"tools/call","params":5}',
                    '{"jsonrpc":"2.0","id":902,"method":"tools/call","params":{"name":7}}', '[1,2,3]', '"text"',
                    '{"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":null}}'):
            server.proc.stdin.write(raw + "\n")
            server.proc.stdin.flush()
        time.sleep(2)
        check(server.proc.poll() is None, "the server survives malformed JSON-RPC messages")
        alive(server, "after malformed requests")

        # 1. Killed while a request is running inside it: a Lua busy loop
        # bounded to 20 s keeps the runtime busy for as long as needed.
        sent = time.time()
        slot = server.send("tools/call", {"name": "lua_exec", "arguments": {
            "code": "local n = 0 for i = 1, 4e9 do n = n + 1 end return n", "timeout_ms": 20000,
            "mutation_permission": True}})
        time.sleep(1.5)
        slot_was_pending = slot.empty()
        process.kill()
        killed = time.time()
        try:
            reply = slot.get(timeout=bound)
        except queue.Empty:
            reply = None
        check(reply is not None, "the request in flight is answered within %d s of the kill" % bound)
        if reply is not None:
            failed = "error" in reply or reply.get("result", {}).get("isError")
            check(failed, "the request in flight reports a failure, not a result", reply)
            check(slot_was_pending, "the request was still running when the target was killed")
            print("      answered %.1f s after the kill, %.1f s after it was sent" % (time.time() - killed, time.time() - sent))
        for name, args in [("memory_read", {"address": manifest["u32"], "type": "u32"}),
                           ("scan_new", {"type": "i32", "value": "100"}), ("debug_threads", {}),
                           ("context_get", {})]:
            reply, elapsed, error, content = server.call(name, args, timeout=bound)
            check(reply is not None and error and elapsed < bound,
                  "%s on the dead target fails within %d s (%.1f s)" % (name, bound, elapsed), content)
        _, _, _, targets = server.call("cortex_targets", {})
        check(targets.get("count") == 0, "the dead target is no longer listed", targets)
        alive(server, "after the target died")

        # 2. Killed while the debugger holds a hardware breakpoint.
        process, manifest = launch("b")
        processes.append(process)
        _, _, error, content = server.call("cortex_attach", {"pid": manifest["pid"]})
        check(not error and content.get("ok"), "attach target b", content)
        reply, _, error, content = server.call("debug_breakpoint_add", {
            "address": manifest["u32"], "kind": "hw_write", "size": 4, "action": "log", "mutation_permission": True})
        check(not error, "hardware write breakpoint set on target b", content)
        process.kill()
        time.sleep(1)
        reply, elapsed, error, content = server.call("debug_breakpoint_list", {}, timeout=bound)
        check(reply is not None and elapsed < bound,
              "debug_breakpoint_list answers within %d s after the debuggee died (%.1f s)" % (bound, elapsed), content)
        alive(server, "after the debuggee died")

        # 3. A new target is usable on the same server.
        process, manifest = launch("c")
        processes.append(process)
        _, _, error, content = server.call("cortex_attach", {"pid": manifest["pid"]})
        check(not error and content.get("ok"), "attach target c on the same server", content)
        _, _, error, content = server.call("memory_read", {"address": manifest["u32"], "type": "u32"})
        check(not error and content.get("result", {}).get("value") == 0xDEADBEEF, "memory_read on target c", content)
        _, _, error, content = server.call("debug_breakpoint_add", {
            "address": manifest["u32"], "kind": "hw_write", "size": 4, "action": "log", "mutation_permission": True})
        check(not error, "the debugger works on target c", content)
        server.close()
        server = None

        # 4. External engines that are absent or fail.
        failing = os.path.join(workdir, "failing-engine.bat")
        with open(failing, "w") as handle:
            handle.write("@echo engine failure 1>&2\r\n@exit /b 3\r\n")
        fixture = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures", "dotnet", "Cortex.Fixture.dll")
        engines = Client(runner + [cortex, "mcp", "--tools", "all", "--artifact-root", os.path.dirname(fixture),
                                   "--ilspycmd", failing, "--ghidra-dir", os.path.join(workdir, "no-ghidra-here")],
                         bundle)
        engines.initialize()
        reply, elapsed, error, providers = engines.call("cortex_analysis_providers", {"refresh": True}, timeout=bound)
        check(reply is not None and not error and elapsed < bound, "providers listed within %d s" % bound, providers)
        rows = {row.get("name"): row for row in providers.get("providers", [])} if isinstance(providers, dict) else {}
        for name in ("ghidra", "ilspy"):
            row = rows.get(name, {})
            check(row and not row.get("available"), "%s is reported unavailable" % name, row)
            check(bool(row.get("availability")), "%s says why: %s" % (name, str(row.get("availability"))[:90]), row)
        reply, elapsed, error, content = engines.call("cortex_analysis_start", {
            "provider": "ilspy", "feature": "list_types", "path": fixture}, timeout=bound)
        check(reply is not None and error and elapsed < bound,
              "starting a job on an unavailable engine is refused within %d s" % bound, content)
        reply, elapsed, error, content = engines.call("cortex_analysis_jobs", {}, timeout=bound)
        check(reply is not None and not error, "the job service still answers", content)
        alive(engines, "after the engine failures")
        engines.close()

        # 5. An engine that starts, then fails in the middle of a job.
        crashing = os.path.join(workdir, "crashing-engine.bat")
        with open(crashing, "w") as handle:
            handle.write('@if "%~1"=="--version" (echo ilspycmd: 9.1.0.7988& exit /b 0)\r\n'
                         "@echo decompiler crashed 1>&2\r\n@exit /b 5\r\n")
        engines = Client(runner + [cortex, "mcp", "--tools", "all", "--artifact-root", os.path.dirname(fixture),
                                   "--ilspycmd", crashing, "--analysis-cache", os.path.join(workdir, "cache")], bundle)
        engines.initialize()
        _, _, error, providers = engines.call("cortex_analysis_providers", {"refresh": True}, timeout=bound)
        rows = {row.get("name"): row for row in providers.get("providers", [])} if isinstance(providers, dict) else {}
        check(rows.get("ilspy", {}).get("available"), "the crashing engine passes its version check", rows.get("ilspy"))
        job_ids = []
        for attempt in range(2):
            _, _, error, started = engines.call("cortex_analysis_start", {
                "provider": "ilspy", "feature": "list_types", "path": fixture}, timeout=bound)
            check(not error and started.get("job_id"), "job %d starts" % (attempt + 1), started)
            job_ids.append(started.get("job_id"))
        for index, job_id in enumerate(job_ids):
            state = {}
            deadline = time.time() + 120
            while time.time() < deadline:
                _, _, _, state = engines.call("cortex_analysis_get", {"job_id": job_id}, timeout=bound)
                if state.get("state") in ("failed", "succeeded", "cancelled", "completed"):
                    break
                time.sleep(0.5)
            check(state.get("state") == "failed", "job %d ends as failed, not stuck (%s)" % (index + 1, state.get("state")), state)
            check("crashed" in json.dumps(state) or "5" in str(state.get("exit_code", "")),
                  "job %d keeps the engine's error output or exit code" % (index + 1), state)
        alive(engines, "after an engine failed mid-job")
        engines.close()
    finally:
        if server is not None:
            server.close()
        for process in processes:
            try:
                process.kill()
            except Exception:
                pass

    print("\nrobustness E2E: %s" % ("PASS" if not failures else "FAIL (%d)" % len(failures)))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
