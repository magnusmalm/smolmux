---
name: smolmux
description: >
  Drive live serial (and GDB) devices through smolmux brokers and MCP tools.
  Use when debugging UART/MCU consoles, ESP32 crash loops, multi-client port
  sharing, flash coexistence, boot stages, or smolmux-mcp / smolmux-gdb-mcp.
---

# smolmux — agent skill

smolmux is a **long-lived device multiplexer**, not an IDE. A **broker** holds
one wire open; humans and agents attach as clients. MCP binaries
(`smolmux-mcp`, `smolmux-gdb-mcp`) are ordinary clients over stdio.

Docs: `docs/MCP-SETUP.md` in the smolmux tree (or Pro zip).

## When to use which MCP

| Need                         | Binary            |
| ---------------------------- | ----------------- |
| UART, expect, history, boot  | `smolmux-mcp`     |
| SWD/JTAG, regs, board probe  | `smolmux-gdb-mcp` |

Run **both** when correlating panics on serial with GDB halt state
(prompts `analyze_crash` on gdb-mcp).

## Start a broker first

```bash
smolmux /dev/ttyUSB0 -b 115200
# or: smolmux-monitor -L   # list live brokers
```

`smolmux-mcp` does **not** auto-spawn a broker. If the broker is down, MCP
still connects; tools return offline guidance. Start the broker and retry.

## Serial workflow (preferred)

1. `serial_port_status` / `serial_boot_status`
2. Writes: `serial_send_command` (shell + expect) or `serial_write` (raw)
3. Listen-only: **`serial_wait_for`** (regex, no TX; observers OK)
4. Lossless capture: **`serial_output_history` with `since_seq`**
   - Response is JSON: `cursor`, `dropped`, `has_more`, `chunks`
   - Pass `cursor` back as the next `since_seq`
5. **`serial_read` is drain-only** — fine for a quick peek, bad as sole log
6. Crashes: `serial_get_incidents` — critical panics may **abort** waits as
   `[ABORTED anomaly:…]` (not a quiet timeout)

Prompts (slash commands if the client supports them): `bringup`,
`debug_serial`, `flash_safe`.

## Flash coexistence

```
serial_suspend  →  esptool / OpenOCD / avrdude  →  serial_resume
```

Or CLI: `smolmux-cli with-port <cmd> …` (always resumes). Do not fight
TIOCEXCL by opening the same TTY while the broker holds it.

## ESP32 dual-USB boards

Many boards have **UART/bridge** (CH34x/CP210x/FTDI) and **native USB**.
VID/PID of a bridge names the **adapter**, not the MCU. Prefer the UART
port for reliable flash and continuous serial. After native-USB flash,
the port may re-enumerate — re-check `serial_list_ports`.

Built-in anomaly patterns (no profile required): Guru Meditation, brownout,
abort, stack smash, task WDT, `rst:0x…` (warning only; does not abort waits).

## Multi-client

Observers can read and `serial_wait_for`. Controllers write. If writes fail,
check status / takeover — another agent or human may hold control.
