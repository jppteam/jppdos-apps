#!/usr/bin/env python3
"""
deploy.py — deploy built JPPDOS apps to a J++Device over JPPD-SMP.

Scans the build output directory for apps, lets you pick which ones to send in
a multiselect terminal UI, auto-discovers the device's serial port, and uploads
everything inside a single consent-gated session (one Allow press for the whole
batch).

    ./deploy.py                          # pick apps + port interactively
    ./deploy.py testapp_native           # deploy one app, auto-discover the port
    ./deploy.py --all --port /dev/ttyACM0
    ./deploy.py --dist dist --list       # just show what's deployable

The device shows a consent dialog when the session opens — press Allow on the
device to proceed. Uploaded files land in /sd/apps/<app_id>/ on the SD card.

Requires: pip install pyserial
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import time
import zlib

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("Error: pyserial is not installed.  Run: pip install pyserial",
          file=sys.stderr)
    sys.exit(1)

# ---- Protocol constants (JPPD-SMP v1) ---------------------------------------

BAUD_RATE     = 115200
SOF           = b'\x01JPP'
PROTO_VERSION = 1
CHUNK_SIZE    = 1024

CMD_SESSION_START   = 0x00
CMD_SESSION_END     = 0x01
CMD_GET_INFO        = 0x02
CMD_FS_MKDIR        = 0x11
CMD_FS_UPLOAD_BEGIN = 0x14
CMD_FS_UPLOAD_CHUNK = 0x15
CMD_FS_UPLOAD_END   = 0x16

ST_OK         = 0x00
ST_ERR_EXISTS = 0x04

STATUS_NAMES = {
    0x00: 'OK',
    0x01: 'ERR_DENIED',
    0x02: 'ERR_NOT_FOUND',
    0x03: 'ERR_IO',
    0x04: 'ERR_EXISTS',
    0x05: 'ERR_INVALID',
    0x06: 'ERR_BUSY',
    0x07: 'ERR_NO_SESSION',
    0x08: 'ERR_TRANSFER',
    0x09: 'ERR_OVERFLOW',
    0x0A: 'ERR_APP_RUNNING',
}

# Espressif USB-Serial-JTAG (the ESP32-C6's only USB port — there is no
# separate UART bridge chip on this board).
ESPRESSIF_VID = 0x303A
USB_JTAG_PID  = 0x1001

# Build intermediates an app build may leave next to the deploy artefacts.
# These must never reach the SD card: the firmware never reads them and they
# bloat the transfer.
_SKIP_SUFFIXES = ('.o', '.d', '.map', '.lst')

# ---- CRC helpers ------------------------------------------------------------

def _crc16_ccitt_false(data: bytes) -> int:
    """CRC-16/CCITT-FALSE: poly=0x1021, init=0xFFFF, no reflection, no final XOR."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
            crc &= 0xFFFF
    return crc


def _crc32(data: bytes) -> int:
    """CRC-32/ISO-HDLC — identical to Python zlib.crc32."""
    return zlib.crc32(data) & 0xFFFFFFFF

# ---- Frame codec ------------------------------------------------------------

def _build_frame(seq: int, cmd: int, body: bytes = b'') -> bytes:
    """Assemble a JPPD-SMP command frame."""
    payload   = bytes([seq, cmd, 0x00]) + body   # SEQ | CMD | FLAGS=0 | BODY
    len_bytes = struct.pack('<H', len(payload))
    crc       = _crc16_ccitt_false(len_bytes + payload)
    return SOF + len_bytes + payload + struct.pack('<H', crc)


def _read_frame(ser: serial.Serial, timeout: float) -> tuple:
    """Scan for the next valid response frame; return (seq, status, body).

    ESP_LOG text shares this physical channel, so the scan simply skips any
    byte that is not part of the SOF magic. Raises RuntimeError on timeout or
    CRC mismatch.
    """
    deadline  = time.monotonic() + timeout
    sof_idx   = 0
    debug_log = bool(os.environ.get("JPPD_SMP_DEBUG"))
    noise = bytearray()

    def _flush_noise():
        if debug_log and noise:
            sys.stderr.write("\x1b[2m" + noise.decode("utf-8", "replace") + "\x1b[0m")
            sys.stderr.flush()
            noise.clear()

    # Fixed poll timeout, set once: reassigning ser.timeout per byte triggers a
    # tcsetattr() on the tty each time, expensive enough on some USB-CDC
    # drivers to eat seconds of the deadline just scanning past log chatter.
    ser.timeout = 0.5
    while True:
        if time.monotonic() >= deadline:
            _flush_noise()
            raise RuntimeError("Timeout waiting for response SOF")
        b = ser.read(1)
        if not b:
            continue
        byte = b[0]
        if byte == SOF[sof_idx]:
            sof_idx += 1
            if sof_idx == 4:
                _flush_noise()
                break
        elif byte == SOF[0]:
            if debug_log and sof_idx == 0:
                noise.append(byte)
            sof_idx = 1
        else:
            if debug_log:
                noise.append(byte)
                if byte == 0x0A:      # newline — flush a complete log line
                    _flush_noise()
            sof_idx = 0

    ser.timeout = 2.0
    len_bytes = ser.read(2)
    if len(len_bytes) < 2:
        raise RuntimeError("Timeout reading LEN field")
    plen = struct.unpack_from('<H', len_bytes)[0]
    if plen < 2:
        raise RuntimeError(f"Invalid payload length {plen}")

    # Bounded by the caller's deadline, with a small floor so a read() right at
    # the deadline edge is not issued with a zero/negative timeout.
    ser.timeout = max(deadline - time.monotonic(), 0.5)
    payload = ser.read(plen)
    if len(payload) < plen:
        raise RuntimeError(f"Short payload: got {len(payload)}, expected {plen}")

    ser.timeout = 2.0
    crc_bytes = ser.read(2)
    if len(crc_bytes) < 2:
        raise RuntimeError("Timeout reading CRC field")
    recv_crc = struct.unpack_from('<H', crc_bytes)[0]

    calc_crc = _crc16_ccitt_false(len_bytes + payload)
    if calc_crc != recv_crc:
        raise RuntimeError(
            f"CRC mismatch: calculated 0x{calc_crc:04X}, received 0x{recv_crc:04X}")

    return payload[0], payload[1], payload[2:]   # seq, status, body

# ---- Session ----------------------------------------------------------------

class SMPSession:
    def __init__(self, port: str):
        self._ser  = serial.Serial(port, BAUD_RATE, timeout=2.0)
        self._seq  = 0
        self._open = False

    def close(self) -> None:
        self._ser.close()

    def _next_seq(self) -> int:
        s = self._seq
        self._seq = (self._seq + 1) & 0xFF
        return s

    def _cmd(self, cmd: int, body: bytes = b'', timeout: float = 10.0,
             retries: int = 0) -> tuple:
        """Send *cmd* and wait for the response whose SEQ matches.

        On timeout the command is re-sent up to *retries* extra times, which
        makes stop-and-wait uploads resilient to a dropped chunk frame or a
        lost ACK: the device re-ACKs a re-sent chunk without writing it twice.
        Only pass retries>0 for idempotent commands (chunk upload) — never for
        SESSION_START or UPLOAD_END, whose device-side state makes a re-send
        ambiguous. A response with a stale SEQ is skipped, not raised, so a
        late ACK arriving during the retry window does not desync the stream.
        """
        last_exc = None
        for _attempt in range(retries + 1):
            seq = self._next_seq()
            self._ser.write(_build_frame(seq, cmd, body))
            deadline = time.monotonic() + timeout
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    last_exc = RuntimeError(
                        f"Timeout waiting for response to cmd 0x{cmd:02X}")
                    break
                try:
                    resp_seq, status, resp_body = _read_frame(self._ser, remaining)
                except RuntimeError as exc:
                    last_exc = exc     # timeout / CRC error — re-send if attempts remain
                    break
                if resp_seq == seq:
                    return status, resp_body
        raise last_exc

    def _require_ok(self, status: int, context: str, also_ok: tuple = ()) -> None:
        if status != ST_OK and status not in also_ok:
            name = STATUS_NAMES.get(status, f"0x{status:02X}")
            raise RuntimeError(f"{context}: {name}")

    def session_start(self) -> None:
        status, _ = self._cmd(CMD_SESSION_START, bytes([PROTO_VERSION]), timeout=60.0)
        self._require_ok(status, "SESSION_START")
        self._open = True

    def session_end(self) -> None:
        """Close the session. A no-op once the session is already closed, so
        the caller's cleanup path can call it unconditionally."""
        if not self._open:
            return
        self._open = False
        status, _ = self._cmd(CMD_SESSION_END, timeout=5.0)
        self._require_ok(status, "SESSION_END")

    def get_info(self) -> dict:
        """GET_INFO: firmware version, username, hwid, SD usage, SD label."""
        status, body = self._cmd(CMD_GET_INFO)
        self._require_ok(status, "GET_INFO")

        def _cstr(buf: bytes, offset: int) -> tuple:
            end = buf.index(b'\x00', offset)
            return buf[offset:end].decode('utf-8', errors='replace'), end + 1

        fw_version, off = _cstr(body, 0)
        username,   off = _cstr(body, off)
        hwid,       off = _cstr(body, off)
        sd_total, sd_used, sd_free = struct.unpack_from('<QQQ', body, off)
        off += 24
        sd_label, off = _cstr(body, off)
        return {
            'fw_version': fw_version, 'username': username, 'hwid': hwid,
            'sd_total': sd_total, 'sd_used': sd_used, 'sd_free': sd_free,
            'sd_label': sd_label,
        }

    def mkdir(self, path: str) -> None:
        status, _ = self._cmd(CMD_FS_MKDIR, path.encode() + b'\x00')
        # ERR_EXISTS is fine — the directory is already there.
        self._require_ok(status, f"FS_MKDIR {path!r}", also_ok=(ST_ERR_EXISTS,))

    def upload_file(self, local_path: str, remote_path: str,
                    progress: bool = True) -> None:
        with open(local_path, 'rb') as f:
            data = f.read()
        n_chunks = (len(data) + CHUNK_SIZE - 1) // CHUNK_SIZE if data else 0

        begin_body = struct.pack('<I', len(data)) + remote_path.encode() + b'\x00'
        status, resp = self._cmd(CMD_FS_UPLOAD_BEGIN, begin_body)
        self._require_ok(status, f"UPLOAD_BEGIN {remote_path!r}")
        xfer_id = resp[0]

        for idx in range(n_chunks):
            chunk = data[idx * CHUNK_SIZE:(idx + 1) * CHUNK_SIZE]
            body  = bytes([xfer_id]) + struct.pack('<H', idx) + chunk
            status, _ = self._cmd(CMD_FS_UPLOAD_CHUNK, body, timeout=10.0, retries=4)
            self._require_ok(status, f"UPLOAD_CHUNK {idx}")
            if progress:
                pct = (idx + 1) * 100 // n_chunks
                print(f"      chunk {idx + 1}/{n_chunks}  {pct}%", end='\r', flush=True)
        if progress and n_chunks:
            print(" " * 40, end='\r')

        end_body = bytes([xfer_id]) + struct.pack('<I', _crc32(data))
        status, _ = self._cmd(CMD_FS_UPLOAD_END, end_body)
        self._require_ok(status, f"UPLOAD_END {remote_path!r}")

# ---- App discovery ----------------------------------------------------------

class App:
    """One deployable app directory under the dist root."""

    def __init__(self, app_id: str, path: str, manifest: dict):
        self.app_id   = app_id
        self.path     = path
        self.manifest = manifest
        self.files    = sorted(
            f for f in os.listdir(path)
            if not f.startswith('.')
            and not f.endswith(_SKIP_SUFFIXES)
            and os.path.isfile(os.path.join(path, f))
        )

    @property
    def name(self) -> str:
        return self.manifest.get('name') or self.app_id

    @property
    def app_type(self) -> str:
        return self.manifest.get('app_type', '?')

    @property
    def version(self) -> str:
        return self.manifest.get('version', '?')

    @property
    def total_bytes(self) -> int:
        return sum(os.path.getsize(os.path.join(self.path, f)) for f in self.files)

    def label(self) -> str:
        return (f"{self.app_id:<18} {self.name:<16} "
                f"{self.app_type:<12} v{self.version:<8} "
                f"{len(self.files)} files, {self.total_bytes / 1024:,.1f} KB")


def discover_apps(dist_root: str) -> list:
    """Every <dist_root>/<app_id>/ that holds a manifest.json, sorted by id."""
    if not os.path.isdir(dist_root):
        return []
    apps = []
    for entry in sorted(os.listdir(dist_root)):
        path = os.path.join(dist_root, entry)
        manifest_path = os.path.join(path, 'manifest.json')
        if not os.path.isdir(path) or not os.path.isfile(manifest_path):
            continue
        try:
            with open(manifest_path) as f:
                manifest = json.load(f)
        except (OSError, json.JSONDecodeError) as exc:
            print(f"Warning: skipping {entry}: unreadable manifest.json ({exc})",
                  file=sys.stderr)
            continue
        app_id = manifest.get('app_id') or entry
        apps.append(App(app_id, path, manifest))
    return apps

# ---- Serial port discovery --------------------------------------------------

class Port:
    def __init__(self, device: str, description: str, is_device: bool):
        self.device      = device
        self.description = description
        self.is_device   = is_device      # matches the ESP32-C6 USB-Serial-JTAG

    def label(self) -> str:
        mark = "J++Device" if self.is_device else "         "
        return f"{mark}  {self.device:<24} {self.description}"


def discover_ports() -> list:
    """Serial ports, likely J++Devices first.

    A device is 'likely' when it presents the Espressif USB-Serial-JTAG
    VID:PID. Everything else is still listed — a USB hub or a different
    enumeration can hide the ids — just ranked below.
    """
    ports = []
    for p in list_ports.comports():
        is_device = (p.vid == ESPRESSIF_VID and p.pid == USB_JTAG_PID)
        # macOS enumerates both /dev/tty.* (call-in) and /dev/cu.* (call-out)
        # for the same device; only cu.* is usable for an outgoing session.
        if sys.platform == 'darwin' and '/tty.' in p.device:
            continue
        ports.append(Port(p.device, p.description or '', is_device))
    ports.sort(key=lambda p: (not p.is_device, p.device))
    return ports

# ---- Terminal UI ------------------------------------------------------------

def _interactive() -> bool:
    return sys.stdin.isatty() and sys.stdout.isatty()


def _select_curses(title: str, options: list, multi: bool,
                   preselected: set) -> list:
    """Full-screen picker. Returns chosen indices, or None if cancelled."""
    import curses

    def _run(stdscr):
        curses.curs_set(0)
        try:
            curses.use_default_colors()
        except curses.error:
            pass
        cursor   = 0
        selected = set(preselected) if multi else set()
        keys = ("↑/↓ move · SPACE toggle · a all · ENTER confirm · q cancel"
                if multi else "↑/↓ move · ENTER select · q cancel")

        while True:
            stdscr.erase()
            height, width = stdscr.getmaxyx()
            stdscr.addnstr(0, 0, title, width - 1, curses.A_BOLD)
            stdscr.addnstr(1, 0, keys, width - 1, curses.A_DIM)

            # Scroll the list so the cursor stays visible on short terminals.
            view_rows = max(height - 4, 1)
            first = max(0, min(cursor - view_rows // 2, len(options) - view_rows))
            first = max(first, 0)
            for row, idx in enumerate(range(first, min(first + view_rows,
                                                       len(options)))):
                mark = ("[x] " if idx in selected else "[ ] ") if multi else "    "
                attr = curses.A_REVERSE if idx == cursor else curses.A_NORMAL
                stdscr.addnstr(row + 3, 0, f"{mark}{options[idx]}", width - 1, attr)
            stdscr.refresh()

            ch = stdscr.getch()
            if ch in (curses.KEY_UP, ord('k')):
                cursor = (cursor - 1) % len(options)
            elif ch in (curses.KEY_DOWN, ord('j')):
                cursor = (cursor + 1) % len(options)
            elif multi and ch == ord(' '):
                selected.symmetric_difference_update({cursor})
            elif multi and ch in (ord('a'), ord('A')):
                selected = set() if len(selected) == len(options) \
                    else set(range(len(options)))
            elif ch in (curses.KEY_ENTER, 10, 13):
                return sorted(selected) if multi else [cursor]
            elif ch in (ord('q'), ord('Q')):
                return None
            elif ch == 27:
                # Either a bare ESC (cancel) or the lead byte of an escape
                # sequence curses did not fold into a KEY_* code — which is
                # what a terminal sending CSI arrows to an application-keypad
                # ncurses looks like. Drain the rest before deciding, so a
                # mis-negotiated keypad mode cannot make Up/Down quit.
                stdscr.nodelay(True)
                tail = []
                while True:
                    nxt = stdscr.getch()
                    if nxt == -1:
                        break
                    tail.append(nxt)
                stdscr.nodelay(False)
                if not tail:
                    return None
                if tail[-1] in (ord('A'), ord('B')):
                    cursor = (cursor + (-1 if tail[-1] == ord('A') else 1)) \
                        % len(options)

    return curses.wrapper(_run)


def _select_plain(title: str, options: list, multi: bool,
                  preselected: set) -> list:
    """Numbered-prompt fallback for terminals without curses."""
    print(f"\n{title}")
    for i, opt in enumerate(options, 1):
        mark = "*" if multi and (i - 1) in preselected else " "
        print(f"  {mark}{i:>2}. {opt}")
    prompt = ("Numbers (e.g. 1,3), 'a' for all, blank to cancel: " if multi
              else "Number (blank to cancel): ")
    try:
        raw = input(prompt).strip()
    except EOFError:
        return None
    if not raw:
        return None
    if multi and raw.lower() in ('a', 'all'):
        return list(range(len(options)))
    picked = []
    for tok in raw.replace(' ', ',').split(','):
        if not tok:
            continue
        if not tok.isdigit() or not (1 <= int(tok) <= len(options)):
            print(f"Invalid choice: {tok}", file=sys.stderr)
            return None
        picked.append(int(tok) - 1)
        if not multi:
            break
    return sorted(set(picked)) or None


def select(title: str, options: list, multi: bool = False,
           preselected: set = frozenset()) -> list:
    """Pick from *options*; returns chosen indices or None if cancelled."""
    if not options:
        return None
    try:
        import curses  # noqa: F401
    except ImportError:
        return _select_plain(title, options, multi, preselected)
    try:
        return _select_curses(title, options, multi, preselected)
    except Exception:
        # A terminal curses cannot drive (dumb TERM, no tty control) — the
        # numbered prompt works anywhere.
        return _select_plain(title, options, multi, preselected)

# ---- Deployment -------------------------------------------------------------

def deploy(session: SMPSession, app: App) -> None:
    remote_dir = f'/sd/apps/{app.app_id}'
    print(f"  {app.app_id} → {remote_dir}/  "
          f"({len(app.files)} files, {app.total_bytes:,} B)")
    session.mkdir(remote_dir)
    for filename in app.files:
        local = os.path.join(app.path, filename)
        print(f"    {filename}  ({os.path.getsize(local):,} B)")
        session.upload_file(local, f'{remote_dir}/{filename}')

# ---- Entry point ------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(
        prog='deploy.py',
        description='Deploy built JPPDOS apps to a J++Device over JPPD-SMP.',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Examples:\n"
            "  ./deploy.py                            pick apps + port interactively\n"
            "  ./deploy.py testapp_native             one app, auto-discovered port\n"
            "  ./deploy.py --all --port /dev/ttyACM0  every built app, explicit port\n"
            "  ./deploy.py --list                     show what is deployable\n"
        ),
    )
    parser.add_argument('app_ids', nargs='*',
                        help='App IDs to deploy (default: choose interactively)')
    parser.add_argument('--dist', default='dist', metavar='DIR',
                        help='Build output root holding <app_id>/ dirs (default: dist)')
    parser.add_argument('--port', metavar='PORT',
                        help='Serial port (default: auto-discover, ask if ambiguous)')
    parser.add_argument('--all', action='store_true',
                        help='Deploy every app found under --dist')
    parser.add_argument('--list', action='store_true',
                        help='List deployable apps and serial ports, then exit')
    args = parser.parse_args()

    # --- apps ---
    apps = discover_apps(args.dist)
    if args.list:
        print(f"Apps under {args.dist}/:")
        for app in apps:
            print(f"  {app.label()}")
        if not apps:
            print("  (none)")
        print("\nSerial ports:")
        ports = discover_ports()
        for p in ports:
            print(f"  {p.label()}")
        if not ports:
            print("  (none)")
        return 0

    if not apps:
        print(f"Error: no built apps found under {args.dist}/.\n"
              f"Build one first, e.g.:\n"
              f"  docker run --rm -v \"$PWD/apps/testapp_native:/app\" jppd-app-sdk",
              file=sys.stderr)
        return 1

    by_id = {app.app_id: app for app in apps}
    if args.app_ids:
        unknown = [a for a in args.app_ids if a not in by_id]
        if unknown:
            print(f"Error: not built under {args.dist}/: {', '.join(unknown)}\n"
                  f"Available: {', '.join(by_id)}", file=sys.stderr)
            return 1
        chosen = [by_id[a] for a in args.app_ids]
    elif args.all:
        chosen = apps
    else:
        if not _interactive():
            print("Error: no app selected and no terminal to ask on.\n"
                  "Pass app IDs, or --all.", file=sys.stderr)
            return 1
        picked = select(f"Select apps to deploy  ({args.dist}/)",
                        [a.label() for a in apps], multi=True)
        if not picked:
            print("Nothing selected.")
            return 1
        chosen = [apps[i] for i in picked]

    empty = [a.app_id for a in chosen if not a.files]
    if empty:
        print(f"Error: no uploadable files in: {', '.join(empty)}", file=sys.stderr)
        return 1

    # --- port ---
    port = args.port
    if port is None:
        ports = discover_ports()
        likely = [p for p in ports if p.is_device]
        if len(likely) == 1:
            port = likely[0].device
            print(f"Found device on {port} ({likely[0].description})")
        elif not ports:
            print("Error: no serial ports found. Connect the device, or pass --port.",
                  file=sys.stderr)
            return 1
        elif not _interactive():
            print("Error: could not pick a port unambiguously; pass --port.\n"
                  "Candidates: " + ", ".join(p.device for p in ports), file=sys.stderr)
            return 1
        else:
            picked = select("Select the device serial port",
                            [p.label() for p in ports], multi=False)
            if not picked:
                print("No port selected.")
                return 1
            port = ports[picked[0]].device

    # --- upload ---
    total = sum(a.total_bytes for a in chosen)
    print(f"\nDeploying {len(chosen)} app(s), {total:,} B → {port}")
    print("Waiting for device consent (press Allow on the device)...")

    session = None
    try:
        session = SMPSession(port)
        session.session_start()

        info = session.get_info()
        print(f"Connected: JPPDOS {info['fw_version']}  ({info['hwid']})")
        print(f"SD card:   {info['sd_label'] or '(no label)'}  "
              f"{info['sd_used'] / (1024 * 1024):,.1f} / "
              f"{info['sd_total'] / (1024 * 1024):,.1f} MB used\n")
        if info['sd_free'] < total:
            raise RuntimeError(
                f"not enough space on the SD card: {info['sd_free']:,} B free, "
                f"{total:,} B needed")

        for app in chosen:
            deploy(session, app)

        session.session_end()
        print(f"\nDone. Deployed: {', '.join(a.app_id for a in chosen)}")
        return 0

    except KeyboardInterrupt:
        print("\nInterrupted.", file=sys.stderr)
        return 1
    except Exception as exc:
        print(f"\nError: {exc}", file=sys.stderr)
        return 1
    finally:
        if session is not None:
            try:
                session.session_end()
            except Exception:
                pass
            session.close()


if __name__ == '__main__':
    sys.exit(main())
