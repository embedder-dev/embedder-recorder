#!/usr/bin/env python3
"""
Record trace data from RTT channel 2 for a fixed duration.

The data stream contains both compact trace events and inline metadata
chunks (event 0x303). No separate metadata channel is used.

Captures:
  - RTT channel 2 (compact trace data + inline metadata)  -> <output_dir>/stream

Requires: pylink-square  (pip install pylink-square)

Usage:
    python tools/rtt_ctf_record.py [--serial <jlink_serial>] [--duration <seconds>] [--output <dir>]
"""

import argparse
import os
import sys
import time

try:
    import pylink
except ImportError:
    print("ERROR: pylink-square is required. Install with: pip install pylink-square")
    sys.exit(1)

RTT_DATA_CHANNEL = 2
TARGET_DEVICE = "nRF52832_xxAA"


def connect_jlink(serial=None):
    """Connect to J-Link and start RTT."""
    jlink = pylink.JLink()
    if serial:
        jlink.open(serial_no=int(serial))
    else:
        jlink.open()

    jlink.set_tif(pylink.enums.JLinkInterfaces.SWD)
    jlink.connect(TARGET_DEVICE)
    jlink.rtt_start()

    # Wait for RTT control block
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

    # Drain stale data
    while jlink.rtt_read(RTT_DATA_CHANNEL, 4096):
        pass

    return jlink


def main():
    parser = argparse.ArgumentParser(
        description="Record trace data from RTT channel (includes inline metadata)")
    parser.add_argument("--serial", help="J-Link serial number")
    parser.add_argument("--duration", type=float, default=20.0,
                        help="Recording duration in seconds (default: 20)")
    parser.add_argument("--output", default="ctf_capture",
                        help="Output directory (default: ctf_capture)")
    parser.add_argument("--reset", action="store_true", default=True,
                        help="Reset target before recording (default: true)")
    parser.add_argument("--no-reset", action="store_false", dest="reset",
                        help="Do not reset target before recording")
    args = parser.parse_args()

    os.makedirs(args.output, exist_ok=True)

    print("Connecting to J-Link (%s)..." % TARGET_DEVICE)
    jlink = connect_jlink(args.serial)
    print("Connected.")

    if args.reset:
        print("Resetting target...")
        jlink.reset(halt=False)
        time.sleep(0.5)

    # Record data stream (contains events + inline metadata chunks)
    data_path = os.path.join(args.output, "stream")
    print("Recording trace data from RTT channel %d for %.0fs..."
          % (RTT_DATA_CHANNEL, args.duration))

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
                print("  [%5.1fs / %.0fs] %d bytes captured"
                      % (elapsed, args.duration, total_bytes))
                last_print = now

    elapsed = time.time() - start
    print("\nRecording complete.")
    print("  Duration: %.1fs" % elapsed)
    print("  Data:     %d bytes -> %s" % (total_bytes, data_path))

    jlink.rtt_stop()
    jlink.close()


if __name__ == "__main__":
    main()
