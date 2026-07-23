# CLAUDE.md

## Project

smolmux - C11 portable device multiplexer. Holds a device connection open (serial UART, GDB stub) and multiplexes access to multiple clients over Unix sockets using newline-delimited JSON. Includes expect engine, anomaly detection, output history, and structured logging. Standalone MCP servers (`smolmux-mcp` serial, `smolmux-gdb-mcp` GDB/SWD with unknown-board probing) expose device access to AI agents. Deps: cJSON (vendored). Optional: PCRE2 (regex). Network sinks (TCP, WebSocket) use raw POSIX sockets.

Design patterns from smolclaw.

Full architecture: see `DESIGN.md`.

## Build & Test

```bash
cmake -B build && cmake --build build -j$(nproc)   # Build
ctest --test-dir build                               # All tests
./build/test_protocol                                # Single test
```

Feature selection: `cmake --build build --target menuconfig` (interactive), `cp configs/defconfig.uart .config` (profile), or `-DSM_ENABLE_UART=OFF` (CLI override).

## Compiler Standards

- C11 strict: `-Wall -Wextra -Wpedantic -Wno-unused-parameter`, `CMAKE_C_EXTENSIONS OFF`
- POSIX APIs via `_POSIX_C_SOURCE=200809L` (plus `_GNU_SOURCE`, `_DEFAULT_SOURCE`, `_XOPEN_SOURCE=600` - see `CMakeLists.txt`)
- Zero warnings policy

## Architecture

**Entry point:** `src/main.c` - arg parsing, link/sink wiring, broker startup.

**Core loop:** `src/broker.c` - epoll event loop -> read from link -> feed expect engine -> feed anomaly detector -> record history/logs -> broadcast to clients -> handle client messages.

**Key locations:**

| Component         | Location              | Purpose                                             |
| ----------------- | --------------------- | --------------------------------------------------- |
| Broker            | `src/broker.c`        | Event loop, client management, message dispatch     |
| Client            | `src/client.c`        | Single Unix socket client handler                   |
| Protocol          | `src/protocol.c`      | JSON message encode/decode, base64 helpers          |
| Expect            | `src/expect.c`        | Concurrent regex matching on byte stream            |
| Ring buffer       | `src/ring_buffer.c`   | Timestamped output history                          |
| Anomaly           | `src/anomaly.c`       | Crash/error pattern detection                       |
| Boot tracker      | `src/boot_stage.c`    | Ordered boot markers -> progress / furthest / stall |
| Autoresponder     | `src/autoresponder.c` | Standing expect->send rules fired in the read path  |
| I/O log           | `src/io_log.c`        | JSONL structured log                                |
| Text log          | `src/text_log.c`      | Human-readable device log with rotation             |
| Constants         | `src/constants.h`     | All defaults, limits, sizes                         |
| UART link         | `src/links/uart.c`    | Serial port via termios                             |
| GDB link          | `src/links/gdb.c`     | GDB/MI subprocess link (dumb byte pipe)             |
| Serial MCP server | `src/mcp_client.c`    | Standalone `smolmux-mcp` (serial tools over stdio)  |
| GDB MCP server    | `src/gdb_mcp.c`       | Standalone `smolmux-gdb-mcp`: tools + board-probing |
| GDB profile       | `src/gdb_profile.c`   | `*.gdb-profile.json` loader/serializer              |
| MI parse          | `src/util/mi_parse.c` | Client-side GDB/MI record parser                    |
| Utilities         | `src/util/`           | `sm_strbuf_t`, base64, JSON helpers                 |

## Coding Patterns

**Naming:** `sm_` prefix for functions/types, `SM_` for macros. All structs need tag names for forward declaration: `typedef struct sm_foo { ... } sm_foo_t;`

**Vtable polymorphism:** Links use C function pointer structs (`src/links/`):
```c
typedef struct sm_link {
    const char *name;
    int  (*open)(struct sm_link *self);
    void (*close)(struct sm_link *self);
    int  (*read_fd)(struct sm_link *self);
    int  (*write_fd)(struct sm_link *self);
    int  (*write_data)(struct sm_link *self, const uint8_t *data, size_t len);
    int  (*has_write_pending)(struct sm_link *self);
    int  (*flush_write_queue)(struct sm_link *self);
    int  (*send_break)(struct sm_link *self, int duration_ms);
    int  (*set_param)(struct sm_link *self, const char *key, const char *value);
    int  (*get_status)(struct sm_link *self, cJSON *out);
    void (*destroy)(struct sm_link *self);
    int  (*connect_begin)(struct sm_link *self);  /* optional async reconnect */
    int  (*connect_poll)(struct sm_link *self);
    size_t (*filter_rx)(struct sm_link *self, const uint8_t *in, size_t in_len,
                        uint8_t *out);             /* optional RX filter */
    int silence_normal;   /* 1 = skip idle link_health checks (e.g. GDB) */
    void *data;
} sm_link_t;
```

**Adding a new link:** (1) Create `src/links/mylink.c/.h` with `sm_link_t *sm_mylink_new(config)` factory. (2) Implement vtable methods. (3) Register in `src/main.c` wiring. (4) Add to `CMakeLists.txt` `SM_SOURCES`. (5) If gated by feature flag, guard in `main.c` with `#if SM_ENABLE_X`.

**Standalone MCP servers** (`smolmux-mcp`, `smolmux-gdb-mcp`) are separate binaries that connect to a broker as ordinary clients over the wire protocol - the broker/link stay dumb byte pipes, all protocol/MI intelligence is client-side. `smolmux-gdb-mcp` (`src/gdb_mcp.c`, built when `SM_ENABLE_GDB`) token-tags each GDB/MI command and matches its result out of the raw output stream (`src/util/mi_parse.c`). **Adding a gdb tool** (e.g. more board-probing): write `tool_foo()`, register it in **both** `dispatch_tool()` and `build_gdb_tools_list()`, and cover it in `tests/test_gdb_mcp.c` (which drives the real binary against `tests/fake_gdb.c` - extend fake_gdb's per-address memory answers for anything that reads target memory). Board-probing = `gdb_identify_target` (decode CPUID + vendor ID registers -> SoC) and `gdb_generate_profile` (identify -> starter `*.gdb-profile.json`); Cortex-M scope today - see `docs/board-probing.md`.

**Memory ownership:** Caller owns returned strings (must free). Broker owns registered links/clients (frees on destroy). cJSON objects follow cJSON's ownership model (add to parent = parent owns).

**Error handling:** Return -1 on failure, 0 on success. NULL propagation. Avoid `assert` in production code (none present). Avoid `goto` except for localized error-cleanup jumps - current uses are `goto send_result` in the MCP sink (`src/sinks/mcp.c`) and `goto done` cleanup in `src/board_manifest.c`. Log errors via `sm_log()` macros.

## Feature Flags

Managed via Kconfig (`Kconfig` root file + kconfiglib). Generated at configure time by `scripts/kconfig_genconfig.py` -> `sm_features.h` + `sm_features.cmake`.

**Critical:** Use `#if SM_ENABLE_X` (not `#ifdef`) - values are always 0 or 1. Guards go **only** in wiring files (`main.c`). Feature code itself has zero ifdefs - disabled features are excluded from `SM_SOURCES` in CMakeLists.txt.

## Testing

Custom macro-based framework in `tests/test_main.h` (`ASSERT`, `ASSERT_STR_EQ`, `ASSERT_INT_EQ`, `RUN_TEST`, `TEST_REPORT`). Copied from smolclaw.

Tests use PTY pairs (`openpty()`) to simulate serial devices - no hardware needed. Integration tests start a broker in-process, connect clients via Unix sockets, and verify message exchange.

**Adding a test:** (1) Create `tests/test_foo.c` with `#include "test_main.h"`. (2) Add to `CMakeLists.txt` as `add_executable` + `add_test`. (3) If feature-gated, wrap in `if(SM_ENABLE_X)`.

## Wire Protocol

Newline-delimited JSON over Unix sockets. All binary data base64-encoded.

Client -> Broker: `hello`, `send`, `send_expect`, `takeover`, `release`, `status`, `pin_control`, `set_baud`, `suspend`, `resume`, `history_request`, `incidents_request`, `configure_anomaly`, `interrupt_autoboot`, `configure_autoresponder`, `autoresponders_request`

Broker -> Client: `welcome`, `output`, `input_echo`, `expect_result`, `status_response`, `error`, `history_response`, `incidents_response`, `anomaly`, `suspended`, `resumed`, `link_down`, `link_up`, `autoboot_result`, `boot_stage`, `boot_stall`, `autoresponders_response`, `autoresponder_fired`
