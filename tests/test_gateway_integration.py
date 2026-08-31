#!/usr/bin/env python3
"""
Gateway integration test.

Drives the real `llmkit gateway` binary (stdio and HTTP modes) against a
mock MCP backend and a mock OpenAI-compatible LLM, validating:

  - initialize reports the llmkit-gateway serverInfo with a tools capability
  - tools/list exposes exactly the two gateway tools (discover, invoke)
  - discover with a working LLM returns the LLM-selected tools' full specs
    (name, description, inputSchema, server) and matched_by == "llm"
  - discover falls back to keyword matching when the LLM is unreachable
  - discover respects backend `hide` and `whitelist` filtering
  - discover without a string `query` argument is an error
  - invoke forwards name + arguments to the backend and echoes the request id
  - invoke of an unknown tool is a JSON-RPC error
  - HTTP listen mode serves the same protocol
"""
import http.client
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
    port = s.getsockname()[1]
    s.close()
    return port


def run_gateway(cfg_yaml, requests):
    """Run the gateway over stdio; return (returncode, list_of_parsed_json)."""
    tmp = "/tmp/llmkit_gateway_itest"
    os.makedirs(tmp, exist_ok=True)
    cfg_path = os.path.join(tmp, "gateway.yml")
    with open(cfg_path, "w") as f:
        f.write(cfg_yaml)
    stdin = "\n".join(json.dumps(r) for r in requests) + "\n"
    proc = subprocess.run(
        [BIN, "gateway", "-c", cfg_path],
        input=stdin, capture_output=True, text=True, timeout=60, cwd=ROOT,
    )
    responses = []
    for line in proc.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            responses.append(json.loads(line))
        except json.JSONDecodeError:
            pass
    return proc.returncode, responses


def resp_for(responses, rid):
    for r in responses:
        if r.get("id") == rid:
            return r
    return None


def result_text(response):
    """Extract the text of the first content item of a tools/call result."""
    content = response.get("result", {}).get("content", [])
    if content and content[0].get("type") == "text":
        return content[0].get("text", "")
    return None


# ---------------------------------------------------------------------------
# Mock OpenAI-compatible chat completions server: always selects fs.get_time
# ---------------------------------------------------------------------------
class MockLLM(BaseHTTPRequestHandler):
    bodies = []
    lock = threading.Lock()

    def log_message(self, *a):
        pass  # silence

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8", "replace")
        with MockLLM.lock:
            MockLLM.bodies.append(body)
        resp = {
            "id": "chat_mock",
            "model": "mock",
            "choices": [{
                "index": 0,
                "message": {"role": "assistant", "content": '["fs.get_time"]'},
                "finish_reason": "stop",
            }],
            "usage": {"prompt_tokens": 10, "completion_tokens": 4, "total_tokens": 14},
        }
        data = json.dumps(resp).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def main():
    print("=== test_gateway_integration ===")

    # ---------------- stdio mode: basic protocol ----------------
    print("-- basic protocol --")
    cfg = (
        'llm:\n'
        '  api_base: "http://127.0.0.1:1/v1"\n'  # unreachable -> keyword fallback
        '  model: "mock"\n'
        'mcps:\n'
        f'  - name: "fs"\n'
        f'    cmdline: "python3 {FIX}/fake_mcp.py"\n'
        '    namespace: "fs"\n'
    )
    rc, resps = run_gateway(cfg, [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
        {"jsonrpc": "2.0", "method": "notifications/initialized"},
        {"jsonrpc": "2.0", "id": 2, "method": "ping"},
        {"jsonrpc": "2.0", "id": 3, "method": "tools/list"},
    ])
    check("exit code 0", rc == 0, f"rc={rc}")

    init = resp_for(resps, 1)
    check("initialize echoes id", init is not None)
    if init:
        info = init.get("result", {}).get("serverInfo", {})
        cap = init.get("result", {}).get("capabilities", {})
        check("serverInfo name", info.get("name") == "llmkit-gateway", str(info))
        check("tools capability only", "tools" in cap and "resources" not in cap
              and "prompts" not in cap, str(cap))
        check("protocolVersion present",
              bool(init.get("result", {}).get("protocolVersion")))

    check("notification gets no response", len(resps) == 3, str(resps))

    ping = resp_for(resps, 2)
    check("ping returns empty result", ping is not None and ping.get("result") == {})

    tl = resp_for(resps, 3)
    names = [t.get("name") for t in tl.get("result", {}).get("tools", [])] if tl else []
    check("tools/list exactly discover+invoke", names == ["discover", "invoke"], str(names))
    if names == ["discover", "invoke"]:
        tools = tl["result"]["tools"]
        check("discover schema requires query",
              tools[0].get("inputSchema", {}).get("required") == ["query"],
              json.dumps(tools[0].get("inputSchema")))
        check("invoke schema requires name",
              tools[1].get("inputSchema", {}).get("required") == ["name"],
              json.dumps(tools[1].get("inputSchema")))

    # ---------------- keyword fallback (LLM unreachable) ----------------
    print("-- discover keyword fallback --")
    rc, resps = run_gateway(cfg, [
        {"jsonrpc": "2.0", "id": 10, "method": "tools/call",
         "params": {"name": "discover", "arguments": {"query": "current time"}}},
        {"jsonrpc": "2.0", "id": 11, "method": "tools/call",
         "params": {"name": "discover", "arguments": {"query": "echo my input"}}},
        {"jsonrpc": "2.0", "id": 12, "method": "tools/call",
         "params": {"name": "discover", "arguments": {"query": "zzzqqq xyzzy"}}},
        {"jsonrpc": "2.0", "id": 13, "method": "tools/call",
         "params": {"name": "discover", "arguments": {}}},
    ])
    d_time = resp_for(resps, 10)
    payload = json.loads(result_text(d_time)) if d_time else {}
    check("keyword fallback flagged", payload.get("matched_by") == "keyword",
          json.dumps(payload))
    got = [t.get("name") for t in payload.get("tools", [])]
    check("keyword matches get_time", got == ["fs.get_time"], str(got))
    if got == ["fs.get_time"]:
        spec = payload["tools"][0]
        check("full specs returned",
              spec.get("description") == "Get the current time"
              and spec.get("inputSchema", {}).get("type") == "object"
              and spec.get("server") == "fs",
              json.dumps(spec))

    d_echo = resp_for(resps, 11)
    payload = json.loads(result_text(d_echo)) if d_echo else {}
    got = [t.get("name") for t in payload.get("tools", [])]
    check("keyword matches echo", got == ["fs.echo"], str(got))

    d_none = resp_for(resps, 12)
    payload = json.loads(result_text(d_none)) if d_none else {}
    check("no match -> empty tools", payload.get("tools") == [], json.dumps(payload))

    d_bad = resp_for(resps, 13)
    check("missing query is error",
          d_bad is not None and "error" in d_bad and "query" in d_bad["error"]["message"],
          json.dumps(d_bad))

    # ---------------- LLM-driven discovery ----------------
    print("-- discover via LLM --")
    port = free_port()
    llm_ready = threading.Event()

    def run_llm_mock():
        httpd = ThreadingHTTPServer(("127.0.0.1", port), MockLLM)
        llm_ready.set()
        httpd.serve_forever()

    th = threading.Thread(target=run_llm_mock, daemon=True)
    th.start()
    llm_ready.wait(5)

    cfg_llm = (
        'llm:\n'
        f'  api_base: "http://127.0.0.1:{port}/v1"\n'
        '  model: "mock"\n'
        'mcps:\n'
        f'  - name: "fs"\n'
        f'    cmdline: "python3 {FIX}/fake_mcp.py"\n'
        '    namespace: "fs"\n'
    )
    rc, resps = run_gateway(cfg_llm, [
        {"jsonrpc": "2.0", "id": 20, "method": "tools/call",
         "params": {"name": "discover", "arguments": {"query": "what time is it"}}},
    ])
    d_llm = resp_for(resps, 20)
    payload = json.loads(result_text(d_llm)) if d_llm else {}
    check("llm matched_by", payload.get("matched_by") == "llm", json.dumps(payload))
    got = [t.get("name") for t in payload.get("tools", [])]
    check("llm selection returns specs", got == ["fs.get_time"], str(got))
    bodies = list(MockLLM.bodies)
    check("llm received query + catalog",
          len(bodies) == 1 and "what time is it" in bodies[0]
          and "fs.get_time" in bodies[0] and "fs.echo" in bodies[0],
          f"bodies={len(bodies)}")
    if bodies:
        req = json.loads(bodies[0])
        check("discovery request carries no tools block",
              req.get("tools") in (None, []), str(req.get("tools"))[:80])

    # ---------------- filtering: hide and whitelist ----------------
    print("-- hide / whitelist filtering --")
    cfg_hide = (
        'llm:\n'
        '  api_base: "http://127.0.0.1:1/v1"\n'
        '  model: "mock"\n'
        'mcps:\n'
        f'  - name: "fs"\n'
        f'    cmdline: "python3 {FIX}/fake_mcp.py"\n'
        '    namespace: "fs"\n'
        f'  - name: "hid"\n'
        f'    cmdline: "python3 {FIX}/fake_mcp.py"\n'
        '    namespace: "hid"\n'
        '    hide: true\n'
    )
    rc, resps = run_gateway(cfg_hide, [
        {"jsonrpc": "2.0", "id": 30, "method": "tools/call",
         "params": {"name": "discover", "arguments": {"query": "time"}}},
    ])
    payload = json.loads(result_text(resp_for(resps, 30))) if resps else {}
    got = sorted(t.get("name") for t in payload.get("tools", []))
    check("hidden backend not discoverable", got == ["fs.get_time"], str(got))

    cfg_wl = (
        'llm:\n'
        '  api_base: "http://127.0.0.1:1/v1"\n'
        '  model: "mock"\n'
        'mcps:\n'
        f'  - name: "fs"\n'
        f'    cmdline: "python3 {FIX}/fake_mcp.py"\n'
        '    namespace: "fs"\n'
        '    whitelist:\n'
        '      - "fs.get_time"\n'
    )
    rc, resps = run_gateway(cfg_wl, [
        {"jsonrpc": "2.0", "id": 31, "method": "tools/call",
         "params": {"name": "discover", "arguments": {"query": "echo"}}},
        {"jsonrpc": "2.0", "id": 32, "method": "tools/call",
         "params": {"name": "discover", "arguments": {"query": "time"}}},
    ])
    payload = json.loads(result_text(resp_for(resps, 31))) if resps else {}
    check("whitelist excludes echo", payload.get("tools") == [], json.dumps(payload))
    payload = json.loads(result_text(resp_for(resps, 32))) if resps else {}
    got = [t.get("name") for t in payload.get("tools", [])]
    check("whitelist includes get_time", got == ["fs.get_time"], str(got))

    # ---------------- invoke ----------------
    print("-- invoke --")
    rc, resps = run_gateway(cfg, [
        {"jsonrpc": "2.0", "id": 40, "method": "tools/call",
         "params": {"name": "invoke",
                    "arguments": {"name": "fs.get_time", "arguments": {"tz": "UTC"}}}},
        {"jsonrpc": "2.0", "id": 41, "method": "tools/call",
         "params": {"name": "invoke", "arguments": {"name": "nope.missing"}}},
        {"jsonrpc": "2.0", "id": 42, "method": "tools/call",
         "params": {"name": "invoke", "arguments": {}}},
        {"jsonrpc": "2.0", "id": 43, "method": "tools/call",
         "params": {"name": "teleport", "arguments": {}}},
    ])
    inv = resp_for(resps, 40)
    text = result_text(inv) if inv else None
    check("invoke forwards to backend",
          text is not None and "called:get_time" in text and "UTC" in text, str(text))
    check("invoke echoes request id", inv is not None and inv.get("id") == 40)

    nf = resp_for(resps, 41)
    check("invoke unknown tool errors",
          nf is not None and "error" in nf
          and "not found" in nf["error"]["message"], json.dumps(nf))

    noname = resp_for(resps, 42)
    check("invoke without name errors",
          noname is not None and "error" in noname
          and "name" in noname["error"]["message"], json.dumps(noname))

    unk = resp_for(resps, 43)
    check("unknown gateway tool errors",
          unk is not None and "error" in unk
          and "Unknown tool" in unk["error"]["message"], json.dumps(unk))

    # ---------------- HTTP listen mode ----------------
    print("-- HTTP mode --")
    hport = free_port()
    cfg_http_path = "/tmp/llmkit_gateway_itest/gateway_http.yml"
    with open(cfg_http_path, "w") as f:
        f.write(cfg)
    proc = subprocess.Popen(
        [BIN, "gateway", "-c", cfg_http_path, "-l", f"127.0.0.1:{hport}"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=ROOT,
    )
    try:
        ok = False
        conn = None
        for _ in range(50):
            try:
                conn = http.client.HTTPConnection("127.0.0.1", hport, timeout=5)
                conn.connect()
                ok = True
                break
            except OSError:
                if proc.poll() is not None:
                    break
                import time
                time.sleep(0.1)
        check("http server accepts connections", ok)
        if ok:
            body = json.dumps({"jsonrpc": "2.0", "id": 50, "method": "initialize", "params": {}})
            conn.request("POST", "/", body,
                         {"Content-Type": "application/json",
                          "Content-Length": str(len(body))})
            resp = json.loads(conn.getresponse().read())
            check("http initialize",
                  resp.get("result", {}).get("serverInfo", {}).get("name") == "llmkit-gateway",
                  json.dumps(resp))

            body = json.dumps({
                "jsonrpc": "2.0", "id": 51, "method": "tools/call",
                "params": {"name": "invoke",
                           "arguments": {"name": "fs.get_time", "arguments": {"tz": "EST"}}},
            })
            conn.request("POST", "/", body,
                         {"Content-Type": "application/json",
                          "Content-Length": str(len(body))})
            resp = json.loads(conn.getresponse().read())
            content = resp.get("result", {}).get("content", [{}])[0].get("text", "")
            check("http invoke", "called:get_time" in content and "EST" in content,
                  json.dumps(resp))
            conn.close()
    finally:
        proc.terminate()
        proc.wait(timeout=10)

    # ---------------- summary ----------------
    print(f"\n{tests_run} checks, {len(tests_failed)} failed")
    if tests_failed:
        print("FAILED:", *tests_failed, sep="\n  ")
        return 1
    print("All gateway integration tests passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
