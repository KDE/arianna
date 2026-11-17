#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Production-binary reader history/refresh test; run inside dbus-run-session."""
import argparse
import ast
import hashlib
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import uuid
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--in-private-bus", action="store_true", help=argparse.SUPPRESS)
    options = parser.parse_args()
    binary = options.binary.resolve()
    temporary = tempfile.TemporaryDirectory(prefix="arianna-reader-control-")
    root = options.output_dir.resolve() if options.output_dir else Path(temporary.name)
    root.mkdir(parents=True, exist_ok=True)
    if not options.in_private_bus:
        # Portal helpers may inherit output streams. A log file keeps them from
        # holding CTest's or the caller's stdout pipe open after the test exits.
        with (root / "session.log").open("w") as output:
            result = subprocess.run(["dbus-run-session", "--", sys.executable, str(Path(__file__).resolve()),
                "--in-private-bus", "--binary", str(binary), "--output-dir", str(root)],
                stdout=output, stderr=subprocess.STDOUT, timeout=150)
        if result.returncode:
            print((root / "session.log").read_text()[-10000:])
        else:
            print(f"PASS: reader history and server restart; logs: {root}")
        return result.returncode
    env = os.environ.copy()
    for key in ("DATA", "CONFIG", "CACHE", "RUNTIME"):
        directory = root / key.lower()
        directory.mkdir(mode=0o700, exist_ok=True)
        env[f"XDG_{key}_HOME" if key != "RUNTIME" else "XDG_RUNTIME_DIR"] = str(directory)
    env.update(QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software",
               QTWEBENGINE_DISABLE_SANDBOX="1", QTWEBENGINE_CHROMIUM_FLAGS="--disable-gpu",
               SPEECHD_ADDRESS="unix_socket:/nonexistent/reader-control-integration")
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    (root / "config/arianna").write_text(
        f"[BookDelivery]\nbookServerAddress=127.0.0.1\nbookServerPort={port}\n"
        "[AiAssistance]\nsanskritStructureOnImport=false\n")
    database = root / "data/arianna/library.sqlite"
    discovery = root / "runtime/arianna-bookserver.json"
    processes = []
    handles = []
    events = []

    def wait(predicate, description, seconds=20):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            try:
                value = predicate()
                if value:
                    return value
            except (OSError, sqlite3.Error, ValueError):
                pass
            time.sleep(.05)
        raise AssertionError(f"Timeout: {description}; logs: {root}")

    def rows():
        with sqlite3.connect(database) as connection:
            connection.row_factory = sqlite3.Row
            return {row["fileName"]: dict(row) for row in connection.execute("SELECT * FROM books")}

    def fixture(name):
        path = root / f"{name}.epub"
        identifier = f"uuid:{uuid.uuid4()}"
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr("mimetype", "application/epub+zip")
            archive.writestr("META-INF/container.xml", "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container'>"
                "<rootfiles><rootfile full-path='package.opf' media-type='application/oebps-package+xml'/></rootfiles></container>")
            archive.writestr("package.opf", f"<package xmlns='http://www.idpf.org/2007/opf' version='3.0' unique-identifier='id'>"
                f"<metadata xmlns:dc='http://purl.org/dc/elements/1.1/'><dc:identifier id='id'>{identifier}</dc:identifier>"
                f"<dc:title>{name}</dc:title><dc:language>en</dc:language></metadata><manifest>"
                "<item id='chapter' href='chapter.xhtml' media-type='application/xhtml+xml'/></manifest>"
                "<spine><itemref idref='chapter'/></spine></package>")
            archive.writestr("chapter.xhtml", "<html xmlns='http://www.w3.org/1999/xhtml'><head><title>History</title></head>"
                "<body><p>One two three four five. नित्यो śrī.</p><p>Another paragraph.</p></body></html>")
        return path

    def start(name, *arguments):
        handle = (root / f"{name}.log").open("w")
        handles.append(handle)
        process = subprocess.Popen([str(binary), *map(str, arguments)], env=env, stdout=handle, stderr=subprocess.STDOUT)
        processes.append(process)
        return process

    def command(*arguments, expected=0):
        result = subprocess.run([str(binary), *map(str, arguments)], env=env, capture_output=True, text=True, timeout=30)
        events.append({"arguments": arguments, "exitCode": result.returncode, "output": result.stdout + result.stderr})
        assert result.returncode == expected, events[-1]
        return result

    def service(process):
        result = subprocess.run(["gdbus", "call", "--session", "--dest", "org.freedesktop.DBus",
            "--object-path", "/org/freedesktop/DBus", "--method", "org.freedesktop.DBus.ListNames"],
            env=env, capture_output=True, text=True, check=True)
        return next((name for name in ast.literal_eval(result.stdout)[0]
                     if name.startswith("org.kde.arianna-") and name.endswith(f"-{process.pid}")), None)

    def close(process):
        name = service(process)
        assert name, f"Reader not registered on D-Bus: {process.pid}"
        subprocess.run(["gdbus", "call", "--session", "--dest", name, "--object-path", "/MainApplication",
            "--method", "org.qtproject.Qt.QCoreApplication.quit"], env=env, capture_output=True, check=True, timeout=10)
        assert process.wait(timeout=15) == 0

    try:
        command("--reopen-already-opened-books")
        assert not discovery.exists()
        a, b, transient = (fixture(name) for name in ("A", "B", "Transient"))
        transient_hash = hashlib.sha256(transient.read_bytes()).hexdigest()
        p1 = start("A", a)
        wait(lambda: rows().get(str(a), {}).get("currentLocation"), "first reader ready")
        p2 = start("B", b)
        wait(lambda: rows().get(str(b), {}).get("currentLocation"), "second reader ready")
        p3 = start("transient", "--no-library-import", transient)
        wait(lambda: service(p3), "transient reader ready")
        wait(lambda: "opening book" in (root / "transient.log").read_text(), "transient EPUB request")
        before = rows()
        assert set(before) == {str(a), str(b)}
        assert all(not row["lastClosedTime"] for row in before.values())
        original_server = json.loads(discovery.read_text())["pid"]
        command("--reopen-already-opened-books", "--restart-new-bookserver")
        new_server = json.loads(discovery.read_text())["pid"]
        assert original_server != new_server
        assert all(process.poll() is None for process in (p1, p2, p3))
        assert all("Refreshed reader sessions: 1" in wait(lambda p=path: p.read_text(), "reader log")
                   for path in (root / "A.log", root / "B.log", root / "transient.log"))
        for path in (root / "A.log", root / "B.log", root / "transient.log"):
            wait(lambda p=path: p.read_text().count("Book or TOC not available") >= 2, "reader reloaded after server restart")
            assert "Book error" not in path.read_text()
        # Repeat a real D-Bus request UUID: reload is performed exactly once.
        request_id = str(uuid.uuid4())
        previous_refreshes = (root / "A.log").read_text().count("Refreshed reader sessions:")
        for _ in range(2):
            result = subprocess.run(["gdbus", "call", "--session", "--dest", service(p1), "--object-path", "/Reader",
                "--method", "org.kde.arianna.Reader1.RefreshReaders", request_id], env=env,
                capture_output=True, text=True, check=True, timeout=10)
            assert ast.literal_eval(result.stdout)[0] == 1
        assert (root / "A.log").read_text().count("Refreshed reader sessions:") == previous_refreshes + 1
        command("--reopen-already-opened-books")
        after = rows()
        assert set(after) == set(before)
        assert all(not row["lastClosedTime"] for row in after.values())
        assert all(after[key]["currentLocation"] == before[key]["currentLocation"] for key in before)
        close(p2)
        close(p1)
        close(p3)
        assert hashlib.sha256(transient.read_bytes()).hexdigest() == transient_hash
        closed = rows()
        assert closed[str(a)]["lastClosedTime"] > closed[str(b)]["lastClosedTime"]
        assert closed[str(a)]["lastClosedOpenedTime"] < closed[str(b)]["lastClosedOpenedTime"]
        wait(lambda: not discovery.exists(), "server exit after last reader")
        restored = start("restored", "--open-last-closed-book=2")
        wait(lambda: (root / "restored.log").read_text().count("Reopening last closed book:") == 2, "two restored readers")
        log = (root / "restored.log").read_text()
        assert log.index(str(a)) < log.index(str(b)), "Restore opening order"
        wait(lambda: rows()[str(b)]["lastOpenedTime"] != closed[str(b)]["lastOpenedTime"], "restored document ready")
        command("--reopen-already-opened-books")
        assert "Refreshed reader sessions: 2" in (root / "restored.log").read_text()
        close(restored)
        assert rows()[str(a)]["lastClosedOpenedTime"] < rows()[str(b)]["lastClosedOpenedTime"]
        for value in ("0", "-1", "no", "2147483648"):
            command(f"--open-last-closed-book={value}", expected=1)
        last = start("last-opened", "--open-last-opened-book")
        wait(lambda: service(last), "existing last-opened option")
        close(last)
        events.append({"result": "PASS", "originalServerPid": original_server, "newServerPid": new_server,
                       "readerPidsPreserved": [p1.pid, p2.pid, p3.pid], "restoredOrder": [str(a), str(b)]})
        print(f"PASS: reader history, refresh, real server restart, transient registration; logs: {root}")
    finally:
        for process in processes:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        # Only the server recorded in this isolated runtime directory belongs to this test.
        if discovery.exists():
            try:
                os.kill(json.loads(discovery.read_text())["pid"], 15)
            except (OSError, ValueError):
                pass
        for handle in handles:
            handle.close()
        (root / "results.json").write_text(json.dumps(events, indent=2))


if __name__ == "__main__":
    sys.exit(main())
