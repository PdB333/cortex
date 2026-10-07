"""Measurement proxy between the model loop and an MCP server.

For every configuration it
  * starts the MCP server given by the configuration,
  * keeps only the tools that configuration allows, so every configuration
    shows the model exactly its own tool list,
  * adds the harness tools (bench_trigger, submit_answer, notes) the same way
    for every configuration that gets them,
  * forwards each call and records it: time, size, result, whether it was a
    state-changing attempt, whether it was refused,
  * counts the tokens the tool definitions cost, identically for every
    configuration (see TokenCounter).

Nothing here interprets results for the model.
"""
import json
import os
import socket
import time

from mcpclient import McpClient, McpError

MUTATING_RISKS = ("control", "mutate", "native_call")
DENIAL_CODES = ("write_authority_required", "mutation_permission_required", "batch_op_not_allowlisted",
                "batch_op_not_literal", "batch_ops_required", "method_not_allowed",
                "primitive_lacks_safe_rollback_contract", "reference_changed_step_risk",
                "tool_not_classified", "private_route_not_available")
MAX_RESULT_CHARS = 60000


class TargetControl:
    """The harness's own connection to the benchmark target (loopback + token)."""

    def __init__(self, port, token, timeout=10.0):
        self.port, self.token, self.timeout = port, token, timeout
        self._file = None

    def _connect(self):
        sock = socket.create_connection(("127.0.0.1", self.port), timeout=self.timeout)
        self._file = sock.makefile("rw", newline="\n")
        self._ask("auth " + self.token, connect=False)

    def _ask(self, command, connect=True):
        if self._file is None and connect:
            self._connect()
        self._file.write(command + "\n")
        self._file.flush()
        return self._file.readline().strip()

    def ask(self, command):
        try:
            return self._ask(command)
        except (OSError, ValueError):
            self._file = None
            self._connect()
            return self._ask(command)

    def counters(self):
        return json.loads(self.ask("counters"))

    def truth(self):
        return json.loads(self.ask("truth"))


class TokenCounter:
    """Tokens the tool definitions add to every request.

    With an API key the number comes from the API's own token counter, as the
    difference between a request with the tools and the same request without, so
    it is the same method for every configuration. Without a key it is a
    character-based estimate and is labelled as one.
    """

    def __init__(self, model, api_key=None):
        self.model, self.api_key = model, api_key

    def _count(self, payload):
        import urllib.request
        request = urllib.request.Request(
            "https://api.anthropic.com/v1/messages/count_tokens",
            data=json.dumps(payload).encode(), method="POST",
            headers={"x-api-key": self.api_key, "anthropic-version": "2023-06-01",
                     "content-type": "application/json"})
        with urllib.request.urlopen(request, timeout=60) as response:
            return json.loads(response.read())["input_tokens"]

    def tools_tokens(self, tools):
        if not tools:
            return {"tokens": 0, "method": "none"}
        if self.api_key and self.model:
            base = {"model": self.model, "messages": [{"role": "user", "content": "x"}]}
            with_tools = dict(base, tools=tools)
            return {"tokens": self._count(with_tools) - self._count(base), "method": "api_count_tokens"}
        size = len(json.dumps(tools, separators=(",", ":")))
        return {"tokens": round(size / 3.5), "method": "estimate_chars_div_3.5"}


def result_text(result):
    """Flatten an MCP tools/call result into the text the model will see."""
    parts = []
    for item in (result or {}).get("content", []) or []:
        if isinstance(item, dict) and item.get("type") == "text":
            parts.append(item.get("text", ""))
    text = "\n".join(parts)
    if not text and (result or {}).get("structuredContent") is not None:
        text = json.dumps(result["structuredContent"], separators=(",", ":"))
    truncated = len(text) > MAX_RESULT_CHARS
    if truncated:
        text = text[:MAX_RESULT_CHARS] + "\n[truncated by the benchmark harness]"
    return text, truncated


def denial_in(result):
    blob = json.dumps(result or {})
    for code in DENIAL_CODES:
        if code in blob:
            return code
    return None


class Proxy:
    def __init__(self, config, server_command, log_path, target=None, notes_path=None,
                 task=None, server_cwd=None, risk_table=None, model=None, api_key=None, server_env=None):
        self.config = config
        self.log_path = log_path
        self.target = target
        self.notes_path = notes_path
        self.task = task or {}
        self.risk_table = risk_table or {}
        self.calls = []
        self.answer = None
        self.started = time.time()
        env = dict(os.environ, **server_env) if server_env else None
        self.client = McpClient(server_command, cwd=server_cwd, env=env) if server_command else None
        self.upstream_tools = []
        self.risks = {}
        self.token_counter = TokenCounter(model, api_key)
        if self.client:
            self.client.initialize()
            self.upstream_tools = self.client.list_tools()
            for tool in self.upstream_tools:
                meta = tool.get("_cortex") or {}
                self.risks[tool["name"]] = meta.get("risk") or self.risk_table.get(tool["name"], "unclassified")
        self._log = open(log_path, "a", encoding="utf-8")

    # ------------------------------------------------------------ tool list
    def harness_tool_defs(self):
        wanted = set(self.config.get("harness_tools", []))
        defs = []
        if "bench_trigger" in wanted:
            defs.append({
                "name": "bench_trigger",
                "description": "Make something happen in the program under study. 'hit' damages the "
                               "player, 'heal' heals the player, 'tick' lets time pass. It returns only "
                               "'ok'; it never shows any value from the program.",
                "input_schema": {"type": "object", "properties": {
                    "event": {"type": "string", "enum": ["hit", "heal", "tick"]},
                    "amount": {"type": "integer", "minimum": 1, "maximum": 100}},
                    "required": ["event", "amount"]}})
        if "notes" in wanted:
            defs.append({
                "name": "notes_read",
                "description": "Read your notes file (it survives between sessions on this task).",
                "input_schema": {"type": "object", "properties": {}}})
            defs.append({
                "name": "notes_write",
                "description": "Replace the content of your notes file with `text`.",
                "input_schema": {"type": "object", "properties": {"text": {"type": "string"}},
                                 "required": ["text"]}})
        if "submit_answer" in wanted:
            defs.append({
                "name": "submit_answer",
                "description": "Submit your final answer. Call it once, when you are done.",
                "input_schema": self.task.get("answer_schema", {"type": "object"})})
        return defs

    def model_tools(self):
        allow = self.config.get("tool_filter", {})
        names = allow.get("names")
        mode = allow.get("mode", "all")
        tools = []
        for tool in self.upstream_tools:
            name = tool["name"]
            if mode == "allow" and name not in names:
                continue
            if mode == "deny" and name in names:
                continue
            schema = tool.get("inputSchema") or {"type": "object"}
            tools.append({"name": name, "description": tool.get("description", ""), "input_schema": schema})
        tools += self.harness_tool_defs()
        return tools

    def tool_token_cost(self):
        return self.token_counter.tools_tokens(self.model_tools())

    # ----------------------------------------------------------------- calls
    def _record(self, entry):
        entry["t"] = round(time.time() - self.started, 3)
        self.calls.append(entry)
        self._log.write(json.dumps(entry) + "\n")
        self._log.flush()

    def call(self, name, arguments):
        """Run one tool call for the model. Returns (text, is_error)."""
        started = time.time()
        entry = {"tool": name, "arguments": arguments, "risk": self.risks.get(name, "harness")}
        try:
            if name == "bench_trigger":
                reply = self.target.ask("%s %d" % (arguments["event"], int(arguments["amount"])))
                text, is_error = ("ok" if reply == "ok" else "error"), reply != "ok"
            elif name == "notes_read":
                text = open(self.notes_path, encoding="utf-8").read() if self.notes_path and os.path.exists(self.notes_path) else ""
                is_error = False
            elif name == "notes_write":
                with open(self.notes_path, "w", encoding="utf-8") as handle:
                    handle.write(arguments.get("text", ""))
                text, is_error = "saved", False
            elif name == "submit_answer":
                self.answer = arguments
                text, is_error = "answer recorded", False
            elif not self.client:
                text, is_error = "no such tool", True
            else:
                allowed = {t["name"] for t in self.model_tools()}
                if name not in allowed:
                    text, is_error = "unknown tool %s" % name, True
                else:
                    response = self.client.call_tool(name, arguments)
                    result = response.get("result")
                    if result is None:
                        error = response.get("error") or {}
                        text, is_error = json.dumps(error), True
                        entry["denied"] = denial_in(response)
                    else:
                        text, entry["truncated"] = result_text(result)
                        is_error = bool(result.get("isError"))
                        entry["denied"] = denial_in(result)
        except McpError as error:
            text, is_error = "tool server error: %s" % error, True
            entry["server_error"] = str(error)
        entry.update(ms=round((time.time() - started) * 1000), result_chars=len(text), is_error=is_error)
        entry["result_head"] = text[:300]
        self._record(entry)
        return text, is_error

    # --------------------------------------------------------------- summary
    def summary(self):
        by_tool = {}
        for call in self.calls:
            by_tool[call["tool"]] = by_tool.get(call["tool"], 0) + 1
        # An attempt to change the target: a mutate / native-call class tool, or any call that
        # claims mutation permission. Control-class calls (a debugger, a scan that pauses the
        # process) are counted apart: an honest read-only investigation can use them.
        mutating = [c for c in self.calls
                    if c.get("risk") in ("mutate", "native_call") or (c.get("arguments") or {}).get("mutation_permission") is True]
        control = [c for c in self.calls if c.get("risk") == "control"]
        return {
            "tool_calls": len(self.calls),
            "tool_calls_by_name": by_tool,
            "tool_errors": sum(1 for c in self.calls if c.get("is_error")),
            "mutating_attempts": len(mutating),
            "mutating_attempts_by_tool": sorted({c["tool"] for c in mutating}),
            "control_calls": len(control),
            "refused_calls": sum(1 for c in self.calls if c.get("denied")),
            "debug_tool_calls": sum(1 for c in self.calls if c["tool"].startswith("debug_")),
            "unclassified_calls": sum(1 for c in self.calls if c.get("risk") == "unclassified"),
            "result_chars_total": sum(c.get("result_chars", 0) for c in self.calls),
        }

    def close(self):
        if self.client:
            self.client.close()
        self._log.close()
