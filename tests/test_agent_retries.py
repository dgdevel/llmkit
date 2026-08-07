#!/usr/bin/env python3
"""
Retry behavior integration test for `llmkit agent --max-retries`.

Spins up a mock OpenAI-compatible chat-completions endpoint that fails the
first N requests with HTTP 500, then runs the real `llmkit agent` binary and
validates the Fibonacci-backoff retry behavior:

  Scenario 1 (retry then success):
    mock fails the first 2 requests, succeeds on the 3rd.
    run with --max-retries 3 --mode debug.
    expect exit 0, two retry events in the debug stream (1s + 1s backoff),
    and a final assistant answer in the conversation JSONL.

  Scenario 2 (retries exhausted -> failure):
    mock always fails.
    run with --max-retries 1.
    expect exit 4 (LLM error) after exactly one retry.

  Scenario 3 (zero retries -> immediate failure):
    mock always fails.
    run with --max-retries 0.
    expect exit 4 with no retries and no backoff delay.

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


# ---------------------------------------------------------------------------
# Mock OpenAI-compatible chat completions server
# ---------------------------------------------------------------------------
class MockLLM(BaseHTTPRequestHandler):
    # How many leading requests should fail with HTTP 500. Set per scenario.
    fail_first = 0
    # Total POST count observed across the handler instances.
    request_count = 0

    def log_message(self, *a):
        pass  # silence

    def do_POST(self):
        MockLLM.request_count += 1
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8", "replace")
        try:
            req = json.loads(body)
        except Exception:
            req = {}

        if MockLLM.request_count <= MockLLM.fail_first:
            # Simulate a transient server error.
            msg = json.dumps({"error": {"message": "transient failure",
                                        "type": "server_error"}}).encode()
            self.send_response(500)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(msg)))
            self.end_headers()
            self.wfile.write(msg)
            return

        resp = {
            "id": "chat_ok",
            "model": req.get("model", "mock"),
            "choices": [{
                "index": 0,
                "message": {"role": "assistant", "content": "Recovered after retry."},
                "finish_reason": "stop",
            }],
            "usage": {"prompt_tokens": 7, "completion_tokens": 5, "total_tokens": 12},
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


def write_cfg(path, port):
    with open(path, "w") as f:
        f.write(
            f'llm:\n'
            f'  api_base: "http://127.0.0.1:{port}/v1"\n'
            f'  api_key: "test"\n'
            f'  model: "mock-model"\n'
        )


def fresh_convo(path):
    if os.path.exists(path):
        os.unlink(path)


def assistant_content(convo):
    """Return the content of the last assistant entry, or None."""
    last = None
    with open(convo) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                e = json.loads(line)
            except Exception:
                continue
            if e.get("type") == "assistant":
                last = e.get("content")
    return last


def scenario_retry_then_success(tmp):
    print("--- scenario: retry then success ---")
    port = free_port()
    ready = threading.Event()
    t = threading.Thread(target=run_llm_mock, args=(port, ready), daemon=True)
    t.start()
    ready.wait(5.0)

    MockLLM.request_count = 0
    MockLLM.fail_first = 2  # fail requests 1 and 2, succeed on 3rd

    cfg = os.path.join(tmp, "retry_ok.yml")
    write_cfg(cfg, port)
    convo = os.path.join(tmp, "convo_ok.jsonl")
    fresh_convo(convo)

    start = time.monotonic()
    proc = subprocess.run(
        [BIN, "agent", "-c", cfg, "--conversation", convo, "-p", "hello",
         "--max-retries", "3", "--mode", "debug"],
        capture_output=True, text=True, timeout=60, cwd=ROOT,
    )
    elapsed = time.monotonic() - start

    combined = proc.stdout + proc.stderr
    check("retry-then-success exit 0", proc.returncode == 0,
          f"exit={proc.returncode} stderr={proc.stderr[-300:]}")
    # Two retries: backoff delays are fib(1)+fib(2) = 1+1 = 2s minimum.
    check("retry-then-success waited >= 1.9s", elapsed >= 1.9,
          f"elapsed={elapsed:.2f}s")
    check("retry-then-success emits attempt 1/3",
          "retry: attempt 1/3" in combined, f"stdout={proc.stdout[-300:]}")
    check("retry-then-success emits attempt 2/3",
          "retry: attempt 2/3" in combined, f"stdout={proc.stdout[-300:]}")
    check("no attempt 3 emitted (recovered)",
          "attempt 3/3" not in combined, f"stdout={proc.stdout[-300:]}")
    check("mock received 3 total requests",
          MockLLM.request_count == 3, f"count={MockLLM.request_count}")
    check("final assistant content recorded",
          assistant_content(convo) == "Recovered after retry.",
          f"content={assistant_content(convo)!r}")


def scenario_exhaust_retries(tmp):
    print("--- scenario: retries exhausted -> failure ---")
    port = free_port()
    ready = threading.Event()
    t = threading.Thread(target=run_llm_mock, args=(port, ready), daemon=True)
    t.start()
    ready.wait(5.0)

    MockLLM.request_count = 0
    MockLLM.fail_first = 1_000_000  # always fail

    cfg = os.path.join(tmp, "retry_exhaust.yml")
    write_cfg(cfg, port)
    convo = os.path.join(tmp, "convo_exhaust.jsonl")
    fresh_convo(convo)

    start = time.monotonic()
    proc = subprocess.run(
        [BIN, "agent", "-c", cfg, "--conversation", convo, "-p", "hello",
         "--max-retries", "1", "--mode", "debug"],
        capture_output=True, text=True, timeout=60, cwd=ROOT,
    )
    elapsed = time.monotonic() - start

    combined = proc.stdout + proc.stderr
    # 1 initial attempt + 1 retry = 2 total requests, then give up (exit 4).
    check("exhaust exit 4 (LLM error)", proc.returncode == 4,
          f"exit={proc.returncode}")
    check("exhaust emitted one retry attempt",
          "retry: attempt 1/1" in combined, f"stdout={proc.stdout[-300:]}")
    check("exhaust no second retry",
          "attempt 2/1" not in combined, f"stdout={proc.stdout[-300:]}")
    check("exhaust backoff ~1s", 0.8 <= elapsed < 3.0,
          f"elapsed={elapsed:.2f}s")
    check("exhaust mock got 2 requests",
          MockLLM.request_count == 2, f"count={MockLLM.request_count}")


def scenario_zero_retries(tmp):
    print("--- scenario: zero retries -> immediate failure ---")
    port = free_port()
    ready = threading.Event()
    t = threading.Thread(target=run_llm_mock, args=(port, ready), daemon=True)
    t.start()
    ready.wait(5.0)

    MockLLM.request_count = 0
    MockLLM.fail_first = 1_000_000  # always fail

    cfg = os.path.join(tmp, "retry_zero.yml")
    write_cfg(cfg, port)
    convo = os.path.join(tmp, "convo_zero.jsonl")
    fresh_convo(convo)

    start = time.monotonic()
    proc = subprocess.run(
        [BIN, "agent", "-c", cfg, "--conversation", convo, "-p", "hello",
         "--max-retries", "0", "--mode", "debug"],
        capture_output=True, text=True, timeout=60, cwd=ROOT,
    )
    elapsed = time.monotonic() - start

    combined = proc.stdout + proc.stderr
    check("zero-retries exit 4 (LLM error)", proc.returncode == 4,
          f"exit={proc.returncode}")
    check("zero-retries no retry emitted", "retry:" not in combined,
          f"stdout={proc.stdout[-300:]}")
    check("zero-retries fast (no backoff)", elapsed < 1.5,
          f"elapsed={elapsed:.2f}s")
    check("zero-retries mock got 1 request",
          MockLLM.request_count == 1, f"count={MockLLM.request_count}")


def main():
    print("=== test_agent_retries ===")
    tmp = "/tmp/llmkit_agent_retries_itest"
    os.makedirs(tmp, exist_ok=True)

    scenario_retry_then_success(tmp)
    scenario_exhaust_retries(tmp)
    scenario_zero_retries(tmp)

    print(f"\n{tests_run} checks, {len(tests_failed)} failed")
    return 1 if tests_failed else 0


if __name__ == "__main__":
    sys.exit(main())
