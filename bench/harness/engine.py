"""The agent loop. Home-made and small on purpose.

One run = one task on one configuration. The loop calls the model, executes the
tool calls it asks for through the proxy, and stops when the model submits an
answer, ends its turn, or hits a cap. A cap is a failure with root cause
"budget"; it is never a silent truncation.
"""
import json
import time

from models import ApiError

SYSTEM_PROMPT = (
    "You are an agent that works on a program running on a Windows machine. "
    "You can only act through the tools you are given. When you have finished, call "
    "submit_answer once with your answer. Be concise."
)


def usage_total(usage):
    return (usage.get("input_tokens", 0) + usage.get("output_tokens", 0) +
            usage.get("cache_creation_input_tokens", 0) + usage.get("cache_read_input_tokens", 0))


def run_agent(model, proxy, user_prompt, caps, transcript_path, system=SYSTEM_PROMPT):
    """Returns a dict: status, root_cause, turns, usage, wall_s, answer."""
    tools = proxy.model_tools()
    messages = [{"role": "user", "content": user_prompt}]
    totals = {"input_tokens": 0, "output_tokens": 0, "cache_creation_input_tokens": 0,
              "cache_read_input_tokens": 0}
    started = time.time()
    outcome = {"status": "failure", "root_cause": None, "turns": 0}
    transcript = open(transcript_path, "w", encoding="utf-8")

    def log(kind, payload):
        transcript.write(json.dumps({"t": round(time.time() - started, 3), "kind": kind, **payload}) + "\n")
        transcript.flush()

    log("user", {"text": user_prompt})
    try:
        while True:
            if time.time() - started > caps["max_wall_s"]:
                outcome["root_cause"] = "budget:time"
                break
            if usage_total(totals) >= caps["max_total_tokens"]:
                outcome["root_cause"] = "budget:tokens"
                break
            if len(proxy.calls) >= caps["max_tool_calls"]:
                outcome["root_cause"] = "budget:tool_calls"
                break
            if outcome["turns"] >= caps["max_turns"]:
                outcome["root_cause"] = "budget:turns"
                break
            try:
                response = model.create(system, messages, tools)
            except ApiError as error:
                outcome["root_cause"] = "harness:api_error"
                outcome["detail"] = str(error)
                break
            outcome["turns"] += 1
            for key in totals:
                totals[key] += (response.get("usage") or {}).get(key, 0) or 0
            content = response.get("content", [])
            messages.append({"role": "assistant", "content": content})
            log("assistant", {"content": content, "usage": response.get("usage")})

            stop = response.get("stop_reason")
            uses = [block for block in content if block.get("type") == "tool_use"]
            if stop == "refusal":
                # The model's safety classifiers declined. This is an outcome to report, not something
                # to route around: no fallback model is used, so every run stays on the frozen model.
                category = (response.get("stop_details") or {}).get("category")
                outcome["root_cause"] = "refusal" + (":%s" % category if category else "")
                break
            if stop == "max_tokens":
                outcome["root_cause"] = "output_truncated"
                break
            if not uses:
                outcome["root_cause"] = "no_answer"
                break
            results = []
            for block in uses:
                text, is_error = proxy.call(block["name"], block.get("input") or {})
                results.append({"type": "tool_result", "tool_use_id": block["id"],
                                "content": text, "is_error": is_error})
                log("tool", {"name": block["name"], "input": block.get("input"),
                             "is_error": is_error, "result_head": text[:500]})
                if len(proxy.calls) >= caps["max_tool_calls"] and proxy.answer is None:
                    break
            messages.append({"role": "user", "content": results})
            if proxy.answer is not None:
                outcome["status"] = "answered"
                break
    finally:
        transcript.close()
    outcome["usage"] = totals
    outcome["usage"]["processed_tokens"] = usage_total(totals)
    outcome["wall_s"] = round(time.time() - started, 2)
    outcome["answer"] = proxy.answer
    outcome["api_retries"] = getattr(model, "retries", 0)
    return outcome
