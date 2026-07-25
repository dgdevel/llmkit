#!/usr/bin/env python3
"""
Phase 12 agent integration test.

Spins up a mock OpenAI-compatible chat-completions HTTP endpoint and a
mock stdio MCP backend, then runs the real `llmkit agent` binary against
them and validates the conversation JSONL that llmkit writes:

  Scenario A (tool round-trip):
    turn 1: LLM requests a tool call  -> MCP backend returns "called:..."
    turn 2: LLM produces a final text answer (no tool calls)

  Expected JSONL entry order:
    meta, user, assistant(+tool_call), tool_result, assistant(final)

The mock LLM decides turn 1 vs turn 2 by inspecting whether the last
message role is "user" (-> request tool) or "tool" (-> final answer).

Exit code 0 = pass, 1 = fail.
"""
import json
import os
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.environ.get("BIN", os.path.join(ROOT, "llmkit"))
FIX = os.path.join(ROOT, "tests", "fixtures")

tests_run = 0
tests_failed = []
TOOL_CALL_ID = "call_abc123"


def check(name, cond, detail=""):
    global tests_run
    tests_run += 1
    if cond:
        print(f"  [ok] {name}")
    else:
        tests_failed.append(name)
        print(f"  [FAIL] {name}: {detail}")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


# ---------------------------------------------------------------------------
# Mock OpenAI-compatible chat completions server
# ---------------------------------------------------------------------------
class MockLLM(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass  # silence

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8", "replace")
        try:
            req = json.loads(body)
        except Exception:
            req = {}
        messages = req.get("messages", [])
        last_role = messages[-1].get("role", "") if messages else ""

        if last_role == "tool" or len(messages) >= 4:
            # Second turn: produce a final text answer, no tool calls.
            resp = {
                "id": "chat_final",
                "model": req.get("model", "mock"),
                "choices": [{
                    "index": 0,
                    "message": {"role": "assistant", "content": "Done. The time is known."},
                    "finish_reason": "stop",
                }],
                "usage": {"prompt_tokens": 10, "completion_tokens": 8, "total_tokens": 18},
            }
        else:
            # First turn: request exactly one tool call.
            resp = {
                "id": "chat_turn1",
                "model": req.get("model", "mock"),
                "choices": [{
                    "index": 0,
                    "message": {
                        "role": "assistant",
                        "content": None,
                        "tool_calls": [{
                            "id": TOOL_CALL_ID,
                            "type": "function",
                            "function": {
                                "name": "ns.get_time",
                                "arguments": '{"tz":"UTC"}',
                            },
                        }],
                    },
                    "finish_reason": "tool_calls",
                }],
                "usage": {"prompt_tokens": 5, "completion_tokens": 3, "total_tokens": 8},
            }
        data = json.dumps(resp).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def run_llm_mock(port, ready):
    httpd = ThreadingHTTPServer(("127.0.0.1", port), MockLLM)
    ready.set()
    httpd.serve_forever()


def main():
    print("=== test_agent_integration ===")

    port = free_port()
    ready = threading.Event()
    t = threading.Thread(target=run_llm_mock, args=(port, ready), daemon=True)
    t.start()
    ready.wait(5.0)

    tmp = "/tmp/llmkit_agent_itest"
    os.makedirs(tmp, exist_ok=True)
    convo = os.path.join(tmp, "convo.jsonl")
    if os.path.exists(convo):
        os.unlink(convo)

    cfg_path = os.path.join(tmp, "agent.yml")
    with open(cfg_path, "w") as f:
        f.write(
            f'llm:\n'
            f'  api_base: "http://127.0.0.1:{port}/v1"\n'
            f'  api_key: "test"\n'
            f'  model: "mock-model"\n'
            f'agent:\n'
            f'  system_prompt: "Be brief."\n'
            f'mcps:\n'
            f'  - name: ns\n'
            f'    cmdline: "python3 {FIX}/fake_mcp.py"\n'
            f'    init_timeout: "5s"\n'
            f'    call_timeout: "5s"\n'
        )

    proc = subprocess.run(
        [BIN, "agent", "-c", cfg_path, "-o", convo, "-p", "What time is it?"],
        capture_output=True, text=True, timeout=30, cwd=ROOT,
    )
    check("agent exit code 0", proc.returncode == 0,
          f"exit={proc.returncode} stderr={proc.stderr[-400:]}")

    if proc.returncode != 0:
        print(proc.stdout)
        print(proc.stderr)
        return 1

    # Parse the conversation JSONL.
    entries = []
    with open(convo) as f:
        for line in f:
            line = line.strip()
            if line:
                try:
                    entries.append(json.loads(line))
                except Exception as e:
                    check("jsonl line valid", False, f"bad JSON: {e}")
    types = [e.get("type") for e in entries]
    check("first entry is meta", types and types[0] == "meta", f"types={types}")
    check("entry sequence is correct",
          types == ["meta", "user", "assistant", "tool_call", "tool_result", "assistant"],
          f"types={types}")

    # Validate the user entry.
    user = next((e for e in entries if e.get("type") == "user"), {})
    check("user content preserved",
          user.get("content") == "What time is it?", f"user={user}")
    check("user source is cli", user.get("source") == "cli", f"user={user}")

    # Validate the tool_call entry.
    tc = next((e for e in entries if e.get("type") == "tool_call"), {})
    check("tool_call id matches LLM response",
          tc.get("id") == TOOL_CALL_ID, f"tc={tc}")
    check("tool_call name is namespaced",
          tc.get("name") == "ns.get_time", f"tc={tc}")
    check("tool_call mcp_server set",
          tc.get("mcp_server") == "ns", f"tc={tc}")

    # Validate the tool_result entry (backend echoes "called:get_time {...}").
    tr = next((e for e in entries if e.get("type") == "tool_result"), {})
    check("tool_result name matches tool_call",
          tr.get("name") == "ns.get_time", f"tr={tr}")
    check("tool_result references call_id",
          tr.get("call_id") == TOOL_CALL_ID, f"tr={tr}")
    check("tool_result is not error",
          tr.get("is_error") is False, f"tr={tr}")
    check("tool_result contains backend echo",
          "called:get_time" in (tr.get("result") or ""), f"tr={tr}")

    # Validate the final assistant entry.
    final_asst = [e for e in entries if e.get("type") == "assistant"]
    check("two assistant entries", len(final_asst) == 2, f"count={len(final_asst)}")
    if len(final_asst) == 2:
        check("final assistant has content",
              "Done" in (final_asst[1].get("content") or ""), f"asst={final_asst[1]}")
        check("final assistant has usage",
              "total_tokens" in (final_asst[1].get("usage") or {}), f"asst={final_asst[1]}")

    # Validate the meta entry.
    meta = entries[0] if entries else {}
    check("meta version is 1", meta.get("version") == 1, f"meta={meta}")
    check("meta config_hash present",
          meta.get("config_hash", "").startswith("sha256:"), f"meta={meta}")
    check("meta run_id is uuid", len(meta.get("run_id", "")) == 36, f"meta={meta}")

    print(f"\n{tests_run} checks, {len(tests_failed)} failed")
    return 1 if tests_failed else 0


if __name__ == "__main__":
    sys.exit(main())
