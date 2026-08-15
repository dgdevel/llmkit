#!/usr/bin/env python3
"""
Agent-as-tool (subagents) integration test.

Spins up a mock OpenAI-compatible chat-completions endpoint and runs the real
`llmkit agent` binary with a config that exposes one subagent ("calculator")
backed by a private stdio MCP server (tests/fixtures/fake_mcp.py).

Flow exercised:
  main turn 1:   LLM calls the subagent tool `calculator {expression:"2+3"}`
  subagent turn 1: LLM calls the MCP tool `calc.echo`
  subagent turn 2: final text -> becomes the main agent's tool_result
  main turn 2:   final text answer

The mock LLM distinguishes the two conversations by their system prompt.

Scenario B exercises a name-only MCP reference: the subagent references the
main agent's MCP server instead of defining its own.

Scenario D exercises nested subagents (depth 2) and asserts the full
sub-conversation traces - bracketed by subagent_start/subagent_end and
scoped by depth/subagent/run_id - are retained in the persisted JSONL,
that each nested agent rebuilds only its own history, and that
`llmkit response` still returns the top-level answer.

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

MAIN_SYS = "You are the main agent."
SUB_SYS = "You are the calculator subagent."

# Scenario D: nested subagents (depth 2). Three conversations routed by
# system prompt: dispatcher -> digger.
D_MAIN_SYS = "You are the dispatching main agent."
D_SUB_SYS = "You are the level-1 dispatcher subagent."
D_INNER_SYS = "You are the level-2 digger subagent."


def check(name, cond, detail=""):
    global tests_run
    tests_run += 1
    if cond:
        print(f"  [ok] {name}")
    else:
        print(f"  [FAIL] {name}: {detail}")
        tests_failed.append(name)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class MockLLM(BaseHTTPRequestHandler):
    """Routes by system prompt: main conversation vs subagent conversation."""

    bodies = []
    lock = threading.Lock()

    def log_message(self, *a):
        pass

    @staticmethod
    def tool_call_resp(cid, name, args):
        return {
            "id": f"chat_{cid}",
            "model": "mock",
            "choices": [
                {
                    "index": 0,
                    "message": {
                        "role": "assistant",
                        "content": None,
                        "tool_calls": [
                            {
                                "id": cid,
                                "type": "function",
                                "function": {"name": name, "arguments": args},
                            }
                        ],
                    },
                    "finish_reason": "tool_calls",
                }
            ],
            "usage": {"prompt_tokens": 5, "completion_tokens": 3, "total_tokens": 8},
        }

    @staticmethod
    def text_resp(cid, content):
        return {
            "id": f"chat_{cid}",
            "model": "mock",
            "choices": [
                {
                    "index": 0,
                    "message": {"role": "assistant", "content": content},
                    "finish_reason": "stop",
                }
            ],
            "usage": {"prompt_tokens": 12, "completion_tokens": 6, "total_tokens": 18},
        }

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8", "replace")
        with MockLLM.lock:
            MockLLM.bodies.append(body)
        try:
            req = json.loads(body)
        except Exception:
            req = {}
        messages = req.get("messages", [])
        system = next((m.get("content") for m in messages if m.get("role") == "system"), "")
        last_role = messages[-1].get("role", "") if messages else ""

        if system == SUB_SYS:
            # Subagent conversation.
            if last_role == "tool":
                resp = self.text_resp("sub_final", "The expression evaluates to 42.")
            else:
                resp = self.tool_call_resp("call_sub_1", "calc.echo", '{"text":"2+3"}')
        elif system == D_MAIN_SYS:
            if last_role == "tool":
                resp = self.text_resp("d_main_final", "Dispatch done.")
            else:
                resp = self.tool_call_resp("call_d1", "dispatcher", '{"where":"here"}')
        elif system == D_SUB_SYS:
            if last_role == "tool":
                resp = self.text_resp("d_sub_final", "Level one got: GOLD")
            else:
                resp = self.tool_call_resp("call_d2", "digger", '{"where":"here"}')
        elif system == D_INNER_SYS:
            resp = self.text_resp("d_inner_final", "GOLD")
        else:
            # Main conversation.
            if last_role == "tool":
                resp = self.text_resp("main_final", "The subagent says 42.")
            else:
                resp = self.tool_call_resp(
                    "call_main_1", "calculator", '{"expression":"2+3"}'
                )

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


def read_jsonl(path):
    entries = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                entries.append(json.loads(line))
    return entries


def scenario_a(port, tmp):
    """Private MCP server inside the subagent."""
    print("[scenario] subagent with its own private MCP server")
    convo = os.path.join(tmp, "convo_a.jsonl")
    if os.path.exists(convo):
        os.unlink(convo)
    cfg_path = os.path.join(tmp, "agent_a.yml")
    # The user prompt interpolates {expression} from the tool call arguments;
    # built via concatenation so YAML sees single braces.
    user_prompt = "Resolve the expression " + "{expression}" + " now"
    with open(cfg_path, "w") as f:
        f.write(
            f"llm:\n"
            f'  api_base: "http://127.0.0.1:{port}/v1"\n'
            f'  api_key: "test"\n'
            f'  model: "mock-model"\n'
            f"agent:\n"
            f'  system_prompt: "{MAIN_SYS}"\n'
            f"subagents:\n"
            f"  - tool_definition:\n"
            f"      name: calculator\n"
            f"      description: A mathematic helper\n"
            f"      attributes:\n"
            f"        expression:\n"
            f"          type: string\n"
            f"          description: the expression to be evaluated\n"
            f'    system_prompt: "{SUB_SYS}"\n'
            f'    user_prompt: "{user_prompt}"\n'
            f"    mcps:\n"
            f"      - name: calc\n"
            f'        cmdline: "python3 {FIX}/fake_mcp.py"\n'
            f'        init_timeout: "5s"\n'
            f'        call_timeout: "5s"\n'
        )

    proc = subprocess.run(
        [BIN, "agent", "-c", cfg_path, "--conversation", convo, "-p", "How much is 2+3?"],
        capture_output=True,
        text=True,
        timeout=60,
        cwd=ROOT,
    )
    check("agent exit code 0", proc.returncode == 0,
          f"exit={proc.returncode} stderr={proc.stderr[-400:]}")
    if proc.returncode != 0:
        print(proc.stdout)
        print(proc.stderr)
        return

    entries = read_jsonl(convo)
    types = [e.get("type") for e in entries]

    top = [e for e in entries if "run_id" not in e or e.get("type") == "meta"]
    scoped = [e for e in entries if "run_id" in e]
    check("top-level entry sequence is correct",
          [e.get("type") for e in top] ==
          ["meta", "user", "assistant", "tool_call", "tool_result", "assistant"],
          f"types={[e.get('type') for e in top]}")

    # ---- Nested trace: brackets + scoped entries between the parent pair ----
    idx_start = next((i for i, e in enumerate(entries)
                      if e.get("type") == "subagent_start"), -1)
    idx_end = next((i for i, e in enumerate(entries)
                    if e.get("type") == "subagent_end"), -1)
    idx_tc = next((i for i, e in enumerate(entries)
                   if e.get("type") == "tool_call"), -1)
    idx_tr = next((i for i, e in enumerate(entries)
                   if e.get("type") == "tool_result" and "run_id" not in e), -1)
    check("subagent trace is bracketed inside the parent tool pair",
          -1 < idx_tc < idx_start < idx_end < idx_tr,
          f"tc={idx_tc} start={idx_start} end={idx_end} tr={idx_tr}")

    start = entries[idx_start] if idx_start >= 0 else {}
    end = entries[idx_end] if idx_end >= 0 else {}
    run_id = start.get("run_id", "")
    check("subagent_start carries depth/name/call_id/args",
          start.get("depth") == 1 and start.get("subagent") == "calculator" and
          start.get("call_id") == "call_main_1" and
          start.get("arguments") == '{"expression":"2+3"}', f"start={start}")
    check("subagent_end carries turns and success",
          end.get("depth") == 1 and end.get("turns") == 2 and
          end.get("is_error") is False, f"end={end}")

    inner = entries[idx_start + 1:idx_end]
    check("nested block holds the full sub-conversation",
          [e.get("type") for e in inner] ==
          ["user", "assistant", "tool_call", "tool_result", "assistant"],
          f"types={[e.get('type') for e in inner]}")
    check("every nested entry shares the run's scope",
          all(e.get("depth") == 1 and e.get("subagent") == "calculator" and
              e.get("run_id") == run_id for e in inner if e.get("type") != "meta"),
          f"inner={inner}")
    sub_user = inner[0] if inner else {}
    check("nested user entry records the interpolated prompt",
          sub_user.get("content") == "Resolve the expression 2+3 now" and
          sub_user.get("source") == "subagent", f"sub_user={sub_user}")
    sub_tc = inner[2] if len(inner) > 2 else {}
    check("nested tool_call recorded with its MCP server",
          sub_tc.get("name") == "calc.echo" and sub_tc.get("mcp_server") == "calc" and
          sub_tc.get("id") == "call_sub_1", f"sub_tc={sub_tc}")
    sub_final = inner[-1] if inner else {}
    check("nested final assistant recorded",
          sub_final.get("content") == "The expression evaluates to 42.", f"final={sub_final}")

    tc = next((e for e in entries if e.get("type") == "tool_call" and "run_id" not in e), {})
    check("tool_call targets the subagent tool", tc.get("name") == "calculator", f"tc={tc}")
    check("tool_call mcp_server is subagent",
          tc.get("mcp_server") == "subagent", f"tc={tc}")

    tr = next((e for e in entries
               if e.get("type") == "tool_result" and "run_id" not in e), {})
    check("tool_result carries the subagent final answer",
          (tr.get("result") or "") == "The expression evaluates to 42.", f"tr={tr}")
    check("tool_result is not error", tr.get("is_error") is False, f"tr={tr}")

    final = [e for e in entries if e.get("type") == "assistant" and "run_id" not in e]
    check("two main assistant entries", len(final) == 2, f"count={len(final)}")
    scoped_payload = [e for e in scoped
                      if e.get("type") not in ("meta", "subagent_start", "subagent_end")]
    check("scoped entries were written", len(scoped_payload) == 5,
          f"scoped={len(scoped_payload)}")

    check("quiet stdout prints only the final answer",
          proc.stdout.strip() == "The subagent says 42.", f"stdout={proc.stdout!r}")

    # `llmkit response` must surface the top-level answer, never a nested one.
    resp = subprocess.run(
        [BIN, "response", "--conversation", convo],
        capture_output=True, text=True, timeout=30, cwd=ROOT)
    check("response command returns the top-level answer",
          resp.returncode == 0 and resp.stdout.strip() == "The subagent says 42.",
          f"exit={resp.returncode} stdout={resp.stdout!r}")

    # ---- Inspect what the mock LLM actually received ----
    sub_reqs = []
    main_reqs = []
    with MockLLM.lock:
        for b in MockLLM.bodies:
            req = json.loads(b)
            msgs = req.get("messages", [])
            system = next((m.get("content") for m in msgs if m.get("role") == "system"), "")
            (sub_reqs if system == SUB_SYS else main_reqs).append(req)

    check("subagent ran its own conversation", len(sub_reqs) == 2, f"n={len(sub_reqs)}")
    if sub_reqs:
        first = sub_reqs[0]
        user = next((m.get("content") for m in first["messages"] if m.get("role") == "user"), "")
        check("user_prompt interpolated the expression attribute",
              user == "Resolve the expression 2+3 now", f"user={user!r}")
        tools = first.get("tools", [])
        tool_names = sorted(t["function"]["name"] for t in tools)
        check("subagent sees only its own MCP tools",
              tool_names == ["calc.echo", "calc.get_time"], f"tools={tool_names}")
        # The subagent tool itself must not be re-exposed to the subagent.
        check("subagent tool not re-exposed to itself",
              "calculator" not in tool_names, f"tools={tool_names}")
        if tools:
            schema = tools[0]["function"].get("parameters", {})
            check("tool schema parsed as JSON object", schema.get("type") == "object",
                  f"schema={schema}")

    check("main conversation had exactly 2 requests", len(main_reqs) == 2,
          f"n={len(main_reqs)}")
    if len(main_reqs) == 2:
        msgs2 = main_reqs[1].get("messages", [])
        roles = [m.get("role") for m in msgs2]
        check("parent's next turn sees only the top-level history",
              roles == ["system", "user", "assistant", "tool"], f"roles={roles}")
        all_text = json.dumps(msgs2)
        check("parent's next turn excludes nested entries",
              "Resolve the expression" not in all_text and
              "call_sub_1" not in all_text and "calc.echo" not in all_text,
              f"messages={msgs2}")
        tool_msg = next((m for m in msgs2 if m.get("role") == "tool"), {})
        check("parent's tool message carries the subagent answer",
              tool_msg.get("content") == "The expression evaluates to 42.",
              f"tool_msg={tool_msg}")
    if sub_reqs:
        # The subagent's second request must rebuild only its own scope.
        msgs2 = sub_reqs[1].get("messages", [])
        roles = [m.get("role") for m in msgs2]
        check("subagent's next turn sees only its own history",
              roles == ["system", "user", "assistant", "tool"], f"roles={roles}")
        sub_all = json.dumps(msgs2)
        check("subagent's next turn excludes the parent's entries",
              "How much is 2+3?" not in sub_all and "call_main_1" not in sub_all,
              f"messages={msgs2}")
    if main_reqs:
        tools = main_reqs[0].get("tools", [])
        tool_names = [t["function"]["name"] for t in tools]
        calc = next((t for t in tools
                     if t["function"]["name"] == "calculator"), None)
        check("main agent sees the calculator tool", calc is not None,
              f"tools={tool_names}")
        if calc:
            fn = calc["function"]
            check("calculator description forwarded",
                  fn.get("description") == "A mathematic helper", f"fn={fn}")
            params = fn.get("parameters", {})
            check("calculator schema has required expression",
                  params.get("required") == ["expression"], f"params={params}")
            props = params.get("properties", {})
            check("calculator schema property type",
                  props.get("expression", {}).get("type") == "string", f"props={props}")
        # The main agent must not see the subagent's private MCP tools.
        check("main agent does not see subagent private tools",
              "calc.echo" not in tool_names and "calc.get_time" not in tool_names,
              f"tools={tool_names}")


def scenario_b(port, tmp):
    """Name-only MCP reference resolving to the main mcps list."""
    print("[scenario] subagent referencing the main agent's MCP server")
    MockLLM.bodies.clear()
    convo = os.path.join(tmp, "convo_b.jsonl")
    if os.path.exists(convo):
        os.unlink(convo)
    cfg_path = os.path.join(tmp, "agent_b.yml")
    with open(cfg_path, "w") as f:
        f.write(
            f"llm:\n"
            f'  api_base: "http://127.0.0.1:{port}/v1"\n'
            f'  api_key: "test"\n'
            f'  model: "mock-model"\n'
            f"agent:\n"
            f'  system_prompt: "{MAIN_SYS}"\n'
            f"mcps:\n"
            f"  - name: shared\n"
            f'    cmdline: "python3 {FIX}/fake_mcp.py"\n'
            f'    init_timeout: "5s"\n'
            f'    call_timeout: "5s"\n'
            f"subagents:\n"
            f"  - tool_definition:\n"
            f"      name: calculator\n"
            f'    system_prompt: "{SUB_SYS}"\n'
            f'    user_prompt: "Go"\n'
            f"    mcps:\n"
            f"      - name: shared\n"
        )

    proc = subprocess.run(
        [BIN, "agent", "-c", cfg_path, "--conversation", convo, "-p", "use the calculator"],
        capture_output=True,
        text=True,
        timeout=60,
        cwd=ROOT,
    )
    check("agent exit code 0 (reference config)", proc.returncode == 0,
          f"exit={proc.returncode} stderr={proc.stderr[-400:]}")
    if proc.returncode != 0:
        print(proc.stdout)
        print(proc.stderr)
        return

    entries = read_jsonl(convo)
    tr = next((e for e in entries
               if e.get("type") == "tool_result" and "run_id" not in e), {})
    check("referenced server answered inside the subagent",
          (tr.get("result") or "") == "The expression evaluates to 42.", f"tr={tr}")

    # The mock asks for "calc.echo" but the referenced server only exposes
    # shared.* tools: the failure the old temp-file design hid is now part
    # of the retained trace.
    nested_fail = next((e for e in entries
                        if e.get("type") == "tool_result" and e.get("run_id")), {})
    check("nested trace retains the inner tool failure",
          nested_fail.get("is_error") is True and
          (nested_fail.get("result") or "") == "Tool definition not found",
          f"nested_fail={nested_fail}")

    # The subagent must see the referenced server's namespaced tools.
    sub_reqs = []
    with MockLLM.lock:
        for b in MockLLM.bodies:
            req = json.loads(b)
            msgs = req.get("messages", [])
            system = next((m.get("content") for m in msgs if m.get("role") == "system"), "")
            if system == SUB_SYS:
                sub_reqs.append(req)
    if sub_reqs:
        tool_names = sorted(t["function"]["name"] for t in sub_reqs[0].get("tools", []))
        check("subagent sees referenced server tools (shared.* namespace)",
              tool_names == ["shared.echo", "shared.get_time"], f"tools={tool_names}")


def scenario_d(port, tmp):
    """Nested subagents: a depth-2 trace bracketed inside the depth-1 one."""
    print("[scenario] nested subagents (depth 2)")
    MockLLM.bodies.clear()
    convo = os.path.join(tmp, "convo_d.jsonl")
    if os.path.exists(convo):
        os.unlink(convo)
    cfg_path = os.path.join(tmp, "agent_d.yml")
    with open(cfg_path, "w") as f:
        f.write(
            f"llm:\n"
            f'  api_base: "http://127.0.0.1:{port}/v1"\n'
            f'  api_key: "test"\n'
            f'  model: "mock-model"\n'
            f"agent:\n"
            f'  system_prompt: "{D_MAIN_SYS}"\n'
            f"subagents:\n"
            f"  - tool_definition:\n"
            f"      name: dispatcher\n"
            f"      description: dispatches work\n"
            f"      attributes:\n"
            f"        where:\n"
            f"          type: string\n"
            f'    system_prompt: "{D_SUB_SYS}"\n'
            f'    user_prompt: "Dig {{where}}"\n'
            f"    subagents:\n"
            f"      - tool_definition:\n"
            f"          name: digger\n"
            f"          description: digs where told\n"
            f"          attributes:\n"
            f"            where:\n"
            f"              type: string\n"
            f'        system_prompt: "{D_INNER_SYS}"\n'
            f'        user_prompt: "Dig at {{where}}"\n'
        )

    proc = subprocess.run(
        [BIN, "agent", "-c", cfg_path, "--conversation", convo, "-p", "find gold"],
        capture_output=True,
        text=True,
        timeout=60,
        cwd=ROOT,
    )
    check("agent exit code 0 (nested config)", proc.returncode == 0,
          f"exit={proc.returncode} stderr={proc.stderr[-400:]}")
    if proc.returncode != 0:
        print(proc.stdout)
        print(proc.stderr)
        return

    entries = read_jsonl(convo)
    top = [e for e in entries if "run_id" not in e or e.get("type") == "meta"]
    check("top-level sequence stays clean with nesting",
          [e.get("type") for e in top] ==
          ["meta", "user", "assistant", "tool_call", "tool_result", "assistant"],
          f"types={[e.get('type') for e in top]}")
    check("top-level tool_result carries the level-1 answer",
          (top[-2].get("result") or "") == "Level one got: GOLD", f"tr={top[-2]}")

    starts = [e for e in entries if e.get("type") == "subagent_start"]
    ends = [e for e in entries if e.get("type") == "subagent_end"]
    check("two bracket pairs (depth 1 and 2)",
          len(starts) == 2 and len(ends) == 2, f"starts={len(starts)} ends={len(ends)}")
    d1 = next((e for e in starts if e.get("depth") == 1), {})
    d2 = next((e for e in starts if e.get("depth") == 2), {})
    check("depth-1 bracket identifies the dispatcher",
          d1.get("subagent") == "dispatcher" and d1.get("call_id") == "call_d1",
          f"d1={d1}")
    check("depth-2 bracket identifies the digger and its caller",
          d2.get("subagent") == "digger" and d2.get("call_id") == "call_d2",
          f"d2={d2}")
    check("each level has its own run_id",
          d1.get("run_id") and d2.get("run_id") and d1.get("run_id") != d2.get("run_id"),
          f"d1={d1.get('run_id')} d2={d2.get('run_id')}")

    i_d1s = entries.index(d1)
    i_d2s = entries.index(d2)
    i_d2e = entries.index(next(e for e in ends if e.get("run_id") == d2.get("run_id")))
    i_d1e = entries.index(next(e for e in ends if e.get("run_id") == d1.get("run_id")))
    check("depth-2 block is nested inside the depth-1 block",
          i_d1s < i_d2s < i_d2e < i_d1e,
          f"{i_d1s} < {i_d2s} < {i_d2e} < {i_d1e}")

    d1_tc = next((e for e in entries[i_d1s:i_d1e]
                  if e.get("type") == "tool_call" and e.get("depth") == 1), {})
    check("depth-1 run records its call into the nested subagent",
          d1_tc.get("name") == "digger" and d1_tc.get("id") == "call_d2" and
          d1_tc.get("mcp_server") == "subagent", f"d1_tc={d1_tc}")

    inner = entries[i_d2s + 1:i_d2e]
    check("depth-2 block holds the inner sub-conversation",
          [e.get("type") for e in inner] == ["user", "assistant"],
          f"types={[e.get('type') for e in inner]}")
    check("depth-2 entries share the depth-2 scope",
          all(e.get("depth") == 2 and e.get("subagent") == "digger" and
              e.get("run_id") == d2.get("run_id") for e in inner), f"inner={inner}")
    d1_tr = next((e for e in entries[i_d1s:i_d1e]
                  if e.get("type") == "tool_result" and e.get("depth") == 1), {})
    check("depth-1 tool_result holds the depth-2 answer",
          (d1_tr.get("result") or "") == "GOLD", f"d1_tr={d1_tr}")

    check("quiet stdout prints only the main answer",
          proc.stdout.strip() == "Dispatch done.", f"stdout={proc.stdout!r}")
    resp = subprocess.run(
        [BIN, "response", "--conversation", convo],
        capture_output=True, text=True, timeout=30, cwd=ROOT)
    check("response command returns the main answer under nesting",
          resp.returncode == 0 and resp.stdout.strip() == "Dispatch done.",
          f"exit={resp.returncode} stdout={resp.stdout!r}")


def scenario_c(tmp):
    """Config validation failures exit 1 with a clear message."""
    print("[scenario] config validation failures")
    bad = os.path.join(tmp, "bad.yml")
    with open(bad, "w") as f:
        f.write(
            "llm:\n  api_base: http://127.0.0.1:9/v1\n"
            "subagents:\n"
            "  - tool_definition:\n      name: c1\n"
            "    mcps:\n      - name: nosuch\n"
        )
    proc = subprocess.run(
        [BIN, "agent", "-c", bad, "-p", "hi"],
        capture_output=True, text=True, timeout=30, cwd=ROOT,
    )
    check("unresolved reference exits 1", proc.returncode == 1,
          f"exit={proc.returncode} stderr={proc.stderr[-200:]}")


def main():
    print("=== test_agent_subagent ===")
    port = free_port()
    ready = threading.Event()
    t = threading.Thread(target=run_llm_mock, args=(port, ready), daemon=True)
    t.start()
    ready.wait(5.0)

    tmp = "/tmp/llmkit_subagent_itest"
    os.makedirs(tmp, exist_ok=True)

    scenario_a(port, tmp)
    scenario_b(port, tmp)
    scenario_d(port, tmp)
    scenario_c(tmp)

    print()
    if tests_failed:
        print(f"{tests_run - len(tests_failed)}/{tests_run} checks passed")
        print("FAILED:", ", ".join(tests_failed))
        return 1
    print(f"All {tests_run} checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
