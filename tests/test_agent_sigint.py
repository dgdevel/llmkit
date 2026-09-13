#!/usr/bin/env python3
"""
SIGINT handling integration test for `llmkit agent`.

Spins up a mock OpenAI-compatible chat-completions endpoint (and the fake
stdio MCP backend for the tool scenario), runs the real `llmkit agent`
binary, and interrupts it with SIGINT:

  Scenario 1 (SIGINT during the LLM request):
    the mock holds the first request open; SIGINT must abort it promptly.
    expect exit 130, a conversation file ending with an `error` entry
    (code 130, recoverable true) and no assistant entry.

  Scenario 2 (SIGINT during a tool call):
    the fake MCP backend sleeps on tools/call; SIGINT arrives mid-call.
    expect exit 130, the tool's REAL result recorded (the call ran to
    completion), no second LLM request, and an `error` entry.

  Scenario 3 (resume after interrupt):
    append a torn partial line to scenario 2's file, then rerun the agent
    on it. Expect exit 0: the partial line is trimmed at open and the run
    completes with a final assistant answer.

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

TOOL_CALL_SLEEP = 3  # seconds the fake MCP tool sleeps; SIGINT lands inside

tests_run = 0
tests_failed = []


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


def write_slow_mcp(path):
    """fake_mcp.py wrapper that sleeps TOOl_CALL_SLEEP seconds per tools/call."""
    with open(path, "w") as f:
        f.write("import os\n")
        f.write(f"os.environ['FAKE_MCP_SLEEP_CALL'] = '{TOOL_CALL_SLEEP}'\n")
        f.write(f"exec(open({os.path.join(FIX, 'fake_mcp.py')!r}).read())\n")


# ---------------------------------------------------------------------------
# Mock OpenAI-compatible chat completions server
# ---------------------------------------------------------------------------
class MockLLM(BaseHTTPRequestHandler):
    # Leading requests are held open for hold_seconds before answering
    # (simulating a slow provider, so the test can SIGINT mid-request).
    hold_first = 0
    hold_seconds = 30
    request_count = 0
    first_request_seen = threading.Event()

    def log_message(self, *a):
        pass  # silence

    def do_POST(self):
        MockLLM.request_count += 1
        n = MockLLM.request_count
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8", "replace")
        try:
            req = json.loads(body)
        except Exception:
            req = {}

        if n <= MockLLM.hold_first:
            MockLLM.first_request_seen.set()
            time.sleep(MockLLM.hold_seconds)

        # Turn detection: last message role "tool" = tool results present,
        # so produce the final answer; otherwise request the tool call.
        msgs = req.get("messages", [])
        last_role = msgs[-1].get("role") if msgs else "user"
        if last_role == "tool":
            resp = {
                "id": "chat_final",
                "model": req.get("model", "mock"),
                "choices": [{
                    "index": 0,
                    "message": {"role": "assistant", "content": "All done."},
                    "finish_reason": "stop",
                }],
                "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15},
            }
        else:
            resp = {
                "id": "chat_tool",
                "model": req.get("model", "mock"),
                "choices": [{
                    "index": 0,
                    "message": {
                        "role": "assistant",
                        "content": None,
                        "tool_calls": [{
                            "id": "call_1",
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


def read_entries(path):
    """Parse the JSONL file; returns (entries, has_partial_line)."""
    entries = []
    partial = False
    if not os.path.exists(path):
        return entries, partial
    with open(path, "rb") as f:
        data = f.read()
    lines = data.split(b"\n")
    if lines and lines[-1] != b"":
        partial = True
    for line in lines:
        line = line.strip()
        if not line:
            continue
        try:
            entries.append(json.loads(line))
        except Exception:
            pass  # torn line
    return entries, partial


def start_agent(cfg, convo, prompt="hello"):
    return subprocess.Popen(
        [BIN, "agent", "-c", cfg, "--conversation", convo, "-p", prompt, "--mode", "debug"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=ROOT,
    )


def scenario_interrupt_llm_request(tmp):
    print("--- scenario 1: SIGINT during the LLM request ---")
    port = free_port()
    ready = threading.Event()
    t = threading.Thread(target=run_llm_mock, args=(port, ready), daemon=True)
    t.start()
    ready.wait(5.0)

    MockLLM.request_count = 0
    MockLLM.hold_first = 1
    MockLLM.hold_seconds = 30
    MockLLM.first_request_seen.clear()

    cfg = os.path.join(tmp, "sigint_llm.yml")
    with open(cfg, "w") as f:
        f.write(f'llm:\n  api_base: "http://127.0.0.1:{port}/v1"\n'
                f'  api_key: "test"\n  model: "mock-model"\n')
    convo = os.path.join(tmp, "sigint_llm.jsonl")
    if os.path.exists(convo):
        os.unlink(convo)

    proc = start_agent(cfg, convo)
    MockLLM.first_request_seen.wait(10.0)
    start = time.monotonic()
    proc.send_signal(2)  # SIGINT
    proc.wait(timeout=30)
    elapsed = time.monotonic() - start
    out, err = proc.communicate()

    check("llm-interrupt exit 130", proc.returncode == 130, f"exit={proc.returncode}")
    check("llm-interrupt fast (request aborted, not timed out)", elapsed < 15.0,
          f"elapsed={elapsed:.2f}s")
    check("llm-interrupt exactly one mock request", MockLLM.request_count == 1,
          f"count={MockLLM.request_count}")

    entries, partial = read_entries(convo)
    check("llm-interrupt file has no partial line", not partial, "torn line present")
    check("llm-interrupt no assistant entry yet",
          all(e.get("type") != "assistant" for e in entries),
          f"types={[e.get('type') for e in entries]}")
    err_entries = [e for e in entries if e.get("type") == "error"]
    check("llm-interrupt error entry recorded", len(err_entries) == 1,
          f"errors={err_entries}")
    if len(err_entries) == 1:
        check("llm-interrupt error code 130", err_entries[0].get("code") == 130,
              f"code={err_entries[0].get('code')}")
        check("llm-interrupt error recoverable", err_entries[0].get("recoverable") is True,
              f"recoverable={err_entries[0].get('recoverable')}")
    check("llm-interrupt file ends with error entry",
          entries and entries[-1].get("type") == "error",
          f"last={entries[-1].get('type') if entries else None}")
    return convo


def scenario_interrupt_tool_call(tmp):
    print("--- scenario 2: SIGINT during a tool call ---")
    port = free_port()
    ready = threading.Event()
    t = threading.Thread(target=run_llm_mock, args=(port, ready), daemon=True)
    t.start()
    ready.wait(5.0)

    MockLLM.request_count = 0
    MockLLM.hold_first = 0
    MockLLM.first_request_seen.clear()

    slow_mcp = os.path.join(tmp, "slow_mcp.py")
    write_slow_mcp(slow_mcp)
    cfg = os.path.join(tmp, "sigint_tool.yml")
    with open(cfg, "w") as f:
        f.write(f'llm:\n  api_base: "http://127.0.0.1:{port}/v1"\n'
                f'  api_key: "test"\n  model: "mock-model"\n'
                f'mcps:\n  - name: ns\n    cmdline: "python3 {slow_mcp}"\n'
                f'    init_timeout: "10s"\n    call_timeout: "30s"\n')
    convo = os.path.join(tmp, "sigint_tool.jsonl")
    if os.path.exists(convo):
        os.unlink(convo)

    proc = start_agent(cfg, convo, prompt="What time is it?")
    # Wait until the tool call entry is on disk, then interrupt mid-execution.
    deadline = time.monotonic() + 20
    tool_written = False
    while time.monotonic() < deadline:
        entries, _ = read_entries(convo)
        if any(e.get("type") == "tool_call" for e in entries):
            tool_written = True
            break
        if proc.poll() is not None:
            break
        time.sleep(0.05)
    check("tool-interrupt tool_call entry written", tool_written, "no tool_call seen")
    proc.send_signal(2)  # SIGINT mid-tool
    start = time.monotonic()
    proc.wait(timeout=60)
    elapsed = time.monotonic() - start
    out, err = proc.communicate()

    check("tool-interrupt exit 130", proc.returncode == 130, f"exit={proc.returncode}")
    # The agent must WAIT for the in-flight tool (~TOOL_CALL_SLEEP left) rather
    # than dying instantly and leaving the call dangling.
    check("tool-interrupt waited for the tool", elapsed >= TOOL_CALL_SLEEP / 2.0,
          f"elapsed={elapsed:.2f}s")

    entries, partial = read_entries(convo)
    check("tool-interrupt file has no partial line", not partial, "torn line present")
    tcs = [e for e in entries if e.get("type") == "tool_call"]
    trs = [e for e in entries if e.get("type") == "tool_result"]
    check("tool-interrupt one tool_call", len(tcs) == 1, f"count={len(tcs)}")
    check("tool-interrupt one tool_result", len(trs) == 1, f"count={len(trs)}")
    if len(trs) == 1:
        check("tool-interrupt real tool result recorded",
              "called:get_time" in (trs[0].get("result") or ""),
              f"result={trs[0].get('result')!r}")
        check("tool-interrupt result not an error", trs[0].get("is_error") is False,
              f"is_error={trs[0].get('is_error')}")
    check("tool-interrupt no second LLM request", MockLLM.request_count == 1,
          f"count={MockLLM.request_count}")
    err_entries = [e for e in entries if e.get("type") == "error"]
    check("tool-interrupt error entry recorded", len(err_entries) == 1,
          f"errors={err_entries}")
    check("tool-interrupt file ends with error entry",
          entries and entries[-1].get("type") == "error",
          f"last={entries[-1].get('type') if entries else None}")
    return convo


def scenario_resume_after_interrupt(tmp, convo):
    print("--- scenario 3: resume after interrupt (trim + continue) ---")
    port = free_port()
    ready = threading.Event()
    t = threading.Thread(target=run_llm_mock, args=(port, ready), daemon=True)
    t.start()
    ready.wait(5.0)

    MockLLM.request_count = 0
    MockLLM.hold_first = 0

    cfg = os.path.join(tmp, "sigint_resume.yml")
    with open(cfg, "w") as f:
        f.write(f'llm:\n  api_base: "http://127.0.0.1:{port}/v1"\n'
                f'  api_key: "test"\n  model: "mock-model"\n')

    # Simulate a hard kill: a torn partial line with no terminating newline.
    with open(convo, "a") as f:
        f.write('{"type":"tool_call","id":"call_torn","name":"ns.get_time","arg')

    proc = subprocess.run(
        [BIN, "agent", "-c", cfg, "--conversation", convo, "-p", "And now?",
         "--mode", "debug"],
        capture_output=True, text=True, timeout=60, cwd=ROOT,
    )
    check("resume exit 0", proc.returncode == 0,
          f"exit={proc.returncode} stderr={proc.stderr[-400:]}")

    entries, partial = read_entries(convo)
    check("resume partial line trimmed", not partial, "torn line still present")
    check("resume torn tool_call dropped",
          all(e.get("id") != "call_torn" for e in entries if e.get("type") == "tool_call"),
          "torn entry survived")
    check("resume final assistant recorded",
          any(e.get("type") == "assistant" and e.get("content") == "All done."
              for e in entries),
          f"types={[e.get('type') for e in entries[-4:]]}")
    # Turn 1: the model re-issues the tool call given the recovered history
    # (last role is the resumed user prompt), turn 2: the final answer.
    check("resume tool round + answer served", MockLLM.request_count == 2,
          f"count={MockLLM.request_count}")


def main():
    print("=== test_agent_sigint ===")
    if not os.path.exists(BIN):
        print(f"  [FAIL] binary not found: {BIN}")
        return 1
    tmp = "/tmp/llmkit_agent_sigint_itest"
    os.makedirs(tmp, exist_ok=True)

    scenario_interrupt_llm_request(tmp)
    convo = scenario_interrupt_tool_call(tmp)
    scenario_resume_after_interrupt(tmp, convo)

    print(f"\n{tests_run} checks, {len(tests_failed)} failed")
    return 1 if tests_failed else 0


if __name__ == "__main__":
    sys.exit(main())
