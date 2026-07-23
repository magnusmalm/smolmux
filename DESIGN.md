# smolmux Design Document

## What this is

smolmux is a portable C11 device multiplexer that holds a device connection open (serial port, GDB stub, JTAG, etc.) and multiplexes access to multiple clients over Unix sockets using newline-delimited JSON. It includes an expect-like pattern matching engine, anomaly detection, output history, and structured logging.

Goals:

1. **Single static binary** - deploy to embedded Linux targets by copying one file
2. **Lower latency** - C + epoll (critical for YMODEM, fast device responses)
3. **Smaller footprint** - runs alongside the device under test on resource-constrained systems
4. **Unified tool** - one codebase for serial UART, GDB, and future device interfaces
5. **Build-time feature selection** - targets that only need UART don't carry GDB/websocket code

Design patterns, build system, and coding conventions follow the same "smol"
style as [smolclaw](https://github.com/magnusmalm/smolclaw) (shared C11 patterns,
Kconfig, tests).

## Terminology

| Term                    | Meaning                                                        |
| ----------------------- | -------------------------------------------------------------- |
| **link**                | Device-facing bidirectional adapter (reads/writes the target). |
| **sink**                | Consumer-facing output adapter; may also send commands back.   |
| **core**                | The central broker/muxer (see below).                          |
| **target** / **device** | The embedded system on the other end of a link.                |
| **client**              | A program connected to smolmux via Unix socket (see below).    |

- **link** - examples: UART serial link, GDB MI link, JTAG link.
- **sink** - examples: terminal sink, log sink, TCP sink, websocket sink, MCP sink.
- **core** - owns the event loop, message bus, client connections, role enforcement, history buffer, and logging.
- **client** - e.g. monitor, MCP server, watcher, custom tooling.

### Data flow

```
┌───────────┐  ┌───────────┐
│ uart link │  │  gdb link │   <- device-facing, each compiled in/out via Kconfig
└─────┬─────┘  └─────┬─────┘
      │              │
      ▼              ▼
┌──────────────────────────────┐
│         smolmux core         │  <- event loop, message bus, timestamping,
│  history · logging · anomaly │     role enforcement, expect engine
└──┬───────┬───────┬───────┬───┘
   │       │       │       │
   ▼        ▼        ▼
┌──────┐┌──────┐┌──────┐
│ tcp  ││  ws  ││ mcp  │  <- sinks, also compiled in/out
│ sink ││ sink ││ sink │
└──────┘└──────┘└──────┘
```

(A terminal is a Unix-socket client - `smolmux-monitor` - not a sink; logging
is handled by core modules `io_log.c`/`text_log.c`, not a sink.)

Links are **exclusive** - exactly one link is active at a time. The core doesn't multiplex between different device types simultaneously (you don't talk UART and GDB to the same target). Which link is compiled in is a build-time choice.

Sinks are **concurrent** - multiple sinks can consume the same data stream. A terminal sink, log sink, and MCP sink can all be active simultaneously (this is the whole point of the multiplexer).

## Architecture

### Core components

| Component            | Responsibility                                                   |
| -------------------- | ---------------------------------------------------------------- |
| **Event loop**       | `epoll`/`poll` based. Manages link I/O, client sockets, timers.  |
| **Message bus**      | Routes data between link, core modules, and clients.             |
| **Client manager**   | Unix socket server: connections, JSON dispatch, roles, takeover. |
| **Ring buffer**      | Timestamped output history; non-destructive replay queries.      |
| **Expect engine**    | Concurrent regex pattern matching on the byte stream.            |
| **Anomaly detector** | Passive pattern matching for crash signatures.                   |
| **Boot tracker**     | Ordered boot markers -> progress, furthest stage, stall.         |
| **Autoresponder**    | Standing expect->send rules fired in the read path.              |
| **I/O log**          | JSONL structured log of all I/O and incidents, timestamped.      |
| **Text log**         | Human-readable device output log, timestamped, daily rotation.   |

- **Event loop** - no libevent dependency; raw POSIX for minimal footprint.
- **Message bus** - internal binary framing (not JSON - JSON is only for the wire protocol to clients).
- **Client manager** - roles are observer/controller.
- **Ring buffer** - configurable size (default 2 MB).
- **Expect engine** - multiple simultaneous expects from different clients, each with independent timeouts; uses POSIX ERE (`regcomp`/`regexec`) on byte buffers.
- **Anomaly detector** - signatures: kernel panic, HardFault, segfault, OOM, watchdog. Configurable patterns, pre-crash context window, cooldown to prevent re-firing.
- **Autoresponder** - standing `expect->send` rules matched on the read-path stream (a sliding-window regex engine like the anomaly detector). When a rule matches, the broker writes the rule's response straight to the device with zero client round-trip - for boot menus ("Press any key"), y/n prompts, or a login sequence - and broadcasts `autoresponder_fired`. Per-rule cooldown guards against re-firing on still-visible text; `once` disables a rule after its first fire. Configured live over the wire (`configure_autoresponder`).
- **Boot tracker** - a profile's ordered `boot_stages` (name + regex) matched on the stream. Reports which stages were reached (with timestamps), the furthest stage, and whether the boot stalled (no advance for `boot_stall_timeout_ms`, terminal stage unreached). Broadcasts a `boot_stage` event per stage reached; also surfaced in `status_response.boot`. A one-shot timerfd, re-armed on each advance and disarmed at the terminal stage, pushes a single proactive `boot_stall` event if the timeout elapses mid-boot (no client polling). Resets on link reconnect or when stage 0 re-appears (a new boot). Order-tolerant matching: a dropped intermediate marker does not block later stages.

### Client output path (broadcast, backpressure, coalescing)

A chunk of device output is JSON-encoded **once** per broadcast, wrapped in a
refcounted shared line (`sm_shared_line_t`), and each client acquires a
reference - the fan-out cost is one encode regardless of client count, not one
per client.

Each client owns a fixed-size write queue (`SM_CLIENT_WRITE_QUEUE_SIZE`, 256
entries). Writes are non-blocking; a partial write records how many bytes went
out (`offset`) and re-arms `EPOLLOUT`. If the queue fills (a client that will
not drain), further messages are dropped and counted (`wq_drops`, surfaced in
status) rather than blocking the broker.

To relieve queue pressure for a slow-but-draining client, consecutive queued
`output` messages are **coalesced**: the new line's payload is merged into the
last queued entry so one dequeue delivers both. Two guards bound this:

- **Never merge into a partially-flushed head** (`offset != 0`): the head may
  already be mid-transmission on the socket, and rewriting it would re-send the
  bytes already delivered, corrupting the client's stream.
- **Head-size cap** (`SM_CLIENT_COALESCE_MAX_BYTES`, 16 KB): each merge decodes
  the queued head's payload, appends, and re-encodes the whole thing, so an
  unbounded merged head would make a fully-stalled client's per-line cost grow
  with backlog depth (O(K²) overall). Once the merged entry reaches the cap,
  merging stops and the next line starts a fresh entry, keeping per-line cost
  at O(cap) while still bounding the queue well under its 256-entry limit.

### Vtable interfaces

Following the same vtable polymorphism pattern as
[smolclaw](https://github.com/magnusmalm/smolclaw), links and sinks are defined
as function pointer structs.

#### Link vtable

```c
typedef struct sm_link {
    const char *name;                          /* "uart", "gdb", etc. */

    int  (*open)(struct sm_link *self);        /* Connect to device. 0=ok, -1=err */
    void (*close)(struct sm_link *self);       /* Release device */
    int  (*read_fd)(struct sm_link *self);     /* FD for epoll registration (or -1) */
    int  (*write_fd)(struct sm_link *self);    /* FD for EPOLLOUT when write queue pending */
    int  (*write_data)(struct sm_link *self,   /* Enqueue/send data to device */
                       const uint8_t *data, size_t len);
    int  (*has_write_pending)(struct sm_link *self);
    int  (*flush_write_queue)(struct sm_link *self);
    int  (*send_break)(struct sm_link *self,   /* Serial break / equivalent */
                       int duration_ms);
    int  (*set_param)(struct sm_link *self,    /* Set link-specific parameter */
                      const char *key, const char *value);
    int  (*get_status)(struct sm_link *self,   /* Fill status into JSON object */
                       cJSON *out);
    void (*destroy)(struct sm_link *self);

    /* Optional: non-blocking reconnect path (serial-tcp). NULL -> open() on reconnect. */
    int  (*connect_begin)(struct sm_link *self); /* 0=up, 1=in progress, -1=fail */
    int  (*connect_poll)(struct sm_link *self);  /* 1=up, 0=pending, -1=fail */

    /* Optional: strip/transform RX before expect/history/anomaly (e.g. telnet IAC). */
    size_t (*filter_rx)(struct sm_link *self, const uint8_t *in, size_t in_len,
                        uint8_t *out);

    int silence_normal;   /* 1 = skip idle link_health (e.g. GDB) */
    void *data;           /* Link-specific state */
} sm_link_t;
```

The `read_fd()` method returns a file descriptor that the core registers with epoll. When readable, the core calls `read()` on it directly and feeds the bytes into the expect engine, history buffer, anomaly detector, boot tracker, and autoresponder (which may write a response straight back), then broadcasts to clients.

For links that don't map to a single FD (e.g., a GDB stub over TCP that needs packet reassembly), the link can use an internal pipe - write reassembled data to the pipe's write end, return the read end from `read_fd()`.

#### Sink vtable

```c
typedef struct sm_sink {
    const char *name;                          /* "tcp", "ws", "mcp", etc. */

    int  (*start)(struct sm_sink *self, void *broker);
    void (*stop)(struct sm_sink *self);
    void (*on_output)(struct sm_sink *self,
                      const uint8_t *data, size_t len, double ts);
    void (*on_event)(struct sm_sink *self,
                     const char *event, const char *payload);
    void (*on_readable)(struct sm_sink *self);
    void (*on_expect_result)(struct sm_sink *self, const char *id,
                             int matched, const uint8_t *data, size_t data_len,
                             const char *pattern);

    void (*destroy)(struct sm_sink *self);

    void *data;
    int fd;                                    /* fd to watch in epoll, -1 = none */
} sm_sink_t;
```

Sinks are primarily output consumers. Some sinks (like the MCP sink or TCP sink) also accept commands back from their consumers - these are routed through the core's client manager, same as Unix socket clients.

### Wire protocol

The client-facing wire protocol is **newline-delimited JSON** over Unix sockets
(and TCP/WS sinks). All binary data (serial bytes) is base64-encoded in JSON
messages. The message set covers session control, expect, history, anomaly, boot
stages, autoboot flood, and autoresponder. Canonical list:
`smolmux --help-protocol` and `src/protocol.c` `msg_type_map`.

This protocol is kept because:
- It's already proven and debuggable (`socat` + `jq` for inspection)
- MCP integration requires JSON anyway
- The overhead is negligible compared to serial baud rates

**Message types:**

Client -> Broker: `hello`, `send`, `send_expect`, `takeover`, `release`, `status`, `pin_control`, `set_baud`, `suspend`, `resume`, `history_request`, `incidents_request`, `configure_anomaly`, `interrupt_autoboot`, `configure_autoresponder`, `autoresponders_request`

Broker -> Client: `welcome`, `output`, `input_echo`, `expect_result`, `status_response`, `error`, `history_response`, `incidents_response`, `anomaly`, `autoboot_result`, `boot_stage`, `boot_stall`, `autoresponders_response`, `autoresponder_fired`, `suspended`, `resumed`, `link_down`, `link_up`

### Roles and access control

- **observer** - receives all output, cannot send
- **controller** - can send commands to the device
- **takeover** - one controller can claim exclusive send access (blocks other controllers)
- **suspend/resume** - a controller can temporarily release the device so external tools (YMODEM, flashers) can access it directly

### Standalone MCP servers

Two MCP servers connect to a running broker as ordinary clients (Unix socket or
TCP) and expose device access to AI agents over MCP stdio JSON-RPC. They are
separate binaries, not sinks - the broker and its links stay **dumb byte pipes**;
all protocol intelligence lives client-side, correlated over the wire protocol.

- **smolmux-mcp** (`src/mcp_client.c`) - serial tools: `serial_send_command`,
  `serial_read`, output history, anomaly/incidents, pin control, etc.
- **smolmux-gdb-mcp** (`src/gdb_mcp.c`) - GDB/SWD debugging over a broker holding
  a `--gdb` link. It prefixes each GDB/MI command with a numeric token, sends the
  bytes through the broker to gdb's stdin, and scans the raw output stream for the
  matching `<token>^...` result record (`src/util/mi_parse.c`); async records
  (`*stopped`, `~console`) are captured as they stream by. The broker/link never
  parse MI. Target context (register set, fault registers, peripheral map, RTOS
  commands) comes from a `*.gdb-profile.json` (`src/gdb_profile.c`). 21 tools, 3
  prompts, 2 resources.

**Unknown-board probing (ARM Cortex-M today).** Beyond the debugging primitives,
gdb-mcp helps an agent identify and map a Cortex-M board it has never seen.
The identify path uses architectural SCB CPUID (`0xE000ED00`), a light CoreSight
ROM-table probe, and best-effort vendor ID registers (STM32 / SAM / nRF). It is
**not** a universal multi-ISA prober yet; RISC-V, Xtensa (e.g. ESP32 USB-JTAG),
and other families are intentional follow-ons once the Cortex-M path is solid.

- `gdb_identify_target` - structured core/SoC guess with an evidence trail.
- `gdb_generate_profile` - starter `*.gdb-profile.json` (registers, fault map
  when present, seeded peripherals); optional write under `~/.config/smolmux/`.
- `probe_unknown_board` prompt and `smolmux-gdb://board-probing` resource.

See `docs/board-exploration-workflow.md` and `docs/board-probing.md`.

## Build system

### CMake + Kconfig

Same pattern as [smolclaw](https://github.com/magnusmalm/smolclaw):

1. **Kconfig** file defines feature flags (`SM_ENABLE_UART`, `SM_ENABLE_GDB`, `SM_ENABLE_SINK_TCP`, etc.)
2. **`scripts/kconfig_genconfig.py`** runs at configure time, produces:
 - `sm_features.h` - C header with `#define SM_ENABLE_X 0` or `#define SM_ENABLE_X 1`
 - `sm_features.cmake` - CMake variables `set(SM_ENABLE_X ON/OFF)`
3. **CMakeLists.txt** conditionally includes source files based on feature flags
4. **Feature guards** only appear in wiring code (`main.c`), never inside feature modules
5. **Disabled features** are excluded from `SM_SOURCES` entirely - no dead code

### Build modes

| Mode              | Command                                 | Result                           |
| ----------------- | --------------------------------------- | -------------------------------- |
| Dynamic (default) | `cmake -B build && cmake --build build` | Links against system libs        |
| Static (musl)     | `cmake -B build -DSM_MUSL_STATIC=ON`    | Zero runtime deps, single binary |
| Minimal           | `cp configs/defconfig.minimal .config`  | UART only, no sinks/watcher/GDB  |
| UART profile      | `cp configs/defconfig.uart .config`     | UART + MCP sink + watcher        |

- **Minimal** / **UART profile** - copy the defconfig, then run `cmake -B build`. The UART profile excludes GDB/TCP/WS.

### Dependencies

**Always required:**
- cJSON (vendored, single file) - JSON parsing for wire protocol

**Optional (feature-gated):**
- PCRE2 - if `SM_ENABLE_PCRE2` is on; otherwise falls back to POSIX ERE

**Core has zero external dependencies** beyond POSIX + cJSON. The event loop uses raw `epoll_wait`/`poll`. Serial I/O uses `termios`. TCP and WebSocket sinks use raw POSIX sockets. This is intentional - the minimal UART-only build should compile and run on any Linux system.

## Naming conventions

| Scope            | Convention             | Example                                         |
| ---------------- | ---------------------- | ----------------------------------------------- |
| Functions/types  | `sm_` prefix           | `sm_broker_run()`, `sm_link_t`                  |
| Macros/constants | `SM_` prefix           | `SM_VERSION`, `SM_MAX_CLIENTS`                  |
| Feature flags    | `SM_ENABLE_X`          | `SM_ENABLE_UART`, `SM_ENABLE_GDB`               |
| Struct tags      | Named for forward decl | `typedef struct sm_broker { ... } sm_broker_t;` |

## Directory structure

```
smolmux/
├── CMakeLists.txt
├── Kconfig
├── CLAUDE.md
├── DESIGN.md                      <- this file
├── cmake/
│   └── kconfig.cmake              <- Kconfig integration (smol-style)
├── scripts/
│   └── kconfig_genconfig.py       <- feature flag code generation
├── configs/
│   ├── defconfig.uart             <- UART + MCP sink + watcher
│   ├── defconfig.embedded         <- embedded target profile
│   ├── defconfig.minimal          <- minimal feature set
│   └── defconfig.full             <- all features enabled
├── deps/
│   └── cJSON/                     <- vendored JSON parser
├── src/
│   ├── main.c                     <- entry point, arg parsing, wiring
│   ├── constants.h                <- all defaults and limits
│   ├── broker.c / broker.h        <- core broker: event loop, client mgmt
│   ├── client.c / client.h        <- single client connection handler
│   ├── protocol.c / protocol.h    <- JSON message encode/decode
│   ├── ring_buffer.c / .h         <- timestamped ring buffer (history)
│   ├── expect.c / expect.h        <- concurrent pattern matching engine
│   ├── regex_engine.c / .h        <- POSIX ERE / PCRE2 backend
│   ├── anomaly.c / anomaly.h      <- crash/anomaly pattern detector
│   ├── device_profile.c / .h      <- JSON device profile loader
│   ├── io_log.c / io_log.h        <- JSONL structured I/O log
│   ├── text_log.c / text_log.h    <- human-readable text log + rotation
│   ├── logger.c / logger.h        <- sm_log() logging macros/backend
│   ├── watcher.c                  <- standalone anomaly reporter binary
│   ├── monitor.c / cli.c          <- companion client binaries
│   ├── mcp_client.c               <- standalone smolmux-mcp server binary (stdio)
│   ├── gdb_mcp.c                  <- standalone smolmux-gdb-mcp server binary (stdio)
│   ├── gdb_profile.c / .h         <- *.gdb-profile.json loader + serializer
│   ├── mcp_broker_conn.c / .h     <- shared broker connection for the MCP clients
│   ├── util/
│   │   ├── base64.c / base64.h    <- base64 encode/decode
│   │   ├── str.c / str.h          <- growable string buffer (sm_strbuf_t)
│   │   ├── json_helpers.c / .h    <- cJSON convenience wrappers
│   │   ├── shared_line.c / .h     <- refcounted shared broadcast line (encode once)
│   │   ├── mi_parse.c / .h        <- GDB/MI record parser (client-side)
│   │   ├── sock_util.c / .h       <- socket helpers
│   │   └── sha1.c / .h            <- WebSocket handshake
│   ├── links/
│   │   ├── link.h                 <- sm_link_t vtable
│   │   ├── link_wq.c / .h         <- non-blocking link write queue
│   │   ├── uart.c / uart.h        <- serial port link (termios)
│   │   ├── gdb.c / gdb.h          <- GDB MI link
│   │   └── serial_tcp.c / .h      <- serial-over-TCP link (telnet-aware)
│   ├── sink.h                     <- sm_sink_t vtable
│   └── sinks/
│       ├── tcp.c / tcp.h          <- TCP remote access sink
│       ├── ws.c / ws.h            <- WebSocket sink
│       ├── mcp.c / mcp.h          <- MCP stdio sink
│       ├── mcp_internal.h         <- MCP sink internal shared declarations
│       └── mcp_tools.c            <- MCP tool implementations
└── tests/
    ├── test_main.h                <- macro test framework (smol-style)
    ├── test_protocol.c            <- JSON message roundtrips
    ├── test_ring_buffer.c         <- history buffer
    ├── test_expect.c              <- pattern matching
    ├── test_base64.c              <- base64 encode/decode
    ├── test_broker.c              <- PTY-based integration tests
    ├── test_uart.c                <- serial I/O via PTY pairs
    └── ...                        <- feature-gated tests (gdb, mcp, tcp, ws, watcher)
```

## Regex strategy

The expect engine needs to match patterns against a growing byte buffer. Two options:

### Option A: POSIX ERE (default, zero dependencies)

Use `regcomp()`/`regexec()` from libc. Available everywhere.

**Pros:** Zero deps, works on any POSIX system, sufficient for common patterns (prompt matching, error detection).

**Cons:** No non-greedy quantifiers, no lookbehind, no `\d` shorthand (use `[0-9]`), no dotall mode (`.` doesn't match `\n`). Pattern syntax is less expressive than Python's `re`.

**Workaround for dotall:** The expect engine matches against a contiguous byte buffer. We can preprocess the buffer to replace `\n` with a sentinel before matching, or use `[^\n]|[\n]` in patterns. Alternatively, since we're matching against accumulated bytes (not line-by-line), we can use `REG_NEWLINE` flag control.

### Option B: PCRE2 (optional, feature-gated)

Compile with `SM_ENABLE_PCRE2=ON` for full Perl-compatible regex.

**Pros:** Feature parity with Python's `re` module. Non-greedy, lookahead, dotall, named groups.

**Cons:** Adds a dependency (~500 KB static). Not available on all embedded systems.

### Decision

Default to **POSIX ERE** for zero-dep builds. Gate PCRE2 behind `SM_ENABLE_PCRE2`. The expect engine uses a function pointer for the match operation - the concrete implementation is selected at build time. Typical console patterns (prompt matching like `\$\s*$`, error detection like `Kernel panic`) work fine with POSIX ERE.

## Design targets (C11)

| Aspect      | Target                                              |
|-------------|-----------------------------------------------------|
| Runtime     | Single static binary (musl)                         |
| Binary size | ~100-300 KB static                                  |
| Memory      | ~1-2 MB RSS                                         |
| Latency     | epoll + direct syscalls (us-scale hot path)         |
| Serial I/O  | termios, non-blocking                               |
| Regex       | POSIX ERE default; optional PCRE2                   |
| JSON        | cJSON (vendored)                                    |
| MCP         | Custom MCP stdio servers (`smolmux-mcp`, gdb-mcp)   |
| Portability | Linux (musl static); BSDs possible later            |
