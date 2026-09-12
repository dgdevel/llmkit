#!/usr/bin/env python3
"""
Built-in tools (`llmkit mcp`) integration test.

Drives the real `llmkit mcp` binary (stdio and HTTP modes) against a local
HTTP server, validating:

  - a missing or unknown tool list is rejected with exit code 2
  - initialize reports the llmkit-tools serverInfo with a tools capability
  - tools/list exposes exactly the requested tools
  - online_fetch converts a served HTML page to markdown (title heading,
    no scripts, links preserved)
  - online_fetch passes non-HTML text through unchanged
  - online_fetch returns "HTTP Status <code>" for non-200 responses
  - online_fetch truncates output over 100000 characters with a note
  - online_search without a string `query` argument is an error
  - unknown tools and unknown methods are JSON-RPC errors
  - notifications get no reply; ping answers {}
  - HTTP listen mode serves the same protocol

online_search's live DuckDuckGo call is intentionally not exercised here
(network-dependent); its parser is covered by tests/test_tools.c.
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


def run_tools(args, requests, timeout=60):
    """Run `llmkit mcp` over stdio; return (returncode, list_of_parsed_json)."""
    stdin = "\n".join(json.dumps(r) for r in requests) + "\n"
    proc = subprocess.run(
        [BIN, "mcp"] + args,
        input=stdin, capture_output=True, text=True, timeout=timeout, cwd=ROOT,
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


def text_of(response):
    return response["result"]["content"][0]["text"]


# ---------------------------------------------------------------------
# Local HTTP server with deterministic fixtures
# ---------------------------------------------------------------------

BIG_PAGE = "<html><head><title>Big</title></head><body><p>" + ("filler " * 30000) + "</p></body></html>"

PAGES = {
    "/page": (
        200,
        "text/html; charset=utf-8",
        "<html><head><title>My Test Page</title>"
        "<script>window.evil = 1;</script><style>p { color: red; }</style></head>"
        "<body><nav>menu stuff</nav><article><h1>Welcome</h1>"
        "<p>Here is a <a href='https://example.com/deep'>deep link</a> and "
        "<b>bold</b> text.</p><ul><li>one</li><li>two</li></ul></article></body></html>",
    ),
    "/plain": (200, "text/plain", "just plain text\nline two\n"),
    "/json": (200, "application/json", '{"key": "value"}'),
    "/missing": (404, "text/html", "<html><body>not found</body></html>"),
    "/big": (200, "text/html", BIG_PAGE),
    "/binary": (200, "image/png", b"\x89PNG-not-really"),
}


def start_server():
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            entry = PAGES.get(self.path)
            if entry is None:
                self.send_response(404)
                self.send_header("Content-Type", "text/plain")
                body = b"no route"
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            status, ctype, body = entry
            if isinstance(body, str):
                body = body.encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, fmt, *args):
            pass

    srv = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=srv.serve_forever, daemon=True)
    thread.start()
    return srv, f"http://127.0.0.1:{srv.server_address[1]}"


# ---------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------

def test_cli_arg_validation():
    print("CLI argument validation:")
    proc = subprocess.run([BIN, "mcp"], capture_output=True, text=True, timeout=30, cwd=ROOT)
    check("no tool list exits 2", proc.returncode == 2, f"rc={proc.returncode}")
    check("no tool list names available tools",
          "online_search" in proc.stderr and "online_fetch" in proc.stderr,
          f"stderr={proc.stderr!r}")

    proc = subprocess.run([BIN, "mcp", "no_such_tool"], capture_output=True, text=True,
                          timeout=30, cwd=ROOT)
    check("unknown tool exits 2", proc.returncode == 2, f"rc={proc.returncode}")
    check("unknown tool named in error", "no_such_tool" in proc.stderr,
          f"stderr={proc.stderr!r}")


def test_initialize_and_listing():
    print("initialize and tools/list:")
    rc, responses = run_tools(["online_search,online_fetch"], [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
        {"jsonrpc": "2.0", "id": 2, "method": "ping"},
        {"jsonrpc": "2.0", "method": "notifications/initialized"},
        {"jsonrpc": "2.0", "id": 3, "method": "tools/list"},
    ])
    check("serves requests", rc == 0, f"rc={rc}")

    init = resp_for(responses, 1)
    check("initialize answered", init is not None)
    info = init.get("result", {}).get("serverInfo", {})
    check("serverInfo is llmkit-tools", info.get("name") == "llmkit-tools", f"{info}")
    check("protocol version present",
          bool(init.get("result", {}).get("protocolVersion")),
          "no protocolVersion")

    check("notification gets no response",
          len(responses) == 3, f"got {len(responses)} responses")
    ping = resp_for(responses, 2)
    check("ping answered with empty result", ping is not None and ping.get("result") == {},
          f"{ping}")

    listing = resp_for(responses, 3)
    names = [t["name"] for t in listing.get("result", {}).get("tools", [])]
    check("tools/list exposes both tools",
          names == ["online_search", "online_fetch"], f"{names}")
    schemas = {t["name"]: t.get("inputSchema", {}) for t in listing.get("result", {}).get("tools", [])}
    check("search schema requires query",
          schemas.get("online_search", {}).get("required") == ["query"], f"{schemas}")

    rc, responses = run_tools(["online_search"], [
        {"jsonrpc": "2.0", "id": 1, "method": "tools/list"},
    ])
    listing = resp_for(responses, 1)
    names = [t["name"] for t in listing.get("result", {}).get("tools", [])]
    check("subset selection exposes one tool", names == ["online_search"], f"{names}")


def test_online_fetch(base):
    print("online_fetch:")
    url = base + "/page"
    rc, responses = run_tools(["online_fetch"], [
        {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
         "params": {"name": "online_fetch", "arguments": {"url": url}}},
    ])
    resp = resp_for(responses, 1)
    check("fetch answered", resp is not None)
    check("fetch is not an error", resp.get("result", {}).get("isError") is False, f"{resp}")
    text = text_of(resp)
    check("title becomes heading", text.startswith("# My Test Page"), f"{text[:120]!r}")
    check("content converted to markdown", "Here is a" in text and "- one" in text,
          f"{text[:200]!r}")
    check("link preserved", "[deep link](https://example.com/deep)" in text,
          f"{text[:200]!r}")
    check("script stripped", "window.evil" not in text, f"{text[:200]!r}")
    check("nav stripped", "menu stuff" not in text, f"{text[:200]!r}")

    rc, responses = run_tools(["online_fetch"], [
        {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
         "params": {"name": "online_fetch", "arguments": {"url": base + "/plain"}}},
    ])
    text = text_of(resp_for(responses, 1))
    check("plain text passed through", text == "just plain text\nline two\n", f"{text!r}")

    rc, responses = run_tools(["online_fetch"], [
        {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
         "params": {"name": "online_fetch", "arguments": {"url": base + "/json"}}},
    ])
    text = text_of(resp_for(responses, 1))
    check("json passed through", '"key": "value"' in text, f"{text!r}")

    rc, responses = run_tools(["online_fetch"], [
        {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
         "params": {"name": "online_fetch", "arguments": {"url": base + "/missing"}}},
    ])
    text = text_of(resp_for(responses, 1))
    check("404 reported as HTTP Status 404", text == "HTTP Status 404", f"{text!r}")

    rc, responses = run_tools(["online_fetch"], [
        {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
         "params": {"name": "online_fetch", "arguments": {"url": "not-a-url"}}},
    ])
    resp = resp_for(responses, 1)
    check("non-http URL rejected", "error" in resp, f"{resp}")

    rc, responses = run_tools(["online_fetch"], [
        {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
         "params": {"name": "online_fetch", "arguments": {}}},
    ])
    resp = resp_for(responses, 1)
    check("missing url is a JSON-RPC error", "error" in resp, f"{resp}")

    rc, responses = run_tools(["online_fetch"], [
        {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
         "params": {"name": "online_fetch", "arguments": {"url": base + "/big"}}},
    ])
    text = text_of(resp_for(responses, 1))
    check("big page truncated at 100k", len(text) < 101000, f"len={len(text)}")
    check("truncation note present", "[content truncated at 100000 characters]" in text,
          "note missing")

    rc, responses = run_tools(["online_fetch"], [
        {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
         "params": {"name": "online_fetch", "arguments": {"url": base + "/binary"}}},
    ])
    text = text_of(resp_for(responses, 1))
    check("binary content announced", "non-text content" in text, f"{text!r}")


def test_online_search_errors():
    print("online_search argument handling:")
    rc, responses = run_tools(["online_search"], [
        {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
         "params": {"name": "online_search", "arguments": {}}},
    ])
    resp = resp_for(responses, 1)
    check("missing query is a JSON-RPC error", "error" in resp, f"{resp}")

    rc, responses = run_tools(["online_search"], [
        {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
         "params": {"name": "online_fetch", "arguments": {"url": "https://example.com"}}},
    ])
    resp = resp_for(responses, 1)
    check("disabled tool is unknown", "error" in resp, f"{resp}")


def test_http_mode(base):
    print("HTTP listen mode:")
    import http.client
    port = free_port()
    proc = subprocess.Popen([BIN, "mcp", "online_search,online_fetch", "-l", f"127.0.0.1:{port}"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=ROOT)
    try:
        # Wait for the server to accept connections.
        import time
        conn = None
        for _ in range(50):
            try:
                conn = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
                conn.connect()
                break
            except OSError:
                if conn is not None:
                    conn.close()
                conn = None
                time.sleep(0.1)
        check("HTTP server came up", conn is not None)

        body = json.dumps({"jsonrpc": "2.0", "id": 7, "method": "tools/list"})
        conn.request("POST", "/", body=body, headers={"Content-Type": "application/json"})
        resp = json.loads(conn.getresponse().read().decode("utf-8"))
        names = [t["name"] for t in resp.get("result", {}).get("tools", [])]
        check("HTTP tools/list works", names == ["online_search", "online_fetch"], f"{names}")

        body = json.dumps({"jsonrpc": "2.0", "id": 8, "method": "tools/call",
                           "params": {"name": "online_fetch",
                                      "arguments": {"url": base + "/plain"}}})
        conn.request("POST", "/", body=body, headers={"Content-Type": "application/json"})
        resp = json.loads(conn.getresponse().read().decode("utf-8"))
        check("HTTP tools/call works",
              resp.get("result", {}).get("content", [{}])[0].get("text") ==
              "just plain text\nline two\n", f"{resp}")
        conn.close()
    finally:
        proc.terminate()
        proc.wait(timeout=10)


def main():
    print("llmkit mcp integration test")
    srv, base = start_server()
    try:
        test_cli_arg_validation()
        test_initialize_and_listing()
        test_online_fetch(base)
        test_online_search_errors()
        test_http_mode(base)
    finally:
        srv.shutdown()

    print(f"\n{tests_run} checks, {len(tests_failed)} failed")
    if tests_failed:
        for name in tests_failed:
            print(f"  FAILED: {name}")
        sys.exit(1)
    sys.exit(0)


if __name__ == "__main__":
    main()
