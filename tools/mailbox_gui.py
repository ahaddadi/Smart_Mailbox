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
from tkinter import ttk
from typing import Optional

import requests
from bleak import BleakClient, BleakScanner
from PIL import Image, ImageTk

DEVICE_NAME = "Smart_Mailbox"
MESSAGE_UUID = "4ac8a682-9736-4e5d-932b-e9b31405049c"


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

        self._build_ui()
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.after(50, self._poll_events)

    def _build_ui(self):
        self.status_var = tk.StringVar(value="Disconnected")
        ttk.Label(self, textvariable=self.status_var, foreground="#555").pack(pady=(10, 4))

        conn = ttk.Frame(self)
        conn.pack(pady=2)
        ttk.Button(conn, text="Connect", command=self.ble.connect).pack(side="left", padx=4)
        ttk.Button(conn, text="Disconnect", command=self.ble.disconnect).pack(side="left", padx=4)
        ttk.Button(conn, text="Refresh Status", command=self._refresh_status).pack(side="left", padx=4)

        controls = ttk.LabelFrame(self, text="Controls")
        controls.pack(fill="x", padx=12, pady=8)
        controls.columnconfigure(0, weight=1)

        self.led_var = tk.StringVar(value="LED: ?")
        ttk.Label(controls, textvariable=self.led_var).grid(row=0, column=0, sticky="w", padx=6, pady=4)
        ttk.Button(controls, text="On", width=6, command=lambda: self.ble.send("led=on")).grid(row=0, column=1, padx=2)
        ttk.Button(controls, text="Off", width=6, command=lambda: self.ble.send("led=off")).grid(row=0, column=2, padx=2)

        self.relay_var = tk.StringVar(value="Relay: ?")
        ttk.Label(controls, textvariable=self.relay_var).grid(row=1, column=0, sticky="w", padx=6, pady=4)
        ttk.Button(controls, text="On", width=6, command=lambda: self.ble.send("relay=on")).grid(row=1, column=1, padx=2)
        ttk.Button(controls, text="Off", width=6, command=lambda: self.ble.send("relay=off")).grid(row=1, column=2, padx=2)

        wifi = ttk.LabelFrame(self, text="WiFi")
        wifi.pack(fill="x", padx=12, pady=8)
        wifi.columnconfigure(1, weight=1)

        scan_row = ttk.Frame(wifi)
        scan_row.grid(row=0, column=0, columnspan=2, sticky="ew", padx=6, pady=(4, 0))
        ttk.Button(scan_row, text="Scan Networks", command=self._wifi_scan).pack(side="left")
        self.scan_status_var = tk.StringVar(value="")
        ttk.Label(scan_row, textvariable=self.scan_status_var, foreground="#777").pack(side="left", padx=8)

        self.network_list = tk.Listbox(wifi, height=5)
        self.network_list.grid(row=1, column=0, columnspan=2, sticky="ew", padx=6, pady=4)
        self.network_list.bind("<<ListboxSelect>>", self._on_network_selected)
        self._known_networks = []  # in-order, de-duplicated SSIDs from the last scan

        ttk.Label(wifi, text="SSID").grid(row=2, column=0, sticky="w", padx=6, pady=4)
        self.ssid_entry = ttk.Entry(wifi)
        self.ssid_entry.grid(row=2, column=1, sticky="ew", padx=6, pady=4)

        ttk.Label(wifi, text="Password").grid(row=3, column=0, sticky="w", padx=6, pady=4)
        self.pass_entry = ttk.Entry(wifi, show="*")
        self.pass_entry.grid(row=3, column=1, sticky="ew", padx=6, pady=4)

        ttk.Button(wifi, text="Connect WiFi", command=self._wifi_connect).grid(row=4, column=0, columnspan=2, pady=6)
        self.wifi_var = tk.StringVar(value="WiFi: ?")
        ttk.Label(wifi, textvariable=self.wifi_var).grid(row=5, column=0, columnspan=2, pady=(0, 4))

        stream = ttk.LabelFrame(self, text="Camera")
        stream.pack(fill="both", expand=True, padx=12, pady=8)

        btns = ttk.Frame(stream)
        btns.pack(pady=4)
        ttk.Button(btns, text="Stream On", command=lambda: self.ble.send("stream=on")).pack(side="left", padx=4)
        ttk.Button(btns, text="Stream Off", command=self._stream_off).pack(side="left", padx=4)

        self.video_label = ttk.Label(stream, text="(no stream)", anchor="center", background="#222", foreground="#aaa")
        self.video_label.pack(fill="both", expand=True, padx=6, pady=6)

        log_frame = ttk.LabelFrame(self, text="Log")
        log_frame.pack(fill="both", padx=12, pady=(0, 10))
        self.log_text = tk.Text(log_frame, height=6, state="disabled", wrap="word")
        self.log_text.pack(fill="both", expand=True)

    # --- actions ---

    def _wifi_connect(self):
        ssid = self.ssid_entry.get().strip()
        password = self.pass_entry.get()
        if not ssid:
            self._log("[GUI] Enter an SSID first")
            return
        self.ble.send(f"router_ssid={ssid};router_password={password};wifi=on")

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

    # --- event queue draining (runs on the Tk main thread) ---

    def _poll_events(self):
        try:
            while True:
                kind, payload = self.event_q.get_nowait()
                if kind == "status":
                    self.status_var.set(payload)
                    self._log(f"[STATUS] {payload}")
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
        self._log(f"[NOTIFY] {text}")
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

    def _start_stream_reader(self, url: str):
        self._stop_stream_reader()
        self.video_label.configure(text=f"Connecting to {url}...", image="")
        self.stream_reader = MJPEGReader(url, self.event_q)
        self.stream_reader.start()

    def _stop_stream_reader(self):
        if self.stream_reader:
            self.stream_reader.stop()
            self.stream_reader = None
        self.video_label.configure(image="", text="(no stream)")
        self.video_label.image = None

    def _show_frame(self, jpeg_bytes: bytes):
        try:
            img = Image.open(io.BytesIO(jpeg_bytes))
            img.thumbnail((380, 380))
            photo = ImageTk.PhotoImage(img)
            self.video_label.configure(image=photo, text="")
            self.video_label.image = photo  # keep a reference alive
        except Exception as e:
            self._log(f"[VIDEO] frame decode error: {e}")

    def _log(self, text: str):
        self.log_text.configure(state="normal")
        self.log_text.insert("end", text + "\n")
        self.log_text.see("end")
        self.log_text.configure(state="disabled")

    def _on_close(self):
        self._stop_stream_reader()
        self.ble.disconnect()
        self.destroy()


if __name__ == "__main__":
    MailboxApp().mainloop()
