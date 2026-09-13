#!/usr/bin/env python3
"""
Phase 12 proxy integration test.

Drives the real `llmkit proxy` binary (stdio mode) against multiple mock
MCP backends and validates the proxy feature matrix:

  - namespace:   backend tool names get the server name as a prefix
  - whitelist:   only listed tools appear in tools/list
  - blacklist:   listed tools are hidden from tools/list
  - rename:      a namespaced name is exposed under a new name
  - redefine:    a tool description is overridden
  - hide:        a whole backend is invisible in aggregate listings
  - multi-backend aggregation: tools from 2 servers merge into one list
  - resources:   resources/list aggregates with name+uri namespacing and
                 filtering; resources/read routes by namespaced uri,
                 forwards backend errors, and rejects unknown uris
  - prompts:     prompts/list aggregates with namespaced names;
                 prompts/get routes by namespaced name, forwards
                 arguments, backend errors, and rejects unknown names
  - malformed:   invalid JSON, missing method, unknown method, and
                 tools/call of an unknown tool return JSON-RPC errors
  - backend failure: a backend that dies after initialize is skipped in
                 aggregate listings and yields "Backend request failed"
                 errors for routed calls; a backend whose cmdline cannot
                 start makes the proxy exit nonzero
  - HTTP listen mode serves the same protocol

The proxy talks line-delimited JSON-RPC over stdio.  We feed a fixed
request script and assert on the JSON responses.
"""
import http.client
import json
import os
import socket
import subprocess
import sys
import time

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


def run_proxy(cfg_yaml, requests):
    """Run the proxy over stdio; return (returncode, list_of_parsed_json).

    Each entry of `requests` may be a dict (encoded as one JSON line) or a
    raw string (sent verbatim, for malformed-line tests)."""
    tmp = "/tmp/llmkit_proxy_itest"
    os.makedirs(tmp, exist_ok=True)
    cfg_path = os.path.join(tmp, "proxy.yml")
    with open(cfg_path, "w") as f:
        f.write(cfg_yaml)
    stdin = "\n".join(r if isinstance(r, str) else json.dumps(r) for r in requests) + "\n"
    proc = subprocess.run(
        [BIN, "proxy", "-c", cfg_path],
        input=stdin, capture_output=True, text=True, timeout=20, cwd=ROOT,
    )
    responses = []
    for line in proc.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            responses.append(json.loads(line))
        except Exception:
            pass
    return proc.returncode, responses


def find_result(responses, req_id):
    for r in responses:
        if r.get("id") == req_id:
            return r.get("result")
    return None


def find_response(responses, req_id):
    """Return the full response object (result or error) for a request id."""
    for r in responses:
        if r.get("id") == req_id:
            return r
    return None


INIT = [
    {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
    {"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}},
]
FAKE = f"python3 {FIX}/fake_mcp.py"


def scenario_namespace():
    print("[scenario] namespace prefixing")
    rc, resp = run_proxy(
        f"mcps:\n  - name: alpha\n    cmdline: \"{FAKE}\"\n",
        INIT + [{"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}}],
    )
    check("proxy exits 0", rc == 0, f"rc={rc}")
    result = find_result(resp, 2)
    tool_names = sorted(t["name"] for t in (result or {}).get("tools", []))
    check("namespaced: alpha.get_time present", "alpha.get_time" in tool_names,
          f"tools={tool_names}")
    check("namespaced: alpha.echo present", "alpha.echo" in tool_names,
          f"tools={tool_names}")


def scenario_whitelist():
    print("[scenario] whitelist hides non-listed tools")
    rc, resp = run_proxy(
        f"mcps:\n  - name: alpha\n    cmdline: \"{FAKE}\"\n    namespace: alpha\n"
        f"    whitelist:\n      - \"alpha.get_time\"\n",
        INIT + [{"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}}],
    )
    check("proxy exits 0", rc == 0, f"rc={rc}")
    result = find_result(resp, 2)
    tool_names = [t["name"] for t in (result or {}).get("tools", [])]
    check("whitelist keeps alpha.get_time", "alpha.get_time" in tool_names,
          f"tools={tool_names}")
    check("whitelist drops alpha.echo", "alpha.echo" not in tool_names,
          f"tools={tool_names}")


def scenario_blacklist():
    print("[scenario] blacklist drops listed tools")
    rc, resp = run_proxy(
        f"mcps:\n  - name: alpha\n    cmdline: \"{FAKE}\"\n    namespace: alpha\n"
        f"    blacklist:\n      - \"alpha.echo\"\n",
        INIT + [{"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}}],
    )
    check("proxy exits 0", rc == 0, f"rc={rc}")
    result = find_result(resp, 2)
    tool_names = [t["name"] for t in (result or {}).get("tools", [])]
    check("blacklist keeps alpha.get_time", "alpha.get_time" in tool_names,
          f"tools={tool_names}")
    check("blacklist drops alpha.echo", "alpha.echo" not in tool_names,
          f"tools={tool_names}")


def scenario_rename():
    print("[scenario] rename exposes tool under new name")
    rc, resp = run_proxy(
        f"mcps:\n  - name: alpha\n    cmdline: \"{FAKE}\"\n    namespace: alpha\n"
        f"    rename:\n      \"alpha.get_time\": \"clock\"\n",
        INIT + [{"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}}],
    )
    check("proxy exits 0", rc == 0, f"rc={rc}")
    result = find_result(resp, 2)
    tool_names = [t["name"] for t in (result or {}).get("tools", [])]
    check("rename exposes 'clock'", "clock" in tool_names, f"tools={tool_names}")
    check("rename removes alpha.get_time", "alpha.get_time" not in tool_names,
          f"tools={tool_names}")


def scenario_redefine():
    print("[scenario] redefine overrides description")
    rc, resp = run_proxy(
        f"mcps:\n  - name: alpha\n    cmdline: \"{FAKE}\"\n    namespace: alpha\n"
        f"    redefine:\n      \"alpha.get_time\": \"NEW DESC\"\n",
        INIT + [{"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}}],
    )
    check("proxy exits 0", rc == 0, f"rc={rc}")
    result = find_result(resp, 2)
    tools = (result or {}).get("tools", [])
    gt = next((t for t in tools if t.get("name") == "alpha.get_time"), None)
    check("redefined description applied",
          gt is not None and gt.get("description") == "NEW DESC",
          f"tool={gt}")


def scenario_hide():
    print("[scenario] hide makes a whole backend invisible")
    rc, resp = run_proxy(
        f"mcps:\n  - name: alpha\n    cmdline: \"{FAKE}\"\n    namespace: alpha\n"
        f"  - name: secret\n    cmdline: \"{FAKE}\"\n    hide: true\n",
        INIT + [{"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}}],
    )
    check("proxy exits 0", rc == 0, f"rc={rc}")
    result = find_result(resp, 2)
    tool_names = [t["name"] for t in (result or {}).get("tools", [])]
    check("alpha tools present", "alpha.get_time" in tool_names, f"tools={tool_names}")
    check("hidden backend tools absent",
          not any(n.startswith("secret") for n in tool_names), f"tools={tool_names}")


def scenario_multi_backend():
    print("[scenario] multi-backend aggregation")
    rc, resp = run_proxy(
        f"mcps:\n  - name: alpha\n    cmdline: \"{FAKE}\"\n    namespace: alpha\n"
        f"  - name: beta\n    cmdline: \"{FAKE}\"\n    namespace: beta\n",
        INIT + [{"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}}],
    )
    check("proxy exits 0", rc == 0, f"rc={rc}")
    result = find_result(resp, 2)
    tool_names = sorted(t["name"] for t in (result or {}).get("tools", []))
    check("aggregates alpha.get_time", "alpha.get_time" in tool_names,
          f"tools={tool_names}")
    check("aggregates beta.get_time", "beta.get_time" in tool_names,
          f"tools={tool_names}")
    check("aggregates alpha.echo + beta.echo",
          "alpha.echo" in tool_names and "beta.echo" in tool_names,
          f"tools={tool_names}")


def scenario_tools_call_routing():
    print("[scenario] tools/call routes to correct backend by namespace")
    rc, resp = run_proxy(
        f"mcps:\n  - name: alpha\n    cmdline: \"{FAKE}\"\n    namespace: alpha\n"
        f"  - name: beta\n    cmdline: \"{FAKE}\"\n    namespace: beta\n",
        INIT + [
            {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}},
            {"jsonrpc": "2.0", "id": 3, "method": "tools/call",
             "params": {"name": "beta.echo", "arguments": {"msg": "hi"}}},
        ],
    )
    check("proxy exits 0", rc == 0, f"rc={rc}")
    result = find_result(resp, 3)
    text = ""
    for c in (result or {}).get("content", []):
        text += c.get("text", "")
    check("backend received de-namespaced name",
          "called:echo" in text, f"content={result}")


def scenario_ping_and_initialize():
    print("[scenario] initialize + ping")
    rc, resp = run_proxy(
        f"mcps:\n  - name: alpha\n    cmdline: \"{FAKE}\"\n",
        [
            {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
            {"jsonrpc": "2.0", "id": 2, "method": "ping", "params": {}},
        ],
    )
    check("proxy exits 0", rc == 0, f"rc={rc}")
    init_result = find_result(resp, 1)
    check("initialize returns protocolVersion",
          (init_result or {}).get("protocolVersion") is not None, f"init={init_result}")
    check("initialize returns serverInfo name llmkit-proxy",
          (init_result or {}).get("serverInfo", {}).get("name") == "llmkit-proxy",
          f"init={init_result}")
    ping_result = find_result(resp, 2)
    check("ping returns a result object", ping_result is not None, f"ping={ping_result}")


def scenario_resources_list():
    print("[scenario] resources/list namespacing and filtering")
    rc, resp = run_proxy(
        f"mcps:\n  - name: alpha\n    cmdline: \"{FAKE}\"\n    namespace: alpha\n"
        f"  - name: beta\n    cmdline: \"{FAKE}\"\n    namespace: beta\n"
        f"    blacklist:\n      - \"beta.notes\"\n",
        INIT + [{"jsonrpc": "2.0", "id": 2, "method": "resources/list", "params": {}}],
    )
    check("proxy exits 0", rc == 0, f"rc={rc}")
    result = find_result(resp, 2)
    resources = (result or {}).get("resources", [])
    names = sorted(r["name"] for r in resources)
    check("resource names get the namespace prefix", "alpha.notes" in names,
          f"resources={names}")
    check("resource uris get the namespace prefix",
          "alpha.memo://notes/demo" in [r.get("uri", "") for r in resources],
          f"resources={resources}")
    check("blacklist drops the beta resource", "beta.notes" not in names,
          f"resources={names}")
    alpha = next((r for r in resources if r.get("name") == "alpha.notes"), None)
    check("resource description preserved",
          alpha is not None and alpha.get("description") == "Demo notes",
          f"resource={alpha}")


def scenario_resources_read():
    print("[scenario] resources/read routing")
    rc, resp = run_proxy(
        f"mcps:\n  - name: alpha\n    cmdline: \"{FAKE}\"\n    namespace: alpha\n",
        INIT + [
            {"jsonrpc": "2.0", "id": 2, "method": "resources/read",
             "params": {"uri": "alpha.memo://notes/demo"}},
            {"jsonrpc": "2.0", "id": 3, "method": "resources/read",
             "params": {"uri": "nowhere://missing"}},
            {"jsonrpc": "2.0", "id": 4, "method": "resources/read",
             "params": {"uri": "alpha.memo://nope"}},
        ],
    )
    check("proxy exits 0", rc == 0, f"rc={rc}")
    result = find_result(resp, 2)
    contents = (result or {}).get("contents", [])
    check("read returns the resource text",
          contents and contents[0].get("text") == "hello from resource",
          f"result={result}")
    r3 = find_response(resp, 3)
    check("uri outside every namespace errors",
          r3 is not None and "error" in r3
          and "Resource not found" in r3["error"]["message"], json.dumps(r3))
    r4 = find_response(resp, 4)
    check("backend resource error forwarded",
          r4 is not None and "error" in r4
          and "Unknown resource" in r4["error"]["message"], json.dumps(r4))


def scenario_prompts_list():
    print("[scenario] prompts/list aggregation")
    rc, resp = run_proxy(
        f"mcps:\n  - name: alpha\n    cmdline: \"{FAKE}\"\n    namespace: alpha\n"
        f"  - name: beta\n    cmdline: \"{FAKE}\"\n    namespace: beta\n",
        INIT + [{"jsonrpc": "2.0", "id": 2, "method": "prompts/list", "params": {}}],
    )
    check("proxy exits 0", rc == 0, f"rc={rc}")
    result = find_result(resp, 2)
    prompts = (result or {}).get("prompts", [])
    names = sorted(p["name"] for p in prompts)
    check("prompt names get the namespace prefix",
          names == ["alpha.greet", "beta.greet"], f"prompts={names}")
    alpha = next((p for p in prompts if p.get("name") == "alpha.greet"), None)
    check("prompt description preserved",
          alpha is not None and alpha.get("description") == "Greet someone",
          f"prompt={alpha}")
    args = (alpha or {}).get("arguments", [])
    check("prompt arguments preserved",
          args and args[0].get("name") == "who" and args[0].get("required") is True,
          f"prompt={alpha}")


def scenario_prompts_get():
    print("[scenario] prompts/get routing")
    rc, resp = run_proxy(
        f"mcps:\n  - name: alpha\n    cmdline: \"{FAKE}\"\n    namespace: alpha\n"
        f"  - name: beta\n    cmdline: \"{FAKE}\"\n    namespace: beta\n",
        INIT + [
            {"jsonrpc": "2.0", "id": 2, "method": "prompts/get",
             "params": {"name": "beta.greet", "arguments": {"who": "world"}}},
            {"jsonrpc": "2.0", "id": 3, "method": "prompts/get",
             "params": {"name": "gamma.greet"}},
            {"jsonrpc": "2.0", "id": 4, "method": "prompts/get",
             "params": {"name": "alpha.nope"}},
        ],
    )
    check("proxy exits 0", rc == 0, f"rc={rc}")
    result = find_result(resp, 2)
    messages = (result or {}).get("messages", [])
    text = messages[0].get("content", {}).get("text", "") if messages else ""
    check("prompt arguments forwarded to backend",
          "please greet world" in text, f"result={result}")
    r3 = find_response(resp, 3)
    check("name outside every namespace errors",
          r3 is not None and "error" in r3
          and "Prompt not found" in r3["error"]["message"], json.dumps(r3))
    r4 = find_response(resp, 4)
    check("backend prompt error forwarded",
          r4 is not None and "error" in r4
          and "Unknown prompt" in r4["error"]["message"], json.dumps(r4))


def scenario_malformed_requests():
    print("[scenario] malformed and unknown requests")
    rc, resp = run_proxy(
        f"mcps:\n  - name: alpha\n    cmdline: \"{FAKE}\"\n",
        [
            {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
            {"jsonrpc": "2.0", "id": 2, "params": {}},
            {"jsonrpc": "2.0", "id": 3, "method": "bogus/method", "params": {}},
            {"jsonrpc": "2.0", "id": 4, "method": "tools/call",
             "params": {"name": "ghost.tool", "arguments": {}}},
            '{"jsonrpc": "2.0", "id": 99, "method": ',
        ],
    )
    check("proxy exits 0", rc == 0, f"rc={rc}")
    r2 = find_response(resp, 2)
    check("missing method errors",
          r2 is not None and "error" in r2
          and "Method not specified" in r2["error"]["message"], json.dumps(r2))
    r3 = find_response(resp, 3)
    check("unknown method errors",
          r3 is not None and "error" in r3
          and "Method not supported" in r3["error"]["message"], json.dumps(r3))
    r4 = find_response(resp, 4)
    check("tools/call of unknown tool errors",
          r4 is not None and "error" in r4
          and "Tool not found" in r4["error"]["message"], json.dumps(r4))
    r99 = next((r for r in resp if r.get("id") is None and "error" in r), None)
    check("invalid JSON line gets a parse error with null id",
          r99 is not None and "Parse error" in r99["error"]["message"], json.dumps(r99))


def scenario_backend_death():
    print("[scenario] backend dying on its next request")
    # alpha answers the initialize handshake, then exits the first time it
    # is asked for tools/list -- every later request to it must error while
    # the healthy beta backend keeps serving.
    dying = f"python3 {FIX}/fake_mcp.py --die-on=tools/list"
    rc, resp = run_proxy(
        f"mcps:\n  - name: alpha\n    cmdline: \"{dying}\"\n    namespace: alpha\n"
        f"  - name: beta\n    cmdline: \"{FAKE}\"\n    namespace: beta\n",
        INIT + [
            {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}},
            {"jsonrpc": "2.0", "id": 3, "method": "tools/call",
             "params": {"name": "alpha.echo", "arguments": {}}},
            {"jsonrpc": "2.0", "id": 4, "method": "resources/read",
             "params": {"uri": "alpha.memo://notes/demo"}},
            {"jsonrpc": "2.0", "id": 5, "method": "prompts/get",
             "params": {"name": "alpha.greet"}},
        ],
    )
    check("proxy exits 0", rc == 0, f"rc={rc}")
    result = find_result(resp, 2)
    names = sorted(t["name"] for t in (result or {}).get("tools", []))
    check("dead backend skipped in tools/list",
          names == ["beta.echo", "beta.get_time"], f"tools={names}")
    for rid, what in ((3, "tools/call"), (4, "resources/read"), (5, "prompts/get")):
        r = find_response(resp, rid)
        check(f"{what} on dead backend errors",
              r is not None and "error" in r
              and "Backend request failed" in r["error"]["message"], json.dumps(r))


def scenario_connect_failure():
    print("[scenario] backend failing to start")
    rc, resp = run_proxy(
        "mcps:\n  - name: ghost\n    cmdline: \"this-binary-does-not-exist\"\n",
        [{"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}}],
    )
    check("proxy exits nonzero when no backend can start", rc != 0, f"rc={rc}")
    check("no JSON output on connect failure", not resp, f"resp={resp}")


def scenario_http_listen():
    print("[scenario] HTTP listen mode")
    port = free_port()
    tmp = "/tmp/llmkit_proxy_itest"
    os.makedirs(tmp, exist_ok=True)
    cfg_path = os.path.join(tmp, "proxy_http.yml")
    with open(cfg_path, "w") as f:
        f.write(f"mcps:\n  - name: alpha\n    cmdline: \"{FAKE}\"\n    namespace: alpha\n")
    proc = subprocess.Popen(
        [BIN, "proxy", "-c", cfg_path, "-l", f"127.0.0.1:{port}"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=ROOT,
    )
    try:
        conn = None
        ok = False
        for _ in range(50):
            try:
                conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
                conn.connect()
                ok = True
                break
            except OSError:
                if proc.poll() is not None:
                    break
                time.sleep(0.1)
        check("http proxy accepts connections", ok, "never came up")
        if ok:

            def post(obj):
                body = json.dumps(obj)
                conn.request("POST", "/", body,
                             {"Content-Type": "application/json",
                              "Content-Length": str(len(body))})
                return json.loads(conn.getresponse().read())

            r = post({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
            check("http initialize reports llmkit-proxy",
                  r.get("result", {}).get("serverInfo", {}).get("name") == "llmkit-proxy",
                  json.dumps(r))
            r = post({"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}})
            names = [t["name"] for t in r.get("result", {}).get("tools", [])]
            check("http tools/list aggregated", "alpha.get_time" in names,
                  json.dumps(names))
            r = post({"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                      "params": {"name": "alpha.echo", "arguments": {"msg": "hi"}}})
            text = r.get("result", {}).get("content", [{}])[0].get("text", "")
            check("http tools/call routed", "called:echo" in text, json.dumps(r))
            r = post({"jsonrpc": "2.0", "id": 4, "method": "resources/read",
                      "params": {"uri": "alpha.memo://notes/demo"}})
            text = r.get("result", {}).get("contents", [{}])[0].get("text", "")
            check("http resources/read routed", text == "hello from resource",
                  json.dumps(r))
            r = post({"jsonrpc": "2.0", "id": 5, "method": "prompts/get",
                      "params": {"name": "alpha.greet", "arguments": {"who": "http"}}})
            msgs = r.get("result", {}).get("messages", [{}])
            check("http prompts/get routed",
                  msgs and "http" in msgs[0].get("content", {}).get("text", ""),
                  json.dumps(r))
            conn.close()
    finally:
        proc.terminate()
        proc.wait(timeout=10)


def main():
    print("=== test_proxy_integration ===")
    scenario_ping_and_initialize()
    scenario_namespace()
    scenario_whitelist()
    scenario_blacklist()
    scenario_rename()
    scenario_redefine()
    scenario_hide()
    scenario_multi_backend()
    scenario_tools_call_routing()
    scenario_resources_list()
    scenario_resources_read()
    scenario_prompts_list()
    scenario_prompts_get()
    scenario_malformed_requests()
    scenario_backend_death()
    scenario_connect_failure()
    scenario_http_listen()
    print(f"\n{tests_run} checks, {len(tests_failed)} failed")
    return 1 if tests_failed else 0


if __name__ == "__main__":
    sys.exit(main())
