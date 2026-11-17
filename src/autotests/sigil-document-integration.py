#!/usr/bin/env python3
"""Real Sigil lifecycle regression on a private bus, never personal EPUBs.

Usage: python3 src/autotests/sigil-document-integration.py \
    build-ai-notes/bin/sigilnavigationtest ../Sigil/build/bin/sigil OUTPUT_DIR
Also run Sigil's run_mainwindow_test.py --dbus-navigation with
ARIANNA_QML_DOCUMENT_PROBE pointing to bookreferencechecktest: that harness
inspects the actual active editor resource after Arianna's real QML button.
"""
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import zipfile

probe, sigil, output = (Path(arg).resolve() for arg in sys.argv[1:4])
output.mkdir(parents=True, exist_ok=True)
anchor = os.environ.get('ARIANNA_TEST_ANCHOR', '')
verse = os.environ.get('ARIANNA_TEST_VERSE', '')

def probe_arguments(book, document, expected='document-opened'):
    if anchor:
        return [str(probe), '--anchor-probe', str(sigil), str(book), document, anchor, verse,
                'anchor-prepared' if expected == 'document-opened' else expected]
    return [str(probe), '--document-probe', str(sigil), str(book), document, expected]

def wait_until(predicate, timeout=25):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(0.025)
    raise AssertionError('Timed out')

with tempfile.TemporaryDirectory(prefix='arianna-sigil-document-') as temp:
    directory = Path(temp)
    book = directory / 'target पुस्तक.epub'
    files = {
        'mimetype': 'application/epub+zip',
        'META-INF/container.xml': '<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>',
        'OPS/content.opf': '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">test-document-handoff</dc:identifier><dc:title>Document handoff</dc:title><dc:language>de</dc:language><meta property="dcterms:modified">2026-10-22T00:00:00Z</meta></metadata><manifest><item id="a" href="a/chapter.xhtml" media-type="application/xhtml+xml"/><item id="b" href="b/chapter.xhtml" media-type="application/xhtml+xml"/><item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/></manifest><spine><itemref idref="a"/><itemref idref="b"/></spine></package>',
        'OPS/a/chapter.xhtml': '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>First</title></head><body><p>let śrī कृष्ण</p></body></html>',
        'OPS/b/chapter.xhtml': '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Target</title></head><body><p>let śrī कृष्ण</p></body></html>',
        'OPS/nav.xhtml': '<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><head><title>TOC</title></head><body><nav epub:type="toc"><ol><li><a href="a/chapter.xhtml">First</a></li><li><a href="b/chapter.xhtml">Target</a></li></ol></nav></body></html>',
    }
    with zipfile.ZipFile(book, 'w') as archive:
        for name, text in files.items():
            if name.endswith('.xhtml'):
                text = '<!DOCTYPE html>' + text
            archive.writestr(name, text.encode())
    before = hashlib.sha256(book.read_bytes()).hexdigest()
    results = []
    for mode in ('closed', 'running', 'starting'):
        (directory / mode).mkdir()
        daemon = subprocess.Popen(['dbus-daemon', '--session', '--nofork', '--print-address=1'], stdout=subprocess.PIPE, text=True)
        address = daemon.stdout.readline().strip()
        env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS=address, QT_QPA_PLATFORM='offscreen',
                   QTWEBENGINE_DISABLE_SANDBOX='1', QTWEBENGINE_CHROMIUM_FLAGS='--disable-gpu',
                   SIGIL_SKIP_UPDATE_CHECK='1', SIGIL_PREFS_DIR=str(directory / mode),
                   XDG_CONFIG_HOME=str(directory / mode / 'config'), XDG_DATA_HOME=str(directory / mode / 'data'),
                   XDG_CACHE_HOME=str(directory / mode / 'cache'), ARIANNA_PROBE_HOLD_MS='1500')
        processes = []
        logs = []
        def launch(arguments, name):
            log = (output / f'{mode}-{name}.log').open('w')
            logs.append(log)
            process = subprocess.Popen(arguments, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            processes.append(process)
            return process

        def owner_pid():
            call = subprocess.run(['busctl', '--address='+address, 'call', 'org.freedesktop.DBus',
                                   '/org/freedesktop/DBus', 'org.freedesktop.DBus', 'GetConnectionUnixProcessID',
                                   's', 'org.sigil_ebook.Sigil'], capture_output=True, text=True)
            return int(call.stdout.split()[1]) if call.returncode == 0 else None

        def editor_pids():
            pids = []
            for entry in Path('/proc').iterdir():
                if not entry.name.isdecimal():
                    continue
                try:
                    if (entry/'exe').resolve() == sigil and ('DBUS_SESSION_BUS_ADDRESS='+address).encode() in (entry/'environ').read_bytes().split(b'\0'):
                        pids.append(int(entry.name))
                except (OSError, ProcessLookupError):
                    pass
            return pids
        try:
            initial = None
            if mode != 'closed':
                initial = launch([str(sigil), str(book)], 'editor')
                if mode == 'running':
                    assert wait_until(owner_pid) == initial.pid
                else:
                    # /proc discovery must see the real executable, not a shell wrapper.
                    wait_until(lambda: initial.pid in editor_pids())
            request = launch(probe_arguments(book, 'OPS/b/chapter.xhtml'), 'probe')
            owner = wait_until(owner_pid)
            max_processes = 0
            while request.poll() is None:
                max_processes = max(max_processes, len(editor_pids()))
                time.sleep(0.025)
            assert request.returncode == 0, (mode, 'navigation probe failed')
            assert max_processes == 1, (mode, max_processes)
            if initial:
                assert owner == initial.pid and initial.poll() is None, (mode, 'owner changed')
                # A new user request reuses the same process; invalid document fails without fallback.
                repeated = launch(probe_arguments(book, 'OPS/a/chapter.xhtml'), 'repeat')
                assert repeated.wait(timeout=25) == 0
                missing = launch(probe_arguments(book, 'OPS/missing.xhtml', 'open-failed'), 'missing-document')
                assert missing.wait(timeout=25) == 0
                assert editor_pids() == [initial.pid] and owner_pid() == owner
            assert hashlib.sha256(book.read_bytes()).hexdigest() == before
            results.append(dict(mode=mode, ownerPid=owner, maximumSigilProcesses=max_processes,
                                unchangedEpub=True, status='passed'))
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGTERM)
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait()
            # A cold editor is a child of the probe; remove any remaining private-bus process.
            for pid in editor_pids():
                try:
                    os.kill(pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
            daemon.terminate()
            daemon.wait(timeout=5)
            for log in logs:
                log.close()
    (output/'lifecycle-results.json').write_text(json.dumps(results, indent=2)+'\n')
    print(json.dumps(results, indent=2))
