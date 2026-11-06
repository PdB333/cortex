#!/usr/bin/env python3
"""E2E: the shared investigation context is per process, end to end.

Two test targets get the runtime (`cortex.exe inject`), then one
`cortex.exe mcp --tools all --allow-state` attaches both and checks, against
the live runtimes:

  1. each context reports its own pid and process generation;
  2. an observation recorded on one target never appears on the other;
  3. a call pinned to the other target's generation is refused by the host
     (stale_target_generation), and a stale expected_generation is refused by
     the runtime itself before anything is recorded;
  4. detach and reattach keep the context of a process that is still running;
  5. killing one target leaves the other's context and calls intact, and the
     dead target is not silently replaced by the live one;
  6. an agent's state-changing call is announced as agent.action_* events and
     recorded in the history with agent provenance;
  7. the desktop-only selection route is not in tools/list.

    python tests/context_isolation_e2e.py \\
        --cortex stage/Cortex/cortex.exe \\
        --target stage/Cortex/e2e/cortex_test_target_x64.exe [--runner wine]

Exit code 0 = pass.
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

    def notify(self, method):
        self.proc.stdin.write(json.dumps({"jsonrpc": "2.0", "method": method}) + "\n")
        self.proc.stdin.flush()

    def initialize(self):
        reply = self.request("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                            "clientInfo": {"name": "context-isolation-e2e", "version": "1"}})
        self.notify("notifications/initialized")
        return reply

    def call(self, name, arguments, timeout=60):
        """Returns (is_error, structuredContent)."""
        reply = self.request("tools/call", {"name": name, "arguments": arguments}, timeout)
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
                if manifest.get("pid"):
                    return process, manifest
            except ValueError:
                pass
        time.sleep(0.2)
    process.kill()
    raise RuntimeError("target manifest never appeared: " + manifest_path)


def wait_for_runtime(bundle, pid, timeout=60):
    """`inject` returns once the DLL is loaded; the runtime writes its pipe
    token when its own start-up has finished (seconds later under Wine)."""
    names = [os.path.join(bundle, "cortex.mcp.%d.token" % pid)] + [
        os.path.join(bundle, "runtime", arch, "cortex.mcp.%d.token" % pid) for arch in ("x64", "x86")]
    deadline = time.time() + timeout
    while time.time() < deadline:
        if any(os.path.exists(name) and os.path.getsize(name) > 0 for name in names):
            return True
        time.sleep(0.25)
    return False


def route(content):
    """The route body inside a primitive tool's structuredContent."""
    return content.get("result", content) if isinstance(content, dict) else {}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cortex", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--runner", default="", help="command prefix, e.g. wine")
    arguments = parser.parse_args()

    runner = shlex.split(arguments.runner)
    cortex = os.path.abspath(arguments.cortex)
    target = os.path.abspath(arguments.target)
    bundle = os.path.dirname(cortex)
    workdir = tempfile.mkdtemp(prefix="cortex-context-")
    failures = []

    def check(condition, message, detail=None):
        print(("ok    " if condition else "FAIL  ") + message)
        if not condition:
            if detail is not None:
                print("      " + json.dumps(detail)[:600])
            failures.append(message)

    processes = []
    server = None
    try:
        for tag in ("a", "b"):
            process, manifest = start_target(runner, target, workdir, tag)
            processes.append((process, manifest))
            inject = subprocess.run(runner + [cortex, "inject", str(manifest["pid"])], cwd=bundle,
                                    capture_output=True, text=True, timeout=120)
            check(inject.returncode == 0, "runtime loaded into target %s (pid %d)" % (tag, manifest["pid"]),
                  (inject.stdout + inject.stderr).strip()[-300:])
            check(wait_for_runtime(bundle, manifest["pid"]), "runtime of target %s finished starting" % tag)
        pid_a = processes[0][1]["pid"]
        pid_b = processes[1][1]["pid"]

        server = Mcp(runner + [cortex, "mcp", "--tools", "all", "--allow-state"], bundle)
        server.initialize()

        for pid in (pid_a, pid_b):
            error, content = server.call("cortex_attach", {"pid": pid})
            check(not error, "attach pid %d" % pid, content)
        # A targetless server lists host tools only; the runtime's appear once attached.
        names = {tool["name"] for tool in server.request("tools/list", {}).get("result", {}).get("tools", [])}
        check({"context_get", "context_events", "context_observation_add"} <= names,
              "context tools are listed", sorted(n for n in names if n.startswith("context")))
        check("context_select" not in names and "context_share" not in names,
              "desktop-only context routes are not MCP tools")
        error, listing = server.call("cortex_targets", {})
        generations = {row["pid"]: row["generation"] for row in listing.get("targets", [])}
        check(pid_a in generations and pid_b in generations and generations[pid_a] != generations[pid_b],
              "both targets attached with distinct generations", listing)
        gen_a, gen_b = generations.get(pid_a, 0), generations.get(pid_b, 0)

        def on(pid, generation, name, args):
            payload = dict(args)
            payload["_cortex_target"] = pid
            payload["_cortex_generation"] = generation
            return server.call(name, payload)

        # 1 + 2: own identity, no cross-talk.
        for pid, generation, tag in ((pid_a, gen_a, "A"), (pid_b, gen_b, "B")):
            error, content = on(pid, generation, "context_observation_add",
                                {"statement": "only in %s" % tag, "kind": "note", "mutation_permission": True})
            check(not error and route(content).get("ok"), "observation recorded on %s" % tag, content)
        for pid, generation, mine, other in ((pid_a, gen_a, "only in A", "only in B"),
                                             (pid_b, gen_b, "only in B", "only in A")):
            error, content = on(pid, generation, "context_get", {})
            body = route(content)
            data = body.get("data", {})
            statements = [entry.get("statement") for entry in data.get("entries", [])]
            check(not error and body.get("target", {}).get("pid") == pid and
                  body.get("target", {}).get("generation") == generation,
                  "context of pid %d reports its own pid and generation" % pid, body.get("target"))
            check(mine in statements and other not in statements,
                  "context of pid %d holds only its own observation" % pid, statements)
            mine_entry = next((e for e in data.get("entries", []) if e.get("statement") == mine), {})
            check(mine_entry.get("provenance", {}).get("source") == "agent" and
                  mine_entry.get("status") == "client_assertion",
                  "observation carries agent provenance and stays an assertion", mine_entry)

        # 3: stale generations.
        error, content = on(pid_a, gen_b, "context_get", {})
        check(error and "stale_target_generation" in json.dumps(content),
              "host refuses a call pinned to another process generation", content)
        error, content = on(pid_a, gen_a, "context_observation_add",
                            {"statement": "must not land", "expected_generation": gen_a + 1,
                             "mutation_permission": True})
        check(error and route(content).get("error") == "stale_target_generation",
              "runtime refuses a stale expected_generation", content)
        error, content = on(pid_a, gen_a, "context_get", {})
        statements = [e.get("statement") for e in route(content).get("data", {}).get("entries", [])]
        check("must not land" not in statements, "nothing was recorded by the refused call", statements)

        # 6: agent actions are visible to the person.
        error, content = on(pid_a, gen_a, "context_events", {"_query": {"since": 0, "limit": 256}})
        events = route(content).get("data", {}).get("events", [])
        requested = [e for e in events if e.get("type") == "agent.action_requested"
                     and e.get("data", {}).get("tool") == "context_observation_add"]
        completed = [e for e in events if e.get("type") == "agent.action_completed"
                     and e.get("data", {}).get("tool") == "context_observation_add"]
        check(requested and completed, "agent.action_requested/completed published for the observation",
              [e.get("type") for e in events][-20:])
        check(all("only in A" not in json.dumps(e.get("data", {})) and
                  "statement" in e.get("data", {}).get("argument_names", []) for e in requested),
              "agent.action_* carry argument names, not values", requested[:1])
        check(any(e.get("type") == "context.observation_added" for e in events),
              "context.observation_added published")
        check("gap" in route(content).get("data", {}), "context_events reports gaps")
        error, content = on(pid_a, gen_a, "context_get", {})
        history = route(content).get("data", {}).get("history", [])
        check(any(row.get("tool") == "context_observation_add" and row.get("provenance", {}).get("source") == "agent"
                  for row in history), "the call is in the context history with agent provenance", history[-5:])

        # 4: detach and reattach the same live process.
        error, content = server.call("cortex_detach", {"_cortex_target": pid_a, "_cortex_generation": gen_a})
        check(not error, "detach A", content)
        error, content = server.call("cortex_attach", {"pid": pid_a})
        check(not error, "reattach A", content)
        error, content = on(pid_a, gen_a, "context_get", {})
        statements = [e.get("statement") for e in route(content).get("data", {}).get("entries", [])]
        check(not error and "only in A" in statements, "context survives detach and reattach of a live process",
              statements)

        # 5: A dies; B is untouched and A is not aliased to B.
        processes[0][0].kill()
        processes[0][0].wait(timeout=30)
        time.sleep(1.5)
        error, content = on(pid_a, gen_a, "context_get", {})
        body = route(content)
        check(error or body.get("target", {}).get("pid") != pid_b,
              "a call for the dead target is not served by the live one", content)
        error, content = on(pid_b, gen_b, "context_get", {})
        statements = [e.get("statement") for e in route(content).get("data", {}).get("entries", [])]
        check(not error and "only in B" in statements and "only in A" not in statements,
              "the live target's context is intact after the other one died", statements)
    finally:
        if server:
            server.close()
        for process, _ in processes:
            if process.poll() is None:
                process.kill()

    if failures:
        print("\n%d check(s) failed" % len(failures))
        if server and server.stderr:
            print("server stderr (tail):\n" + "".join(server.stderr[-20:]))
        return 1
    print("\nPASS: investigation context isolation across targets, generations and reattach")
    return 0


if __name__ == "__main__":
    sys.exit(main())
