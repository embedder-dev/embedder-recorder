# SPDX-License-Identifier: Apache-2.0

if(CONFIG_EMBEDDER_TRACE)

zephyr_include_directories(include)

# Force-include our tracing_user.h before any Zephyr code compiles.
# Uses the same include guard (_TRACE_USER_H) as Zephyr's stock
# tracing_user.h, preventing the stock header from being loaded.
zephyr_compile_options(
  $<$<COMPILE_LANGUAGE:C>:-include ${CMAKE_CURRENT_SOURCE_DIR}/include/embedder/tracing_user.h>
)

zephyr_library_named(embedder_trace)

zephyr_library_sources(
  src/trace_init.c
  src/trace_kernel.c
  src/trace_metadata.c
)

zephyr_library_sources_ifdef(CONFIG_EMBEDDER_TRACE_TRANSPORT_RTT
  src/sink_rtt.c
)

zephyr_library_sources_ifdef(CONFIG_EMBEDDER_TRACE_TRANSPORT_ITM
  src/sink_itm.c
)

zephyr_library_sources_ifdef(CONFIG_EMBEDDER_TRACE_USER_EVENTS
  src/trace_user.c
)

zephyr_library_sources_ifdef(CONFIG_EMBEDDER_TRACE_RICH_EVENTS
  src/trace_rich.c
)

zephyr_library_sources_ifdef(CONFIG_EMBEDDER_TRACE_MEMORY
  src/trace_mem.c
)

endif()
