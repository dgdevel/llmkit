#!/usr/bin/env python3
"""
Anthropic-provider agent integration test.

Spins up a mock Anthropic Messages API HTTP endpoint and a mock stdio MCP
backend, then runs the real `llmkit agent` binary with `llm.provider:
"anthropic"` and validates both the wire requests and the conversation
JSONL that llmkit writes:

  Scenario A (tool round-trip):
    turn 1: LLM returns a tool_use block  -> MCP backend returns "called:..."
    turn 2: LLM returns a final text block

  Expected JSONL entry order:
    meta, user, assistant(+tool_call), tool_result, assistant(final)

The mock decides turn 1 vs turn 2 by checking whether the request's last
message carries a tool_result block.

Exit code 0 = pass, 1 = fail.
"""
import json
import os
import socket
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.environ.get("BIN", os.path.join(ROOT, "llmkit"))
FIX = os.path.join(ROOT, "tests", "fixtures")

tests_run = 0
tests_failed = []
TOOL_USE_ID = "toolu_mock_01"
API_KEY = "test-key"


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


def blocks_of(message):
    """Normalize an Anthropic message's content to a list of blocks."""
    content = message.get("content")
    if isinstance(content, list):
        return content
    return []


def last_message(req):
    msgs = req.get("messages", [])
    return msgs[-1] if msgs else {}


# ---------------------------------------------------------------------------
# Mock Anthropic Messages API server
# ---------------------------------------------------------------------------
class MockAnthropic(BaseHTTPRequestHandler):
    # Raw request paths, headers and bodies received by this server.
    requests = []  # list of (path, headers, body_dict)
    lock = threading.Lock()

    def log_message(self, *a):
        pass  # silence

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length).decode("utf-8", "replace")
        try:
            req = json.loads(raw)
        except Exception:
            req = {}
        with MockAnthropic.lock:
            MockAnthropic.requests.append((self.path, dict(self.headers), req))

        has_tool_result = any(
            b.get("type") == "tool_result" for b in blocks_of(last_message(req))
        )

        if has_tool_result:
            # Second turn: final text answer.
            resp = {
                "id": "msg_final",
                "type": "message",
                "role": "assistant",
                "model": req.get("model", "claude-mock"),
                "content": [{"type": "text", "text": "Done. The time is known."}],
                "stop_reason": "end_turn",
                "stop_sequence": None,
                "usage": {
                    "input_tokens": 120,
                    "output_tokens": 8,
                    "cache_read_input_tokens": 80,
                    "cache_creation_input_tokens": 20,
                },
            }
        else:
            # First turn: request exactly one tool call.
            resp = {
                "id": "msg_turn1",
                "type": "message",
                "role": "assistant",
                "model": req.get("model", "claude-mock"),
                "content": [
                    {
                        "type": "tool_use",
                        "id": TOOL_USE_ID,
                        "name": "ns.get_time",
                        "input": {"tz": "UTC"},
                    }
                ],
                "stop_reason": "tool_use",
                "stop_sequence": None,
                "usage": {"input_tokens": 5, "output_tokens": 3},
            }

        data = json.dumps(resp).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def run_llm_mock(port, ready):
    httpd = ThreadingHTTPServer(("127.0.0.1", port), MockAnthropic)
    ready.set()
    httpd.serve_forever()


def messages_span(req):
    """Serialized 'messages' array items of a request body (compact JSON,
    excluding the surrounding brackets), so a later turn's span can be
    prefix-compared against an earlier turn's span."""
    s = json.dumps(req.get("messages", []), separators=(",", ":"))
    return s[1:-1] if s.startswith("[") and s.endswith("]") else s


def main():
    print("=== test_agent_integration_anthropic ===")

    port = free_port()
    ready = threading.Event()
    t = threading.Thread(target=run_llm_mock, args=(port, ready), daemon=True)
    t.start()
    ready.wait(5.0)

    tmp = "/tmp/llmkit_agent_anthropic_itest"
    os.makedirs(tmp, exist_ok=True)
    convo = os.path.join(tmp, "convo.jsonl")
    if os.path.exists(convo):
        os.unlink(convo)

    cfg_path = os.path.join(tmp, "agent.yml")
    with open(cfg_path, "w") as f:
        f.write(
            f'llm:\n'
            f'  provider: "anthropic"\n'
            f'  api_base: "http://127.0.0.1:{port}/v1"\n'
            f'  api_key: "{API_KEY}"\n'
            f'  model: "claude-mock"\n'
            f'  max_tokens: 1024\n'
            f'agent:\n'
            f'  system_prompt: "Be brief."\n'
            f'mcps:\n'
            f'  - name: ns\n'
            f'    cmdline: "python3 {FIX}/fake_mcp.py"\n'
            f'    init_timeout: "5s"\n'
            f'    call_timeout: "5s"\n'
        )

    proc = subprocess.run(
        [BIN, "agent", "-c", cfg_path, "--conversation", convo, "-p", "What time is it?"],
        capture_output=True, text=True, timeout=30, cwd=ROOT,
    )
    check("agent exit code 0", proc.returncode == 0,
          f"exit={proc.returncode} stderr={proc.stderr[-400:]}")

    if proc.returncode != 0:
        print(proc.stdout)
        print(proc.stderr)
        return 1

    # -------------------------------------------------------------------
    # Wire requests: endpoint, auth headers, body shape.
    # -------------------------------------------------------------------
    reqs = MockAnthropic.requests
    check("mock received exactly 2 requests", len(reqs) == 2, f"n={len(reqs)}")
    if len(reqs) == 2:
        (path1, h1, b1), (_, _, b2) = reqs

        check("requests hit {api_base}/messages", path1 == "/v1/messages", f"path={path1!r}")
        check("x-api-key header sent", h1.get("x-api-key") == API_KEY,
              f"x-api-key={h1.get('x-api-key')!r}")
        check("anthropic-version header sent", h1.get("anthropic-version") == "2023-06-01",
              f"version={h1.get('anthropic-version')!r}")

        check("turn 1 has top-level system param",
              b1.get("system") == "Be brief.", f"system={b1.get('system')!r}")
        check("turn 1 has max_tokens from config", b1.get("max_tokens") == 1024,
              f"max_tokens={b1.get('max_tokens')!r}")
        check("turn 1 model passed through", b1.get("model") == "claude-mock",
              f"model={b1.get('model')!r}")

        msgs1 = b1.get("messages", [])
        check("turn 1 messages are [user] only",
              len(msgs1) == 1 and msgs1[0].get("role") == "user",
              f"msgs1={json.dumps(msgs1)[:200]}")
        blocks1 = blocks_of(msgs1[0]) if msgs1 else []
        check("user content is a text block",
              len(blocks1) == 1 and blocks1[0].get("type") == "text"
              and blocks1[0].get("text") == "What time is it?",
              f"blocks1={blocks1}")

        tools1 = b1.get("tools", [])
        check("turn 1 tools use input_schema shape",
              len(tools1) == 2
              and all("name" in t and "description" in t and "input_schema" in t
                      and "function" not in t for t in tools1)
              and any(t.get("name") == "ns.get_time" for t in tools1),
              f"tools1={json.dumps(tools1)[:200]}")

        msgs2 = b2.get("messages", [])
        roles2 = [m.get("role") for m in msgs2]
        check("turn 2 roles alternate user/assistant/user",
              roles2 == ["user", "assistant", "user"], f"roles2={roles2}")
        if len(msgs2) == 3:
            asst_blocks = blocks_of(msgs2[1])
            use = next((b for b in asst_blocks if b.get("type") == "tool_use"), None)
            check("turn 2 assistant message carries the tool_use block",
                  use is not None and use.get("id") == TOOL_USE_ID
                  and use.get("name") == "ns.get_time"
                  and use.get("input") == {"tz": "UTC"},
                  f"use={use}")
            last_blocks = blocks_of(msgs2[2])
            res = next((b for b in last_blocks if b.get("type") == "tool_result"), None)
            check("turn 2 user message carries the tool_result block",
                  res is not None and res.get("tool_use_id") == TOOL_USE_ID
                  and "called:get_time" in (res.get("content") or ""),
                  f"res={res}")

        # Prefix stability: the serialized turn-1 message span is a prefix of
        # the serialized turn-2 span (append-only history, deterministic
        # builder) -- this is what keeps provider prompt caches warm.
        check("turn 2 messages extend turn 1 byte-prefix",
              messages_span(b2).startswith(messages_span(b1)),
              f"m1={messages_span(b1)[:120]!r} m2={messages_span(b2)[:120]!r}")

    # -------------------------------------------------------------------
    # Conversation JSONL.
    # -------------------------------------------------------------------
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
    check("entry sequence is correct",
          types == ["meta", "user", "assistant", "tool_call", "tool_result", "assistant"],
          f"types={types}")

    tc = next((e for e in entries if e.get("type") == "tool_call"), {})
    check("tool_call id matches tool_use id", tc.get("id") == TOOL_USE_ID, f"tc={tc}")
    check("tool_call name is namespaced", tc.get("name") == "ns.get_time", f"tc={tc}")

    tr = next((e for e in entries if e.get("type") == "tool_result"), {})
    check("tool_result references call_id", tr.get("call_id") == TOOL_USE_ID, f"tr={tr}")
    check("tool_result contains backend echo",
          "called:get_time" in (tr.get("result") or ""), f"tr={tr}")

    final_asst = [e for e in entries if e.get("type") == "assistant"]
    check("two assistant entries", len(final_asst) == 2, f"count={len(final_asst)}")
    if len(final_asst) == 2:
        check("final assistant has content",
              "Done" in (final_asst[1].get("content") or ""), f"asst={final_asst[1]}")
        fu = final_asst[1].get("usage") or {}
        check("final assistant maps usage (prompt=120+80+20, completion=8)",
              fu.get("prompt_tokens") == 220 and fu.get("completion_tokens") == 8
              and fu.get("total_tokens") == 228,
              f"usage={fu}")
        check("final assistant persists cache tokens",
              fu.get("cached_tokens") == 80 and fu.get("cache_creation_tokens") == 20,
              f"usage={fu}")

    print(f"\n{tests_run} checks, {len(tests_failed)} failed")
    return 1 if tests_failed else 0


if __name__ == "__main__":
    sys.exit(main())
