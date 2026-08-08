#!/usr/bin/env python3
"""Integration test: prefix-cache-aware compaction (agent.compact.*).

Verifies end-to-end that:
  1. a long conversation triggers compaction on the first turn,
  2. the canonical JSONL is never rewritten (append-only),
  3. a projection sidecar (<convo>.context.json) is written,
  4. resuming the conversation reuses the sidecar and keeps the
     provider-visible prefix byte-identical (only the tail grows),
  5. the response command still reads the raw JSONL.
"""

import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BIN = os.path.join(ROOT, "llmkit")
FIXTURES = os.path.join(HERE, "fixtures")

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


def matching_bracket(s, open_idx):
    """Index of the bracket matching the '[' at open_idx, respecting JSON
    string literals (nested arrays and brackets inside strings handled)."""
    depth = 0
    in_str = False
    esc = False
    for i in range(open_idx, len(s)):
        c = s[i]
        if in_str:
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == '"':
                in_str = False
            continue
        if c == '"':
            in_str = True
        elif c == "[":
            depth += 1
        elif c == "]":
            depth -= 1
            if depth == 0:
                return i
    return -1


def messages_array(body):
    """Byte span of the 'messages' array (including brackets)."""
    i = body.find('"messages":')
    if i < 0:
        return None
    j = body.find("[", i)
    if j < 0:
        return None
    k = matching_bracket(body, j)
    if k < 0:
        return None
    return body[j:k + 1]


class MockLLM(BaseHTTPRequestHandler):
    bodies = []
    lock = threading.Lock()

    def log_message(self, *a):
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8", "replace")
        with MockLLM.lock:
            MockLLM.bodies.append(body)
        req = json.loads(body)
        messages = req.get("messages", [])
        # 4+ messages (or a tool result) -> final answer, else a tool call.
        if len(messages) >= 4:
            resp = {
                "id": "chat_final",
                "model": req.get("model", "mock"),
                "choices": [{
                    "index": 0,
                    "message": {"role": "assistant", "content": "Final answer"},
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
                            "function": {"name": "ns.get_time", "arguments": "{}"},
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


def seed_conversation(path):
    """Seed a 12-entry conversation (6 user/assistant pairs) with long content
    so the bytes/4 estimate exceeds the tiny compaction budget."""
    filler = "Filler text for the conversation. " * 20  # ~520 bytes per message
    with open(path, "w") as f:
        for i in range(1, 7):
            f.write(json.dumps({
                "type": "user",
                "content": f"Seeded user question {i}. {filler}",
                "source": "cli",
            }) + "\n")
            f.write(json.dumps({
                "type": "assistant",
                "content": f"Seeded assistant reply {i}. {filler}",
                "model": "m",
            }) + "\n")


def write_config(path, port):
    with open(path, "w") as f:
        f.write(f'llm:\n'
                f'  api_base: "http://127.0.0.1:{port}/v1"\n'
                f'  api_key: "k"\n'
                f'  model: "mock-model"\n'
                f'agent:\n'
                f'  system_prompt: "Be brief."\n'
                f'  compact:\n'
                f'    enabled: true\n'
                f'    max_tokens: 100\n'
                f'    threshold: 0.8\n'
                f'    summarize: false\n'
                f'mcps:\n'
                f'  - name: ns\n'
                f'    cmdline: "python3 {FIXTURES}/fake_mcp.py"\n'
                f'    init_timeout: "5s"\n'
                f'    call_timeout: "5s"\n')


def main():
    port = free_port()
    httpd = ThreadingHTTPServer(("127.0.0.1", port), MockLLM)
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()

    tmp = tempfile.mkdtemp(prefix="llmkit_compact_")
    try:
        convo = os.path.join(tmp, "convo.jsonl")
        sidecar = convo + ".context.json"
        cfg = os.path.join(tmp, "compact.yml")
        seed_conversation(convo)
        write_config(cfg, port)

        seeded = open(convo).read()
        seeded_lines = seeded.count("\n")

        # ---- Run 1: compaction triggers on the first turn ----
        MockLLM.bodies = []
        p1 = subprocess.run(
            [BIN, "agent", "-c", cfg, "--conversation", convo, "-p", "Now what?"],
            capture_output=True, text=True, timeout=90, cwd=ROOT)
        check("run 1 exit 0", p1.returncode == 0, f"rc={p1.returncode} err={p1.stderr[-300:]}")
        check("run 1 issues exactly 1 request", len(MockLLM.bodies) == 1,
              f"n={len(MockLLM.bodies)}")
        check("projection sidecar written", os.path.exists(sidecar))

        with open(convo) as f:
            after1 = f.read()
        check("canonical is append-only (12 -> 15 lines)",
              after1.count("\n") == seeded_lines + 3,  # meta + user + assistant
              f"lines={after1.count(chr(10))}")
        check("seeded entries unchanged verbatim", after1.startswith(seeded))

        b1 = MockLLM.bodies[0]
        m1 = json.loads(b1)["messages"]
        check("run 1 request is projection + tail (5 messages)",
              len(m1) == 5 and [m["role"] for m in m1] ==
              ["system", "user", "user", "assistant", "user"],
              f"roles={[m.get('role') for m in m1]}")
        if len(m1) >= 3:
            check("projection pins system + first user verbatim",
                  m1[0]["content"] == "Be brief." and
                  m1[1]["content"].startswith("Seeded user question 1."),
                  f"m0={m1[0].get('content')!r} m1={m1[1].get('content')[:40]!r}")
            check("summary placeholder present",
                  "[Earlier conversation compacted" in m1[2].get("content", ""),
                  f"summary={m1[2].get('content')[:60]!r}")
        if len(m1) == 5:
            check("tail keeps recent turns verbatim",
                  m1[3]["content"].startswith("Seeded assistant reply 6.") and
                  m1[4]["content"] == "Now what?",
                  f"m3={m1[3].get('content')[:40]!r} m4={m1[4].get('content')!r}")

        with open(sidecar) as f:
            side = json.load(f)
        check("sidecar carries covered_count", side.get("covered_count", 0) > 0,
              f"sidecar={side}")
        check("sidecar carries hashes",
              str(side.get("covered_prefix_hash", "")).startswith("sha256:") and
              str(side.get("prompt_cache_key", "")).startswith("sha256:"),
              f"sidecar={side}")
        check("sidecar carries projection", isinstance(side.get("projection"), list)
              and len(side["projection"]) > 0, f"sidecar={side}")

        # ---- Run 2: resume reuses the projection, prefix stays byte-identical ----
        MockLLM.bodies = []
        p2 = subprocess.run(
            [BIN, "agent", "-c", cfg, "--conversation", convo, "-p", "Anything else?"],
            capture_output=True, text=True, timeout=90, cwd=ROOT)
        check("run 2 exit 0", p2.returncode == 0, f"rc={p2.returncode} err={p2.stderr[-300:]}")
        check("run 2 issues exactly 1 request", len(MockLLM.bodies) == 1,
              f"n={len(MockLLM.bodies)}")
        b2 = MockLLM.bodies[0]
        m2 = json.loads(b2)["messages"]
        check("run 2 request appends to the projection (7 messages)",
              len(m2) == 7 and m2[0:5] == m1,
              f"roles={[m.get('role') for m in m2]}")
        raw1, raw2 = messages_array(b1), messages_array(b2)
        check("run 2 messages array starts with run 1 bytes",
              raw1 is not None and raw2 is not None and len(raw2) > len(raw1) and
              raw2[:len(raw1) - 1] == raw1[:-1] and raw2[len(raw1) - 1] == ",",
              f"len1={len(raw1) if raw1 else 0} len2={len(raw2) if raw2 else 0}")

        with open(convo) as f:
            after2 = f.read()
        check("canonical still append-only after run 2",
              after2.count("\n") == seeded_lines + 6,  # two more entries
              f"lines={after2.count(chr(10))}")
        check("seeded entries still unchanged", after2.startswith(seeded))

        # ---- response command still reads the raw JSONL ----
        r = subprocess.run([BIN, "response", "--conversation", convo],
                           capture_output=True, text=True, timeout=30, cwd=ROOT)
        check("response prints last assistant answer from raw JSONL",
              r.returncode == 0 and r.stdout.strip() == "Final answer",
              f"rc={r.returncode} out={r.stdout!r}")
    finally:
        httpd.shutdown()
        shutil.rmtree(tmp, ignore_errors=True)

    print(f"\n{len(tests_failed)} of {tests_run} checks failed")
    sys.exit(1 if tests_failed else 0)


if __name__ == "__main__":
    main()
