"""
Steering integration test.

Validates that `llmkit agent --steer` reads additional user messages from
stdin during a run and injects them as `source: "steer"` user entries into
the conversation JSONL, which then appear in the next LLM call.

Flow:
  - Mock LLM: turn-by-turn state machine keyed on the *content* of the last
    user message seen in the request.
      * last user message contains "steer"  -> assistant replies with
        "STEERED_REPLY" (no tool calls), and records that steering happened.
      * last user message is the initial prompt -> request a tool call.
      * last message role is "tool" -> final text answer.
  - We feed a steering message via stdin at the start (before the process
    runs). Since drain point A runs at the top of turn 1, the steer message
    is injected before the first LLM call and should appear in the first
    request's messages.

Exit 0 = pass, 1 = fail.
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

tests_failed = []
TOOL_CALL_ID = "call_steer_1"


def check(name, cond, detail=""):
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


# Track what the LLM saw, for assertions.
seen_requests = []


class MockLLM(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8", "replace")
        try:
            req = json.loads(body)
        except Exception:
            req = {}
        messages = req.get("messages", [])
        seen_requests.append(messages)
        last_role = messages[-1].get("role", "") if messages else ""
        last_user_content = ""
        for m in reversed(messages):
            if m.get("role") == "user":
                last_user_content = m.get("content", "")
                break

        if "STEER_NOW" in last_user_content:
            # Steering message seen: reply with a recognizable marker.
            resp = {
                "id": "chat_steer",
                "model": req.get("model", "mock"),
                "choices": [{
                    "index": 0,
                    "message": {"role": "assistant", "content": "STEERED_REPLY"},
                    "finish_reason": "stop",
                }],
                "usage": {"prompt_tokens": 9, "completion_tokens": 4, "total_tokens": 13},
            }
        elif last_role == "tool":
            resp = {
                "id": "chat_final",
                "model": req.get("model", "mock"),
                "choices": [{
                    "index": 0,
                    "message": {"role": "assistant", "content": "Final answer."},
                    "finish_reason": "stop",
                }],
                "usage": {"prompt_tokens": 10, "completion_tokens": 8, "total_tokens": 18},
            }
        else:
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
    print("=== test_agent_steer ===")
    port = free_port()
    ready = threading.Event()
    t = threading.Thread(target=run_llm_mock, args=(port, ready), daemon=True)
    t.start()
    ready.wait(5.0)

    tmp = "/tmp/llmkit_agent_steer_itest"
    os.makedirs(tmp, exist_ok=True)
    convo = os.path.join(tmp, "convo.jsonl")
    if os.path.exists(convo):
        os.unlink(convo)

    cfg = os.path.join(tmp, "config.yml")
    with open(cfg, "w") as f:
        f.write(
            f'llm:\n  api_base: "http://127.0.0.1:{port}/v1"\n'
            f'  model: "mock"\nagent:\n  system_prompt: "You are a test bot."\n'
            f'mcps:\n  - name: "ns"\n'
            f'    cmdline: "python3 {FIX}/fake_mcp.py"\n'
            f'    init_timeout: "5s"\n'
            f'    call_timeout: "5s"\n'
        )

    # The steering message, blank-line terminated so it's a complete message.
    stdin_data = b"Please use STEER_NOW to reply.\n\n"

    proc = subprocess.run(
        [
            BIN, "agent",
            "-c", cfg,
            "--conversation", convo,
            "-p", "What time is it?",
            "--mode", "debug",
            "--steer",
        ],
        input=stdin_data,
        capture_output=True,
        timeout=30,
    )

    print("  [agent exit code]", proc.returncode)
    print("  [agent stdout]", proc.stdout.decode("utf-8", "replace")[:400])
    print("  [agent stderr]", proc.stderr.decode("utf-8", "replace")[:800])

    # Read the conversation JSONL.
    entries = []
    with open(convo) as f:
        for line in f:
            line = line.strip()
            if line:
                entries.append(json.loads(line))

    types = [e.get("type") for e in entries]
    print("  [entry types]", types)

    # 1. There must be a user entry with source "steer".
    steer_entries = [e for e in entries if e.get("type") == "user" and e.get("source") == "steer"]
    check("steer user entry present", len(steer_entries) == 1,
          f"expected 1 steer entry, got {len(steer_entries)}")
    if steer_entries:
        check("steer content correct",
              "STEER_NOW" in steer_entries[0].get("content", ""),
              f"content={steer_entries[0].get('content')!r}")

    # 2. The steer entry must appear BEFORE the first assistant entry
    #    (so it is part of the context of the very first LLM call).
    first_assistant_idx = next(
        (i for i, e in enumerate(entries) if e.get("type") == "assistant"), None)
    steer_idx = next(
        (i for i, e in enumerate(entries)
         if e.get("type") == "user" and e.get("source") == "steer"), None)
    check("steer before first assistant",
          steer_idx is not None and first_assistant_idx is not None and steer_idx < first_assistant_idx,
          f"steer_idx={steer_idx} first_assistant_idx={first_assistant_idx}")

    # 3. The first LLM request must have included the steering content.
    first_req_user_contents = [
        m.get("content", "") for m in (seen_requests[0] if seen_requests else [])
        if m.get("role") == "user"
    ]
    check("steer in first LLM request",
          any("STEER_NOW" in c for c in first_req_user_contents),
          f"user contents seen: {first_req_user_contents}")

    # 4. The assistant must have produced the steered reply.
    assistant_contents = [e.get("content", "") for e in entries if e.get("type") == "assistant"]
    check("steered reply present",
          any("STEERED_REPLY" in c for c in assistant_contents),
          f"assistant contents: {assistant_contents}")

    if tests_failed:
        print(f"\n{len(tests_failed)} FAILED: {tests_failed}")
        return 1
    print("\nAll steering checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
