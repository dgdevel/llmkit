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

The proxy talks line-delimited JSON-RPC over stdio.  We feed a fixed
request script and assert on the JSON responses.
"""
import json
import os
import subprocess
import sys

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


def run_proxy(cfg_yaml, requests):
    """Run the proxy over stdio; return (returncode, list_of_parsed_json)."""
    tmp = "/tmp/llmkit_proxy_itest"
    os.makedirs(tmp, exist_ok=True)
    cfg_path = os.path.join(tmp, "proxy.yml")
    with open(cfg_path, "w") as f:
        f.write(cfg_yaml)
    stdin = "\n".join(json.dumps(r) for r in requests) + "\n"
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


def main():
    print("=== test_proxy_integration ===")
    scenario_initialize_stable = True
    scenario_ping_and_initialize()
    scenario_namespace()
    scenario_whitelist()
    scenario_blacklist()
    scenario_rename()
    scenario_redefine()
    scenario_hide()
    scenario_multi_backend()
    scenario_tools_call_routing()
    print(f"\n{tests_run} checks, {len(tests_failed)} failed")
    return 1 if tests_failed else 0


if __name__ == "__main__":
    sys.exit(main())
