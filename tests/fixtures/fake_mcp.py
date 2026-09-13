#!/usr/bin/env python3
"""
Minimal fake MCP server over stdio, used by the Phase 12 integration tests.

Speaks the 2025-11-25 MCP line-delicted JSON-RPC protocol:
  - initialize        -> protocolVersion + capabilities + serverInfo
  - notifications/*    -> no response
  - tools/list        -> a fixed list of tools (2 by default)
  - tools/call        -> echoes the tool name + arguments back as text
  - resources/list    -> one fixed resource (notes)
  - resources/read    -> text contents for the known resource, error otherwise
  - prompts/list      -> one fixed prompt (greet)
  - prompts/get       -> a message echoing the prompt arguments, error otherwise

Environment overrides:
  FAKE_MCP_TOOL_NAME   (default "get_time")
  FAKE_MCP_SLEEP_CALL   (seconds to sleep on tools/call, for timeout tests)
  FAKE_MCP_DIE_AFTER_INIT  (exit right after the initialize handshake,
                            simulating a backend that dies mid-session)
  FAKE_MCP_DIE_ON       (exit without responding when this method arrives,
                            simulating a backend dying on the next request)
"""
import json
import os
import sys
import time

TOOL_NAME = os.environ.get("FAKE_MCP_TOOL_NAME", "get_time")
SLEEP_CALL = float(os.environ.get("FAKE_MCP_SLEEP_CALL", "0"))
DIE_AFTER_INIT = bool(os.environ.get("FAKE_MCP_DIE_AFTER_INIT", ""))
DIE_ON = os.environ.get("FAKE_MCP_DIE_ON", "")

# CLI overrides (backend cmdlines are argv-split without a shell, so
# "FOO=bar python3 fake_mcp.py" cannot be used to pass environment):
#   --die-after-init     exit right after the initialize handshake
#   --die-on=METHOD      exit without responding when METHOD arrives
for arg in sys.argv[1:]:
    if arg == "--die-after-init":
        DIE_AFTER_INIT = True
    elif arg.startswith("--die-on="):
        DIE_ON = arg.split("=", 1)[1]


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
    if DIE_ON and method == DIE_ON:
        sys.exit(0)
    if method == "initialize":
        respond(req, {
            "protocolVersion": "2025-11-25",
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "fake", "version": "1.0"},
        })
        if DIE_AFTER_INIT:
            sys.exit(0)
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
    elif method == "resources/list":
        respond(req, {"resources": [
            {"name": "notes", "uri": "memo://notes/demo",
             "description": "Demo notes", "mimeType": "text/plain"},
        ]})
    elif method == "resources/read":
        uri = req.get("params", {}).get("uri", "")
        if uri == "memo://notes/demo":
            respond(req, {"contents": [
                {"uri": uri, "mimeType": "text/plain", "text": "hello from resource"}]})
        else:
            respond(req, error={"code": -32602,
                                "message": "Unknown resource: {}".format(uri)})
    elif method == "prompts/list":
        respond(req, {"prompts": [
            {"name": "greet", "description": "Greet someone",
             "arguments": [{"name": "who", "description": "Who to greet",
                            "required": True}]},
        ]})
    elif method == "prompts/get":
        name = req.get("params", {}).get("name", "")
        if name == "greet":
            who = req.get("params", {}).get("arguments", {}).get("who", "?")
            respond(req, {"description": "Greet someone", "messages": [
                {"role": "user", "content": {"type": "text",
                                             "text": "please greet {}".format(who)}}]})
        else:
            respond(req, error={"code": -32602,
                                "message": "Unknown prompt: {}".format(name)})
    elif method == "ping":
        respond(req, {})
