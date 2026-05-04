#!/usr/bin/env python3
"""One-off 25s RTT trace capture targeting the nRF9160 DK GNSS demo.

Mirrors tools/rtt_ctf_record.py but with TARGET_DEVICE=nRF9160_xxAA so we can
record the nrf9160dk_gnss_demo build without editing the shared script.
"""

import argparse
import os
import sys
import time

import pylink

RTT_DATA_CHANNEL = 2
TARGET_DEVICE = "nRF9160_xxAA"


def connect_jlink(serial=None):
    jlink = pylink.JLink()
    if serial:
        jlink.open(serial_no=int(serial))
    else:
        jlink.open()

    jlink.set_tif(pylink.enums.JLinkInterfaces.SWD)
    jlink.connect(TARGET_DEVICE)
    jlink.rtt_start()

    timeout = time.time() + 5
    while time.time() < timeout:
        try:
            jlink.rtt_read(0, 1)
            break
        except pylink.errors.JLinkRTTerminalError:
            time.sleep(0.1)
    else:
        print("ERROR: RTT control block not found. Is firmware running?")
        sys.exit(1)

    while jlink.rtt_read(RTT_DATA_CHANNEL, 4096):
        pass

    return jlink


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--serial")
    parser.add_argument("--duration", type=float, default=25.0)
    parser.add_argument("--output", default="ctf_capture_cursor_25s")
    parser.add_argument("--reset", action="store_true", default=True)
    parser.add_argument("--no-reset", action="store_false", dest="reset")
    args = parser.parse_args()

    os.makedirs(args.output, exist_ok=True)

    print(f"Connecting to J-Link ({TARGET_DEVICE})...")
    jlink = connect_jlink(args.serial)
    print("Connected.")

    if args.reset:
        print("Resetting target...")
        jlink.reset(halt=False)
        time.sleep(0.5)

    data_path = os.path.join(args.output, "stream")
    print(f"Recording RTT channel {RTT_DATA_CHANNEL} for {args.duration:.0f}s...")

    total_bytes = 0
    start = time.time()
    end = start + args.duration
    last_print = start

    with open(data_path, "wb") as f:
        while time.time() < end:
            data = jlink.rtt_read(RTT_DATA_CHANNEL, 4096)
            if data:
                f.write(bytes(data))
                total_bytes += len(data)
            else:
                time.sleep(0.01)
            now = time.time()
            if now - last_print >= 1.0:
                elapsed = now - start
                print(f"  [{elapsed:5.1f}s / {args.duration:.0f}s] "
                      f"{total_bytes} bytes captured")
                last_print = now

    elapsed = time.time() - start
    print("\nRecording complete.")
    print(f"  Duration: {elapsed:.1f}s")
    print(f"  Data:     {total_bytes} bytes -> {data_path}")

    jlink.rtt_stop()
    jlink.close()


if __name__ == "__main__":
    main()
