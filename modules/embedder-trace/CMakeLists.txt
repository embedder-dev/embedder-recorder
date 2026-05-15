# SPDX-License-Identifier: Apache-2.0

if(CONFIG_EMBEDDER_TRACE)

zephyr_include_directories(include)

# Select the correct tracing_user.h for the Zephyr version.
# Zephyr 4.x has a circular dependency (kernel.h -> heap_constants.h)
# when tracing_user.h is force-included, so we use a lightweight
# version with forward declarations instead of #include <zephyr/kernel.h>.
if(KERNEL_VERSION_MAJOR GREATER_EQUAL 4)
  set(TRACING_USER_H ${CMAKE_CURRENT_SOURCE_DIR}/include/embedder/tracing_user_v4.h)
else()
  set(TRACING_USER_H ${CMAKE_CURRENT_SOURCE_DIR}/include/embedder/tracing_user_v3.h)
endif()

# Force-include the selected header before any Zephyr code compiles.
# Uses the same include guard (_TRACE_USER_H) as Zephyr's stock
# tracing_user.h, preventing the stock header from being loaded.
zephyr_compile_options(
  $<$<COMPILE_LANGUAGE:C>:-include ${TRACING_USER_H}>
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
