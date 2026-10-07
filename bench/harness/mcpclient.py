"""Minimal JSON-RPC-over-stdio MCP client used by the benchmark proxy.

It speaks just enough of the protocol: initialize, tools/list, tools/call. Every
request has a timeout; a server that dies or stalls is reported, never waited on
forever. Server stderr is kept for the run log.
"""
import json
import subprocess
import threading
import time


class McpError(Exception):
    pass


class McpClient:
    def __init__(self, command, cwd=None, env=None):
        self.command = command
        self.process = subprocess.Popen(
            command, cwd=cwd, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, bufsize=1)
        self._next_id = 0
        self._lock = threading.Lock()
        self._responses = {}
        self._cond = threading.Condition()
        self.stderr_lines = []
        self._closed = False
        threading.Thread(target=self._read_stdout, daemon=True).start()
        threading.Thread(target=self._read_stderr, daemon=True).start()

    def _read_stdout(self):
        for line in self.process.stdout:
            line = line.strip()
            if not line:
                continue
            try:
                message = json.loads(line)
            except ValueError:
                continue
            if "id" in message and ("result" in message or "error" in message):
                with self._cond:
                    self._responses[message["id"]] = message
                    self._cond.notify_all()
        with self._cond:
            self._closed = True
            self._cond.notify_all()

    def _read_stderr(self):
        for line in self.process.stderr:
            self.stderr_lines.append(line.rstrip())
            del self.stderr_lines[:-200]

    def alive(self):
        return self.process.poll() is None

    def request(self, method, params=None, timeout=60.0):
        with self._lock:
            self._next_id += 1
            request_id = self._next_id
        message = {"jsonrpc": "2.0", "id": request_id, "method": method}
        if params is not None:
            message["params"] = params
        try:
            self.process.stdin.write(json.dumps(message) + "\n")
            self.process.stdin.flush()
        except (BrokenPipeError, OSError) as error:
            raise McpError("server pipe closed: %s" % error)
        deadline = time.time() + timeout
        with self._cond:
            while request_id not in self._responses:
                if self._closed:
                    raise McpError("server exited")
                remaining = deadline - time.time()
                if remaining <= 0:
                    raise McpError("timeout after %.0fs waiting for %s" % (timeout, method))
                self._cond.wait(remaining)
            return self._responses.pop(request_id)

    def notify(self, method, params=None):
        message = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            message["params"] = params
        self.process.stdin.write(json.dumps(message) + "\n")
        self.process.stdin.flush()

    def initialize(self, name="cortex-bench", version="1"):
        response = self.request("initialize", {
            "protocolVersion": "2025-11-25", "capabilities": {},
            "clientInfo": {"name": name, "version": version}}, timeout=60)
        self.notify("notifications/initialized")
        return response

    def list_tools(self):
        tools, cursor = [], None
        while True:
            response = self.request("tools/list", {"cursor": cursor} if cursor else {}, timeout=60)
            result = response.get("result") or {}
            tools += result.get("tools", [])
            cursor = result.get("nextCursor")
            if not cursor:
                return tools

    def call_tool(self, name, arguments, timeout=120.0):
        return self.request("tools/call", {"name": name, "arguments": arguments}, timeout=timeout)

    def close(self):
        try:
            self.process.stdin.close()
        except Exception:
            pass
        try:
            self.process.wait(timeout=5)
        except Exception:
            self.process.kill()
