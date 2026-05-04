#!/usr/bin/env python3
"""
Analyze a compact trace stream for dropped packets.

Reads a captured stream file and detects drops via:
  1. Overflow events (ID 0x301) reporting firmware-side drops
  2. Timestamp discontinuities (large gaps)
  3. Sync-resync gaps (corrupted bytes between valid packets)
  4. trace_health events (ID 0x300) reporting periodic drop counts

Supports the compact wire format:
  Event:  [u8 length][u16 event_id][varint ts_delta][payload...]
  Sync:   [u8 length=15][u16 event_id=0x302][u32 magic][u32 abs_ts][u32 freq]

Can also connect live to RTT and analyze in real time.

Usage:
    python tools/rtt_ctf_drops.py ctf_capture/stream
    python tools/rtt_ctf_drops.py --live [--serial <jlink_serial>] [--duration 10]
"""

import argparse
import struct
import sys
import time

SYNC_MAGIC = 0xE3BD0001
SYNC_EVENT_ID = 0x302
OVERFLOW_EVENT_ID = 0x301
HEALTH_EVENT_ID = 0x300

KNOWN_EVENTS = {
    0x10: "thread_switched_out",
    0x11: "thread_switched_in",
    0x12: "thread_priority_set",
    0x13: "thread_create",
    0x14: "thread_abort",
    0x15: "thread_suspend",
    0x16: "thread_resume",
    0x17: "thread_ready",
    0x18: "thread_pending",
    0x19: "thread_info",
    0x1A: "thread_name_set",
    0x1B: "isr_enter",
    0x1C: "isr_exit",
    0x1E: "idle",
    0x21: "semaphore_init",
    0x22: "semaphore_give_enter",
    0x23: "semaphore_give_exit",
    0x24: "semaphore_take_enter",
    0x25: "semaphore_take_blocking",
    0x26: "semaphore_take_exit",
    0x27: "semaphore_reset",
    0x28: "mutex_init",
    0x29: "mutex_lock_enter",
    0x2A: "mutex_lock_blocking",
    0x2B: "mutex_lock_exit",
    0x2C: "mutex_unlock_enter",
    0x2D: "mutex_unlock_exit",
    0x2E: "timer_init",
    0x2F: "timer_start",
    0x30: "timer_stop",
    0x31: "timer_status_sync_enter",
    0x32: "timer_status_sync_blocking",
    0x33: "timer_status_sync_exit",
    0x100: "capture_start",
    0x101: "capture_stop",
    0x102: "user_event",
    0x103: "counter",
    0x104: "app_metadata",
    0x110: "trace_string",
    0x111: "sample_i32",
    0x112: "sample_f32",
    0x113: "state",
    0x114: "interval_begin",
    0x115: "interval_end",
    0x116: "section_begin",
    0x117: "section_end",
    0x118: "channel_meta",
    0x119: "section_begin_pc",
    0x200: "heap_alloc",
    0x201: "heap_free",
    0x202: "mem_slab_alloc",
    0x203: "mem_slab_free",
    0x204: "stack_sample",
    0x205: "stack_meta",
    0x300: "trace_health",
    0x301: "overflow",
    0x302: "sync",
}


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


def find_sync(raw, start=0):
    """Scan for the next sync packet in the stream.

    Sync packets have: [u8 len=15][u16 eid=0x302][u32 magic=0xE3BD0001]...
    We scan for the 4-byte magic at offset +3 from any byte position.
    """
    end = len(raw) - 15  # minimum sync packet size
    pos = start
    while pos <= end:
        # Quick scan for magic bytes (little-endian 0xE3BD0001)
        if (raw[pos + 3] == 0x01 and raw[pos + 4] == 0x00
                and raw[pos + 5] == 0xBD and raw[pos + 6] == 0xE3):
            # Verify length and event_id
            pkt_len = raw[pos]
            eid = struct.unpack_from("<H", raw, pos + 1)[0]
            if pkt_len == 15 and eid == SYNC_EVENT_ID:
                return pos
        pos += 1
    return -1


def parse_sync_packet(raw, offset):
    """Parse a sync packet at offset. Returns (abs_ts, cpu_freq) or None."""
    if offset + 15 > len(raw):
        return None
    pkt_len = raw[offset]
    eid = struct.unpack_from("<H", raw, offset + 1)[0]
    magic = struct.unpack_from("<I", raw, offset + 3)[0]
    if pkt_len != 15 or eid != SYNC_EVENT_ID or magic != SYNC_MAGIC:
        return None
    abs_ts = struct.unpack_from("<I", raw, offset + 7)[0]
    cpu_freq = struct.unpack_from("<I", raw, offset + 11)[0]
    return abs_ts, cpu_freq


def analyze_stream(raw, verbose=False):
    """Analyze a compact trace stream for drops and anomalies."""
    # Find first sync to establish entry point
    sync_pos = find_sync(raw)
    if sync_pos < 0:
        return {
            "total_bytes": len(raw),
            "synced": False,
            "skipped_to_sync": 0,
            "event_count": 0,
            "sync_count": 0,
            "overflow_events": [],
            "health_reports": [],
            "timestamp_gaps": [],
            "resync_points": [],
            "unknown_event_count": 0,
        }

    skipped = sync_pos
    if skipped > 0 and verbose:
        print("  [SYNC] Skipped %d bytes to first sync at offset %d"
              % (skipped, sync_pos))

    offset = sync_pos
    event_count = 0
    sync_count = 0
    unknown_event_count = 0
    abs_timestamp = 0
    cpu_freq = 0
    prev_abs_ts = None

    overflow_events = []
    health_reports = []
    timestamp_gaps = []
    resync_points = []

    while offset < len(raw):
        # Need at least 1 byte for length
        if offset >= len(raw):
            break

        pkt_len = raw[offset]

        # Sanity check length
        if pkt_len < 4 or offset + pkt_len > len(raw):
            # Try to find next sync
            next_sync = find_sync(raw, offset + 1)
            if next_sync < 0:
                break
            resync_bytes = next_sync - offset
            resync_points.append((event_count, offset, resync_bytes))
            if verbose:
                print("  [RESYNC] offset %d: skipped %d bytes to sync"
                      % (offset, resync_bytes))
            offset = next_sync
            continue

        # Parse event_id
        eid = struct.unpack_from("<H", raw, offset + 1)[0]

        if eid == SYNC_EVENT_ID:
            # Sync packet
            result = parse_sync_packet(raw, offset)
            if result:
                abs_timestamp, cpu_freq = result
                if prev_abs_ts is not None:
                    gap = abs_timestamp - prev_abs_ts
                    if gap > 1_000_000_000:  # > 1 second gap
                        gap_ms = gap / 1_000_000.0
                        timestamp_gaps.append((
                            event_count, offset, prev_abs_ts,
                            abs_timestamp, gap
                        ))
                        if verbose:
                            print("  [GAP] event %d @ offset %d: "
                                  "%.1f ms gap between syncs"
                                  % (event_count, offset, gap_ms))
                prev_abs_ts = abs_timestamp
                sync_count += 1
                event_count += 1
                offset += pkt_len
                continue
            # Failed to parse as sync, fall through

        # Regular event or overflow
        varint_offset = offset + 3  # after length + event_id
        ts_delta, varint_len = decode_varint(raw, varint_offset)
        abs_timestamp += ts_delta

        if eid == OVERFLOW_EVENT_ID:
            # Overflow event: payload is u32 drop_count
            payload_offset = varint_offset + varint_len
            if payload_offset + 4 <= offset + pkt_len:
                drop_count = struct.unpack_from(
                    "<I", raw, payload_offset)[0]
                overflow_events.append((
                    event_count, offset, abs_timestamp, drop_count
                ))
                if verbose:
                    print("  [OVERFLOW] event %d @ offset %d: "
                          "%d events dropped"
                          % (event_count, offset, drop_count))

        elif eid == HEALTH_EVENT_ID:
            payload_offset = varint_offset + varint_len
            if payload_offset + 8 <= offset + pkt_len:
                total_dropped = struct.unpack_from(
                    "<I", raw, payload_offset)[0]
                new_dropped = struct.unpack_from(
                    "<I", raw, payload_offset + 4)[0]
                health_reports.append((
                    event_count, offset, abs_timestamp,
                    total_dropped, new_dropped
                ))
                if verbose:
                    print("  [HEALTH] event %d @ offset %d: "
                          "total_dropped=%d new_dropped=%d"
                          % (event_count, offset,
                             total_dropped, new_dropped))

        if eid not in KNOWN_EVENTS:
            unknown_event_count += 1

        event_count += 1
        offset += pkt_len

    return {
        "total_bytes": len(raw),
        "synced": True,
        "skipped_to_sync": skipped,
        "parsed_bytes": offset,
        "unparsed_bytes": len(raw) - offset,
        "event_count": event_count,
        "sync_count": sync_count,
        "overflow_events": overflow_events,
        "health_reports": health_reports,
        "timestamp_gaps": timestamp_gaps,
        "resync_points": resync_points,
        "resync_count": len(resync_points),
        "resync_bytes": sum(r[2] for r in resync_points),
        "unknown_event_count": unknown_event_count,
    }


def print_report(results):
    """Print a human-readable drop analysis report."""
    print("=" * 60)
    print("COMPACT TRACE DROP ANALYSIS REPORT")
    print("=" * 60)

    if not results["synced"]:
        print("\n  [FAIL] No sync packet found in stream")
        print("  Total bytes: %d" % results["total_bytes"])
        return

    print("\nStream overview:")
    print("  Total bytes:           %d" % results["total_bytes"])
    print("  Skipped to first sync: %d" % results["skipped_to_sync"])
    print("  Parsed bytes:          %d" % results.get("parsed_bytes", 0))
    print("  Unparsed tail bytes:   %d" % results.get("unparsed_bytes", 0))
    print("  Events parsed:         %d" % results["event_count"])
    print("  Sync packets:          %d" % results["sync_count"])

    # --- Overflow events ---
    overflow_events = results["overflow_events"]
    print("\n--- Overflow events (0x301) ---")
    if overflow_events:
        total_dropped = sum(o[3] for o in overflow_events)
        print("  Overflow events found:    %d" % len(overflow_events))
        print("  Total events dropped:     %d" % total_dropped)
        print()
        print("  %-8s  %-10s  %-15s  %s"
              % ("Event#", "Offset", "Timestamp", "Dropped"))
        print("  " + "-" * 50)
        for evt, off, ts, drops in overflow_events:
            print("  %-8d  %-10d  %-15d  %d" % (evt, off, ts, drops))
    else:
        print("  No overflow events in stream")

    # --- Resync (corruption / partial packets) ---
    print("\n--- Resync events (corrupted / missing bytes) ---")
    if results["resync_points"]:
        print("  Resync events:      %d" % results["resync_count"])
        print("  Total bytes lost:   %d" % results["resync_bytes"])
        print()
        print("  %-8s  %-10s  %s"
              % ("Event#", "Offset", "Bytes skipped"))
        print("  " + "-" * 36)
        for evt, off, skipped in results["resync_points"]:
            print("  %-8d  %-10d  %d" % (evt, off, skipped))
    else:
        print("  No resync events (clean stream)")

    # --- Timestamp gaps ---
    print("\n--- Timestamp gaps (> 1 second) ---")
    if results["timestamp_gaps"]:
        print("  Large gaps detected:  %d" % len(results["timestamp_gaps"]))
        print()
        print("  %-8s  %-10s  %-12s  %-12s  %s"
              % ("Event#", "Offset", "From (ns)", "To (ns)", "Gap (ms)"))
        print("  " + "-" * 60)
        for evt, off, prev_ts, cur_ts, gap in results["timestamp_gaps"]:
            gap_ms = gap / 1_000_000.0
            print("  %-8d  %-10d  %-12d  %-12d  %.1f"
                  % (evt, off, prev_ts, cur_ts, gap_ms))
    else:
        print("  No large timestamp gaps")

    # --- Health events ---
    print("\n--- Firmware health events (trace_health 0x300) ---")
    if results["health_reports"]:
        print("  Health events found:  %d" % len(results["health_reports"]))
        print()
        print("  %-8s  %-12s  %-15s  %s"
              % ("Event#", "Timestamp", "Total dropped", "New dropped"))
        print("  " + "-" * 52)
        for evt, off, ts, total, new in results["health_reports"]:
            print("  %-8d  %-12d  %-15d  %d" % (evt, ts, total, new))
    else:
        print("  No health events in stream")

    # --- Verdict ---
    has_drops = (
        len(overflow_events) > 0
        or results["resync_count"] > 0
        or any(r[4] > 0 for r in results["health_reports"])
    )

    print("\n" + "=" * 60)
    if has_drops:
        total_fw_drops = sum(o[3] for o in overflow_events)
        health_max = max((r[3] for r in results["health_reports"]),
                         default=0)
        if health_max > total_fw_drops:
            total_fw_drops = health_max

        print("VERDICT: DROPS DETECTED")
        print("  Firmware-reported drops:  %d" % total_fw_drops)
        print("  Transport resyncs:        %d (%d bytes lost)"
              % (results["resync_count"], results["resync_bytes"]))
    else:
        print("VERDICT: NO DROPS")
        print("  %d events parsed cleanly across %d syncs"
              % (results["event_count"], results["sync_count"]))
    print("=" * 60)


def analyze_live(serial=None, duration=10.0, verbose=False):
    """Connect via RTT and analyze the stream live."""
    try:
        import pylink
    except ImportError:
        print("ERROR: pylink-square required for --live mode. "
              "Install with: pip install pylink-square")
        sys.exit(1)

    target = "nRF52832_xxAA"
    rtt_channel = 2

    jlink = pylink.JLink()
    if serial:
        jlink.open(serial_no=int(serial))
    else:
        jlink.open()

    jlink.set_tif(pylink.enums.JLinkInterfaces.SWD)
    jlink.connect(target)
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
        print("ERROR: RTT control block not found")
        sys.exit(1)

    # Drain stale data
    while jlink.rtt_read(rtt_channel, 4096):
        pass

    print("Recording RTT channel %d for %.0fs..." % (rtt_channel, duration))
    buf = bytearray()
    start = time.time()
    end = start + duration
    last_print = start

    while time.time() < end:
        data = jlink.rtt_read(rtt_channel, 4096)
        if data:
            buf.extend(data)
        else:
            time.sleep(0.01)

        now = time.time()
        if now - last_print >= 2.0:
            elapsed = now - start
            print("  [%.0fs] %d bytes" % (elapsed, len(buf)))
            last_print = now

    print("Captured %d bytes in %.1fs\n" % (len(buf), time.time() - start))

    jlink.rtt_stop()
    jlink.close()

    results = analyze_stream(bytes(buf), verbose=verbose)
    print_report(results)


def main():
    parser = argparse.ArgumentParser(
        description="Analyze compact trace stream for dropped packets")
    parser.add_argument("file", nargs="?",
                        help="Path to stream file (e.g. ctf_capture/stream)")
    parser.add_argument("--live", action="store_true",
                        help="Connect to RTT and analyze live")
    parser.add_argument("--serial", help="J-Link serial number (for --live)")
    parser.add_argument("--duration", type=float, default=10.0,
                        help="Live capture duration in seconds (default: 10)")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="Print per-event drop/gap details during parse")
    args = parser.parse_args()

    if args.live:
        analyze_live(serial=args.serial, duration=args.duration,
                     verbose=args.verbose)
    elif args.file:
        with open(args.file, "rb") as f:
            raw = f.read()
        print("Loaded %d bytes from %s\n" % (len(raw), args.file))
        results = analyze_stream(raw, verbose=args.verbose)
        print_report(results)
    else:
        parser.print_help()
        sys.exit(1)


if __name__ == "__main__":
    main()
