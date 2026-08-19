# Board Exploration Workflow

How to use smolmux - manually or with an AI agent - to familiarize yourself with
a fresh embedded board: connect every wire you can, drive the board through its
debug and console interfaces, and capture what you learn into a
[board bring-up template](board-bringup-template.md).

This is the stable runbook. Copy `board-bringup-template.md` per board and fill
it in as you work through the steps below.

## Principles

- **One broker per wire, grouped into a board.** A smolmux broker holds exactly
  one link (one UART, or one GDB/SWD session). A board with SWD + two UARTs is
  three brokers on three sockets. Tag each broker with `--board <name> --role
  <label>` (e.g. `--board samc21-bench --role console`) so discovery can present
  them as one board: `smolmux-cli boards` groups the wires, and `--json` gives an
  agent the board -> wires structure. Still name sockets sensibly and keep the
  template's wiring table as the canonical record.
- **Read facts, don't guess them.** Confirm the silicon from the chip itself
  (CPUID, vendor ID registers, boot banner) before trusting the datasheet, and
  note any discrepancy. Guessed peripheral addresses waste time.
- **Capture as you go.** smolmux already logs every link (I/O log + text log).
  The template is the human-readable distillation; the logs are the raw record.

## Step 0 - Pre-known facts (before touching hardware)

Fill the template's **Pre-known facts** section from the datasheet / reference
manual / schematic: part number, core and architecture, clock tree, RAM and
flash sizes and their memory-map addresses, boot modes/strap pins, debug
transport (SWD/JTAG and its connector), each UART's pins and default baud,
ethernet/other interfaces, power rails. This is what you'll confirm or refute.

Write two smolmux profiles while you're here (both optional but useful):
- a **serial device profile** (`*.smolmux-profile.json`): `prompt_pattern`,
  `command_prefix`, `boot_banner`, `commands`, and `anomaly_patterns` for the
  console you expect (U-Boot, Linux, RTOS shell);
- a **GDB target profile** (`*.gdb-profile.json`): `arch`, `important_registers`,
  `fault_registers`, `peripheral_map` (name -> base address), `rtos`,
  `gdb_init_commands`. See `configs/nrf9151.gdb-profile.json` for the shape.

## Step 1 - Bring up the wires

**UART console** (repeat per UART):

```bash
smolmux /dev/ttyUSB0 -b 115200 -p configs/<board>.smolmux-profile.json \
        -s /tmp/smolmux-<board>-console.sock
```

Verify it's alive: `smolmux-monitor /tmp/smolmux-<board>-console.sock` (Ctrl-] to
exit) and power-cycle or reset the board - you should see the boot banner. Use
`--list-ports` to find serial devices.

**SWD/JTAG** - start a gdbserver (OpenOCD is typical), then a GDB broker:

```bash
openocd -f board/<board>.cfg          # or -f interface/<probe>.cfg -f target/<soc>.cfg
                                       # listens on :3333, prints the detected part
smolmux --gdb --gdb-path arm-none-eabi-gdb --gdb-target localhost:3333 \
        -s /tmp/smolmux-<board>-gdb.sock
```

OpenOCD's own scan usually names the part and flash/RAM sizes on connect - record
that. The broker keeps the SWD session open and multiplexes it.

**Don't know the chip yet?** `board/<board>.cfg` needs a part name you don't have.
[openocd-cold-attach.md](openocd-cold-attach.md) gets a generic Cortex-M DAP
attached (`-expected-id 0`, read the DPIDR + ROM table) just far enough to open
`:3333`, then hands off to the broker and `gdb_identify_target` to name the part.

Record each wire in the template's **Wiring & brokers** table: physical wire ->
probe/adapter -> broker command -> socket -> profile.

**Or bring up the whole board at once.** Instead of starting each broker by
hand, declare the board in a `*.board.json` manifest (the machine-readable form
of the wiring table) and let smolmux start every wire:

```json
{
  "board": "samc21-bench",
  "wires": [
    {"role": "console", "link": "uart", "device": "/dev/ttyACM1", "baud": 115200,
     "profile": "configs/linux-shell.smolmux-profile.json"},
    {"role": "swd", "link": "gdb", "gdb_path": "gdb-multiarch", "target": "localhost:3333",
     "profile": "configs/samc21.gdb-profile.json"}
  ]
}
```

```bash
smolmux-cli board list                           # manifests in ~/.config/smolmux + which are up
smolmux-cli board up configs/samc21.board.json   # start all wires (detached daemons)
smolmux-cli board status samc21-bench            # what's up
smolmux-cli board down samc21-bench              # stop all wires
```

Keep your manifests in `~/.config/smolmux/*.board.json` (or point `board list`
at a directory, or set `$SMOLMUX_BOARD_DIR`) so `board list` shows every board
you can bring up and which are currently running.

`board up` is **detached by default** - each wire runs as an independent daemon
that outlives the launcher (and self-heals link drops), matching how you leave a
bench up for days. It is idempotent (re-running skips wires already up). Use
`--foreground` for bounded/CI runs where the wires should die when the command
exits (Ctrl-C stops them all). Per-wire logs go to
`$XDG_RUNTIME_DIR|/tmp/smolmux-<board>-<role>.log`. `board down` finds the wires
by their `--board` label via live discovery and their pid via `SO_PEERCRED` - no
pidfile, nothing to go stale. See `configs/samc21.board.json` for a sample.

## Step 2 - Identify the silicon over SWD

Point the GDB MCP server at the SWD broker:

```bash
smolmux-gdb-mcp -s /tmp/smolmux-<board>-gdb.sock -p configs/<board>.gdb-profile.json
```

Then, via the `gdb_*` tools:

- **Fast path - `gdb_identify_target {}`.** Decodes the SCB CPUID (ARM Cortex-M
  core + `rNpM` revision), probes the CoreSight ROM table, and best-effort reads
  the well-known vendor ID registers (STM32 `DBGMCU_IDCODE`, SAM `DSU DID`, nRF
  `FICR`), returning structured JSON `{core, revision, vendor_guess, dev_id,
  flash_kb, ram_kb, evidence}`. Start here; the manual reads below are the
  fallback when `vendor_guess` is `unknown` (an unlisted vendor). The MCP prompt
  `probe_unknown_board` / resource `smolmux-gdb://board-probing` (see
  [board-probing.md](board-probing.md)) wrap this whole
  step for an agent.
- **CPUID** - `gdb_read_memory {address:"0xE000ED00", length:4}`. The SCB CPUID
  is architectural on all Cortex-M. Decode little-endian: byte 3 = implementer
  (`0x41` = ARM), the part-number field identifies the core (e.g. `0x410CC601` =
  Cortex-M0+ r0p1, `0x410FC241` = Cortex-M4). This is the ground-truth core check.
- **Vendor device ID** - chip-specific and datasheet-driven: read the part's ID
  register with `gdb_read_memory` (e.g. SAM `DSU/DID`, STM32 `DBGMCU_IDCODE` at
  `0xE0042000`, nRF `FICR`). Confirms the exact SoC, revision, and often flash/RAM
  size fields.
- **Memory map** - probe the expected flash (`0x00000000` / `0x08000000` / ...) and
  SRAM (`0x20000000`) with small `gdb_read_memory` reads to confirm they respond
  and roughly where they end.
- **gdbserver's own view** - `gdb_command {command:"monitor <cmd>"}` surfaces the
  target scan and flash-bank info the gdbserver already has (OpenOCD:
  `monitor mdw`, `monitor flash banks`, `monitor reg`).
- **State snapshot** - `gdb_reset {mode:"halt"}`, then `gdb_read_registers`
  (name-labeled), `gdb_backtrace`, `gdb_status`.
- **Persist a starter profile** - `gdb_generate_profile {path:"~/.config/smolmux/
  <board>.gdb-profile.json"}` writes a `*.gdb-profile.json` for the detected
  core: `arch`, the architectural register set, core-correct `fault_registers`
  (the SCB set on M3/M4/M7/M33, empty on M0/M0+/M23), and a `peripheral_map`
  pre-seeded with the fixed-address Cortex-M system/debug blocks (SCB, NVIC,
  SysTick, DCB, ROM table; +DWT/ITM/FPB/MPU on mainline) plus the confirmed
  vendor debug block (SAM `DSU` / STM32 `DBGMCU` / nRF `FICR`). Add the
  application peripherals (UART/SPI/GPIO) and `rtos` (pass `{rtos:"zephyr"}` to
  stamp it) from the datasheet. The profile is auto-discovered next session.

## Step 3 - Capture the console over UART

Point the serial MCP server at the console broker:

```bash
smolmux-mcp -s /tmp/smolmux-<board>-console.sock -p configs/<board>.smolmux-profile.json
```

- **Boot log** - reset the board, then `serial_output_history` for the full boot
  banner; `serial_get_incidents` for anything the anomaly patterns flagged.
- **Identify the running software** and probe it with `serial_send_command`:
 - U-Boot: `version`, `bdinfo`, `printenv`, `md <addr>`
 - Linux: `uname -a`, `cat /proc/cpuinfo`, `cat /proc/meminfo`, `dmesg | head`
 - RTOS/bare-metal shell: the shell's own `help`/`version`/`ps`
- Use `serial_pin_control` (DTR/RTS/break) or `serial_sysrq` where the target
  supports it; `serial_add_watchdog` to watch for a pattern while you work.

## Step 4 - Enumerate peripherals

For each entry in the GDB profile's `peripheral_map`:
`gdb_read_peripheral {name:"UARTE0", num_registers:16}` and cross-reference the
register dump against the datasheet's register table. Use `gdb_read_fault_registers`
after any crash (Cortex-M3/M4/M7/M33 - see caveats). `gdb_threads` gives the RTOS
thread list when the profile configures `rtos`/`rtos_commands`.

## Step 5 - Correlate and write the doc

Fill the template's **Discovered facts** from Steps 2-4. Explicitly note where
SWD and UART agree (e.g. CPUID core matches the boot banner's reported SoC) and
where reality diverges from the datasheet; write those down.

## Tool quick reference

**Serial (`smolmux-mcp`):** serial_send_command, serial_read, serial_write,
serial_port_status, serial_boot_status, serial_add_autoresponder,
serial_pin_control, serial_sysrq, serial_suspend, serial_resume,
serial_output_history, serial_get_incidents, serial_add_watchdog, serial_monitor,
serial_generate_report, serial_list_ports.

**GDB (`smolmux-gdb-mcp`):** gdb_launch, gdb_breakpoint, gdb_delete_breakpoint,
gdb_continue, gdb_interrupt, gdb_step, gdb_backtrace, gdb_read_registers,
gdb_read_memory, gdb_evaluate, gdb_threads, gdb_load, gdb_reset, gdb_status,
gdb_wait_stop, gdb_console_output, gdb_read_fault_registers, gdb_read_peripheral,
gdb_identify_target, gdb_generate_profile, gdb_command.

## Caveats (hardware notes)

- **Discovery and board grouping.** `smolmux-monitor -L` (or no-args when several
  brokers are up) lists every active broker and what it holds. For agents,
  `smolmux-cli brokers --json` returns a flat machine-readable array (socket,
  board, role, link_type, endpoint, baud, connected, suspended, clients), and
  `smolmux-cli boards --json` returns the same grouped by `--board` - the "board
  object" is derived live from discovery, with no central registry to go stale.
  Enumerate first, then target a wire's socket with `-s`.
- **No ethernet link.** smolmux talks to boards over UART and SWD/GDB only. TCP/WS
  are client-facing sinks, not a way to explore the board over its network port.
- **Cortex-M0/M0+ have no configurable fault registers** (CFSR/HFSR are M3/M4/M7/
  M33). Give those targets a profile with `"fault_registers": []` so
  `gdb_read_fault_registers` reports "none" instead of reading reserved addresses.
- **`gdb_step {mode:"next"}` over a loop/print on a slow SWD link can hang** the
  wait - gdb effectively single-steps it. Recover with `gdb_interrupt`, then
  `gdb_wait_stop`; prefer `stepi`/`finish` or a breakpoint + `gdb_continue`.
- **The broker health is the debug/serial process, not the target.** After a
  target dies and reconnects, re-run `gdb_launch` to reload symbols before
  source-level work.
