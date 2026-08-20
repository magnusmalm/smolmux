# MCP setup - smolmux for AI agents

Paths below assume a source build (`./build/...`). Pro zip buyers: use
[`MCP-SETUP-FULL.md`](MCP-SETUP-FULL.md) for install and binary locations.

smolmux ships two standalone MCP servers. Both are ordinary broker clients:
they connect to a **running broker** over its Unix socket and expose tools to
the agent over stdio JSON-RPC. The broker keeps running when the agent
disconnects; several agents and humans can share the same port.

| Binary            | Exposes         | Needs broker            |
|-------------------|-----------------|-------------------------|
| `smolmux-mcp`     | 16 serial tools | UART or serial-over-TCP |
| `smolmux-gdb-mcp` | 21 GDB tools    | `--gdb --gdb-target ...`|

Also: gdb-mcp adds 2 resources + 3 prompts. Example brokers:

```bash
smolmux /dev/ttyUSB0 -b 115200
smolmux --gdb --gdb-target localhost:3333 -s /tmp/smolmux-gdb.sock
```

## 1. Start a broker

```bash
./build/smolmux /dev/ttyUSB0 -b 115200                      # serial
./build/smolmux --gdb --gdb-target localhost:3333 \
                -s /tmp/smolmux-gdb.sock                    # GDB/SWD (OpenOCD/JLink on :3333)
```

## 2. Register with your agent

`smolmux-mcp` auto-discovers the broker socket from the port name (same idea as
other clients) **only when a single broker is up**. Two or more sockets:
pass **`-s <socket>`** (first-glob is a wrong-board magnet). **`-p <profile>`** to
load a device profile. `smolmux-monitor` accepts the same **`-s <socket>`**
alias (or a positional path). `-c` is controller role, not a socket flag. See
[daily-driver.md](daily-driver.md#client-flags-socket-pin---do-not-mix-them-up).

**Claude Code:**

```bash
claude mcp add serial  -- /path/to/smolmux/build/smolmux-mcp
claude mcp add gdb     -- /path/to/smolmux/build/smolmux-gdb-mcp -s /tmp/smolmux-gdb.sock
```

**Claude Desktop / Cursor** (`claude_desktop_config.json` / `.cursor/mcp.json`):

```json
{
  "mcpServers": {
    "serial": {
      "command": "/path/to/smolmux/build/smolmux-mcp",
      "args": ["-s", "/run/user/1000/smolmux-ttyUSB0.sock"]
    },
    "gdb": {
      "command": "/path/to/smolmux/build/smolmux-gdb-mcp",
      "args": ["-s", "/tmp/smolmux-gdb.sock", "-p", "~/.config/smolmux/myboard.gdb-profile.json"]
    }
  }
}
```

Adjust `/path/to/smolmux` to your checkout (Pro zip users: see the paths in
`MCP-SETUP-FULL.md` included in the bundle).

## 3. What the agent gets

**Serial (`smolmux-mcp`) — 17 tools:**
`serial_read`, `serial_write`, `serial_send_command`, `serial_wait_for`,
`serial_monitor`, `serial_output_history`, `serial_port_status`,
`serial_boot_status`, `serial_list_ports`, `serial_pin_control`,
`serial_sysrq`, `serial_suspend`, `serial_resume`, `serial_get_incidents`,
`serial_generate_report`, `serial_add_watchdog`, `serial_add_autoresponder`

**Capture notes:** `serial_read` drains the MCP session buffer only.
Lossless paging: `serial_output_history` with `since_seq` (JSON
`cursor`/`dropped`/`has_more`/`chunks`); pass `cursor` back as
`since_seq`. Listen-only wait: `serial_wait_for` (no TX; observers OK).

On `initialize`, the serial MCP also sends short **instructions** (workflow
for status, history, incidents, suspend/resume, multi-client). Guided
**prompts** (slash commands in clients that support them):

| Prompt         | Purpose                                      |
| -------------- | -------------------------------------------- |
| `bringup`      | Status, boot stages, proof of life           |
| `debug_serial` | History + incidents + monitor diagnosis      |
| `flash_safe`   | Suspend → external flasher → resume → boot   |

**Anomalies (broker, no profile required for ESP32 panics):** critical hits
such as `Guru Meditation Error` or `Brownout detector was triggered` are
recorded as incidents and **abort pending expects** early so agents do not
wait for a timeout on a dead board. Warning-level lines (e.g. `rst:0x…`)
do not abort. Use `serial_get_incidents` after a quiet or aborted wait.

**Soft-fail:** if no broker is running, `smolmux-mcp` still starts over
stdio. Tool calls return offline guidance (and retry connect). It never
auto-spawns a broker. Optional skill: `skills/smolmux/SKILL.md`.

**Mutate tools** (`serial_write`, `serial_send_command`, pins, SysRq,
suspend, autoresponder add) are listed only when `SMOLMUX_MCP_MUTATE=1`.
After a disconnect, call `serial_port_status` (link event / RX age)
before trusting history; look for `*** smolmux: link down ***`.

**GDB (`smolmux-gdb-mcp`):**
`gdb_status`, `gdb_launch`, `gdb_load`, `gdb_reset`, `gdb_continue`,
`gdb_step`, `gdb_interrupt`, `gdb_wait_stop`, `gdb_breakpoint`,
`gdb_delete_breakpoint`, `gdb_backtrace`, `gdb_threads`,
`gdb_read_registers`, `gdb_read_memory`, `gdb_read_peripheral`,
`gdb_read_fault_registers`, `gdb_evaluate`, `gdb_command`,
`gdb_console_output`, `gdb_identify_target`, `gdb_generate_profile`

Plus MCP resources `smolmux-gdb://target/profile` and
`smolmux-gdb://board-probing`, and guided prompts `diagnose_fault`,
`analyze_crash`, `probe_unknown_board` (halt an unknown board, decode CPUID +
vendor ID registers, emit a starter `*.gdb-profile.json`).

## Remote brokers (TCP)

Both servers accept `--tcp host:port` to reach a broker started with
`--tcp-port`.

**Authentication works.** Export the same token the broker was started with
in the environment of the MCP server, and `sm_msg_hello()` sends it
automatically:

```jsonc
{
  "mcpServers": {
    "serial": {
      "command": "/usr/local/bin/smolmux-mcp",
      "args": ["--tcp", "127.0.0.1:5555"],
      "env": { "SMOLMUX_AUTH_TOKEN": "the-same-token-the-broker-uses" }
    }
  }
}
```

Get it wrong and the tool result says so — `broker rejected this client:
authentication failed`, with a hint naming the variable.

The TCP sink binds `127.0.0.1` by default, and smolmux refuses to serve a
non-loopback bind with no token unless you pass `--insecure-no-auth`. The
wire protocol is cleartext either way, so for a remote broker prefer an SSH
tunnel over exposing the port.

## Troubleshooting

- **"no broker socket found"** - start the broker first; `smolmux-mcp` does
  not spawn one. Check `smolmux-monitor -L` to list live brokers.
- **GDB tools time out** - the gdb broker must be started with `--gdb`, and a
  GDB server (OpenOCD, JLinkGDBServer, qemu `-s`) must be listening on the
  `--gdb-target` address.
- **Two agents, one port** - both can connect; roles arbitrate writes
  (observer/controller/takeover). See `--help-protocol` on the broker binary.
