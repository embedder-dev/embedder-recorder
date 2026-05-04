# Embedder Trace

A Zephyr module that hooks into the kernel's tracing infrastructure to emit
compact, timestamped events over SEGGER RTT or ARM ITM/SWO. All emission
paths are ISR-safe, non-blocking, and heap-free.

## Features

- **Kernel tracing** -- thread switches, ISR enter/exit, idle, semaphores,
  mutexes, timers (via `CONFIG_TRACING_USER`)
- **User events** -- application markers, counters, key/value metadata
- **Rich events** -- typed samples (i32/f32), state transitions, intervals,
  code sections, channel metadata
- **Memory tracking** -- heap alloc/free, mem-slab tracking, periodic stack
  high-water sampling
- **Transports** -- SEGGER RTT (J-Link) or ARM ITM/SWO
- **Late-attach** -- periodic sync packets and metadata re-emission allow the
  host to join mid-stream

## Repository Layout

```
modules/embedder-trace/   The Zephyr module (this is what you integrate)
examples/
  nrf52dk_trace_demo/     Multi-threaded demo for nRF52 DK
  nrf9160dk_gnss_demo/    GNSS acquisition demo for nRF9160 DK
tests/benchmarks/tracing/ Per-event overhead benchmark
tools/                    Python capture and analysis scripts
```

## Integrating Into Your Zephyr Project

### 1. Add the module

Copy or symlink `modules/embedder-trace` into your project tree (or reference
it from wherever you keep it), then register it in your application's
`CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20.0)

list(APPEND ZEPHYR_EXTRA_MODULES
  ${CMAKE_CURRENT_SOURCE_DIR}/path/to/embedder-trace
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(my_app)

target_sources(app PRIVATE src/main.c)
```

### 2. Enable in prj.conf

```ini
# Required -- tells Zephyr to use user-provided trace hooks
CONFIG_TRACING_USER=y

# Enable the module
CONFIG_EMBEDDER_TRACE=y

# Pick a transport
CONFIG_EMBEDDER_TRACE_TRANSPORT_RTT=y     # SEGGER RTT (default)
# CONFIG_EMBEDDER_TRACE_TRANSPORT_ITM=y   # ARM ITM/SWO (Cortex-M3/M4/M7/M33/M55)

# Optional feature tiers
CONFIG_EMBEDDER_TRACE_USER_EVENTS=y       # Markers, counters, metadata
CONFIG_EMBEDDER_TRACE_RICH_EVENTS=y       # Typed samples, state, intervals
# CONFIG_EMBEDDER_TRACE_MEMORY=y          # Heap/stack tracking
```

`CONFIG_TRACING_USER=y` must be set manually -- external modules cannot
auto-select Kconfig choice options.

### 3. RTT buffer tuning (optional)

```ini
CONFIG_EMBEDDER_TRACE_RTT_CHANNEL=2         # Default channel (0=console, 1=modem)
CONFIG_EMBEDDER_TRACE_RTT_BUFFER_SIZE=8192  # 8 KB default, increase for high-rate tracing
```

Buffer-full policy (default is drop):

```ini
CONFIG_EMBEDDER_TRACE_RTT_MODE_DROP=y   # Non-blocking, silently drops (recommended)
# CONFIG_EMBEDDER_TRACE_RTT_MODE_BLOCK=y  # Lossless but may stall
# CONFIG_EMBEDDER_TRACE_RTT_MODE_TRIM=y   # Non-blocking, partial writes
```

### 4. Build and flash

```bash
west build -b <your_board> .
west flash
```

Kernel tracing starts automatically at boot. No application code changes are
needed for basic thread/ISR/sync-primitive visibility.

## Application Instrumentation API

Include `<embedder/trace.h>` and call any of the following. All functions
compile to no-ops when their feature tier is disabled.

### User events (`CONFIG_EMBEDDER_TRACE_USER_EVENTS`)

```c
#include <embedder/trace.h>

embedder_trace_capture_start("my_session");
embedder_trace_event(0x01, arg0, arg1);
embedder_trace_counter(CHANNEL_TEMP, temperature);
embedder_trace_app_metadata("fw_version", "1.2.0");
embedder_trace_capture_stop();
```

### Rich events (`CONFIG_EMBEDDER_TRACE_RICH_EVENTS`)

```c
embedder_trace_channel_meta(CH_ACCEL, "accel_x", "m/s^2");
embedder_trace_sample_i32(CH_ACCEL, reading);
embedder_trace_sample_f32(CH_VOLTAGE, 3.28f);
embedder_trace_state(CH_RADIO, STATE_TX);
embedder_trace_interval_begin(CH_SPI);
/* ... SPI transfer ... */
embedder_trace_interval_end(CH_SPI);
embedder_trace_section_begin("dsp_filter");
/* ... processing ... */
embedder_trace_section_end();
```

### Memory tracking (`CONFIG_EMBEDDER_TRACE_MEMORY`)

```c
void *p = k_malloc(128);
embedder_trace_heap_alloc(p, 128);
/* ... */
embedder_trace_heap_free(p);

embedder_trace_stack_sample_all();  /* Sample all thread stack high-water marks */
```

## Kconfig Reference

| Symbol | Default | Description |
|--------|---------|-------------|
| `EMBEDDER_TRACE` | n | Enable the module |
| `EMBEDDER_TRACE_TRANSPORT_RTT` | y | SEGGER RTT transport |
| `EMBEDDER_TRACE_TRANSPORT_ITM` | n | ARM ITM/SWO transport |
| `EMBEDDER_TRACE_RTT_MODE_DROP` | y | Drop on buffer full |
| `EMBEDDER_TRACE_RTT_MODE_BLOCK` | n | Block on buffer full |
| `EMBEDDER_TRACE_RTT_MODE_TRIM` | n | Trim on buffer full |
| `EMBEDDER_TRACE_SCHEDULING_ONLY` | n | Only trace thread switches and ISRs |
| `EMBEDDER_TRACE_USER_EVENTS` | n | User event API |
| `EMBEDDER_TRACE_RICH_EVENTS` | n | Rich event API (selects USER_EVENTS) |
| `EMBEDDER_TRACE_MEMORY` | n | Memory/stack tracking |
| `EMBEDDER_TRACE_RTT_CHANNEL` | 2 | RTT channel number |
| `EMBEDDER_TRACE_RTT_BUFFER_SIZE` | 8192 | RTT buffer size (bytes) |
| `EMBEDDER_TRACE_ITM_PORT` | 1 | ITM stimulus port |
| `EMBEDDER_TRACE_SYNC_PERIOD_SHIFT` | 8 | Sync every 2^N events |
| `EMBEDDER_TRACE_METADATA_SYNC_DIVISOR` | 4 | Re-emit metadata every N syncs |
| `EMBEDDER_TRACE_HEALTH_INTERVAL_MS` | 5000 | Health check interval (0=disable) |
| `EMBEDDER_TRACE_STACK_SAMPLE_PERIOD_MS` | 1000 | Auto stack sampling interval (0=disable) |

## Tools

Python scripts in `tools/` for capture and analysis (requires `pylink-square`):

- **rtt_ctf_record.py** -- Live RTT capture to file
- **rtt_ctf_check.py** -- Stream validation and event histogram
- **rtt_ctf_drops.py** -- Drop detection and analysis
- **rtt_ctf_time_compare.py** -- Timestamp drift measurement
- **capture_nrf9160_25s.py** -- nRF9160-specific 25s capture

```bash
pip install pylink-square
python tools/rtt_ctf_record.py          # Capture trace data
python tools/rtt_ctf_check.py trace.bin # Validate captured stream
```

## License

Apache-2.0
