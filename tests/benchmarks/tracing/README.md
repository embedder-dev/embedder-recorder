# Embedder Trace Overhead Benchmarks

Measures the per-event overhead of each tracing API in the Embedder
tracing module.

## Build & Run

```bash
west build -b nrf9160dk/nrf9160/ns tests/benchmarks/tracing
west flash
```

Results are printed to the console (UART or RTT channel 0).

## Metrics

| Metric | Description |
|--------|-------------|
| cycles/event | CPU cycles consumed per trace event emission |
| bytes/event | Payload bytes written to the transport per event |

## Configurations to Test

1. **Scheduling-only** — `CONFIG_EMBEDDER_TRACE_SCHEDULING_ONLY=y`
2. **Full kernel events** — default CTF with all kernel hooks
3. **Rich user events** — `CONFIG_EMBEDDER_TRACE_RICH_EVENTS=y`
4. **Memory/stack** — `CONFIG_EMBEDDER_TRACE_MEMORY=y`
5. **RTT transport** — `CONFIG_EMBEDDER_TRACE_TRANSPORT_RTT=y`
6. **ITM transport** — `CONFIG_EMBEDDER_TRACE_TRANSPORT_ITM=y`

## Flash & RAM Footprint

Compare `.map` file sizes with tracing enabled vs disabled:

```bash
# Build with tracing
west build -b nrf9160dk/nrf9160/ns -p always -- -DCONFIG_EMBEDDER_TRACE=y
# Note: flash and RAM from build/zephyr/zephyr.map

# Build without tracing
west build -b nrf9160dk/nrf9160/ns -p always -- -DCONFIG_EMBEDDER_TRACE=n
# Compare flash and RAM delta
```

## Results Template

```
=== Embedder Trace Overhead Benchmark ===
Board: nrf9160dk/nrf9160/ns
NCS version: <version>
Date: <date>

--- User Events (B3) ---
capture_start                   XXXX cycles/event
capture_stop                    XXXX cycles/event
event(id, arg0, arg1)           XXXX cycles/event
counter(ch, val)                XXXX cycles/event
app_metadata(k, v)              XXXX cycles/event

--- Rich Events (B4) ---
sample_i32(ch, val)             XXXX cycles/event
sample_f32(ch, val)             XXXX cycles/event
state(ch, st)                   XXXX cycles/event
interval_begin(ch)              XXXX cycles/event
interval_end(ch)                XXXX cycles/event
section_begin(name)             XXXX cycles/event
section_end()                   XXXX cycles/event
channel_meta(ch, name, unit)    XXXX cycles/event
string(ch, str)                 XXXX cycles/event

--- Memory Events (B5) ---
heap_alloc(ptr, sz)             XXXX cycles/event
heap_free(ptr)                  XXXX cycles/event
stack_sample(thread)            XXXX cycles/event

Flash delta (tracing on vs off): +XX KB
RAM delta (tracing on vs off):   +XX KB
```
