#!/usr/bin/env python3

import argparse
import hashlib
import os
import pathlib
import select
import shutil
import stat
import subprocess
import sys
import tempfile
import termios
import time
import tty


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent.parent
DEFAULT_SOURCE = PROJECT_ROOT / "output" / "out" / "oem"
DEFAULT_AGENT_CMD = "/oem/usr/bin/buildspot-agent.sh --stdio --root /oem"
READY_BANNER = b"BUILDSPOT/1 READY"
ESCAPE_BYTE = b"\x1d"


def eprint(message: str) -> None:
    print(message, file=sys.stderr)


def detect_default_device() -> str:
    for pattern in ("/dev/ttyUSB*", "/dev/ttyACM*"):
        matches = sorted(pathlib.Path("/dev").glob(pathlib.Path(pattern).name))
        if matches:
            return str(matches[0])
    return "/dev/ttyUSB0"


def baud_to_termios(baud: int) -> int:
    mapping = {
        9600: termios.B9600,
        19200: termios.B19200,
        38400: termios.B38400,
        57600: termios.B57600,
        115200: termios.B115200,
        230400: getattr(termios, "B230400", termios.B115200),
    }
    if baud not in mapping:
        raise ValueError(f"Unsupported baud rate: {baud}")
    return mapping[baud]


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            chunk = stream.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def collect_local_index(source_dir: pathlib.Path) -> dict[str, dict[str, object]]:
    entries: dict[str, dict[str, object]] = {}
    for file_path in sorted(path for path in source_dir.rglob("*") if path.is_file()):
        stat_info = file_path.stat()
        relpath = file_path.relative_to(source_dir).as_posix()
        entries[relpath] = {
            "sha": sha256_file(file_path),
            "size": stat_info.st_size,
            "mode": stat.S_IMODE(stat_info.st_mode),
            "path": file_path,
        }
    return entries


def write_index_file(entries: dict[str, dict[str, object]], index_path: pathlib.Path) -> None:
    with index_path.open("w", encoding="utf-8") as stream:
        for relpath in sorted(entries):
            entry = entries[relpath]
            stream.write(f"{relpath};{entry['sha']};{entry['size']};{entry['mode']:04o}\n")


def read_index_file(index_path: pathlib.Path) -> dict[str, dict[str, object]]:
    entries: dict[str, dict[str, object]] = {}
    if not index_path.exists():
        return entries
    with index_path.open("r", encoding="utf-8") as stream:
        for line in stream:
            line = line.rstrip("\n")
            if not line:
                continue
            relpath, file_sha, size, mode = line.split(";", 3)
            entries[relpath] = {
                "sha": file_sha,
                "size": int(size),
                "mode": int(mode, 8),
            }
    return entries


def compute_delta(local_index: dict[str, dict[str, object]], remote_index: dict[str, dict[str, object]]) -> list[str]:
    delta: list[str] = []
    for relpath in sorted(local_index):
        local_entry = local_index[relpath]
        remote_entry = remote_index.get(relpath)
        if remote_entry is None or remote_entry.get("sha") != local_entry["sha"]:
            delta.append(relpath)
    return delta


class SerialLink:
    def __init__(self, device: str, baud: int) -> None:
        self.device = device
        self.baud = baud
        self.fd = os.open(device, os.O_RDWR | os.O_NOCTTY)
        self._buffer = bytearray()
        self._configure()

    def _configure(self) -> None:
        baud_flag = baud_to_termios(self.baud)
        attrs = termios.tcgetattr(self.fd)
        attrs[0] &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK | termios.ISTRIP |
                      termios.INLCR | termios.IGNCR | termios.ICRNL | termios.IXON |
                      termios.IXOFF | termios.IXANY)
        attrs[1] = 0
        attrs[2] &= ~(termios.CSIZE | termios.PARENB)
        attrs[2] |= termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[3] = 0
        attrs[4] = baud_flag
        attrs[5] = baud_flag
        attrs[6][termios.VMIN] = 1
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)

    def close(self) -> None:
        os.close(self.fd)

    def write(self, data: bytes) -> None:
        os.write(self.fd, data)

    def write_line(self, line: str) -> None:
        self.write(line.encode("utf-8") + b"\n")

    def drain_input(self, idle_seconds: float = 0.2) -> None:
        deadline = time.monotonic() + idle_seconds
        while time.monotonic() < deadline:
            ready, _, _ = select.select([self.fd], [], [], 0.05)
            if not ready:
                continue
            chunk = os.read(self.fd, 4096)
            if not chunk:
                break

    def wait_for_substring(self, marker: bytes, timeout: float) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if marker in self._buffer:
                index = self._buffer.index(marker) + len(marker)
                del self._buffer[:index]
                return
            ready, _, _ = select.select([self.fd], [], [], min(0.2, deadline - time.monotonic()))
            if not ready:
                continue
            chunk = os.read(self.fd, 4096)
            if not chunk:
                continue
            self._buffer.extend(chunk)
        raise TimeoutError(f"Timed out waiting for marker {marker!r}")

    def readline(self, timeout: float) -> str:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            newline_index = self._buffer.find(b"\n")
            if newline_index != -1:
                raw = bytes(self._buffer[:newline_index]).rstrip(b"\r")
                del self._buffer[:newline_index + 1]
                return raw.decode("utf-8", errors="replace")
            ready, _, _ = select.select([self.fd], [], [], min(0.2, deadline - time.monotonic()))
            if not ready:
                continue
            chunk = os.read(self.fd, 4096)
            if chunk:
                self._buffer.extend(chunk)
        raise TimeoutError("Timed out waiting for line")

    def expect_line(self, prefix: str, timeout: float) -> str:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self.readline(deadline - time.monotonic())
            if not line:
                continue
            if line.startswith(prefix):
                return line
            if line.startswith("ERR "):
                raise RuntimeError(line)
        raise TimeoutError(f"Timed out waiting for line starting with {prefix!r}")

    def run_lrzsz(self, argv: list[str], cwd: pathlib.Path, timeout: float) -> subprocess.CompletedProcess[bytes]:
        stdin_stream = os.fdopen(os.dup(self.fd), "rb", buffering=0)
        stdout_stream = os.fdopen(os.dup(self.fd), "wb", buffering=0)
        try:
            return subprocess.run(
                argv,
                cwd=cwd,
                stdin=stdin_stream,
                stdout=stdout_stream,
                stderr=subprocess.PIPE,
                timeout=timeout,
                check=False,
            )
        finally:
            stdin_stream.close()
            stdout_stream.close()


class LocalTTY:
    def __init__(self) -> None:
        self.fd = sys.stdin.fileno()
        self._attrs = None

    def __enter__(self):
        self._attrs = termios.tcgetattr(self.fd)
        tty.setraw(self.fd)
        return self

    def __exit__(self, exc_type, exc, tb):
        if self._attrs is not None:
            termios.tcsetattr(self.fd, termios.TCSADRAIN, self._attrs)


def launch_agent(link: SerialLink, agent_command: str, timeout: float) -> None:
    link.drain_input(0.2)
    link.write(b"\n")
    time.sleep(0.1)
    link.write_line(agent_command)
    link.wait_for_substring(READY_BANNER, timeout)


def receive_index(link: SerialLink, temp_dir: pathlib.Path, timeout: float) -> pathlib.Path:
    link.write_line("GET_INDEX")
    link.expect_line("SEND INDEX", timeout)
    result = link.run_lrzsz(["rz", "-b", "-E"], temp_dir, timeout)
    if result.returncode != 0:
        raise RuntimeError(f"rz failed while receiving index: {result.stderr.decode('utf-8', errors='replace').strip()}")
    link.expect_line("OK INDEX", timeout)
    files = sorted(path for path in temp_dir.iterdir() if path.is_file())
    if not files:
        empty_index = temp_dir / ".buildspot.index"
        empty_index.write_text("", encoding="utf-8")
        return empty_index
    return files[0]


def send_file(link: SerialLink, relpath: str, entry: dict[str, object], timeout: float) -> None:
    mode = f"{int(entry['mode']):04o}"
    sha_value = str(entry["sha"])
    size_value = str(entry["size"])
    path_value = pathlib.Path(str(entry["path"]))

    link.write_line(f"PUT {mode} {sha_value} {size_value} {relpath}")
    link.expect_line("READY PUT ", timeout)

    file_timeout = max(timeout, min(600.0, float(entry["size"]) / 8192.0 + 15.0))
    result = link.run_lrzsz(["sz", "-b", str(path_value)], path_value.parent, file_timeout)
    if result.returncode != 0:
        raise RuntimeError(
            f"sz failed for {relpath}: {result.stderr.decode('utf-8', errors='replace').strip()}"
        )
    link.expect_line(f"OK FILE {relpath}", timeout)


def send_index(link: SerialLink, index_path: pathlib.Path, timeout: float) -> None:
    sha_value = sha256_file(index_path)
    size_value = index_path.stat().st_size
    link.write_line(f"PUT_INDEX {sha_value} {size_value}")
    link.expect_line("READY INDEX", timeout)
    result = link.run_lrzsz(["sz", "-b", str(index_path)], index_path.parent, timeout)
    if result.returncode != 0:
        raise RuntimeError(f"sz failed for index: {result.stderr.decode('utf-8', errors='replace').strip()}")
    link.expect_line("OK INDEX_UPDATE", timeout)


def finish_session(link: SerialLink, timeout: float) -> None:
    link.write_line("DONE")
    link.expect_line("BYE", timeout)


def sync_once(link: SerialLink, args: argparse.Namespace) -> None:
    source_dir = pathlib.Path(args.source).resolve()
    if not source_dir.is_dir():
        raise FileNotFoundError(f"OEM source not found: {source_dir}")
    if shutil.which("sz") is None or shutil.which("rz") is None:
        raise RuntimeError("lrzsz is required on the host (sz/rz not found)")

    eprint(f"[buildspot-console] Source: {source_dir}")
    local_index = collect_local_index(source_dir)
    if not local_index:
        raise RuntimeError(f"No files found in {source_dir}")

    work_dir = pathlib.Path(tempfile.mkdtemp(prefix="buildspot-console-"))
    try:
        local_index_path = work_dir / ".buildspot.index"
        write_index_file(local_index, local_index_path)

        launch_agent(link, args.agent_command, args.handshake_timeout)
        link.write_line("HELLO 1")
        link.expect_line("OK HELLO 1", args.timeout)

        remote_dir = work_dir / "remote-index"
        remote_dir.mkdir(parents=True, exist_ok=True)
        remote_index_path = receive_index(link, remote_dir, args.timeout)
        remote_index = read_index_file(remote_index_path)

        delta = compute_delta(local_index, remote_index)
        eprint(f"[buildspot-console] Files to sync: {len(delta)}")
        for index, relpath in enumerate(delta, start=1):
            entry = local_index[relpath]
            eprint(f"[buildspot-console] [{index}/{len(delta)}] {relpath} ({entry['size']} bytes)")
            send_file(link, relpath, entry, args.timeout)

        send_index(link, local_index_path, args.timeout)
        finish_session(link, args.timeout)
        eprint("[buildspot-console] Sync complete")
    finally:
        shutil.rmtree(work_dir, ignore_errors=True)


def run_console(args: argparse.Namespace) -> None:
    link = SerialLink(args.device, args.baud)
    eprint("[buildspot-console] Console mode. Ctrl-] then s=sync, q=quit, c=continue")
    try:
        with LocalTTY():
            while True:
                ready, _, _ = select.select([sys.stdin.fileno(), link.fd], [], [])
                if link.fd in ready:
                    data = os.read(link.fd, 4096)
                    if data:
                        os.write(sys.stdout.fileno(), data)
                if sys.stdin.fileno() in ready:
                    data = os.read(sys.stdin.fileno(), 1)
                    if not data:
                        break
                    if data == ESCAPE_BYTE:
                        os.write(sys.stdout.fileno(), b"\r\n[buildspot-console] command: s=sync q=quit c=continue\r\n")
                        cmd = os.read(sys.stdin.fileno(), 1)
                        if cmd == b"q":
                            break
                        if cmd == b"s":
                            os.write(sys.stdout.fileno(), b"\r\n[buildspot-console] entering sync mode\r\n")
                            sync_once(link, args)
                            os.write(sys.stdout.fileno(), b"\r\n[buildspot-console] back to console\r\n")
                        continue
                    link.write(data)
    finally:
        link.close()


def run_sync(args: argparse.Namespace) -> None:
    link = SerialLink(args.device, args.baud)
    try:
        sync_once(link, args)
    finally:
        link.close()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Buildspot custom UART console/sync MVP")
    parser.add_argument("mode", choices=["sync", "console"], help="Run one-shot sync or interactive console")
    parser.add_argument("--device", default=detect_default_device(), help="Serial device on the host")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate")
    parser.add_argument("--source", default=str(DEFAULT_SOURCE), help="Host OEM source directory")
    parser.add_argument("--agent-command", default=DEFAULT_AGENT_CMD, help="Command sent to the device shell to start sync mode")
    parser.add_argument("--timeout", type=float, default=30.0, help="Control-message timeout in seconds")
    parser.add_argument("--handshake-timeout", type=float, default=5.0, help="Time to wait for the device agent banner")
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    try:
        if args.mode == "console":
            run_console(args)
        else:
            run_sync(args)
        return 0
    except Exception as exc:
        eprint(f"[buildspot-console] ERROR: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())