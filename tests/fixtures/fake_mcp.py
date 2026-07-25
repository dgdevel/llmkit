#!/usr/bin/env python3
"""
Minimal fake MCP server over stdio, used by the Phase 12 integration tests.

Speaks the 2025-11-25 MCP line-delicted JSON-RPC protocol:
  - initialize        -> protocolVersion + capabilities + serverInfo
  - notifications/*    -> no response
  - tools/list        -> a fixed list of tools (2 by default)
  - tools/call        -> echoes the tool name + arguments back as text

Environment overrides:
  FAKE_MCP_TOOL_NAME   (default "get_time")
  FAKE_MCP_SLEEP_CALL   (seconds to sleep on tools/call, for timeout tests)
"""
import json
import os
import sys
import time

TOOL_NAME = os.environ.get("FAKE_MCP_TOOL_NAME", "get_time")
SLEEP_CALL = float(os.environ.get("FAKE_MCP_SLEEP_CALL", "0"))


def respond(req, result=None, error=None):
    rid = req.get("id")
    if rid is None:
        return  # notifications carry no id -> no response
    msg = {"jsonrpc": "2.0", "id": rid}
    if error is not None:
        msg["error"] = error
    else:
        msg["result"] = result if result is not None else {}
    sys.stdout.write(json.dumps(msg) + "\n")
    sys.stdout.flush()


for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    try:
        req = json.loads(line)
    except Exception:
        continue
    method = req.get("method", "")
    if method == "initialize":
        respond(req, {
            "protocolVersion": "2025-11-25",
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "fake", "version": "1.0"},
        })
    elif method.startswith("notifications/"):
        continue
    elif method == "tools/list":
        respond(req, {"tools": [
            {"name": TOOL_NAME, "description": "Get the current time",
             "inputSchema": {"type": "object"}},
            {"name": "echo", "description": "Echo back the input",
             "inputSchema": {"type": "object"}},
        ]})
    elif method == "tools/call":
        if SLEEP_CALL > 0:
            time.sleep(SLEEP_CALL)
        name = req.get("params", {}).get("name", "")
        args = req.get("params", {}).get("arguments", {})
        respond(req, {"content": [{"type": "text",
                                   "text": "called:{} {}".format(name, json.dumps(args))}]})
    elif method == "ping":
        respond(req, {})
