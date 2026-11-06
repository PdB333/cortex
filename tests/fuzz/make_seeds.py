"""Build the seed corpora of the fuzzers from the repository's fixtures.

usage: python make_seeds.py OUT_DIR

fuzz_artifact inputs start with one byte that picks the file name (see
fuzz_artifact.cpp); fuzz_text inputs start with one byte that picks the
decoder (see fuzz_text.cpp).
"""
import base64
import glob
import json
import os
import sys
import zlib

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
NAMES = {"bin": 0, "dll": 1, "exe": 2, "asar": 3, "js": 4, "map": 5, "zip": 7, "jar": 8, "ts": 9}


def put(out, corpus, name, data):
    os.makedirs(os.path.join(out, corpus), exist_ok=True)
    with open(os.path.join(out, corpus, name), "wb") as handle:
        handle.write(data)


def main():
    out = sys.argv[1]
    fixtures = os.path.join(ROOT, "tests", "fixtures")
    files = [
        (os.path.join(fixtures, "electron-app.asar"), "asar"),
        (os.path.join(fixtures, "dotnet", "Cortex.Fixture.dll"), "dll"),
        (os.path.join(fixtures, "java", "Cortex.Fixture.jar"), "jar"),
        (os.path.join(fixtures, "elf", "actor.elf"), "bin"),
    ]
    files += [(p, "exe") for p in glob.glob(os.path.join(ROOT, "tests", "fuzz", "seeds", "pe", "*.exe"))]
    files += [(p, "js") for p in sorted(glob.glob(os.path.join(fixtures, "**", "*.js"), recursive=True))[:20]]
    files += [(p, "ts") for p in sorted(glob.glob(os.path.join(fixtures, "**", "*.ts"), recursive=True))[:5]]
    files += [(p, "map") for p in sorted(glob.glob(os.path.join(fixtures, "**", "*.map"), recursive=True))[:5]]
    for index, (path, kind) in enumerate(files):
        if not os.path.exists(path):
            continue
        data = open(path, "rb").read()
        put(out, "artifact", "seed%d" % index, bytes([NAMES[kind]]) + data)
        if kind in ("dll", "exe") and len(data) <= 65536:
            put(out, "pe_code", "seed%d" % index, data)
        if kind in ("js", "ts"):
            put(out, "text", "js%d" % index, bytes([0 if kind == "js" else 4]) + data[:200000])
        if kind == "map":
            put(out, "text", "map%d" % index, bytes([1]) + data)
    put(out, "text", "inflate", bytes([3]) + zlib.compress(b"hello cortex " * 64)[2:-4])
    put(out, "text", "dataurl", bytes([2]) + b"data:application/json;base64," +
        base64.b64encode(b'{"version":3,"sources":["a.ts"],"mappings":"AAAA"}'))
    put(out, "text", "map", bytes([1]) + json.dumps(
        {"version": 3, "sources": ["a.ts"], "names": [], "mappings": "AAAA;AACA,CAAC"}).encode())
    # Live protocols (fuzz_protocols): mode byte, then the stream.
    def ws_frame(payload, opcode=1, fin=True):
        head = bytes([(0x80 if fin else 0) | opcode])
        n = len(payload)
        if n < 126:
            head += bytes([n])
        elif n < 65536:
            head += bytes([126, n >> 8, n & 255])
        else:
            head += bytes([127]) + n.to_bytes(8, "big")
        return head + payload
    paused = json.dumps({"method": "Debugger.paused", "params": {"reason": "other", "hitBreakpoints": ["1:11:0"],
        "callFrames": [{"functionName": "applyDamage", "location": {"scriptId": "42", "lineNumber": 11},
                        "scopeChain": [{"type": "local", "object": {"objectId": "1"}}]}]}}).encode()
    put(out, "protocols", "ws", bytes([0]) + ws_frame(paused[:40], fin=False) + ws_frame(paused[40:], opcode=0) +
        ws_frame(b"ping", opcode=9) + ws_frame(json.dumps({"id": 3, "result": {"result": []}}).encode()))
    def dap(message):
        body = json.dumps(message).encode()
        return b"Content-Length: %d\r\n\r\n" % len(body) + body
    put(out, "protocols", "dap", bytes([1]) + dap({"seq": 1, "type": "response", "request_seq": 1, "success": True,
        "body": {"supportsConfigurationDoneRequest": True}}) + dap({"seq": 2, "type": "event", "event": "stopped",
        "body": {"reason": "breakpoint", "threadId": 1, "hitBreakpointIds": [1]}}) + dap({"type": "event",
        "event": "output", "body": {"category": "stdout", "output": "h\u00e9llo \u4e16\u754c\n"}}))
    def jdwp(packet_id, flags, a, b, data):
        return (11 + len(data)).to_bytes(4, "big") + packet_id.to_bytes(4, "big") + bytes([flags, a, b]) + data
    put(out, "protocols", "jdwp", bytes([2]) + jdwp(1, 0x80, 0, 0, b"\x00\x00\x00\x08" * 5) +
        jdwp(7, 0, 64, 100, b"\x02\x00\x00\x00\x01\x02\x00\x00\x00\x05" + b"\x00" * 33))
    put(out, "protocols", "text", bytes([3]) + "Actor h\u00e9 \u4e16 \U0001F600".encode() + b"\xc0\x80\xed\xa0\x80\xff")

    # Records on disk and knowledge input (fuzz_stores): mode byte, then the file.
    evidence = {"schema": "cortex.evidence.v1", "id": "ev_00000000000000000000", "revision": 1, "created_ms": 1,
                "class": "hypothesis", "origin": "static", "statement": "CortexDemoApplyDamage writes health",
                "subject": {"sha256": "0" * 64, "identity": "client_asserted"}, "references": [], "relations": [],
                "stale": False, "history": [], "verification": {"status": "not_checked", "trials": []}}
    put(out, "stores", "evidence", bytes([0]) + json.dumps(evidence).encode())
    step = {"title": "Inspect", "tool": "cortex_artifact_inspect", "capability": "static", "effect": "read",
            "status": "pending", "runner": "agent", "arguments": {}, "index": 0}
    investigation = {"schema": "cortex.investigation.v1", "id": "inv_00000000000000000000", "revision": 1,
                     "state": "planned", "title": "t", "question": "q", "workflow": "custom", "plan": [step],
                     "limits": {"max_steps": 32, "max_live_steps": 0, "deadline_minutes": 60}, "deadline_ms": 1,
                     "live_steps_approved": 0, "evidence": [], "created_ms": 1, "journal": []}
    put(out, "stores", "investigation", bytes([1]) + json.dumps(investigation).encode())
    put(out, "stores", "knowledge", bytes([2]) + json.dumps({"id": "demo.health", "revision": 1}).encode() + b"\n" +
        json.dumps({"id": "demo.health", "kind": "fact", "statement": "s", "status": "observed", "evidence": [],
                    "expected_revision": 1}).encode())

    for corpus in sorted(os.listdir(out)):
        print(corpus, len(os.listdir(os.path.join(out, corpus))))


if __name__ == "__main__":
    main()
