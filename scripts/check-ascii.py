#!/usr/bin/env python3
"""check-ascii -- Verify all given files contain only ASCII (0x00-0x7F).

Usage: check-ascii.py <file> [file ...]
Exit code: 0 if all files are pure ASCII, 1 if any non-ASCII found.
Output: <file>:<line>:<col>: error: non-ASCII character <HEX> (<char>)
"""

import sys
import os

VENDOR_MARKER = os.sep + "vendor" + os.sep


def check_file(path: str) -> bool:
    """Return True if the file is pure ASCII."""
    if VENDOR_MARKER in path:
        return True

    ok = True
    with open(path, "rb") as f:
        data = f.read()

    lineno = 1
    col = 0
    for byte in data:
        if byte == ord("\n"):
            lineno += 1
            col = 0
            continue
        if byte > 127:
            print(
                f"{path}:{lineno}:{col}: error: non-ASCII character 0x{byte:02X} ({chr(byte)})",
                file=sys.stderr,
            )
            ok = False
        col += 1

    return ok


def main() -> int:
    if len(sys.argv) < 2:
        print("Usage: check-ascii.py <file> [file ...]", file=sys.stderr)
        return 1

    ok = True
    for path in sys.argv[1:]:
        if not check_file(path):
            ok = False

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
