#!/bin/sh
# Phase 12 black-box integration tests for the `llmkit` binary.
#
# Exercises the real compiled binary via its CLI: exit codes for
# bad args / empty prompt / MCP init timeout, and proxy end-to-end.
#
# Requires: ./llmkit on PATH (or built in repo root), python3.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${BIN:-$ROOT/llmkit}"
FIX="$ROOT/tests/fixtures"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

tests=0
failed=0
fail_msgs=""

# ok NAME : mark a passing check.
ok() {
    tests=$((tests + 1))
    printf "  [ok] %s\n" "$1"
}

# fail NAME MSG : record a failure.
fail() {
    tests=$((tests + 1))
    failed=$((failed + 1))
    fail_msgs="$fail_msgs
  [FAIL] $1: $2"
    printf "  [FAIL] %s: %s\n" "$1" "$2"
}

# assert_exit NAME EXPECTED ACTUAL
assert_exit() {
    if [ "$2" = "$3" ]; then
        ok "$1 (exit $3)"
    else
        fail "$1" "expected exit $2, got $3"
    fi
}

echo "=== test_cli (black-box) ==="

# ----------------------------------------------------------------------
# 1. No arguments -> exit 2 (args error)
# ----------------------------------------------------------------------
"$BIN" >/dev/null 2>&1
assert_exit "no args -> exit 2" 2 $?

# ----------------------------------------------------------------------
# 2. --help -> exit 0, prints usage banner
# ----------------------------------------------------------------------
out=$("$BIN" --help 2>&1)
rc=$?
assert_exit "--help -> exit 0" 0 $rc
case "$out" in
    *"llmkit proxy"*) ok "--help mentions proxy" ;;
    *) fail "--help mentions proxy" "banner missing" ;;
esac

# ----------------------------------------------------------------------
# 3. Unknown command -> exit 2
# ----------------------------------------------------------------------
"$BIN" frobnicate >/dev/null 2>&1
assert_exit "unknown command -> exit 2" 2 $?

# ----------------------------------------------------------------------
# 4. proxy without -c -> exit 2
# ----------------------------------------------------------------------
"$BIN" proxy >/dev/null 2>&1
assert_exit "proxy without -c -> exit 2" 2 $?

# ----------------------------------------------------------------------
# 5. proxy: bad listen address -> exit 2 (spec: invalid listen address)
#    config: a backend whose init will hang, so we reach the listener.
# ----------------------------------------------------------------------
cfg="$TMP/proxy_badaddr.yml"
cat >"$cfg" <<EOF
mcps:
  - name: srv
    cmdline: "python3 $FIX/fake_mcp.py"
EOF
"$BIN" proxy -c "$cfg" -l "127.0.0.1:notaport" >/dev/null 2>&1
assert_exit "proxy bad listen addr -> exit 2" 2 $?

# Bind failure: valid format but a port we cannot bind (privileged port 1
# without root, or port 0 is ambiguous). Use port 1 — bind fails -> exit 3
# (proxy spec: 3 = Server error / bind failure).
if [ "$(id -u)" -ne 0 ]; then
    "$BIN" proxy -c "$cfg" -l "127.0.0.1:1" >/dev/null 2>&1
    assert_exit "proxy bind failure -> exit 3" 3 $?
fi

# ----------------------------------------------------------------------
# 6. MCP init timeout -> exit 6
#    A backend that never responds to `initialize` must trigger the
#    init_timeout path. Use a 1s init_timeout against /bin/cat (a real
#    process that just echoes bytes, never speaks MCP).
# ----------------------------------------------------------------------
cfg="$TMP/init_timeout.yml"
cat >"$cfg" <<EOF
mcps:
  - name: dead
    cmdline: "/bin/cat"
    init_timeout: "1s"
EOF
out="$TMP/convo_init.jsonl"
"$BIN" agent -c "$cfg" -o "$out" -p "hello" >/dev/null 2>&1
assert_exit "MCP init timeout -> exit 6" 6 $?

# ----------------------------------------------------------------------
# 7. Empty prompt -> exit 2
# ----------------------------------------------------------------------
cfg2="$TMP/minimal.yml"
cat >"$cfg2" <<EOF
llm:
  api_base: "http://127.0.0.1:1/v1"
EOF
out2="$TMP/convo_empty.jsonl"
"$BIN" agent -c "$cfg2" -o "$out2" -p "   " >/dev/null 2>&1
assert_exit "empty prompt -> exit 2" 2 $?

# Whitespace-only prompt file should also be rejected.
pf="$TMP/blank.txt"
printf '   \n\n  ' >"$pf"
"$BIN" agent -c "$cfg2" -o "$out2" -p "$pf" >/dev/null 2>&1
assert_exit "blank prompt file -> exit 2" 2 $?

# Empty-string prompt argument.
"$BIN" agent -c "$cfg2" -o "$out2" -p "" >/dev/null 2>&1
assert_exit "empty literal prompt -> exit 2" 2 $?

# ----------------------------------------------------------------------
# 8. Config file does not exist -> exit 1 (config error)
# ----------------------------------------------------------------------
"$BIN" agent -c "$TMP/nope.yml" -o "$out2" -p "hi" >/dev/null 2>&1
assert_exit "missing config -> exit 1" 1 $?

# ----------------------------------------------------------------------
# 9. Proxy end-to-end (stdio): namespace + tools/list
# ----------------------------------------------------------------------
cfgp="$TMP/proxy_ok.yml"
cat >"$cfgp" <<EOF
mcps:
  - name: srv
    cmdline: "python3 $FIX/fake_mcp.py"
    namespace: ns1
EOF
req='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}
{"jsonrpc":"2.0","method":"notifications/initialized","params":{}}
{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"ns1.get_time","arguments":{"x":1}}}'
resp=$(printf '%s\n' "$req" | "$BIN" proxy -c "$cfgp" 2>/dev/null)
rc=$?
assert_exit "proxy stdio e2e -> exit 0" 0 $rc
case "$resp" in
    *"ns1.get_time"*) ok "proxy namespaces tool as ns1.get_time" ;;
    *) fail "proxy namespace" "ns1.get_time not in response" ;;
esac
case "$resp" in
    *"called:get_time"*) ok "proxy routes tools/call to backend" ;;
    *) fail "proxy tools/call routing" "backend response missing" ;;
esac

# ----------------------------------------------------------------------
# 10. Proxy whitelist hides a tool from tools/list (discovery only)
# ----------------------------------------------------------------------
cfgw="$TMP/proxy_whitelist.yml"
cat >"$cfgw" <<EOF
mcps:
  - name: srv
    cmdline: "python3 $FIX/fake_mcp.py"
    namespace: ns1
    whitelist:
      - "ns1.get_time"
EOF
resp_w=$(printf '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}
{"jsonrpc":"2.0","method":"notifications/initialized","params":{}}
{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}\n' \
    | "$BIN" proxy -c "$cfgw" 2>/dev/null)
case "$resp_w" in
    *"ns1.echo"*)
        fail "proxy whitelist hides ns1.echo" "ns1.echo appeared in list" ;;
    *)
        ok "proxy whitelist hides ns1.echo" ;;
esac
case "$resp_w" in
    *"ns1.get_time"*) ok "proxy whitelist keeps ns1.get_time" ;;
    *) fail "proxy whitelist keeps ns1.get_time" "missing from list" ;;
esac

echo ""
echo "$tests tests, $failed failed"
if [ -n "$fail_msgs" ]; then
    echo "$fail_msgs"
fi
exit $([ "$failed" = 0 ] && echo 0 || echo 1)
