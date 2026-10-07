#!/usr/bin/env python3
"""Stand-in for Cheat Engine's side of the bridge, for harness self-tests only.

The real bridge is a Lua script inside Cheat Engine behind a named pipe; `ce_tcp_relay.py` exposes it on TCP with
length-prefixed JSON-RPC frames. This listens on TCP with the same framing and answers a few methods with canned
replies, so the pinned Python MCP server (`mcp_cheatengine.py`, CE_MCP_TRANSPORT=tcp) can be driven by the
proxy without Cheat Engine. It proves the harness path, not Cheat Engine's behaviour.
"""
import json
import socketserver
import struct
import sys

CANNED = {
    "ping": {"success": True, "version": "12.0.0", "message": "CE MCP Bridge Active (fake)"},
    "open_process": {"success": True, "process_id": 0},
    "get_process_info": {"success": True, "process_id": 0, "process_name": "fake"},
}


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        while True:
            header = self.request.recv(4)
            if len(header) < 4:
                return
            size = struct.unpack("<I", header)[0]
            body = b""
            while len(body) < size:
                chunk = self.request.recv(size - len(body))
                if not chunk:
                    return
                body += chunk
            request = json.loads(body)
            result = CANNED.get(request.get("method"), {"success": False, "error": "fake bridge: not implemented",
                                                        "method": request.get("method")})
            payload = json.dumps({"jsonrpc": "2.0", "id": request.get("id"), "result": result}).encode()
            self.request.sendall(struct.pack("<I", len(payload)) + payload)


if __name__ == "__main__":
    socketserver.ThreadingTCPServer.allow_reuse_address = True
    with socketserver.ThreadingTCPServer(("127.0.0.1", int(sys.argv[1])), Handler) as server:
        server.serve_forever()
