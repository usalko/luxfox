#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

make -C "$ROOT_DIR" host >/dev/null

ROOT_DIR="$ROOT_DIR" python3 - <<'PY'
import os
import pty
import subprocess
import tempfile
import time

root = os.environ['ROOT_DIR']
build = os.path.join(root, 'build', 'host-tools')
master, slave = pty.openpty()
slave_path = os.ttyname(slave)
tmp_dir = tempfile.mkdtemp(prefix='ulama-uart-pty-')
ready = os.path.join(tmp_dir, 'ready')
expected = os.path.join(tmp_dir, 'expected.bin')

receiver = subprocess.Popen([
    os.path.join(build, 'ulamad'),
    '--transport', 'udp',
    '--listen', '127.0.0.1:19562',
    '--uart', slave_path,
    '--baud', '420000',
    '--count', '1',
    '--ready-file', ready,
], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

try:
    for _ in range(100):
        if os.path.exists(ready):
            break
        time.sleep(0.05)
    if not os.path.exists(ready):
        raise SystemExit('receiver did not become ready')

    subprocess.check_call([
        os.path.join(build, 'ulama_js_tx'),
        '--transport', 'udp',
        '--peer', '127.0.0.1:19562',
        '--src-node', '10',
        '--dst-node', '1',
        '--channels', '992,1100,300,1400,1811,172,172,172',
        '--count', '1',
        '--crsf-out', expected,
    ], stdout=subprocess.DEVNULL)

    receiver.wait(timeout=5)
    expected_bytes = open(expected, 'rb').read()
    actual = os.read(master, len(expected_bytes))
    if actual != expected_bytes:
        raise SystemExit('pty bytes mismatch')
finally:
    if receiver.poll() is None:
        receiver.kill()
    os.close(master)
    os.close(slave)

print('e2e_udp_crsf_uart_pty: ok')
PY