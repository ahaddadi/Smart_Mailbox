"""
Acts as a BLE central (like the phone) for Smart_Mailbox: scans for the
board, connects, subscribes to notifications, and lets you type commands
(led=on, relay=on, router_ssid=...;router_password=...;wifi=on, stream=on)
that get written to the same characteristic the phone would use.

Install:  pip install bleak
Run:      python ble_console.py
"""

import asyncio
import sys

from bleak import BleakClient, BleakScanner

DEVICE_NAME = "Smart_Mailbox"
MESSAGE_UUID = "4ac8a682-9736-4e5d-932b-e9b31405049c"


def notification_handler(_sender, data: bytearray):
    print(f"\n[NOTIFY] {data.decode(errors='replace')}\n> ", end="", flush=True)


async def find_device():
    print(f"Scanning for '{DEVICE_NAME}'...")
    device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=10.0)
    if device is None:
        print("Device not found. Is the board powered on and advertising?")
        sys.exit(1)
    return device


async def main():
    device = await find_device()
    print(f"Found {device.name} ({device.address})")

    async with BleakClient(device) as client:
        print("Connected.")
        await client.start_notify(MESSAGE_UUID, notification_handler)
        print("Subscribed to notifications.")
        print("Type commands (e.g. led=on, wifi=status, stream=on) and press Enter.")
        print("Type 'quit' to exit.\n")

        loop = asyncio.get_event_loop()
        while True:
            cmd = await loop.run_in_executor(None, input, "> ")
            cmd = cmd.strip()
            if cmd.lower() in ("quit", "exit"):
                break
            if not cmd:
                continue
            await client.write_gatt_char(MESSAGE_UUID, cmd.encode(), response=True)

        await client.stop_notify(MESSAGE_UUID)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
