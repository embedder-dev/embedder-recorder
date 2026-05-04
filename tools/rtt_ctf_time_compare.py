#!/usr/bin/env python3
"""
Compare host wall-clock elapsed time with compact trace event timestamps.

Live mode records RTT channel 2 for a fixed duration and compares the host
monotonic read window with the decoded timestamp span. File mode parses a
saved stream and optionally compares it with a supplied wall-clock duration.

Usage:
    python tools/rtt_ctf_time_compare.py --duration 5
    python tools/rtt_ctf_time_compare.py --input ctf_capture/stream --wall-seconds 5
"""

import argparse
import struct
import sys
import time

try:
    import pylink
except ImportError:
    pylink = None

RTT_DATA_CHANNEL = 2
TARGET_DEVICE = "nRF52832_xxAA"
SYNC_MAGIC = 0xE3BD0001
SYNC_EVENT_ID = 0x302


def connect_jlink(serial=None):
    if pylink is None:
        print("ERROR: pylink-square is required. "
              "Install with: pip install pylink-square")
        sys.exit(1)

    jlink = pylink.JLink()
    if serial:
        jlink.open(serial_no=int(serial))
    else:
        jlink.open()

    jlink.set_tif(pylink.enums.JLinkInterfaces.SWD)
    jlink.connect(TARGET_DEVICE)
    jlink.rtt_start()

    deadline = time.time() + 5
    while time.time() < deadline:
        try:
            jlink.rtt_read(0, 1)
            return jlink
        except pylink.errors.JLinkRTTerminalError:
            time.sleep(0.1)

    print("ERROR: RTT control block not found. Is firmware running?")
    sys.exit(1)


def read_live_stream(serial, duration_s, reset):
    jlink = connect_jlink(serial)
    try:
        if reset:
            jlink.reset(halt=False)
            time.sleep(0.5)

        while jlink.rtt_read(RTT_DATA_CHANNEL, 4096):
            pass

        data = bytearray()
        first_read = None
        last_read = None
        end_time = time.monotonic() + duration_s
        while time.monotonic() < end_time:
            chunk = jlink.rtt_read(RTT_DATA_CHANNEL, 4096)
            now = time.monotonic()
            if chunk:
                if first_read is None:
                    first_read = now
                last_read = now
                data.extend(chunk)
            else:
                time.sleep(0.005)

        return bytes(data), first_read, last_read
    finally:
        jlink.rtt_stop()
        jlink.close()


def decode_varint(raw, offset):
    """Decode a LEB128 varint. Returns (value, bytes_consumed)."""
    val = 0
    shift = 0
    consumed = 0
    while offset < len(raw):
        b = raw[offset]
        val |= (b & 0x7F) << shift
        offset += 1
        consumed += 1
        shift += 7
        if (b & 0x80) == 0:
            break
        if consumed >= 5:
            break
    return val, consumed


def parse_events(raw):
    """Parse compact trace events and extract timestamps."""
    # Find first sync
    sync_pos = -1
    for pos in range(len(raw) - 15):
        if raw[pos] == 15:
            eid = struct.unpack_from("<H", raw, pos + 1)[0]
            magic = struct.unpack_from("<I", raw, pos + 3)[0]
            if eid == SYNC_EVENT_ID and magic == SYNC_MAGIC:
                sync_pos = pos
                break

    if sync_pos < 0:
        return {
            "events": 0,
            "first_ts": None,
            "last_ts": None,
            "sync_count": 0,
            "resync_bytes": sync_pos if sync_pos > 0 else 0,
        }

    offset = sync_pos
    event_count = 0
    sync_count = 0
    abs_timestamp = 0
    first_ts = None
    last_ts = None

    while offset < len(raw):
        pkt_len = raw[offset]
        if pkt_len < 4 or offset + pkt_len > len(raw):
            break

        eid = struct.unpack_from("<H", raw, offset + 1)[0]

        if eid == SYNC_EVENT_ID and pkt_len == 15:
            magic = struct.unpack_from("<I", raw, offset + 3)[0]
            if magic == SYNC_MAGIC:
                abs_timestamp = struct.unpack_from("<I", raw, offset + 7)[0]
                sync_count += 1
        else:
            ts_delta, _ = decode_varint(raw, offset + 3)
            abs_timestamp += ts_delta

        if first_ts is None:
            first_ts = abs_timestamp
        last_ts = abs_timestamp

        event_count += 1
        offset += pkt_len

    return {
        "events": event_count,
        "first_ts": first_ts,
        "last_ts": last_ts,
        "sync_count": sync_count,
        "resync_bytes": sync_pos,
    }


def print_report(stats, host_elapsed_s, byte_count):
    trace_elapsed_s = None
    if stats["first_ts"] is not None and stats["last_ts"] is not None:
        trace_elapsed_s = (stats["last_ts"] - stats["first_ts"]) / 1_000_000_000

    print("Compact trace time comparison")
    print("=============================")
    print("Bytes:            %d" % byte_count)
    print("Events parsed:    %d" % stats["events"])
    print("Sync packets:     %d" % stats["sync_count"])
    print("Skipped to sync:  %d bytes" % stats["resync_bytes"])

    if trace_elapsed_s is None:
        print("Trace elapsed:    n/a")
        return

    print("First timestamp:  %d ns" % stats["first_ts"])
    print("Last timestamp:   %d ns" % stats["last_ts"])
    print("Trace elapsed:    %.6f s" % trace_elapsed_s)

    if host_elapsed_s is None:
        return

    diff_s = trace_elapsed_s - host_elapsed_s
    ratio = trace_elapsed_s / host_elapsed_s if host_elapsed_s > 0 else 0
    ppm = (ratio - 1) * 1_000_000
    print("Host elapsed:     %.6f s" % host_elapsed_s)
    print("Difference:       %.6f s" % diff_s)
    print("Ratio:            %.9f (%.1f ppm)" % (ratio, ppm))


def main():
    parser = argparse.ArgumentParser(
        description="Compare host wall-clock time with trace timestamps")
    parser.add_argument("--input", help="Saved stream file to parse")
    parser.add_argument("--wall-seconds", type=float,
                        help="Wall-clock elapsed seconds for --input mode")
    parser.add_argument("--duration", type=float, default=5.0,
                        help="Live capture duration in seconds")
    parser.add_argument("--serial", help="J-Link serial number")
    parser.add_argument("--no-reset", action="store_true",
                        help="Do not reset target before live capture")
    args = parser.parse_args()

    if args.input:
        with open(args.input, "rb") as f:
            raw = f.read()
        stats = parse_events(raw)
        print_report(stats, args.wall_seconds, len(raw))
        return

    raw, first_read, last_read = read_live_stream(
        args.serial, args.duration, reset=not args.no_reset)
    host_elapsed_s = None
    if first_read is not None and last_read is not None:
        host_elapsed_s = last_read - first_read
    stats = parse_events(raw)
    print_report(stats, host_elapsed_s, len(raw))


if __name__ == "__main__":
    main()
