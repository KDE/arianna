#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Keep activated desktop helpers from holding CTest's output pipe open."""
import subprocess
import sys
import tempfile

with tempfile.TemporaryFile(mode="w+") as output:
    try:
        result = subprocess.run(["dbus-run-session", "--", *sys.argv[1:]],
                                stdout=output, stderr=subprocess.STDOUT, timeout=100)
        status = result.returncode
    except subprocess.TimeoutExpired:
        status = 124
    output.seek(0)
    print(output.read(), end="")
sys.exit(status)
