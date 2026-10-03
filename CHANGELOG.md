# Changelog

## Unreleased

## 0.5.1

- Version is 0.5.1.
- A broker that fails to start after opening its port (for example on a
  socket path too long to bind) no longer reports `port ... is held by
  another process`. The busy-port check ran while the broker itself still
  held the port.
- The UART link clears its exclusive lock (`TIOCNXCL`) when it closes. The
  lock belongs to the tty, so it outlived the close whenever another file
  descriptor kept the tty open, and the next opener got `EBUSY`.
- `serial_add_autoresponder`, `serial_pin_control`, `gdb_threads` and
  `gdb_reset` descriptions name the tool to use instead.

## 0.5.0

- Version is 0.5.0.
- Five read-only serial MCP tools declare an `outputSchema` and return
  `structuredContent` next to the text: `serial_port_status`,
  `serial_boot_status`, `serial_get_incidents`, `serial_list_ports` and
  `serial_output_history` (`mode: "cursor"` with `since_seq`, `"text"`
  without). Agents can read fields instead of parsing prose. The text block
  is unchanged in `smolmux-mcp`.
- Tool errors carry `isError: true` in `smolmux-mcp` and the broker's
  `--mcp` sink. A structured tool that fails returns no `structuredContent`.
- Every serial tool description says when to use it and which sibling to
  use instead (`serial_read` vs `serial_output_history` vs `serial_monitor`
  vs `serial_wait_for`, and so on).
- The `--mcp` sink's `serial_port_status` now reports the same fields as
  `smolmux-mcp` (identity strength and paths, link age, bytes since link up,
  log path). Both now build their answer from the broker's `status_response`.
- `smolmux-mcp` `serial_get_incidents` reports a broker error instead of
  "No anomalies detected.".
- `smolmux-gdb-mcp` tool descriptions say when to use each tool and which
  to use instead (for example `gdb_read_memory` vs `gdb_evaluate` vs
  `gdb_read_peripheral`, `gdb_wait_stop` vs `gdb_interrupt`).
- `status_response` no longer carries duplicate keys. The link's status
  fields are merged only where the broker has not set that key, and the
  complete link view is under a new `link` object. Before, a serial-tcp
  broker sent `port` twice (the device string, then the TCP port number),
  and parsers that keep the last duplicate saw a number.
- `smolmux-monitor` prints "Connected to ..." only after the broker's
  `welcome`. A refused hello (wrong or missing token) prints the broker's
  reason and exits 1 instead of announcing a connection and then
  disconnecting.
- README lists the build prerequisites, including Python `kconfiglib`, which
  the configure step needs.
- The build is free of compiler warnings again. The WebSocket frame length
  check can no longer wrap. A port path longer than 127 bytes no longer gets
  a truncated I/O log file name. Help texts are split under the C11
  string-literal limit (output unchanged).
- README shows the one-port-three-clients demo GIF. The daily-driver TCP
  monitor example passes the auth token.

## 0.4.0

- Version is 0.4.0.
- Loopback TCP and WebSocket listeners now require a token. Started without
  `--auth-token`, the broker generates one into
  `$XDG_RUNTIME_DIR/smolmux-<tcp|ws>-<port>.token`, or `/tmp` when
  `XDG_RUNTIME_DIR` is unset (mode `0600`, removed on exit), so other local
  users and processes can no longer drive the device. `smolmux-monitor`,
  `smolmux-mcp` and `smolmux-gdb-mcp` with `--tcp` to a loopback address read
  it automatically, before every connect, from either place (a broker from
  cron or a system unit often has no `XDG_RUNTIME_DIR`). `smolmux-cli token`
  prints it for an SSH tunnel's far end or a WebSocket client. If `/tmp`
  already holds that name (another user's file or a symlink), the broker
  refuses to start and says to set `XDG_RUNTIME_DIR`. `--insecure-no-auth` restores tokenless serving. **Breaking** for
  other TCP/WS clients on loopback: give them the token.
- The WEAK by-id warnings point at `docs/persistent-serial-devices.md`, which
  exists in the repository and the Pro zip. `docs/PERSISTENT-SERIAL.md`
  (named in the 0.3.0 and 0.3.1 warnings) exists only in the zip.
- `--help-protocol` shows the running version in its handshake example.

## 0.3.1

- Version is 0.3.1.
- `smolmux-monitor` shows a bare LF from the device as CRLF, so firmware
  that ends lines with `\n` alone no longer staircases across the screen.
  Only a raw-mode terminal is affected; piped output is unchanged. `--raw`
  writes device bytes unchanged.
- The broker sends device output before the anomaly, boot-stage, and
  autoresponder events that output triggers, so the matching line shows first.
- After a link reconnect (including `resume` and `with-port`), boot stages are
  matched only against the new boot. Before, the last stage's marker from the
  previous boot could count again.
- `smolmux-monitor` holds event lines while the device is mid-line and prints
  them at the line end (at most 200 ms later), so they no longer split device
  output. `[anomaly]` shows the first line of the match only.

## 0.3.0

- Version is 0.3.0 (`SM_VERSION` and CMake `project VERSION`).
- Named MCU and FPGA boards stay in the Pro zip `profiles/` directory.
- The Pro zip includes a host installer. It copies packed `*.json` into
  `~/.config/smolmux/` with `cp -n`, so files already in that directory are
  left unchanged.
- Zip updates are Polar customer portal only.
- Auto-discover ignores leftover unreachable `.sock` files. Two live
  brokers still require `-s`. `smolmux-cli brokers` lists leftovers.
- `smolmux-cli status --json` (flag after the subcommand) emits JSON.
- `--list-profiles` lists `~/.config/smolmux/` and `./profiles/`, not
  `./configs/`.
- Packed MCP setup uses `~/.local/bin` after the host installer.
- `-h` / `--help` prints usage on stdout. Unknown options still print usage on
  stderr.
- Idle UART does not log a link-health warning or broadcast `link_health`
  degraded. Never-RX idle is one INFO line. GDB `silence_normal` is unchanged.
- Each `smolmux-cli` hello uses `smolmux-cli-<command>-<pid>`, so two CLI
  sessions do not replace each other. MCP same-name replace is unchanged.
- WEAK by-id startup warning points at `docs/PERSISTENT-SERIAL.md`.
- `--list-profiles` prints each profile name once. `~/.config` wins over
  `./profiles/`.
- Human `identity_ambiguous` is a short stderr hint. `--json` still prints
  the candidate list.

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
