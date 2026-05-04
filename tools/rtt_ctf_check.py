#!/usr/bin/env python3
"""
Read the trace data stream from RTT channel 2 and validate compact
stream format compliance. Format descriptor metadata is extracted from
inline metadata events (0x303) in the data stream.

Requires: pylink-square  (pip install pylink-square)

Usage:
    python tools/rtt_ctf_check.py [--serial <jlink_serial>] [--duration <seconds>]
"""

import argparse
import struct
import sys
import time

try:
    import pylink
except ImportError:
    pylink = None

# ---------------------------------------------------------------------------
# Constants matching firmware trace_internal.h / trace_metadata.c
# ---------------------------------------------------------------------------
RTT_DATA_CHANNEL = 2
TARGET_DEVICE = "nRF52832_xxAA"

SYNC_MAGIC = 0xE3BD0001
SYNC_EVENT_ID = 0x302
OVERFLOW_EVENT_ID = 0x301
METADATA_EVENT_ID = 0x303

# Known event IDs
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
    0x303: "metadata",
}


def connect_jlink(serial=None):
    """Connect to J-Link, open RTT, and reset to capture boot data."""
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

    print("Resetting target to capture boot data...")
    jlink.reset(halt=False)
    time.sleep(1.0)

    return jlink


def read_rtt_channel(jlink, channel, max_bytes=8192, timeout_s=2.0):
    """Read all available data from an RTT channel with timeout."""
    buf = bytearray()
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        data = jlink.rtt_read(channel, max_bytes)
        if data:
            buf.extend(data)
            deadline = time.time() + 0.5
        else:
            time.sleep(0.05)
    return bytes(buf)


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


# ---------------------------------------------------------------------------
# Metadata extraction from inline 0x303 events
# ---------------------------------------------------------------------------
def extract_inline_metadata(raw, offset=0):
    """
    Scan the data stream for a complete set of 0x303 metadata chunks.
    Returns (descriptor_text, chunks_found) or (None, 0).
    """
    chunks = {}
    expected_count = None
    pos = offset

    while pos < len(raw):
        if pos >= len(raw):
            break

        pkt_len = raw[pos]
        if pkt_len < 4 or pos + pkt_len > len(raw):
            pos += 1
            continue

        eid = struct.unpack_from("<H", raw, pos + 1)[0]

        if eid == METADATA_EVENT_ID:
            # [len][eid:2][chunk_idx:1][chunk_count:1][chunk_len:2][data]
            if pkt_len >= 7:
                chunk_idx = raw[pos + 3]
                chunk_count = raw[pos + 4]
                chunk_len = struct.unpack_from("<H", raw, pos + 5)[0]
                data_start = pos + 7
                data_end = data_start + chunk_len

                if data_end <= pos + pkt_len:
                    chunks[chunk_idx] = raw[data_start:data_end]
                    expected_count = chunk_count

        pos += pkt_len

    if expected_count is None or len(chunks) < expected_count:
        return None, len(chunks)

    # Reassemble in order
    descriptor = bytearray()
    for i in range(expected_count):
        if i not in chunks:
            return None, len(chunks)
        descriptor.extend(chunks[i])

    return descriptor.decode("ascii", errors="replace"), expected_count


# ---------------------------------------------------------------------------
# Metadata validation
# ---------------------------------------------------------------------------
def validate_metadata(desc_text):
    """Validate the reassembled format descriptor."""
    print("=" * 60)
    print("METADATA (inline 0x303 events)")
    print("=" * 60)

    if desc_text is None:
        print("  [FAIL] Could not reassemble metadata from inline chunks")
        return None

    print("  Descriptor length: %d bytes" % len(desc_text))

    checks = []
    checks.append(("Format header",
                    desc_text.startswith("embedder-trace-compact-v1"),
                    ""))
    checks.append(("Has varint spec",
                    "varint=leb128" in desc_text, ""))
    checks.append(("Has byte_order",
                    "byte_order=le" in desc_text, ""))

    event_lines = [l for l in desc_text.splitlines()
                   if l.strip() and l.strip().startswith("0x")]
    checks.append(("Events declared",
                    len(event_lines) > 0,
                    "%d events" % len(event_lines)))

    has_sync = any("0x302" in l and "sync" in l for l in event_lines)
    checks.append(("Sync event (0x302)", has_sync, ""))

    has_meta = any("0x303" in l and "metadata" in l for l in event_lines)
    checks.append(("Metadata event (0x303)", has_meta, ""))

    for name, ok, detail in checks:
        status = "OK" if ok else "FAIL"
        print("    [%4s] %-30s %s" % (status, name, detail))

    return desc_text


# ---------------------------------------------------------------------------
# Data stream validation
# ---------------------------------------------------------------------------
def validate_data_stream(raw):
    """Parse and validate compact data stream from RTT channel 2."""
    print("\n" + "=" * 60)
    print("DATA STREAM (RTT channel %d)" % RTT_DATA_CHANNEL)
    print("=" * 60)

    if len(raw) == 0:
        print("  [WARN] No data received on data channel")
        return

    print("  Raw bytes received: %d" % len(raw))

    # Find first sync packet
    sync_pos = -1
    for pos in range(len(raw) - 15):
        if raw[pos] == 15:  # length byte
            eid = struct.unpack_from("<H", raw, pos + 1)[0]
            magic = struct.unpack_from("<I", raw, pos + 3)[0]
            if eid == SYNC_EVENT_ID and magic == SYNC_MAGIC:
                sync_pos = pos
                break

    if sync_pos < 0:
        print("  [FAIL] No sync packet found in data stream")
        return

    if sync_pos > 0:
        print("  [INFO] Skipped %d bytes to first sync" % sync_pos)

    # Parse events
    offset = sync_pos
    event_count = 0
    sync_count = 0
    metadata_chunk_count = 0
    error_count = 0
    event_histogram = {}
    abs_timestamp = 0

    while offset < len(raw):
        if offset >= len(raw):
            break

        pkt_len = raw[offset]
        if pkt_len < 4 or offset + pkt_len > len(raw):
            error_count += 1
            break

        eid = struct.unpack_from("<H", raw, offset + 1)[0]

        if eid == SYNC_EVENT_ID and pkt_len == 15:
            magic = struct.unpack_from("<I", raw, offset + 3)[0]
            if magic == SYNC_MAGIC:
                abs_timestamp = struct.unpack_from("<I", raw, offset + 7)[0]
                sync_count += 1
        elif eid == METADATA_EVENT_ID:
            metadata_chunk_count += 1
        else:
            ts_delta, _ = decode_varint(raw, offset + 3)
            abs_timestamp += ts_delta

        event_name = KNOWN_EVENTS.get(eid, "UNKNOWN(0x%04X)" % eid)
        event_histogram[event_name] = event_histogram.get(event_name, 0) + 1

        if eid not in KNOWN_EVENTS:
            error_count += 1

        event_count += 1
        offset += pkt_len

    # Summary
    print("\n  Events parsed:       %d" % event_count)
    print("  Sync packets:        %d" % sync_count)
    print("  Metadata chunks:     %d" % metadata_chunk_count)
    print("  Parse errors:        %d" % error_count)
    print("  Bytes remaining:     %d" % (len(raw) - offset))

    if event_histogram:
        print("\n  Event histogram:")
        for name in sorted(event_histogram.keys()):
            count = event_histogram[name]
            print("    %-35s %d" % (name, count))

    if error_count == 0 and event_count > 0:
        print("\n  [OK] Data stream is compact format compliant")
    elif event_count > 0:
        print("\n  [WARN] Data stream has %d issue(s)" % error_count)
    else:
        print("\n  [WARN] No valid events found")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(
        description="Read RTT data and validate compact format with "
                    "inline metadata")
    parser.add_argument("--serial", help="J-Link serial number")
    parser.add_argument("--duration", type=float, default=3.0,
                        help="Seconds to capture data stream (default: 3)")
    parser.add_argument("--dump-descriptor", action="store_true",
                        help="Print the full format descriptor text")
    args = parser.parse_args()

    print("Connecting to J-Link (%s)..." % TARGET_DEVICE)
    jlink = connect_jlink(args.serial)
    print("Connected. Reading RTT channel %d...\n" % RTT_DATA_CHANNEL)

    # Read data channel (contains both events and inline metadata)
    data_raw = read_rtt_channel(jlink, RTT_DATA_CHANNEL,
                                timeout_s=args.duration)

    # Extract and validate inline metadata
    descriptor, chunk_count = extract_inline_metadata(data_raw)
    print("  Metadata chunks found: %d" % chunk_count)
    validate_metadata(descriptor)

    if args.dump_descriptor and descriptor:
        print("\n  --- Descriptor dump ---")
        for line in descriptor.splitlines():
            print("  " + line)
        print("  --- end descriptor ---")

    # Validate data stream
    validate_data_stream(data_raw)

    # Overall verdict
    print("\n" + "=" * 60)
    meta_ok = descriptor is not None
    print("VERDICT:")
    print("  Metadata: %s" % ("PASS" if meta_ok else "FAIL"))
    print("  Data:     see above")
    print("=" * 60)

    jlink.rtt_stop()
    jlink.close()


if __name__ == "__main__":
    main()
