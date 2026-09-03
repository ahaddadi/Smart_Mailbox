"""
Phone-app-style GUI for Smart_Mailbox: connect over BLE, toggle LED/relay,
provision WiFi, start the camera stream, and watch the MJPEG video inline.

Install:  pip install -r requirements.txt
Run:      python mailbox_gui.py
"""

import asyncio
import io
import queue
import re
import threading
import tkinter as tk
from tkinter import filedialog, ttk
from typing import Optional

import cv2
import numpy as np
import requests
from bleak import BleakClient, BleakScanner
from PIL import Image, ImageTk

DEVICE_NAME = "Smart_Mailbox"
MESSAGE_UUID = "4ac8a682-9736-4e5d-932b-e9b31405049c"

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
    through a thread-safe queue instead of touching bleak directly."""

    def __init__(self, event_q: queue.Queue):
        self.event_q = event_q
        self.client = None
        self.loop = asyncio.new_event_loop()
        threading.Thread(target=self._run_loop, daemon=True).start()

    def _run_loop(self):
        asyncio.set_event_loop(self.loop)
        self.loop.run_forever()

    def connect(self):
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
            await self.client.start_notify(MESSAGE_UUID, self._notify_handler)
            self.event_q.put(("status", f"Connected to {device.address}"))
            self.event_q.put(("connected", None))
        except Exception as e:
            self.event_q.put(("status", f"Connect failed: {e}"))

    def _notify_handler(self, _sender, data: bytearray):
        self.event_q.put(("notify", data.decode(errors="replace")))

    def send(self, cmd: str):
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
    the Content-Length header the firmware sends before each JPEG."""

    def __init__(self, url: str, event_q: queue.Queue):
        super().__init__(daemon=True)
        self.url = url
        self.event_q = event_q
        self._stop = threading.Event()

    def stop(self):
        self._stop.set()

    def run(self):
        try:
            resp = requests.get(self.url, stream=True, timeout=10)
        except Exception as e:
            self.event_q.put(("status", f"Stream connect failed: {e}"))
            return

        buf = b""
        try:
            for chunk in resp.iter_content(chunk_size=4096):
                if self._stop.is_set():
                    break
                if not chunk:
                    continue
                buf += chunk
                while True:
                    idx = buf.find(b"\r\n\r\n")
                    if idx == -1:
                        break
                    header = buf[:idx]
                    m = re.search(rb"Content-Length:\s*(\d+)", header, re.IGNORECASE)
                    if not m:
                        bidx = buf.find(b"--frame")
                        buf = buf[bidx:] if bidx != -1 else b""
                        if bidx == -1:
                            break
                        continue
                    length = int(m.group(1))
                    start = idx + 4
                    if len(buf) < start + length:
                        break
                    frame = buf[start:start + length]
                    buf = buf[start + length:]
                    self.event_q.put(("frame", frame))
        except Exception as e:
            self.event_q.put(("status", f"Stream error: {e}"))
        finally:
            resp.close()


class MailboxApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Smart Mailbox")
        self.geometry("420x760")
        self.minsize(380, 640)

        self.event_q = queue.Queue()
        self.ble = BLEWorker(self.event_q)
        self.stream_reader: Optional[MJPEGReader] = None

        self.recording = False
        self.video_writer: Optional[cv2.VideoWriter] = None
        self.video_path: Optional[str] = None

        self._apply_dark_theme()
        self._build_ui()
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.after(50, self._poll_events)

    def _apply_dark_theme(self):
        self.configure(background=BG)
        style = ttk.Style(self)
        style.theme_use("clam")  # the only built-in theme that honors custom colors on Windows

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
        self.status_var = tk.StringVar(value="Disconnected")
        ttk.Label(self, textvariable=self.status_var, foreground=MUTED_FG).pack(pady=(14, 6))

        conn = ttk.Frame(self)
        conn.pack(pady=(0, 6))
        ttk.Button(conn, text="Connect", command=self.ble.connect).pack(side="left", padx=4)
        ttk.Button(conn, text="Disconnect", command=self.ble.disconnect).pack(side="left", padx=4)
        ttk.Button(conn, text="Refresh Status", command=self._refresh_status).pack(side="left", padx=4)

        controls = ttk.LabelFrame(self, text="CONTROLS", padding=10)
        controls.pack(fill="x", padx=14, pady=8)
        controls.columnconfigure(0, weight=1)

        self.led_var = tk.StringVar(value="LED: ?")
        ttk.Label(controls, textvariable=self.led_var, style="Card.TLabel").grid(row=0, column=0, sticky="w", padx=4, pady=6)
        ttk.Button(controls, text="On", width=6, command=lambda: self.ble.send("led=on")).grid(row=0, column=1, padx=2)
        ttk.Button(controls, text="Off", width=6, command=lambda: self.ble.send("led=off")).grid(row=0, column=2, padx=2)

        self.relay_var = tk.StringVar(value="Relay: ?")
        ttk.Label(controls, textvariable=self.relay_var, style="Card.TLabel").grid(row=1, column=0, sticky="w", padx=4, pady=6)
        ttk.Button(controls, text="On", width=6, command=lambda: self.ble.send("relay=on")).grid(row=1, column=1, padx=2)
        ttk.Button(controls, text="Off", width=6, command=lambda: self.ble.send("relay=off")).grid(row=1, column=2, padx=2)

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

        stream = ttk.LabelFrame(self, text="CAMERA", padding=10)
        stream.pack(fill="both", expand=True, padx=14, pady=8)

        btns = ttk.Frame(stream, style="Card.TFrame")
        btns.pack(pady=4)
        ttk.Button(btns, text="Stream On", command=lambda: self.ble.send("stream=on")).pack(side="left", padx=4)
        ttk.Button(btns, text="Stream Off", command=self._stream_off).pack(side="left", padx=4)

        rec_row = ttk.Frame(stream, style="Card.TFrame")
        rec_row.pack(pady=(0, 6))
        self.record_btn_var = tk.StringVar(value="Start Recording")
        ttk.Button(rec_row, textvariable=self.record_btn_var, command=self._toggle_recording).pack(side="left", padx=4)
        self.recording_status_var = tk.StringVar(value="")
        ttk.Label(rec_row, textvariable=self.recording_status_var, style="Card.TLabel", foreground=ERROR_FG).pack(side="left", padx=8)

        self.video_label = ttk.Label(stream, text="(no stream)", anchor="center", background=FIELD_BG, foreground=MUTED_FG)
        self.video_label.pack(fill="both", expand=True, padx=2, pady=(2, 0))

        log_frame = ttk.LabelFrame(self, text="LOG", padding=8)
        log_frame.pack(fill="both", padx=14, pady=(0, 12))
        self.log_text = tk.Text(
            log_frame, height=6, state="disabled", wrap="word", font=("Consolas", 9),
            bg=FIELD_BG, fg=FG, insertbackground=FG, borderwidth=0, highlightthickness=0,
        )
        self.log_text.tag_configure("error", foreground=ERROR_FG)
        self.log_text.tag_configure("ok", foreground=OK_FG)
        self.log_text.pack(fill="both", expand=True)

    # --- actions ---

    def _wifi_connect(self):
        ssid = self.ssid_entry.get().strip()
        password = self.pass_entry.get()
        if not ssid:
            self._log("[GUI] Enter an SSID first")
            return
        self.ble.send(f"router_ssid={ssid};router_password={password};wifi=on")

    def _wifi_disconnect(self):
        self.ble.send("wifi=off")

    def _refresh_status(self):
        self.ble.send("led=status;relay=status;wifi=status;stream=status")

    def _wifi_scan(self):
        self.network_list.delete(0, "end")
        self._known_networks = []
        self.scan_status_var.set("Scanning...")
        self.ble.send("wifi=scan")

    def _on_network_selected(self, _event):
        selection = self.network_list.curselection()
        if not selection:
            return
        ssid = self._known_networks[selection[0]]
        self.ssid_entry.delete(0, "end")
        self.ssid_entry.insert(0, ssid)

    def _stream_off(self):
        self.ble.send("stream=off")
        self._stop_stream_reader()

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
            self.video_writer.release()
            self.video_writer = None
        self.record_btn_var.set("Start Recording")
        self.recording_status_var.set("")
        if self.video_path:
            self._log(f"Saved recording: {self.video_path}", tag="ok")
        self.video_path = None

    # --- event queue draining (runs on the Tk main thread) ---

    def _poll_events(self):
        try:
            while True:
                kind, payload = self.event_q.get_nowait()
                if kind == "status":
                    self.status_var.set(payload)
                    failed = "failed" in payload.lower() or "not found" in payload.lower()
                    self._log(f"[STATUS] {payload}", tag="error" if failed else None)
                elif kind == "connected":
                    self._refresh_status()
                elif kind == "notify":
                    self._handle_notify(payload)
                elif kind == "frame":
                    self._show_frame(payload)
        except queue.Empty:
            pass
        self.after(50, self._poll_events)

    def _handle_notify(self, text: str):
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
        elif text.startswith("stream_url="):
            self._start_stream_reader(text.split("=", 1)[1])
        elif text == "stream=0":
            self._stop_stream_reader()

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
        if text in self.NOTIFY_TEXT:
            return self.NOTIFY_TEXT[text]
        if text.startswith("stream_url="):
            return "Stream URL: " + text.split("=", 1)[1]
        if text.startswith("jpg_url="):
            return "Snapshot URL: " + text.split("=", 1)[1]
        if text.startswith("wifi_scan_done="):
            return f"WiFi scan complete: {text.split('=', 1)[1]} network(s) found"
        if text.startswith("wifi_scan="):
            return "Found network: " + text.split("=", 1)[1]
        return text

    OK_NOTIFIES = {"led=1", "relay=1", "wifi=1", "stream=1", "ok=router_ssid", "ok=router_password"}

    def _tag_for(self, raw_text: str) -> Optional[str]:
        if raw_text.startswith("err="):
            return "error"
        if raw_text in self.OK_NOTIFIES or raw_text.startswith("wifi_scan="):
            return "ok"
        return None

    def _start_stream_reader(self, url: str):
        self._stop_stream_reader()
        self.video_label.configure(text=f"Connecting to {url}...", image="")
        self.stream_reader = MJPEGReader(url, self.event_q)
        self.stream_reader.start()

    def _stop_stream_reader(self):
        if self.stream_reader:
            self.stream_reader.stop()
            self.stream_reader = None
        if self.recording:
            self._stop_recording()
        self.video_label.configure(image="", text="(no stream)")
        self.video_label.image = None

    def _show_frame(self, jpeg_bytes: bytes):
        try:
            img = Image.open(io.BytesIO(jpeg_bytes)).convert("RGB")
            if self.recording:
                self._write_video_frame(img)

            display_img = img.copy()
            display_img.thumbnail((380, 380))
            photo = ImageTk.PhotoImage(display_img)
            self.video_label.configure(image=photo, text="")
            self.video_label.image = photo  # keep a reference alive
        except Exception as e:
            self._log(f"[VIDEO] frame decode error: {e}", tag="error")

    def _write_video_frame(self, img: Image.Image):
        frame_bgr = cv2.cvtColor(np.array(img), cv2.COLOR_RGB2BGR)
        if self.video_writer is None:
            h, w = frame_bgr.shape[:2]
            fourcc = cv2.VideoWriter_fourcc(*"MJPG")
            self.video_writer = cv2.VideoWriter(self.video_path, fourcc, 10.0, (w, h))
        self.video_writer.write(frame_bgr)

    def _log(self, text: str, tag: Optional[str] = None):
        self.log_text.configure(state="normal")
        if tag:
            self.log_text.insert("end", text + "\n", tag)
        else:
            self.log_text.insert("end", text + "\n")
        self.log_text.see("end")
        self.log_text.configure(state="disabled")

    def _on_close(self):
        self._stop_stream_reader()
        self.ble.disconnect()
        self.destroy()


if __name__ == "__main__":
    MailboxApp().mainloop()
