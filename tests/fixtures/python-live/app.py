# Live fixture for the DAP provider (tests/dap_tests.cpp): the same rule as
# test_target's CortexDemoApplyDamage. Started with
#   python -m debugpy --listen 127.0.0.1:<port> app.py
import time

actor = {"name": "demo", "health": 100, "armor": 25, "hits": 0}


def apply_damage(target, amount):
    absorbed = min(amount // 3, target["armor"]) if target["armor"] > 0 else 0
    target["armor"] = target["armor"] - absorbed
    target["health"] = target["health"] - (amount - absorbed)  # CORTEX_HEALTH_WRITE
    target["hits"] = target["hits"] + 1
    return target["health"]


ticks = 0
while True:
    ticks += 1
    apply_damage(actor, 9)
    if actor["health"] <= 0:
        actor["health"] = 100
        actor["armor"] = 25
    # A stop Cortex did not ask for: it must be recorded and continued.
    if ticks % 50 == 0:
        breakpoint()
    print("tick %d health %d armor %d" % (ticks, actor["health"], actor["armor"]), flush=True)
    time.sleep(0.02)
