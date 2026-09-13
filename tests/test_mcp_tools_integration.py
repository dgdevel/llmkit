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
  - file_scan matches files by glob, filters by per-line regex, omits
    Lines for binary files, caps at 20 records with an exact "<N> more
    files matching" note, and rejects '..' and absolute globs
  - exec runs a subshell command, reports Exit code/Duration/Output,
    captures stderr with stdout, truncates the tail to the last 3KB
    with a note pointing at the full output under .output/ (no line
    count for binary output), and enabling exec auto-enables
    exec_status
  - exec_status answers for background commands: running pids report
    their uptime, killed pids report 128+signal with the output section,
    unknown pids get a plain answer
  - sleep waits at least the requested seconds, replies "Slept for ...",
    rejects values past the 60 second cap and non-numeric arguments
  - file_create creates/overwrites files and reports byte and line
    counts; file_read renders the "File path / Total lines / ----- lines
    from X to Y -----" report, accepts numeric-string offsets, refuses
    offsets past EOF and binary files; file_edit applies whitespace-
    tolerant replacements ("Edit accepted") and refuses out-of-range or
    missing old_strings with the exact refusal strings
  - file tool paths escaping the working directory ('..', absolute) are
    JSON-RPC errors
  - unknown tools and unknown methods are JSON-RPC errors
  - notifications get no reply; ping answers {}
  - HTTP listen mode serves the same protocol

online_search's live DuckDuckGo call is intentionally not exercised here
(network-dependent); its parser is covered by tests/test_tools.c. file_scan
runs against a throwaway tree under the system temp dir (it resolves globs
against the server process's working directory).
"""
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
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


def run_tools(args, requests, timeout=60, cwd=None):
    """Run `llmkit mcp` over stdio; return (returncode, list_of_parsed_json)."""
    stdin = "\n".join(json.dumps(r) for r in requests) + "\n"
    proc = subprocess.run(
        [BIN, "mcp"] + args,
        input=stdin, capture_output=True, text=True, timeout=timeout,
        cwd=cwd if cwd is not None else ROOT,
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


def test_file_scan():
    print("file_scan:")
    root = tempfile.mkdtemp(prefix="llmkit_fs_")
    os.makedirs(os.path.join(root, "sub", "deep"))

    def write(rel, data):
        with open(os.path.join(root, rel), "wb") as f:
            f.write(data if isinstance(data, bytes) else data.encode("utf-8"))

    write("hello.txt", "alpha one\nbeta two\nalpha three\n")
    write("sub/notes.md", "hello\n")
    write("sub/deep/d.txt", "needle\nplain\nneedle again\n")
    write("b.log", b"BIN\x00ARY\n")
    for i in range(1, 26):
        write(f"t{i:02d}.tmp", f"tmp {i:02d}\n")

    def scan(glob, regex=None, rid=1):
        args = {"filenames_glob": glob}
        if regex is not None:
            args["content_lines_regex"] = regex
        return run_tools(["file_scan"], [
            {"jsonrpc": "2.0", "id": rid, "method": "tools/call",
             "params": {"name": "file_scan", "arguments": args}},
        ], cwd=root)

    try:
        rc, responses = run_tools(["file_scan"], [
            {"jsonrpc": "2.0", "id": 1, "method": "tools/list"},
        ])
        listing = resp_for(responses, 1)
        tools = listing.get("result", {}).get("tools", [])
        names = [t["name"] for t in tools]
        check("file_scan listed alone", names == ["file_scan"], f"{names}")
        schema = tools[0].get("inputSchema", {}) if tools else {}
        check("schema requires filenames_glob",
              schema.get("required") == ["filenames_glob"], f"{schema}")
        check("content_lines_regex is optional",
              "content_lines_regex" in schema.get("properties", {}), f"{schema}")

        rc, responses = scan("**/*.txt", regex="alpha")
        resp = resp_for(responses, 1)
        check("glob+regex answered", resp is not None)
        check("glob+regex is not an error", resp.get("result", {}).get("isError") is False,
              f"{resp}")
        text = text_of(resp)
        check("record has relative path", "Path: hello.txt\nSize: 31b\nLines: 3" in text,
              f"{text!r}")
        check("matching lines listed", "Matching lines: 1, 3" in text, f"{text!r}")
        check("non-matching files excluded", "sub/deep/d.txt" not in text, f"{text!r}")
        check("no absolute paths leaked", root not in text, f"{text!r}")

        rc, responses = scan("*.tmp")
        text = text_of(resp_for(responses, 1))
        check("results capped at 20", text.count("Path: ") == 20, f"{text.count('Path: ')}")
        check("exact remaining count note", "\n\n5 more files matching" in text, f"{text[-80:]!r}")

        rc, responses = scan("**")
        text = text_of(resp_for(responses, 1))
        check("double-star spans directories", "Path: sub/deep/d.txt" in text, f"{text!r}")
        check("binary file has no Lines field",
              "Path: b.log\nSize: 8b\n\n" in text, f"{text!r}")

        rc, responses = scan("*.log", regex="BIN")
        text = text_of(resp_for(responses, 1))
        check("binary excluded from regex scan", text == "No files matching: *.log", f"{text!r}")

        rc, responses = scan("*.zzz")
        text = text_of(resp_for(responses, 1))
        check("no-match message", text == "No files matching: *.zzz", f"{text!r}")

        rc, responses = scan("sub/../../etc")
        resp = resp_for(responses, 1)
        check("'..' glob rejected", "error" in resp and "'..'" in resp["error"]["message"],
              f"{resp}")

        rc, responses = scan("/etc/*")
        resp = resp_for(responses, 1)
        check("absolute glob rejected", "error" in resp and "relative" in resp["error"]["message"],
              f"{resp}")

        rc, responses = scan("*.txt", regex="(")
        resp = resp_for(responses, 1)
        check("invalid regex rejected", "error" in resp and "content_lines_regex" in
              resp["error"]["message"], f"{resp}")

        rc, responses = scan("")
        resp = resp_for(responses, 1)
        check("empty glob rejected", "error" in resp, f"{resp}")

        rc, responses = run_tools(["online_fetch"], [
            {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
             "params": {"name": "file_scan", "arguments": {"filenames_glob": "*"}}},
        ])
        resp = resp_for(responses, 1)
        check("file_scan unknown when not selected", "error" in resp, f"{resp}")
    finally:
        shutil.rmtree(root, ignore_errors=True)


def test_exec():
    print("exec / exec_status (stdio):")
    root = tempfile.mkdtemp(prefix="llmkit_exec_")

    # Enabling exec auto-enables its companion status tool.
    rc, responses = run_tools(["exec"], [
        {"jsonrpc": "2.0", "id": 1, "method": "tools/list"},
    ], cwd=root)
    check("exec server exits cleanly", rc == 0, f"rc={rc}")
    names = [t["name"] for t in resp_for(responses, 1)["result"]["tools"]]
    check("exec enables exec_status", names == ["exec", "exec_status"], f"{names}")
    shutil.rmtree(root)

    root = tempfile.mkdtemp(prefix="llmkit_exec_")
    rc, responses = run_tools(["exec,exec_status"], [
        {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
         "params": {"name": "exec", "arguments": {"cmdline": "echo hello"}}},
        {"jsonrpc": "2.0", "id": 2, "method": "tools/call",
         "params": {"name": "exec", "arguments": {"cmdline": "echo boom >&2; exit 7"}}},
        {"jsonrpc": "2.0", "id": 3, "method": "tools/call",
         "params": {"name": "exec_status", "arguments": {"pid": 4242424}}},
        {"jsonrpc": "2.0", "id": 4, "method": "tools/call",
         "params": {"name": "exec", "arguments": {}}},
    ], cwd=root)
    check("explicit exec_status list accepted", rc == 0, f"rc={rc}")
    text = text_of(resp_for(responses, 1))
    check("exec reports exit code", text.startswith("Exit code: 0\n"), repr(text[:40]))
    check("exec reports duration", "\nDuration: " in text, repr(text))
    check("exec captures stdout", text.endswith("Output:\nhello\n"), repr(text))
    check("small output not truncated", "Output truncated" not in text, repr(text))
    text = text_of(resp_for(responses, 2))
    check("nonzero exit code", "Exit code: 7\n" in text, repr(text[:40]))
    check("stderr captured with stdout", "Output:\nboom\n" in text, repr(text))
    check("unknown pid answered",
          "No exec process with pid 4242424" in text_of(resp_for(responses, 3)),
          repr(text_of(resp_for(responses, 3))))
    check("missing cmdline is an error", "error" in resp_for(responses, 4),
          f"{resp_for(responses, 4)}")

    # Output past the 3KB tail window: note with size + line count, and
    # the full output kept under .output/.
    rc, responses = run_tools(["exec"], [
        {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
         "params": {"name": "exec",
                    "arguments": {"cmdline":
                                  "awk 'BEGIN{for(i=1;i<=20000;i++) print \"line\",i}'"}}},
    ], cwd=root)
    text = text_of(resp_for(responses, 1))
    check("truncation note present",
          "Output truncated, full output in file .output/exec_" in text, repr(text[-120:]))
    check("note carries size and line count",
          re.search(r"\(size \d+(\.\d+)?Kb, 20000 lines\)", text) is not None, repr(text[-120:]))
    check("tail keeps the last line at a line boundary",
          "line 20000\nOutput truncated" in text, repr(text[-120:]))
    # Three exec calls ran in this cwd (echo, exit 7, awk): one log each,
    # and awk's holds the complete output.
    logs = sorted(os.listdir(os.path.join(root, ".output")))
    check("one output log per exec call", len(logs) == 3, f"{logs}")
    full = None
    for name in logs:
        with open(os.path.join(root, ".output", name)) as f:
            data = f.read()
        if data.count("\n") == 20000:
            full = data
            break
    check("full output kept in .output", full is not None, "no 20000-line log found")
    check("full output starts at the first line",
          full is not None and full.startswith("line 1\n"), repr(full[:20] if full else ""))

    # Binary output: still truncated, but the note omits the line count.
    rc, responses = run_tools(["exec"], [
        {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
         "params": {"name": "exec", "arguments": {"cmdline": "head -c 100000 /dev/zero"}}},
    ], cwd=root)
    text = text_of(resp_for(responses, 1))
    check("binary truncation note present", "Output truncated" in text, repr(text[-120:]))
    check("binary note omits the line count", " lines)" not in text, repr(text[-120:]))
    shutil.rmtree(root, ignore_errors=True)


def test_file_tools():
    print("file_read / file_create / file_edit (stdio):")
    root = tempfile.mkdtemp(prefix="llmkit_files_")

    def call(tool, args, rid=1):
        return run_tools([tool], [
            {"jsonrpc": "2.0", "id": rid, "method": "tools/call",
             "params": {"name": tool, "arguments": args}},
        ], cwd=root)

    try:
        rc, responses = run_tools(["file_read,file_create,file_edit,sleep"], [
            {"jsonrpc": "2.0", "id": 1, "method": "tools/list"},
        ])
        listing = resp_for(responses, 1)
        names = [t["name"] for t in listing.get("result", {}).get("tools", [])]
        check("file tools listed",
              names == ["sleep", "file_read", "file_create", "file_edit"], f"{names}")
        schemas = {t["name"]: t.get("inputSchema", {}) for t in
                   listing.get("result", {}).get("tools", [])}
        check("file_read requires filepath",
              schemas.get("file_read", {}).get("required") == ["filepath"], f"{schemas}")
        check("file_create requires filepath and content",
              schemas.get("file_create", {}).get("required") == ["filepath", "content"],
              f"{schemas}")
        check("file_edit requires all four arguments",
              schemas.get("file_edit", {}).get("required") ==
              ["filepath", "linefrom", "old_string", "new_string"], f"{schemas}")
        check("file_read exposes optional range args",
              "line_offset" in schemas.get("file_read", {}).get("properties", {}) and
              "lines_length" in schemas.get("file_read", {}).get("properties", {}), f"{schemas}")

        # file_create + file_read round trip.
        rc, responses = call("file_create",
                             {"filepath": "hello.txt", "content": "one\ntwo\nthree\n"})
        text = text_of(resp_for(responses, 1))
        check("create reply counts bytes and lines",
              text == "File created: hello.txt (14 bytes, 3 lines)", repr(text))

        rc, responses = call("file_create",
                             {"filepath": "hello.txt", "content": "one\ntwo\nthree\n"})
        text = text_of(resp_for(responses, 1))
        check("overwrite reply", text.startswith("File overwritten: hello.txt"), repr(text))

        rc, responses = call("file_read", {"filepath": "hello.txt"})
        text = text_of(resp_for(responses, 1))
        check("read report format",
              text == "File path: hello.txt\nTotal lines: 3\n\n"
                      "----- lines from 1 to 3 -----\none\ntwo\nthree\n", repr(text))

        rc, responses = call("file_read",
                             {"filepath": "hello.txt", "line_offset": 2, "lines_length": 1})
        text = text_of(resp_for(responses, 1))
        check("read range is 1-based inclusive",
              "----- lines from 2 to 2 -----\ntwo\n" in text, repr(text))
        check("total lines covers the whole file", "Total lines: 3" in text, repr(text))

        rc, responses = call("file_read", {"filepath": "hello.txt", "line_offset": "2"})
        text = text_of(resp_for(responses, 1))
        check("numeric string offset accepted", "lines from 2 to 3" in text, repr(text))

        rc, responses = call("file_read", {"filepath": "hello.txt", "line_offset": 99})
        text = text_of(resp_for(responses, 1))
        check("offset past EOF refused",
              "Read refused: line_offset 99 is beyond the end of the file (3 lines)" in text,
              repr(text))

        with open(os.path.join(root, "bin.dat"), "wb") as f:
            f.write(b"AB\x00CD\n")
        rc, responses = call("file_read", {"filepath": "bin.dat"})
        check("binary read refused", text_of(resp_for(responses, 1)) == "Read refused: binary file",
              repr(text_of(resp_for(responses, 1))))

        for bad in ["../escape.txt", "/etc/passwd"]:
            rc, responses = call("file_read", {"filepath": bad})
            check(f"read rejects {bad}", "error" in resp_for(responses, 1),
                  f"{resp_for(responses, 1)}")
        rc, responses = call("file_create", {"filepath": "../escape.txt", "content": "x"})
        check("create rejects '..'", "error" in resp_for(responses, 1), f"{resp_for(responses, 1)}")

        # file_edit: whitespace-tolerant match and re-indentation.
        code = "int main(void) {\n    int x = 1;\n    int y = 2;\n    return x + y;\n}\n"
        rc, responses = call("file_create", {"filepath": "code.c", "content": code})
        rc, responses = call("file_edit", {"filepath": "code.c", "linefrom": 2,
                                           "old_string": "int x = 1;", "new_string": "int x = 42;"})
        text = text_of(resp_for(responses, 1))
        check("edit accepted", text == "Edit accepted", repr(text))

        rc, responses = call("file_read",
                             {"filepath": "code.c", "line_offset": 2, "lines_length": 1})
        text = text_of(resp_for(responses, 1))
        check("edit re-indented to the file", "    int x = 42;" in text, repr(text))

        rc, responses = call("file_edit", {"filepath": "code.c", "linefrom": 20,
                                           "old_string": "return x + y;", "new_string": "return 0;"})
        text = text_of(resp_for(responses, 1))
        check("out-of-range edit refused with the real line",
              text == "Edit refused: old_string found at line 4", repr(text))

        rc, responses = call("file_edit", {"filepath": "code.c", "linefrom": 1,
                                           "old_string": "definitely not here",
                                           "new_string": "x"})
        text = text_of(resp_for(responses, 1))
        check("missing old_string refused", text == "Edit refused: old_string not found",
              repr(text))

        rc, responses = call("file_edit", {"filepath": "nope.c", "linefrom": 1,
                                           "old_string": "a", "new_string": "b"})
        check("edit of a missing file is an error", "error" in resp_for(responses, 1),
              f"{resp_for(responses, 1)}")

        rc, responses = call("file_edit", {"filepath": "code.c",
                                           "old_string": "a", "new_string": "b"})
        check("edit without linefrom is an error", "error" in resp_for(responses, 1),
              f"{resp_for(responses, 1)}")
    finally:
        shutil.rmtree(root, ignore_errors=True)


def test_sleep():
    print("sleep:")
    import time

    def call(seconds, rid=1):
        return run_tools(["sleep"], [
            {"jsonrpc": "2.0", "id": rid, "method": "tools/call",
             "params": {"name": "sleep", "arguments": {"seconds": seconds}}},
        ])

    start = time.monotonic()
    rc, responses = call(1)
    elapsed = time.monotonic() - start
    resp = resp_for(responses, 1)
    check("sleep answered", resp is not None and resp.get("result", {}).get("isError") is False,
          f"{resp}")
    check("sleep 1 waited at least a second", elapsed >= 1.0, f"elapsed={elapsed:.2f}s")
    check("sleep reply", text_of(resp) == "Slept for 1 second.", repr(text_of(resp)))

    rc, responses = call(0)
    check("sleep 0 replies at once", text_of(resp_for(responses, 1)) == "Slept for 0 seconds.",
          repr(text_of(resp_for(responses, 1))))

    rc, responses = call("0.2")
    check("numeric string seconds accepted",
          resp_for(responses, 1).get("result", {}).get("isError") is False, f"{resp_for(responses, 1)}")

    rc, responses = call(61)
    check("sleep past the cap is an error", "error" in resp_for(responses, 1),
          f"{resp_for(responses, 1)}")

    rc, responses = call("abc")
    check("non-numeric seconds is an error", "error" in resp_for(responses, 1),
          f"{resp_for(responses, 1)}")

    rc, responses = run_tools(["sleep"], [
        {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
         "params": {"name": "sleep", "arguments": {}}},
    ])
    check("missing seconds is an error", "error" in resp_for(responses, 1),
          f"{resp_for(responses, 1)}")


def test_exec_background():
    print("exec background lifecycle (HTTP):")
    import http.client
    import time
    root = tempfile.mkdtemp(prefix="llmkit_exec_bg_")
    port = free_port()
    proc = subprocess.Popen([BIN, "mcp", "exec", "-l", f"127.0.0.1:{port}"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=root)
    conn = None
    pid = None
    try:
        # Probe with complete ping requests: the server is single-
        # threaded and a connect-only probe would stall it in its
        # slowloris read until the idle-connection timeout.
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                c = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
                body = json.dumps({"jsonrpc": "2.0", "id": 0, "method": "ping"})
                c.request("POST", "/", body=body, headers={"Content-Type": "application/json"})
                c.getresponse().read()
                c.close()
                conn = True
                break
            except OSError:
                time.sleep(0.1)
        check("HTTP exec server came up", conn is not None)
        if conn is None:
            return

        def call(name, args, rid):
            # One connection per call (the server closes after each
            # response); exec blocks up to 10s server-side, so give the
            # read enough room.
            body = json.dumps({"jsonrpc": "2.0", "id": rid, "method": "tools/call",
                               "params": {"name": name, "arguments": args}})
            c = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
            c.request("POST", "/", body=body, headers={"Content-Type": "application/json"})
            resp = json.loads(c.getresponse().read().decode("utf-8"))
            c.close()
            return resp

        text = call("exec", {"cmdline": "sleep 30"}, 1)["result"]["content"][0]["text"]
        m = re.search(r"^PID: (\d+)$", text, re.M)
        check("background reply names the pid", m is not None, repr(text))
        check("background reply points at exec_status",
              "Process still running, use exec_status(" in text, repr(text))
        if m is None:
            return
        pid = int(m.group(1))

        text = call("exec_status", {"pid": pid}, 2)["result"]["content"][0]["text"]
        check("running pid reports its uptime",
              re.fullmatch(rf"PID {pid} still running, started \S+ ago\.", text) is not None,
              repr(text))

        # SIGKILL is async: poll like a real caller until the exit shows.
        os.kill(pid, signal.SIGKILL)
        text = ""
        for _ in range(100):
            text = call("exec_status", {"pid": pid}, 3)["result"]["content"][0]["text"]
            if "Exit code:" in text:
                break
            time.sleep(0.05)
        check("killed pid reports 128+signal", "Exit code: 137" in text, repr(text))
        check("terminated status reports duration", "\nDuration: " in text, repr(text))
        check("terminated status reports output", "Output:\n" in text, repr(text))
        text = call("exec_status", {"pid": pid}, 4)["result"]["content"][0]["text"]
        check("terminated status is repeatable", "Exit code: 137" in text, repr(text))
    finally:
        if pid is not None:
            try:
                os.kill(pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass
        proc.terminate()
        proc.wait(timeout=10)
        shutil.rmtree(root, ignore_errors=True)


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
        test_file_scan()
        test_exec()
        test_file_tools()
        test_sleep()
        test_exec_background()
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
