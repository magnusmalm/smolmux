# Changelog

## Unreleased

- `smolmux-cli shutdown` (alias `stop`) - stop one broker cleanly: SIGTERM by
  its discovered pid, then wait until the socket disappears. Refuses to guess
  when several brokers run and no `-s` is given.
- Busy-port startup failures now name the holding smolmux broker (pid, socket,
  board) with a shutdown hint, or point at `fuser` when the holder is another
  process.
- ESP profiles: first boot stage (`reset`) matches Arduino-ESP32 `rst:0x..`
  cold boots as well as the classic `ESP-ROM:` banner.
- New board manifest example: Waveshare ESP32-S3-Touch-LCD-1.28.

## 0.1.2 - first public release

Portable C11 device multiplexer: one broker holds one wire (serial UART,
GDB/MI, or serial-over-TCP) and multiplexes it to many clients over Unix
sockets with newline-delimited JSON.

This is the first open-source release of smolmux.

### What's in this release

- **Broker core** - epoll event loop, role-based client arbitration
  (observer/controller/takeover), suspend/resume with fd release for flashers
  (`smolmux-cli with-port <cmd>`), auto-reconnect with exponential backoff.
- **Device links** - serial UART (termios), GDB/MI subprocess, serial-over-TCP
  (ser2net/socat/terminal servers; telnet IAC + RFC2217 control).
- **History, logs, anomalies, boot stages** - timestamped output history for
  late joiners, JSONL I/O log, rotating text log, anomaly incidents
  (`smolmux-watcher`), boot-stage tracking with stall events.
- **Expect, autoresponder, U-Boot break-in** - concurrent expect on the stream,
  standing expect->send rules in the broker read path, broker-side key flood
  (optional DTR/RTS reset) for `bootdelay=0` U-Boot.
- **MCP servers** - standalone over stdio: `smolmux-mcp`
  (16 serial tools) and `smolmux-gdb-mcp` (21 GDB tools, fault-register decode
  where the core has them, unknown-board probing on **ARM Cortex-M** today:
  CPUID/vendor-ID identify -> starter profile generation; 2 resources, 3 guided
  prompts). Other architectures planned.
- **Clients** - `smolmux-monitor` (interactive terminal, configurable escape
  prefix), `smolmux-cli` (scripting: send/expect, boards, break-uboot,
  boot-status), broker discovery (`smolmux-monitor -L`, `smolmux-cli boards`).
- **Profiles & boards** - JSON device profiles (prompts, commands, anomaly
  patterns, boot stages), `*.board.json` multi-wire manifests with
  `board up/down/status` lifecycle. Short profile names (`-p uboot`, board
  `"profile"`) resolve under `~/.config/smolmux/`, `profiles/`, and `configs/`;
  missing explicit profiles hard-fail (no silent first-file pick).
- **Build** - Kconfig feature selection (UART-only builds carry no
  GDB/TCP/WebSocket code), musl static builds, zero-warnings C11, full test
  suite (PTY-simulated serial + fake-gdb integration, no hardware needed).

### GDB (this release)

- Broker attaches with MI `-target-select extended-remote` (not CLI
  `target remote`).
- `gdb_reset` flushes GDB's register cache after OpenOCD reset; mode `run` is
  reset halt + flush + `-exec-continue` so breakpoints apply.
- `gdb_read_registers` honors a `names` filter (JSON array of strings; also
  accepts CSV / JSON-array strings).

### Free vs Pro

- Full source is MIT: build it yourself for the complete feature set.
- **smolmux Pro** is convenience: multi-arch static binaries, curated pack,
  email support - one-time purchase (see README Buy link). Not a feature gate.

### Known limitations

- MCP servers cannot authenticate to `--auth-token`-protected TCP brokers
  (Unix socket and unauthenticated loopback TCP only).
- WebSocket sink is loopback-oriented; two hardening items deliberately
  deferred (`docs/issue-ws-hardening-deferred.md`).
- Unknown-board probing identify/generate path is **Cortex-M** architectural
  today; see `docs/board-probing.md`.
- Hardware validation coverage is in `docs/hw-validation.md` - some
  capabilities are PTY/sim-proven only.
