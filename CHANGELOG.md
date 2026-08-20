# Changelog

## Unreleased

## 0.2.0

- CLI/MCP refuse first-glob when more than one broker socket exists
  (the old WEAK-only gate still magnetized every agent onto the first
  STRONG board). `brokers --json` / `-L` list client names. Housekeeping
  drops clients whose peer pid is gone.
- Same hello name replaces the previous connection (stops `claude-mcp`
  stacking). `smolmux-cli gc [--dry-run] [--mcp]` SIGTERMs leftover MCP
  processes (default: dead/orphan; `--mcp` all `*-mcp`). If the broker
  is older and omits peer pids, gc finds `smolmux-mcp` via `/proc`.
- Pro zip includes `daily-driver.md`, `dual-service-usb-cable.md`, and
  the docs they link; packing fails if a staged markdown path is missing.
- History fence on link down/up; status `link_up_ts`, `last_rx_age_ms`,
  `bytes_rx_since_link_up`. MCP records those events.
- `smolmux-cli send` rejects flags after the command (musl-safe).
- `smolmux-monitor -s` socket alias. `--wait-device` / `SMOLMUX_WAIT_DEVICE_S`.
- Weak by-id detection; reconnect refuses a seat change (fail-closed,
  including an unset last seat).
- Named board + WEAK by-id returns `identity_ambiguous` JSON (CLI + MCP).
- `brokers --json` / status include by-id, by-path, and WEAK/STRONG.
- Dual-key `device` object in `*.board.json`. Dual-service USB runbook.
- POSIX-safe default prompts (`[[:space:]]`, not PCRE `\\s`).
- MCP mutate tools hidden unless `SMOLMUX_MCP_MUTATE=1`.
- `serial_send_command` `eol=cr|lf|crlf`; `serial_write` unescapes `\\r`.
- Pipelined client lines drained; TX write-queue order preserved.
- Autoresponder lookback is the incomplete last line only.

- Serial agent UX (Wave 4): explained error hints on tool results; richer
  `serial_list_ports` (by-id, USB VID/PID); recent incidents footer on
  read/wait_for; MCP tool annotations; soft-fail when broker is down
  (stdio MCP stays up, tools retry connect — never auto-spawn); skill
  `skills/smolmux/SKILL.md`.
- Serial agent UX (Wave 3): `listen_expect` wire message + MCP
  `serial_wait_for` (listen-only regex; observers OK; critical anomaly
  abort). History cursor: `history_request` with `since_seq` /
  `max_bytes`; response `cursor`/`dropped`/`has_more`. MCP history with
  `since_seq` returns JSON pages; without stays prose. `serial_read`
  remains drain-only.
- Anomaly/expect agent UX (Wave 2): ESP32/MCU crash signatures are always-on
  builtins (`guru_meditation`, `brownout`, `panic_abort`, `stack_smashing`,
  `task_wdt`, `esp_reset`) without requiring a device profile. Critical
  incidents abort pending expects early (`aborted` / `abort_pattern` on
  `expect_result`; MCP shows `[ABORTED anomaly:…]`). Same-name patterns
  replace instead of double-firing.
- Serial MCP agent UX (Wave 1): `initialize` sends short **instructions**
  (status/history/incidents/suspend workflow). Prompts `bringup`,
  `debug_serial`, and `flash_safe` on both `smolmux-mcp` and the in-broker
  MCP sink (same text source: `src/mcp_instructions.c`).
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
