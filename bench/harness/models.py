"""Model access for the benchmark: a minimal Messages API client and a mock.

No SDK on purpose: the loop, the retries and the accounting are all visible in
this file. The API key is read from ANTHROPIC_API_KEY and never written to a log.
"""
import json
import os
import time
import urllib.error
import urllib.request

API_URL = "https://api.anthropic.com/v1/messages"
API_VERSION = "2023-06-01"


class ApiError(Exception):
    pass


class AnthropicModel:
    def __init__(self, model, temperature=0.0, max_output_tokens=4096, api_key=None, cache_prefix=True):
        self.model = model
        self.temperature = temperature
        self.max_output_tokens = max_output_tokens
        self.api_key = api_key or os.environ.get("ANTHROPIC_API_KEY")
        self.cache_prefix = cache_prefix
        self.retries = 0
        if not self.api_key:
            raise ApiError("ANTHROPIC_API_KEY is not set")

    def create(self, system, messages, tools):
        tool_defs = [dict(t) for t in tools]
        if self.cache_prefix and tool_defs:
            tool_defs[-1]["cache_control"] = {"type": "ephemeral"}
        body = {"model": self.model, "max_tokens": self.max_output_tokens,
                "temperature": self.temperature, "system": system, "messages": messages}
        if tool_defs:
            body["tools"] = tool_defs
        data = json.dumps(body).encode()
        last_error = None
        for attempt in range(6):
            request = urllib.request.Request(API_URL, data=data, method="POST", headers={
                "x-api-key": self.api_key, "anthropic-version": API_VERSION,
                "content-type": "application/json"})
            try:
                with urllib.request.urlopen(request, timeout=180) as response:
                    return json.loads(response.read())
            except urllib.error.HTTPError as error:
                payload = error.read().decode(errors="replace")[:500]
                last_error = "HTTP %s: %s" % (error.code, payload)
                if error.code not in (408, 409, 429, 500, 502, 503, 504, 529):
                    raise ApiError(last_error)
            except (urllib.error.URLError, TimeoutError, OSError) as error:
                last_error = "network: %s" % error
            self.retries += 1
            time.sleep(min(2 ** attempt, 30))
        raise ApiError("giving up after retries: %s" % last_error)


class MockModel:
    """Replays a script: a list of turns, each a list of {"name", "input"} tool calls
    (or {"text": ...}). Used to test the harness without spending anything."""

    model = "mock"
    retries = 0

    def __init__(self, script):
        self.script = list(script)
        self.turn = 0

    def create(self, system, messages, tools):
        if self.turn >= len(self.script):
            return {"stop_reason": "end_turn", "content": [{"type": "text", "text": "(script ended)"}],
                    "usage": {"input_tokens": 0, "output_tokens": 0}}
        step = self.script[self.turn]
        self.turn += 1
        content, uses = [], 0
        for item in step:
            if "text" in item:
                content.append({"type": "text", "text": item["text"]})
            else:
                uses += 1
                content.append({"type": "tool_use", "id": "mock_%d_%d" % (self.turn, uses),
                                "name": item["name"], "input": item.get("input", {})})
        return {"stop_reason": "tool_use" if uses else "end_turn", "content": content,
                "usage": {"input_tokens": 0, "output_tokens": 0}}
