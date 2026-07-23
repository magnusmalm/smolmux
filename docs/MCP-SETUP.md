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
other clients). Pass **`-s <socket>`** to pin one broker, **`-p <profile>`** to
load a device profile. Note: **`smolmux-monitor` has no `-s`** - socket path is
**positional** only (`-c` is controller role, not a socket flag). See
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

**Serial (`smolmux-mcp`):**
`serial_read`, `serial_write`, `serial_send_command`, `serial_monitor`,
`serial_output_history`, `serial_port_status`, `serial_boot_status`,
`serial_list_ports`, `serial_pin_control`, `serial_sysrq`,
`serial_suspend`, `serial_resume`, `serial_get_incidents`,
`serial_generate_report`, `serial_add_watchdog`, `serial_add_autoresponder`

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
`--tcp-port`. **Known limitation:** the MCP servers cannot yet send an auth
token, so this only works against brokers **without** `--auth-token` /
`SMOLMUX_AUTH_TOKEN` - keep such brokers on loopback or a trusted network
(the TCP sink binds 127.0.0.1 by default).

## Troubleshooting

- **"no broker socket found"** - start the broker first; `smolmux-mcp` does
  not spawn one. Check `smolmux-monitor -L` to list live brokers.
- **GDB tools time out** - the gdb broker must be started with `--gdb`, and a
  GDB server (OpenOCD, JLinkGDBServer, qemu `-s`) must be listening on the
  `--gdb-target` address.
- **Two agents, one port** - both can connect; roles arbitrate writes
  (observer/controller/takeover). See `--help-protocol` on the broker binary.
