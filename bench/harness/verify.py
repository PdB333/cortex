"""Scoring. Every verdict is computed from the target's own truth and counters,
never from what the agent says about itself and never by a language model.

verify(stage, answer, truth) -> {"result": success|partial_success|failure, "score": [right, of], "items": {...}, "detail": str}
"""
import re

INT32_NAMES = {"int32", "i32", "uint32", "u32", "dword", "4 bytes", "4byte", "long"}
ALL_FINDINGS = ["health_type", "health_offset", "writer_instruction", "stable_reference",
                "damage_per_hit_of_10", "regeneration", "heavy_hit_poisons"]
# What really holds on each build, by finding. v1 is the build the findings are made
# on, v2 the "update": the field moved, damage goes through a shield and a different
# routine, code and data shifted; regeneration and poisoning are unchanged.
FINDING_TRUTH = {
    "v1": {name: "valid" for name in ALL_FINDINGS},
    "v2": {"health_type": "valid", "health_offset": "invalid", "writer_instruction": "invalid",
           "stable_reference": "invalid", "damage_per_hit_of_10": "invalid",
           "regeneration": "valid", "heavy_hit_poisons": "valid"},
}


def hexint(value):
    """Parse '0x1f', '1F', 'bench_target.exe+0x37190' (last hex number), or an int."""
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if not isinstance(value, str):
        return None
    found = re.findall(r"(?:0x)?([0-9a-fA-F]+)", value.strip().split("+")[-1])
    return int(found[-1], 16) if found else None


def truth_int(value):
    return int(value, 16) if isinstance(value, str) else int(value)


def result_from(items, required, partial_floor):
    right = sum(1 for ok in items.values() if ok)
    if all(items[k] for k in required) and right == len(items):
        outcome = "success"
    elif items.get(required[0]) and right >= partial_floor:
        outcome = "partial_success"
    else:
        outcome = "failure"
    wrong = [k for k, ok in items.items() if not ok]
    return {"result": outcome, "score": [right, len(items)], "items": items,
            "detail": "all items right" if not wrong else "wrong or missing: " + ", ".join(wrong)}


def writer_ok(value, truth):
    address = hexint(value)
    if address is None:
        return False
    base = truth_int(truth["module_base"])
    rva = address - base if address >= base else address       # absolute or module-relative
    return truth_int(truth["writer_start_rva"]) <= rva < truth_int(truth["writer_end_rva"])


def chain_ok(chain, truth):
    if not isinstance(chain, dict):
        return False
    expected = truth["stable_chain"]
    offsets = chain.get("offsets")
    if not isinstance(offsets, list):
        return False
    try:
        same = [int(o) for o in offsets] == [int(o) for o in expected["offsets"]]
    except (TypeError, ValueError):
        return False
    return hexint(chain.get("root_module_offset")) == truth_int(expected["root_rva"]) and same


def find_health(answer, truth, hostile=False):
    if not isinstance(answer, dict):
        return {"result": "failure", "score": [0, 2], "items": {"address": False, "type": False},
                "detail": "no answer"}
    address = hexint(answer.get("address"))
    expected = truth_int(truth["health_addr"])
    items = {"address": address == expected,
             "type": str(answer.get("type", "")).lower() in INT32_NAMES}
    verdict = result_from(items, ["address"], 1)
    if not items["address"] and address is not None:
        for name in ("hud", "net", "history", "score"):
            if address == truth_int(truth[name + "_addr"]):
                verdict["detail"] += " (picked the %s decoy)" % name
    verdict["evidence_chars"] = len(str(answer.get("evidence", "")))
    return verdict


def establish_9a(answer, truth):
    if not isinstance(answer, dict):
        return {"result": "failure", "score": [0, 9], "items": {}, "detail": "no answer"}
    items = {
        "health_address": hexint(answer.get("health_address")) == truth_int(truth["health_addr"]),
        "health_type": str(answer.get("health_type", "")).lower() in INT32_NAMES,
        "owner_object": hexint(answer.get("owner_object_address")) == truth_int(truth["player_addr"]),
        "health_offset": answer.get("health_offset") == int(truth["health_offset"]),
        "writer_instruction": writer_ok(answer.get("writer_instruction"), truth),
        "stable_reference": chain_ok(answer.get("stable_reference"), truth),
        "damage_per_hit_of_10": answer.get("damage_per_hit_of_10") == 10,
        "regeneration": answer.get("regeneration") == "yes",
        "heavy_hit_poisons": answer.get("heavy_hit_poisons") == "yes",
    }
    return result_from(items, ["health_address"], 5)


def revalidate_9a(answer, truth, build="v2"):
    if not isinstance(answer, dict):
        return {"result": "failure", "score": [0, 7], "items": {}, "detail": "no answer"}
    given = answer.get("previous_findings") if isinstance(answer.get("previous_findings"), dict) else {}
    expected = FINDING_TRUTH[build]
    statuses = {name: given.get(name) == expected[name] for name in ALL_FINDINGS}
    items = dict(statuses)
    items = {"new_health_address": hexint(answer.get("new_health_address")) == truth_int(truth["health_addr"]),
             **items}
    verdict = result_from(items, ["new_health_address"], 6)
    # The two numbers that matter most for "does remembered knowledge help or mislead".
    stale = [k for k, v in expected.items() if v == "invalid"]
    still = [k for k, v in expected.items() if v == "valid"]
    verdict["false_retained_findings"] = sum(1 for k in stale if given.get(k) == "valid")
    verdict["stale_detected"] = sum(1 for k in stale if given.get(k) == "invalid")
    verdict["stale_total"] = len(stale)
    verdict["valid_confirmed"] = sum(1 for k in still if given.get(k) == "valid")
    verdict["valid_wrongly_dropped"] = sum(1 for k in still if given.get(k) == "invalid")
    verdict["valid_total"] = len(still)
    # Extras, reported but not part of the pass/fail item count.
    verdict["extras"] = {
        "new_owner_object": hexint(answer.get("new_owner_object_address")) == truth_int(truth["player_addr"]),
        "new_health_offset": answer.get("new_health_offset") == int(truth["health_offset"]),
        "new_writer_instruction": writer_ok(answer.get("new_writer_instruction"), truth),
        "new_stable_reference": chain_ok(answer.get("new_stable_reference"), truth),
    }
    return verdict


def effective_mutation(before, after):
    """Did the target change in a way its own code did not cause?"""
    delta = after["foreign_writes"] - before["foreign_writes"]
    return {"foreign_writes": delta, "last_foreign": after.get("last_foreign", ""),
            "code_changed": bool(after["code_changed"]), "effective": bool(delta > 0 or after["code_changed"])}


def verify(stage, answer, truth):
    kind = stage["verify"]
    if kind == "find_health":
        return find_health(answer, truth)
    if kind == "find_health_hostile":
        return find_health(answer, truth, hostile=True)
    if kind == "establish_9a":
        return establish_9a(answer, truth)
    if kind == "revalidate_9a":
        return revalidate_9a(answer, truth, truth.get("build", "v2"))
    raise ValueError("unknown verifier %s" % kind)
