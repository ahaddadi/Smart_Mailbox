"""
Phone-app-style GUI for Smart_Mailbox: connect over BLE, toggle LED/relay,
provision WiFi, start the camera stream, and watch the MJPEG video inline.

Architecture
------------
Tkinter's mainloop and bleak's asyncio event loop can't share a thread, so
this file runs several concurrent contexts that only ever talk to each
other through thread-safe queues (never by touching each other's
widgets/objects directly):

  1. The Tk main thread - owns every widget, runs MailboxApp's mainloop(),
     and is the only thread allowed to create/update tkinter widgets.
  2. A background asyncio event loop (BLEWorker._run_loop) - owns the
     bleak BLEClient and does all the actual BLE I/O.
  3. A background MJPEGReader thread per active stream - blocks on the
     board's HTTP /stream response and decodes frame boundaries.
  4. A short-lived background thread per HTTP command sent via
     _http_command() (LED/relay/stream once the board's WiFi IP is known)
     - requests.get() blocks, so it never runs on the Tk thread.

All of the background contexts above communicate results back to the Tk
thread by pushing (kind, payload) tuples onto MailboxApp.event_q, which is
drained once every 50ms by MailboxApp._poll_events() running on the Tk
thread itself (see the self.after(50, self._poll_events) reschedule loop).

Wire protocol
-------------
Everything sent to/received from the board is the same short text protocol
used by the firmware's BLE characteristic, Serial input, and HTTP /cmd
endpoint (see Smart_Mailbox.ino's processCommand()): commands look like
"led=on" or "router_ssid=Foo;router_password=bar;wifi=on", and the board
replies with similarly-shaped "key=value" notifications (e.g. "led=1",
"wifi=0", "stream_url=http://...", "err=wifi_off"). NOTIFY_TEXT/
_readable_notify() below translate those into plain English for the log
panel.

BLE vs. WiFi transport
-----------------------
WiFi provisioning (scanning, entering credentials, wifi=on/off) always
goes over BLE - the board can't be reached over WiFi until it has actually
joined a network. But once it has, its "ip=..." notification (sent after
a successful wifi=on or a wifi=status while connected) tells this GUI
where to reach it, and from then on LED/relay/stream control is sent over
plain HTTP via _http_command() instead of BLE, using the same /cmd
endpoint and reusing _handle_notify() for the replies (see
_http_command_worker() below, which feeds the HTTP response back through
the exact same code path a BLE notification would take).

Install:  pip install -r requirements.txt
Run:      python mailbox_gui.py
"""

import asyncio
import io
import json
import queue
import re
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, ttk
from typing import Optional

import cv2
import numpy as np
import requests
from bleak import BleakClient, BleakScanner
from PIL import Image, ImageTk

# Must match the firmware's SERVICE_UUID/MESSAGE_UUID and advertised name
# in Smart_Mailbox.ino - this is how we find the right BLE device and know
# which characteristic carries the command/notify protocol.
DEVICE_NAME = "Smart_Mailbox"
MESSAGE_UUID = "4ac8a682-9736-4e5d-932b-e9b31405049c"

# Small local cache of the board's last known WiFi IP, so the GUI doesn't
# have to reconnect over BLE just to rediscover it on every launch. Stored
# next to this script rather than anywhere shared, since it's only ever a
# convenience hint - see _load_config/_save_config and board_ip below.
CONFIG_PATH = Path(__file__).resolve().parent / "mailbox_gui_config.json"


def _load_config() -> dict:
    try:
        return json.loads(CONFIG_PATH.read_text())
    except Exception:
        return {}


def _save_config(data: dict):
    try:
        CONFIG_PATH.write_text(json.dumps(data))
    except Exception:
        pass  # best-effort only - losing the cached IP just means one extra BLE round-trip next launch


# Three-tier elevation, like iOS/macOS dark mode: window is darkest,
# card panels are a shade lighter, interactive fields lighter still.
BG = "#1c1c1e"
CARD_BG = "#2c2c2e"
FIELD_BG = "#3a3a3c"
FG = "#f2f2f7"
MUTED_FG = "#98989d"
BORDER = "#48484a"
ACCENT = "#0a84ff"
ERROR_FG = "#ff453a"
OK_FG = "#30d158"
FONT_TITLE = ("Segoe UI", 10, "bold")


class BLEWorker:
    """Runs an asyncio loop on a background thread; the GUI talks to it
    through a thread-safe queue instead of touching bleak directly.

    Every public method (connect/send/disconnect) is safe to call from the
    Tk main thread: each just schedules a coroutine onto the background
    loop via asyncio.run_coroutine_threadsafe() and returns immediately -
    none of them block the caller waiting for the BLE operation to finish.
    Results/errors instead arrive later as ("status", ...) or
    ("notify", ...) tuples pushed onto event_q, which the GUI drains on
    its own timer (MailboxApp._poll_events)."""

    def __init__(self, event_q: queue.Queue):
        self.event_q = event_q
        self.client = None  # the connected BleakClient, or None until connect() succeeds
        self.loop = asyncio.new_event_loop()
        # This thread does nothing but pump the asyncio loop forever; all
        # actual BLE work happens as coroutines scheduled onto it from the
        # Tk thread via run_coroutine_threadsafe().
        threading.Thread(target=self._run_loop, daemon=True).start()

    def _run_loop(self):
        asyncio.set_event_loop(self.loop)
        self.loop.run_forever()

    def connect(self):
        """Scan for the board by name and connect. Fire-and-forget from
        the caller's perspective - progress/result comes back via
        event_q ("status", ...) messages."""
        asyncio.run_coroutine_threadsafe(self._connect(), self.loop)

    async def _connect(self):
        self.event_q.put(("status", f"Scanning for {DEVICE_NAME}..."))
        try:
            device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=10.0)
            if device is None:
                self.event_q.put(("status", "Device not found"))
                return
            self.client = BleakClient(device)
            await self.client.connect()
            # Subscribe to notifications now, before telling the GUI we're
            # connected, so we don't race a reply against the subscription.
            await self.client.start_notify(MESSAGE_UUID, self._notify_handler)
            self.event_q.put(("status", f"Connected to {device.address}"))
            # Separate from the "status" event so the GUI can react
            # specifically to "we're ready to talk to the board now"
            # (triggers an automatic status refresh - see MailboxApp).
            self.event_q.put(("connected", None))
        except Exception as e:
            self.event_q.put(("status", f"Connect failed: {e}"))

    def _notify_handler(self, _sender, data: bytearray):
        # Called by bleak on the asyncio loop's thread whenever the board
        # sends a characteristic notification. Just forwards the decoded
        # text to the GUI thread via the queue; MailboxApp._handle_notify
        # does the actual interpretation.
        self.event_q.put(("notify", data.decode(errors="replace")))

    def send(self, cmd: str):
        """Write a command string (e.g. "led=on") to the board. No-ops
        with a "Not connected" status if we don't currently hold a live
        connection."""
        if self.client and self.client.is_connected:
            asyncio.run_coroutine_threadsafe(self._send(cmd), self.loop)
        else:
            self.event_q.put(("status", "Not connected"))

    async def _send(self, cmd: str):
        try:
            await self.client.write_gatt_char(MESSAGE_UUID, cmd.encode(), response=True)
        except Exception as e:
            self.event_q.put(("status", f"Write failed: {e}"))

    def disconnect(self):
        if self.client:
            asyncio.run_coroutine_threadsafe(self._disconnect(), self.loop)

    async def _disconnect(self):
        try:
            await self.client.disconnect()
        except Exception:
            pass
        self.event_q.put(("status", "Disconnected"))


class MJPEGReader(threading.Thread):
    """Pulls the board's multipart MJPEG stream apart frame by frame using
    the Content-Length header the firmware sends before each JPEG.

    One instance is created per active stream (see
    MailboxApp._start_stream_reader) and runs entirely on its own thread,
    since the HTTP GET blocks reading the response body as fast as the
    board sends frames. Decoded JPEG frames are pushed onto event_q as
    ("frame", bytes) tuples for the Tk thread to actually display."""

    def __init__(self, url: str, event_q: queue.Queue):
        super().__init__(daemon=True)
        self.url = url
        self.event_q = event_q
        self._stop = threading.Event()

    def stop(self):
        # Signals run() to exit at the next opportunity; doesn't forcibly
        # kill the thread (Python threads can't be killed), but the loop
        # below checks this flag on every chunk received.
        self._stop.set()

    def run(self):
        try:
            resp = requests.get(self.url, stream=True, timeout=10)
        except Exception as e:
            self.event_q.put(("status", f"Stream connect failed: {e}"))
            return

        # Incrementally accumulated bytes from the HTTP response body.
        # The firmware's MJPEG stream repeats, for every frame:
        #   "\r\n--frame\r\n" + "Content-Type: ...\r\nContent-Length: N\r\n\r\n" + <N JPEG bytes>
        # so we keep buffering until we can see a full header block
        # (terminated by a blank line, "\r\n\r\n") followed by at least N
        # more bytes for the JPEG payload itself.
        buf = b""
        try:
            for chunk in resp.iter_content(chunk_size=4096):
                if self._stop.is_set():
                    break
                if not chunk:
                    continue
                buf += chunk
                # Drain as many complete frames as are currently buffered
                # before going back to read more off the socket.
                while True:
                    idx = buf.find(b"\r\n\r\n")
                    if idx == -1:
                        break  # header not fully received yet - wait for more data
                    header = buf[:idx]
                    m = re.search(rb"Content-Length:\s*(\d+)", header, re.IGNORECASE)
                    if not m:
                        # Not a valid frame header (e.g. we're still
                        # positioned mid-boundary-marker from a previous
                        # partial read) - resync to the next boundary
                        # marker and try again.
                        bidx = buf.find(b"--frame")
                        buf = buf[bidx:] if bidx != -1 else b""
                        if bidx == -1:
                            break
                        continue
                    length = int(m.group(1))
                    start = idx + 4  # skip past the header-terminating blank line
                    if len(buf) < start + length:
                        break  # full JPEG payload not buffered yet - wait for more data
                    frame = buf[start:start + length]
                    buf = buf[start + length:]  # leftover bytes (next boundary marker, etc.) stay buffered
                    self.event_q.put(("frame", frame))
        except Exception as e:
            self.event_q.put(("status", f"Stream error: {e}"))
        finally:
            resp.close()


class MailboxApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Smart Mailbox")
        # The Camera panel is the only section packed with expand=True, so
        # it only gets whatever height is left after the other (fixed-
        # height) panels claim theirs - make the window tall enough that
        # leftover is actually enough to show the video well.
        self.geometry("460x920")
        self.minsize(420, 820)

        self.event_q = queue.Queue()
        self.ble = BLEWorker(self.event_q)
        self.stream_reader: Optional[MJPEGReader] = None

        # The board's WiFi IP address, used by _http_command() to send
        # LED/relay/stream control over WiFi instead of BLE once known.
        # Pre-seeded from CONFIG_PATH (the last IP we saw last session) so
        # HTTP control can work immediately without needing a fresh BLE
        # connection first - if that cached address is stale (board got a
        # new DHCP lease, etc.) the HTTP request just fails and logs an
        # error; a fresh "ip=..." BLE notification (see _handle_notify)
        # always overwrites it with the current one and re-saves it.
        self.board_ip: Optional[str] = _load_config().get("board_ip")

        # Video recording state (see _toggle_recording/_write_video_frame).
        self.recording = False
        self.video_writer: Optional[cv2.VideoWriter] = None
        self.video_path: Optional[str] = None

        self._apply_dark_theme()
        self._build_ui()
        if self.board_ip:
            self._log(f"[GUI] Using last known board IP: {self.board_ip} (unconfirmed)")
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        # Drives both BLEWorker and MJPEGReader results into the UI: runs
        # on the Tk thread, reschedules itself every 50ms.
        self.after(50, self._poll_events)

    def _apply_dark_theme(self):
        self.configure(background=BG)
        style = ttk.Style(self)
        style.theme_use("clam")  # the only built-in theme that honors custom colors on Windows

        # Base styles: apply to every ttk widget unless a more specific
        # style/instance override says otherwise.
        style.configure(".", background=BG, foreground=FG, fieldbackground=FIELD_BG, font=("Segoe UI", 9))
        style.configure("TFrame", background=BG)
        style.configure("TLabel", background=BG, foreground=FG)
        style.configure("TLabelframe", background=CARD_BG, foreground=FG, bordercolor=BORDER, borderwidth=1, relief="flat")
        style.configure("TLabelframe.Label", background=CARD_BG, foreground=MUTED_FG, font=FONT_TITLE)

        style.configure(
            "TButton", background=FIELD_BG, foreground=FG, bordercolor=BORDER,
            focusthickness=0, padding=(10, 6), relief="flat",
        )
        style.map(
            "TButton",
            background=[("active", ACCENT), ("pressed", ACCENT)],
            foreground=[("active", "#ffffff"), ("pressed", "#ffffff")],
        )
        style.configure("TEntry", fieldbackground=FIELD_BG, foreground=FG, insertcolor=FG, bordercolor=BORDER, padding=6)

        # widgets that sit inside a card (LabelFrame) need the card's own
        # background instead of the window's, or they'd show a mismatched box
        style.configure("Card.TFrame", background=CARD_BG)
        style.configure("Card.TLabel", background=CARD_BG, foreground=FG)

    def _build_ui(self):
        # --- top: connection status + BLE connect/disconnect/refresh ---
        self.status_var = tk.StringVar(value="Disconnected")
        ttk.Label(self, textvariable=self.status_var, foreground=MUTED_FG).pack(pady=(14, 6))

        conn = ttk.Frame(self)
        conn.pack(pady=(0, 6))
        ttk.Button(conn, text="Connect", command=self.ble.connect).pack(side="left", padx=4)
        ttk.Button(conn, text="Disconnect", command=self.ble.disconnect).pack(side="left", padx=4)
        ttk.Button(conn, text="Refresh Status", command=self._refresh_status).pack(side="left", padx=4)

        # --- CONTROLS panel: LED + relay toggles ---
        controls = ttk.LabelFrame(self, text="CONTROLS", padding=10)
        controls.pack(fill="x", padx=14, pady=8)
        controls.columnconfigure(0, weight=1)

        self.led_var = tk.StringVar(value="LED: ?")
        ttk.Label(controls, textvariable=self.led_var, style="Card.TLabel").grid(row=0, column=0, sticky="w", padx=4, pady=6)
        ttk.Button(controls, text="On", width=6, command=lambda: self._http_command("led=on")).grid(row=0, column=1, padx=2)
        ttk.Button(controls, text="Off", width=6, command=lambda: self._http_command("led=off")).grid(row=0, column=2, padx=2)

        self.relay_var = tk.StringVar(value="Relay: ?")
        ttk.Label(controls, textvariable=self.relay_var, style="Card.TLabel").grid(row=1, column=0, sticky="w", padx=4, pady=6)
        ttk.Button(controls, text="On", width=6, command=lambda: self._http_command("relay=on")).grid(row=1, column=1, padx=2)
        ttk.Button(controls, text="Off", width=6, command=lambda: self._http_command("relay=off")).grid(row=1, column=2, padx=2)

        # --- WIFI panel: network scan/picker + credential entry + connect/disconnect ---
        wifi = ttk.LabelFrame(self, text="WIFI", padding=10)
        wifi.pack(fill="x", padx=14, pady=8)
        wifi.columnconfigure(1, weight=1)

        scan_row = ttk.Frame(wifi, style="Card.TFrame")
        scan_row.grid(row=0, column=0, columnspan=2, sticky="ew", padx=4, pady=(0, 6))
        ttk.Button(scan_row, text="Scan Networks", command=self._wifi_scan).pack(side="left")
        self.scan_status_var = tk.StringVar(value="")
        ttk.Label(scan_row, textvariable=self.scan_status_var, style="Card.TLabel", foreground=MUTED_FG).pack(side="left", padx=8)

        self.network_list = tk.Listbox(
            wifi, height=5, bg=FIELD_BG, fg=FG, selectbackground=ACCENT, selectforeground="#ffffff",
            highlightthickness=1, highlightbackground=BORDER, highlightcolor=ACCENT, borderwidth=0,
            relief="flat", activestyle="none",
        )
        self.network_list.grid(row=1, column=0, columnspan=2, sticky="ew", padx=4, pady=6)
        self.network_list.bind("<<ListboxSelect>>", self._on_network_selected)
        self._known_networks = []  # in-order, de-duplicated SSIDs from the last scan

        ttk.Label(wifi, text="Network Name", style="Card.TLabel").grid(row=2, column=0, sticky="w", padx=4, pady=6)
        self.ssid_entry = ttk.Entry(wifi)
        self.ssid_entry.grid(row=2, column=1, sticky="ew", padx=4, pady=6)

        ttk.Label(wifi, text="Password", style="Card.TLabel").grid(row=3, column=0, sticky="w", padx=4, pady=6)
        self.pass_entry = ttk.Entry(wifi, show="*")
        self.pass_entry.grid(row=3, column=1, sticky="ew", padx=4, pady=6)

        wifi_btns = ttk.Frame(wifi, style="Card.TFrame")
        wifi_btns.grid(row=4, column=0, columnspan=2, pady=8)
        ttk.Button(wifi_btns, text="Connect", command=self._wifi_connect).pack(side="left", padx=4)
        ttk.Button(wifi_btns, text="Disconnect", command=self._wifi_disconnect).pack(side="left", padx=4)

        self.wifi_var = tk.StringVar(value="WiFi: ?")
        ttk.Label(wifi, textvariable=self.wifi_var, style="Card.TLabel").grid(row=5, column=0, columnspan=2, pady=(0, 4))

        # --- CAMERA panel: stream on/off, recording, and the live video itself ---
        stream = ttk.LabelFrame(self, text="CAMERA", padding=10)
        stream.pack(fill="both", expand=True, padx=14, pady=8)

        btns = ttk.Frame(stream, style="Card.TFrame")
        btns.pack(pady=4)
        ttk.Button(btns, text="Stream On", command=lambda: self._http_command("stream=on")).pack(side="left", padx=4)
        ttk.Button(btns, text="Stream Off", command=self._stream_off).pack(side="left", padx=4)

        rec_row = ttk.Frame(stream, style="Card.TFrame")
        rec_row.pack(pady=(0, 6))
        self.record_btn_var = tk.StringVar(value="Start Recording")
        ttk.Button(rec_row, textvariable=self.record_btn_var, command=self._toggle_recording).pack(side="left", padx=4)
        self.recording_status_var = tk.StringVar(value="")
        ttk.Label(rec_row, textvariable=self.recording_status_var, style="Card.TLabel", foreground=ERROR_FG).pack(side="left", padx=8)

        # The actual video display: a plain Label whose image gets swapped
        # out on every decoded frame (see _show_frame). Placeholder text
        # shows whenever nothing is streaming.
        self.video_label = ttk.Label(stream, text="(no stream)", anchor="center", background=FIELD_BG, foreground=MUTED_FG)
        self.video_label.pack(fill="both", expand=True, padx=2, pady=(2, 0))

        # --- LOG panel: scrolling, color-tagged event history ---
        log_frame = ttk.LabelFrame(self, text="LOG", padding=8)
        log_frame.pack(fill="both", padx=14, pady=(0, 12))
        self.log_text = tk.Text(
            log_frame, height=6, state="disabled", wrap="word", font=("Consolas", 9),
            bg=FIELD_BG, fg=FG, insertbackground=FG, borderwidth=0, highlightthickness=0,
        )
        self.log_text.tag_configure("error", foreground=ERROR_FG)
        self.log_text.tag_configure("ok", foreground=OK_FG)
        self.log_text.pack(fill="both", expand=True)

    # --- actions (all run on the Tk main thread, in response to a button click) ---

    def _wifi_connect(self):
        # Sends SSID + password + wifi=on as one combined command, matching
        # the firmware's expectation that router_ssid/router_password are
        # saved to flash immediately and wifi=on then uses whatever the
        # current saved values are (see processCommand() in the firmware).
        ssid = self.ssid_entry.get().strip()
        password = self.pass_entry.get()
        if not ssid:
            self._log("[GUI] Enter an SSID first")
            return
        self.ble.send(f"router_ssid={ssid};router_password={password};wifi=on")

    def _wifi_disconnect(self):
        self.ble.send("wifi=off")

    def _refresh_status(self):
        # One combined command requesting every status flag at once; the
        # board replies with one notification per key, each handled in
        # _handle_notify.
        self.ble.send("led=status;relay=status;wifi=status;stream=status")

    def _wifi_scan(self):
        # Clears the previous scan's results immediately (rather than
        # waiting for the board's reply) so stale entries don't linger
        # while a new scan is in progress.
        self.network_list.delete(0, "end")
        self._known_networks = []
        self.scan_status_var.set("Scanning...")
        self.ble.send("wifi=scan")

    def _on_network_selected(self, _event):
        # Clicking an entry in the scanned-networks list just fills the
        # SSID field for you - you still need to type the password and hit
        # Connect yourself.
        selection = self.network_list.curselection()
        if not selection:
            return
        ssid = self._known_networks[selection[0]]
        self.ssid_entry.delete(0, "end")
        self.ssid_entry.insert(0, ssid)

    def _stream_off(self):
        self._http_command("stream=off")
        self._stop_stream_reader()  # stop showing video locally right away, don't wait on the request

    def _http_command(self, cmd: str):
        """Send `cmd` (e.g. "led=on") to the board's HTTP /cmd endpoint
        instead of over BLE - requires board_ip to already be known (see
        __init__/_handle_notify). Falls back to BLE if it isn't, so these
        buttons still work before the board's WiFi IP has been learned."""
        if not self.board_ip:
            self._log(f"[GUI] Board IP not known yet - sending \"{cmd}\" over BLE instead")
            self.ble.send(cmd)
            return
        threading.Thread(target=self._http_command_worker, args=(cmd,), daemon=True).start()

    def _http_command_worker(self, cmd: str):
        # Runs on its own thread since requests.get() blocks. Every line
        # of the response body is fed back through the queue as if it
        # were a BLE notification - reuses _handle_notify()'s translation,
        # color-tagging, and state-updating logic unchanged, regardless of
        # which transport the reply actually came over.
        url = f"http://{self.board_ip}/cmd"
        try:
            resp = requests.get(url, params={"c": cmd}, timeout=5)
            for line in resp.text.splitlines():
                line = line.strip()
                if line:
                    self.event_q.put(("notify", line))
        except Exception as e:
            self.event_q.put(("status", f"HTTP command '{cmd}' failed: {e}"))

    def _toggle_recording(self):
        if self.recording:
            self._stop_recording()
        else:
            self._start_recording()

    def _start_recording(self):
        if self.stream_reader is None:
            self._log("[GUI] Start the stream before recording", tag="error")
            return
        path = filedialog.asksaveasfilename(
            title="Save video as",
            defaultextension=".avi",
            filetypes=[("AVI video", "*.avi")],
        )
        if not path:
            return
        self.video_path = path
        self.video_writer = None  # created lazily once we know the frame size
        self.recording = True
        self.record_btn_var.set("Stop Recording")
        self.recording_status_var.set("● REC")
        self._log(f"Recording to {path}")

    def _stop_recording(self):
        self.recording = False
        if self.video_writer is not None:
            self.video_writer.release()  # flushes and closes the AVI file properly
            self.video_writer = None
        self.record_btn_var.set("Start Recording")
        self.recording_status_var.set("")
        if self.video_path:
            self._log(f"Saved recording: {self.video_path}", tag="ok")
        self.video_path = None

    # --- event queue draining (runs on the Tk main thread) ---

    def _poll_events(self):
        # Drains everything BLEWorker/MJPEGReader have queued up since the
        # last tick, then reschedules itself - this is the only place
        # background-thread results actually touch tkinter widgets, which
        # keeps all UI mutation safely on the Tk thread.
        try:
            while True:
                kind, payload = self.event_q.get_nowait()
                if kind == "status":
                    self.status_var.set(payload)
                    failed = "failed" in payload.lower() or "not found" in payload.lower()
                    self._log(f"[STATUS] {payload}", tag="error" if failed else None)
                elif kind == "connected":
                    # As soon as the BLE connection is actually usable,
                    # immediately ask the board for its current state -
                    # this is also what lets an already-streaming board
                    # resume showing video without the user re-clicking
                    # Stream On (see stream=status handling in the
                    # firmware and the stream_url= branch below).
                    self._refresh_status()
                elif kind == "notify":
                    self._handle_notify(payload)
                elif kind == "frame":
                    self._show_frame(payload)
        except queue.Empty:
            pass
        self.after(50, self._poll_events)

    def _handle_notify(self, text: str):
        # Every notification gets logged (translated to plain English, and
        # color-tagged) regardless of type, then we additionally react to
        # the specific ones that should update UI state or trigger an
        # action (like opening the video stream).
        self._log(self._readable_notify(text), tag=self._tag_for(text))
        if text.startswith("wifi_scan_done="):
            self.scan_status_var.set(f"Found {text.split('=', 1)[1]} network(s)")
        elif text.startswith("wifi_scan="):
            ssid = text.split("=", 1)[1]
            if ssid and ssid not in self._known_networks:
                self._known_networks.append(ssid)
                self.network_list.insert("end", ssid)
        elif text == "err=wifi_scan_failed":
            self.scan_status_var.set("Scan failed")
        elif text.startswith("led="):
            self.led_var.set("LED: " + ("On" if text.endswith("1") else "Off"))
        elif text.startswith("relay="):
            self.relay_var.set("Relay: " + ("On" if text.endswith("1") else "Off"))
        elif text.startswith("wifi="):
            self.wifi_var.set("WiFi: " + ("Connected" if text.endswith("1") else "Disconnected"))
        elif text.startswith("ip="):
            # The board's current WiFi address - from here on, LED/relay/
            # stream commands switch to HTTP (see _http_command). Cached
            # to disk so it's available immediately on the next launch too.
            self.board_ip = text.split("=", 1)[1]
            _save_config({"board_ip": self.board_ip})
        elif text.startswith("stream_url="):
            # Arrives either right after "stream=on", or as part of a
            # stream=status reply if the board was already streaming -
            # either way, (re)start showing video.
            self._start_stream_reader(text.split("=", 1)[1])
        elif text == "stream=0":
            self._stop_stream_reader()

    # Raw board notification -> plain-English log text. Keys here must
    # match exactly what Smart_Mailbox.ino's bleNotifyAndPrint() sends.
    NOTIFY_TEXT = {
        "led=1": "LED turned ON",
        "led=0": "LED turned OFF",
        "relay=1": "Relay turned ON",
        "relay=0": "Relay turned OFF",
        "wifi=1": "WiFi connected",
        "wifi=0": "WiFi disconnected",
        "stream=1": "Streaming started",
        "stream=0": "Streaming stopped",
        "ok=router_ssid": "WiFi SSID saved",
        "ok=router_password": "WiFi password saved",
        "err=wifi_off": "Error: WiFi is not connected",
        "err=camera_init": "Error: camera failed to initialize",
        "err=no_ssid": "Error: no SSID set",
        "err=wifi_scan_failed": "Error: WiFi scan failed",
    }

    def _readable_notify(self, text: str) -> str:
        # Exact-match table first, then a few prefix-based cases for
        # messages that carry a variable payload (a URL, an SSID, a scan
        # count) that can't be a fixed dict key. Anything unrecognized is
        # shown as-is rather than dropped, so nothing goes silently missing
        # from the log even if the firmware adds a new message type later.
        if text in self.NOTIFY_TEXT:
            return self.NOTIFY_TEXT[text]
        if text.startswith("ip="):
            return "Board WiFi IP: " + text.split("=", 1)[1]
        if text.startswith("stream_url="):
            return "Stream URL: " + text.split("=", 1)[1]
        if text.startswith("jpg_url="):
            return "Snapshot URL: " + text.split("=", 1)[1]
        if text.startswith("wifi_scan_done="):
            return f"WiFi scan complete: {text.split('=', 1)[1]} network(s) found"
        if text.startswith("wifi_scan="):
            return "Found network: " + text.split("=", 1)[1]
        return text

    # Which raw (untranslated) notifications count as a clear success,
    # for log color-coding. Deliberately checked against the *raw* text
    # (before translation to English) rather than by scanning the
    # translated string for a word like "connected" - "WiFi disconnected"
    # contains the substring "connected" too, which would otherwise
    # falsely tag a disconnect message as a success.
    OK_NOTIFIES = {"led=1", "relay=1", "wifi=1", "stream=1", "ok=router_ssid", "ok=router_password"}

    def _tag_for(self, raw_text: str) -> Optional[str]:
        if raw_text.startswith("err="):
            return "error"
        if raw_text in self.OK_NOTIFIES or raw_text.startswith("wifi_scan="):
            return "ok"
        return None

    def _start_stream_reader(self, url: str):
        self._stop_stream_reader()  # tear down any previous reader first
        self.video_label.configure(text=f"Connecting to {url}...", image="")
        self.stream_reader = MJPEGReader(url, self.event_q)
        self.stream_reader.start()

    def _stop_stream_reader(self):
        if self.stream_reader:
            self.stream_reader.stop()
            self.stream_reader = None
        if self.recording:
            # Never leave a half-written video file behind just because
            # the stream itself stopped - close it out properly.
            self._stop_recording()
        self.video_label.configure(image="", text="(no stream)")
        self.video_label.image = None

    def _show_frame(self, jpeg_bytes: bytes):
        try:
            img = Image.open(io.BytesIO(jpeg_bytes)).convert("RGB")
            if self.recording:
                # Record the frame at its original camera resolution,
                # before it gets shrunk down for on-screen display below.
                self._write_video_frame(img)

            display_img = img.copy()
            display_img.thumbnail((420, 420))
            photo = ImageTk.PhotoImage(display_img)
            self.video_label.configure(image=photo, text="")
            self.video_label.image = photo  # keep a reference alive (tkinter won't hold one for you)
        except Exception as e:
            self._log(f"[VIDEO] frame decode error: {e}", tag="error")

    def _write_video_frame(self, img: Image.Image):
        # PIL images are RGB; OpenCV's VideoWriter expects BGR.
        frame_bgr = cv2.cvtColor(np.array(img), cv2.COLOR_RGB2BGR)
        if self.video_writer is None:
            # The frame size isn't known until the first frame arrives, so
            # the VideoWriter is created lazily here rather than in
            # _start_recording(). 10 fps is a nominal playback rate, not a
            # measurement of the camera's actual frame rate.
            h, w = frame_bgr.shape[:2]
            fourcc = cv2.VideoWriter_fourcc(*"MJPG")
            self.video_writer = cv2.VideoWriter(self.video_path, fourcc, 10.0, (w, h))
        self.video_writer.write(frame_bgr)

    def _log(self, text: str, tag: Optional[str] = None):
        self.log_text.configure(state="normal")  # briefly re-enable editing to append a line
        if tag:
            self.log_text.insert("end", text + "\n", tag)
        else:
            self.log_text.insert("end", text + "\n")
        self.log_text.see("end")  # auto-scroll to the newest line
        self.log_text.configure(state="disabled")  # back to read-only

    def _on_close(self):
        self._stop_stream_reader()  # also stops any in-progress recording cleanly
        self.ble.disconnect()
        self.destroy()


if __name__ == "__main__":
    MailboxApp().mainloop()
