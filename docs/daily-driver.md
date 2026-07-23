# smolmux Daily Driver Configuration

Recommended build and runtime configuration for daily embedded work (U-Boot
bring-up, Zephyr, Linux, YMODEM, MCP clients).

## Recommended Build (UART + MCP + Watcher only)

```bash
cd /path/to/smolmux          # clone or release tree
cp configs/defconfig.embedded .config
cmake -B build -DSM_MUSL_STATIC=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build -j$(nproc)
```

What this build includes:
- `defconfig.embedded` = UART link + MCP sink + Watcher. No GDB, no TCP, no WebSocket.
- Binary stays small (~150-250 KB static) with a narrow feature set.
- `SM_MUSL_STATIC=ON` produces a single portable binary you can scp to any target or lab machine.

**Alternative minimal build (pure UART, no MCP):**
```bash
cp configs/defconfig.minimal .config
cmake -B build ...
```
(`defconfig.minimal` is UART-only - no sinks, no watcher. Note: `defconfig.uart`
still enables the MCP sink and the watcher, so it is not the no-MCP profile.)

## Recommended Runtime Usage

**Quick start (first USB serial adapter):**

```bash
./build/smolmux /dev/ttyUSB0 -b 115200 -v
# other terminal:
./build/smolmux-monitor /dev/ttyUSB0
```

**Convenience launcher** (dated logs under `~/smolmux-logs/`, optional profile):

```bash
# U-Boot-style console (uses configs/uboot.smolmux-profile.json)
./scripts/smolmux-daily uboot /dev/ttyUSB0

# Same profile via STM32MP157 alias
./scripts/smolmux-daily stm32mp157 /dev/ttyUSB0

# No profile
./scripts/smolmux-daily generic /dev/ttyUSB0
```

### Manual equivalent (full control)

```bash
./build/smolmux /dev/ttyUSB0 \
  -b 115200 \
  -p configs/uboot.smolmux-profile.json \
  -l ~/smolmux-logs/run-$(date +%Y%m%d_%H%M) \
  -t ~/smolmux-logs/run-$(date +%Y%m%d_%H%M) \
  -v
```

Always pass `-t` (text log) if you want greppable post-mortems under your log
directory - text logs are line-flushed so live sessions do not leave 0-byte files.

**Flags used most days:**
- `-p` / `--profile` - almost always
- `-l` - I/O JSONL logs (post-mortems)
- `-t` - human-readable text logs with daily rotation
- `-v` - debug logging (turn off once stable)

### Breaking into a `bootdelay=0` bootloader

U-Boot with `bootdelay=0` gives a ~0 ms window to hit a key - too fast to win
*reactively* (watch-then-send through a client or an AI agent always loses the
race). Use the broker-side flood, which streams the key from inside the event
loop and stops the instant the prompt appears:

```bash
smolmux-cli break-uboot                 # spam space, stop on the "=> " prompt
smolmux-cli break-uboot --key q --duration 3000 --interval 5
# Stop patterns vary by vendor prompt (ERE; engine-dependent case rules):
smolmux-cli break-uboot --stop 'STM32MP>|=>|U-Boot>|Hit any key'
```

Then power-cycle / reset the board *while it runs* (or pair with a reset): the
keys are already in flight when the window opens, so it doesn't need to react in
0 ms. Exit 0 = prompt reached, 2 = flooded but no prompt seen. It's the
`interrupt_autoboot` wire message underneath - any client (monitor/CLI/MCP) can
send it.

**Duration:** default `--duration` (~2s) is often enough for a true
`bootdelay=0` window *during* boot. For **reboot-from-Linux** (or a full
power-on while flooding), use **15000** ms - the maximum; the broker clamps
longer requests - so the flood covers the whole path back to U-Boot. Cap
automated retries (e.g. 2) then power-cycle by hand.

**Which wire:** if the board has both early-boot UART and a USB gadget ACM
console, bind break-uboot to the **early** UART *before* reboot - ACM usually
drops until userspace returns. See
[Linux console login and multi-wire](#linux-console-login-and-multi-wire).

Do **not** implement the flood as many small MCP `serial_write` spaces after
SysRq - client latency loses `bootdelay=0`. Use broker-side `break-uboot`.

If the board's reset is wired to a modem line (DTR/RTS on the adapter), let
smolmux drive the whole `reset_and_interrupt` sequence - it asserts reset,
starts the flood, and releases reset after a hold so keys are already streaming
when the device boots (no manual power-cycle):

```bash
smolmux-cli break-uboot --reset dtr                 # DTR reset, active-low
smolmux-cli break-uboot --reset rts --reset-active high --reset-hold 200
```

`--reset <dtr|rts>` picks the line, `--reset-active <low|high>` its polarity
(default low - asserting reset = clearing the line), `--reset-hold <ms>` the
pulse width (default 100, must be < `--duration`). Only works on a real serial
port; a PTY/virtual port can't drive modem lines and the request is rejected.
Many USB-UART dongles do **not** wire RTS/DTR to board NRST - `--reset` then
does nothing; use a physical reset or power-cycle while the flood runs.

### Tracking cold-boot progress (which stage did it die at?)

The flood gets you *into* U-Boot; boot-stage tracking tells you *where a boot
stalled* when it doesn't reach a shell. Declare an ordered list of boot markers
in the device profile:

```json
"boot_stall_timeout_ms": 15000,
"boot_stages": [
    {"name": "spl",       "pattern": "U-Boot SPL"},
    {"name": "uboot",     "pattern": "U-Boot 20"},
    {"name": "kernel",    "pattern": "Starting kernel"},
    {"name": "linux",     "pattern": "Linux version"},
    {"name": "userspace", "pattern": "Freeing unused kernel"},
    {"name": "login",     "pattern": "login:"}
]
```

(This is the pipeline bundled in `configs/linux-shell.smolmux-profile.json`.)
The broker matches these on the console stream and, per stage reached,
broadcasts a `boot_stage` event (`{name, index, total, timestamp}`) to every
client. A `status` query returns a `boot` object:

```json
"boot": {
    "furthest": 2, "total": 6, "stalled": true, "terminal_reached": false,
    "stages": [ {"name":"spl","reached":true,"timestamp":...}, ... ]
}
```

`smolmux-cli status` renders this as a checklist:

```
Boot:      3/6 stages (furthest: kernel) - in progress
  [x] spl
  [x] uboot
  [x] kernel  <-- furthest
  [ ] linux
  [ ] userspace
  [ ] login
```

So "reached kernel handoff, then stalled before Linux banner" is a single status
read instead of eyeballing the log. `stalled` goes true when no new stage is
reached for `boot_stall_timeout_ms` and the terminal (last) stage is unreached.
The broker also **pushes** a one-shot `boot_stall {name, index, total, stalled_ms}`
event the moment that timeout elapses mid-boot, so a watcher/agent is notified
without polling (it re-arms on each advance and never fires once fully booted).
Matching is order-tolerant - a dropped intermediate marker never blocks a later
one. Progress resets on link reconnect or when the first stage re-appears (a new
boot), so a power-cycle or `reboot` starts the count fresh.

For scripting there is `smolmux-cli boot-status`, which prints the same checklist
and exits by state: `0` complete, `2` stalled, `3` in-progress/not-started, `4`
no stages declared (`1` on error). So a bring-up script can block on the boot:

```bash
until smolmux-cli boot-status >/dev/null; do
  [ $? -eq 2 ] && { echo "boot stalled"; break; }   # exit 2 = stalled
  sleep 1
done
```

`smolmux-monitor` also prints boot events inline as they happen - green
`[boot] reached <stage> (n/total)` and, on a stall, red `[boot STALLED] at
<stage> (n/total) after <ms>ms` - and MCP agents can call `serial_boot_status`.

### Answering prompts automatically (autoresponder)

For prompts that always want the same answer - a boot menu, a `[y/N]`
confirmation, an unattended login - register a standing rule and the broker
answers it straight from the read path, no client in the loop:

```bash
# Match the on-screen prompt "Continue? [y/N]".
# --send is the letter y followed by newline (Enter), not the text "y/".
smolmux-cli autorespond add --name confirm \
  --pattern 'Continue\? \[y/N\]' --send $'y\n'
# auto-login once (--once disables the rule after it fires)
smolmux-cli autorespond add --name login --pattern 'login:' --send $'root\n' --once
smolmux-cli autorespond list
smolmux-cli autorespond remove --name confirm
```

For **empty-password** factory images (still show `Password:`), see
[Linux console login and multi-wire](#linux-console-login-and-multi-wire) -
do not enable blank-password autorespond on boards with a real password.

`--send` interprets `\n \r \t \0 \\` escapes (or use bash `$'...'` as above).
A per-rule cooldown (default 1000ms) stops it re-firing while the same prompt
is still on screen; `--once` fires exactly once. This is the
`configure_autoresponder` wire message;
`smolmux-monitor` shows a cyan `[autorespond] <name> fired` line each time one
triggers, and MCP agents can register rules with `serial_add_autoresponder`.
Distinct from `send_expect` (a one-shot, client-driven wait) - an autoresponder
is a standing broker rule that keeps answering.

### When you need GDB mode

```bash
./build/smolmux --gdb \
  --gdb-target localhost:3333 \
  -p configs/nrf9151-zephyr.smolmux-profile.json
```

Optional: `--gdb-path /path/to/gdb` if not on `PATH`.

**Security note - GDB shell guard.** GDB's MI passthrough can run host commands
(`shell`, `python`, `guile`, `make`, ...). smolmux blocks these by default; opt in
with the link param `allow_shell=1` only if you trust every client. This guard
is **best-effort, not a security boundary** - a blocklist cannot be complete for
GDB. If you expose a GDB link to untrusted controllers, confine the broker at
the OS level (seccomp/namespaces) rather than relying on it.

**GDB link behavior notes** (verified against OpenOCD + a Cortex-M target):
- smolmux enables `mi-async` at gdb spawn so gdb keeps answering MI commands
  while the target runs. Without it every command would silently queue in
  gdb's stdin pipe until the next stop. It cannot be toggled later - gdb
  rejects the setting once attached to a live inferior.
- A BREAK (`pin_control` pin=`break`) maps to `-exec-interrupt` and halts a
  running target. Sending it while the target is already halted plants a
  pending interrupt in gdb that immediately re-stops the *next*
  `-exec-continue` - that is gdb semantics, not a broker bug.
- The broker's health is the gdb process, not the debug target: `status` says
  connected even after the remote target dies (gdb reports
  `^error,msg="Remote connection closed"` in the stream on the next target
  operation). Re-attach with the link param `target=<host:port>`.
- After a link auto-reconnect (`link_down`/`link_up`), the fresh gdb has no
  symbol table - re-send `-file-exec-and-symbols` before source-level work.

**High-level GDB tools for agents (`smolmux-gdb-mcp`).** For AI-driven
debugging, point the dedicated GDB MCP server at the broker instead of the
serial `smolmux-mcp`:

```bash
./build/smolmux-gdb-mcp -s /tmp/smolmux-gdb.sock \
  -p configs/nrf9151.gdb-profile.json
```

It exposes 21 tools - `gdb_breakpoint`,
`gdb_continue`, `gdb_interrupt`, `gdb_step`, `gdb_backtrace`,
`gdb_read_registers` (name-labeled), `gdb_read_memory`, `gdb_evaluate`,
`gdb_threads` (RTOS-aware), `gdb_load`, `gdb_reset`, `gdb_wait_stop`,
`gdb_read_fault_registers` (decodes CFSR/HFSR + MMFAR/BFAR), `gdb_read_peripheral`,
`gdb_identify_target` and `gdb_generate_profile` (unknown-board probing, below),
`gdb_command` (raw MI), and more - plus 3 prompts (`diagnose_fault`,
`analyze_crash`, `probe_unknown_board`) and 2 resources
(`smolmux-gdb://target/profile`, `smolmux-gdb://board-probing`). It keeps the
broker/link dumb: each MI command
is token-tagged and its result is matched out of the raw output stream. The
**target profile** (`*.gdb-profile.json`, discovered under
`~/.config/smolmux/` or via `-p` / `SMOLMUX_GDB_PROFILE`) supplies the important
registers, Cortex-M fault-register addresses, peripheral base addresses, and
RTOS commands. `gdb_command` refuses `shell`/`pipe`/`python`/`guile`/`make`
client-side (the link's guard also blocks them).

**Validated on a SAM C21 Xplained Pro (Cortex-M0+).** Notes from that setup:
- On a Cortex-M0+ there are no configurable fault registers (CFSR/HFSR are
  M3/M4/M7). Use a profile with `"fault_registers": []` - `gdb_read_fault_registers`
  then reports "No fault_registers in target profile" instead of reading
  reserved addresses. The bundled `configs/nrf9151.gdb-profile.json` is for the
  M33-class nRF9151, where they apply.
- `gdb_step {mode:"next"}` over a line that loops/calls (e.g. a UART-print
  busy-loop) can take a long time on a slow SWD link because gdb effectively
  single-steps it - `gdb_wait_stop` may time out with the target still running.
  Recover with `gdb_interrupt` (then `gdb_wait_stop`). Prefer `stepi`/`finish`
  or a breakpoint + `gdb_continue` for such lines.

**Probing a board you don't have a profile for.** When you sit down at an
unfamiliar board over SWD, let the agent identify it before you hand-write a
profile:

1. `gdb_identify_target` - decodes the SCB CPUID into the ARM Cortex-M core +
   revision and probes the vendor ID registers (STM32 `DBGMCU`, SAM `DSU`, nRF
   `FICR`) to guess the SoC, with an evidence trail. Halt the target first
   (`gdb_reset {mode:"halt"}` or `gdb_interrupt`).
2. `gdb_generate_profile {path:"~/.config/smolmux/<board>.gdb-profile.json"}` - 
   writes a starter profile for the detected core (arch, register set,
   core-correct `fault_registers`, a `peripheral_map` seeded with the
   architectural Cortex-M + vendor debug blocks). Auto-discovered next session
   under `~/.config/smolmux/` (or pass `-p`); then fill in application
   peripherals (UART/SPI/GPIO) and `rtos` from the datasheet.

The `probe_unknown_board` prompt (or the `smolmux-gdb://board-probing` resource)
lists the same tool sequence. If OpenOCD can't attach because the chip is
unknown, see [openocd-cold-attach.md](openocd-cold-attach.md) for the generic
SWD bootstrap. Validated on a SAM C21 (Cortex-M0+ / Microchip SAM). Full
runbook: [board-exploration-workflow.md](board-exploration-workflow.md).

## Managing multiple wires: brokers & boards

A board usually has several wires - SWD plus one or more UARTs - and each wire is
its own broker (one broker holds one link). smolmux discovers and groups them so
you never have to remember socket paths.

**Discover what's running:**

```bash
smolmux-cli brokers          # every active broker: link, device/target, clients, pid
smolmux-cli boards           # the same, grouped by --board
smolmux-monitor -L           # human listing (no-arg monitor also lists when >1 is up)
```

The `smolmux-cli` variants take `--json` for agents.

**Group a board's wires** by tagging each broker at start with `--board`/`--role`:

```bash
smolmux /dev/ttyACM1 --board samc21 --role console -s /tmp/smolmux-samc21-console.sock
smolmux --gdb --gdb-target localhost:3333 --board samc21 --role swd -s /tmp/smolmux-samc21-gdb.sock
```

**Or bring up the whole board with one command** from a `*.board.json` manifest
(the machine-readable wiring table - see `configs/samc21.board.json` and the
format in `docs/board-exploration-workflow.md`):

```bash
smolmux-cli board list                           # manifests in ~/.config/smolmux + which are up
smolmux-cli board up   configs/samc21.board.json # start all wires (detached daemons)
smolmux-cli board status samc21-bench            # what's up
smolmux-cli board down   samc21-bench            # stop all wires (SIGTERM)
```

`board up` is **detached by default** - each wire is an independent daemon that
outlives the launcher and self-heals link drops - and idempotent (re-running
skips wires already up). Use `--foreground` for bounded/CI runs where the wires
should die when the command exits (Ctrl-C stops them all). `board down` finds a
board's wires by their `--board` label and their pid via `SO_PEERCRED`, so there
is no pidfile to go stale.

## Linux console login and multi-wire

Lessons from factory Buildroot/BusyBox-style images and boards with more than
one serial console.

### Empty password still shows `Password:`

Many embedded images (Buildroot, BusyBox `getty`/`login`) prompt for a password
even when the password is **empty**. Agents and scripts that assume
"no password ⇒ jump straight to shell" will type the next command (`uname -a`,
…) as the password and cascade `Login incorrect`.

**Correct sequence** (user `root`, empty password):

```text
see "login:"     →  write "root\n"
see "Password:"  →  write "\n" only   (empty password)
see a line "#"   →  shell ready; then send --expect '#'
```

**Never** send a shell command while the last prompt was `Password:`. Prefer
raw `write` for the login dance; use `send --expect` only after the shell
prompt is confirmed. Keep one command per send.

**State machine for agents** (use the **latest** prompt in history, not any
match earlier in the ring buffer):

```text
if last prompt is "Password:"  → write "\n" only
elif last prompt is "login:"   → write "root\n"   (or the real user)
elif last line matches shell   → safe for send --expect '#' or '$'
else                           → write "\n", re-read; do not invent passwords
```

Optional **autorespond** for known empty-password factory images only
(dangerous on boards with a real password - opt-in per board, not a default
in the generic `linux-shell` profile):

```bash
smolmux-cli autorespond add --name login --pattern 'login:' --send $'root\n' --once
smolmux-cli autorespond add --name empty-pw \
  --pattern 'Password:' --send $'\n' --once --cooldown 2000
```

MCP: `serial_add_autoresponder` with the same semantics.

### Multi-console boards

Boards often expose **two** (or more) console paths:

| Wire                                | When live                       | Use for           |
| ----------------------------------- | ------------------------------- | ----------------- |
| Early boot UART (U-Boot / earlycon) | Cold boot; often through reboot | U-Boot break-in   |
| USB gadget ACM (or 2nd UART)        | After userspace / UDC           | Linux login shell |

Flooding several candidate ports during break-uboot can be fine; **login** must
use the wire that actually shows `login:` / shell. Across reboot, gadget ACM
often drops until userspace - keep a broker on the early UART for U-Boot work.

### BusyBox vs desktop Linux

Generic `linux-shell` commands include `journalctl` / `ps aux` (desktop-ish).
On BusyBox factory images, prefer `logread`, `dmesg`, and plain `ps` when the
desktop tools are missing. Put board-specific commands in a **board profile**,
not by rewriting the generic pack for one product.

### Link health on an idle shell

A quiet `#` prompt can trip "no data received for Ns" link-health warnings.
That is not a board fault if status still shows Connected and the last line is
a shell prompt - do not treat it as a critical disconnect by default.

### Checklist: agent attach to a Linux factory console

1. Identify wires (`smolmux --list-ports`, by-id); note which shows login vs U-Boot.
2. Start broker: `smolmux <dev> -b 115200 -p <profile>` (socket auto-derived).
3. `smolmux-cli history` / `read` - classify state (login / password / shell / uboot).
4. Login state machine above; do not invent passwords.
5. Only then fingerprint, logs, host-side protocol tests (serial ≠ Ethernet test roles).
6. U-Boot: broker on early UART;  
   `break-uboot --duration 15000+ --stop '…'`; max ~2 automated tries then human power-cycle.
7. Human monitor: `smolmux-monitor <socket>` (positional path; no `-s`).
   Add `-c` only if you need controller role (default is observer).

## Clients

### Client flags (socket pin) - do not mix them up

Clients talk to the **broker Unix socket**, not the tty (discovery may accept a
device path as a *hint* to find the matching socket).

| Client                  | Pin a socket      | Notes                             |
| ----------------------- | ----------------- | --------------------------------- |
| `smolmux` (broker)      | `-s` / `--socket` | Optional; auto-derived if omitted |
| `smolmux-cli`           | `-s` / `--socket` | Omit `-s` to auto-discover        |
| `smolmux-mcp` / gdb-mcp | `-s`              | Same idea as cli                  |
| `smolmux-watcher`       | `-s`              | Same idea as cli                  |
| `smolmux-monitor`       | positional only   | No `-s`. `-c` = controller role   |

```bash
smolmux /dev/ttyUSB0 -b 115200 -p configs/linux-shell.smolmux-profile.json
# Auto socket: $XDG_RUNTIME_DIR/smolmux-ttyUSB0.sock (or /tmp/…)

smolmux-cli status                          # discovery when unambiguous
smolmux-cli -s /run/user/$UID/smolmux-ttyUSB0.sock status
smolmux-monitor /run/user/$UID/smolmux-ttyUSB0.sock
smolmux-monitor -c /run/user/$UID/smolmux-ttyUSB0.sock   # controller + path
# wrong: smolmux-monitor -s …               # unrecognized option: s
```

**Socket paths from long by-id devices:** the broker auto-derives a path that
always fits Unix domain sockets. Short basenames stay readable
(`…/smolmux-ttyUSB0.sock`); long USB by-id basenames use a stable shortened
form (`…/smolmux-<prefix>-<8hex>.sock`). Clients still find the broker via
glob / discovery. Use `-s` only when you want an explicit path.

### Monitor (terminal)

```bash
./build/smolmux-monitor /tmp/smolmux-ttyUSB0.sock
# or: ./build/smolmux-monitor -c /tmp/smolmux-ttyUSB0.sock  # controller role
# or if using TCP sink (advanced)
./build/smolmux-monitor --tcp 192.168.1.42:5555
```

### MCP (for Claude Code / Cursor / other agents)

**Recommended pattern:** run the broker as a normal process, then attach the
standalone MCP server (preferred over in-process `--mcp`):

```bash
# Terminal 1 - broker
./build/smolmux /dev/ttyUSB0 -b 115200 -p configs/uboot.smolmux-profile.json -v

# Terminal 2 - optional human monitor
./build/smolmux-monitor /dev/ttyUSB0
```

Example MCP client config (adjust absolute paths to your install):

```json
{
  "mcpServers": {
    "smolmux": {
      "command": "/path/to/smolmux/build/smolmux-mcp",
      "args": ["/dev/ttyUSB0"],
      "env": {}
    }
  }
}
```

`smolmux-mcp` discovers the broker socket from the port name. For a GDB/SWD
broker, use `smolmux-gdb-mcp` instead.

**Alternative (single process):** broker `--mcp` embeds MCP on stdio:

```json
{
  "mcpServers": {
    "smolmux": {
      "command": "/path/to/smolmux/build/smolmux",
      "args": [
        "/dev/ttyUSB0",
        "-b", "115200",
        "-p", "configs/uboot.smolmux-profile.json",
        "--mcp"
      ]
    }
  }
}
```

Useful serial tools include `serial_send_command`, `serial_read`,
`serial_boot_status`, `serial_add_autoresponder`, `serial_suspend`,
`serial_resume` (full list: `tools/list` from the MCP server).

## Device Profiles Strategy

Keep one high-quality profile per major target family:

- `uboot.smolmux-profile.json` (critical for `bootdelay=0` work)
- `linux-shell.smolmux-profile.json`
- `nrf9151-zephyr.smolmux-profile.json` (generic Zephyr/nRF9151-style console)
- Board manifests: `samc21.board.json`, `newboard.board.json`

Put target-specific prompt patterns, common commands, and anomaly patterns in the profile so your daily command line stays short.

## Sharing the port with external tools (flash, YMODEM, other debuggers)

smolmux holds the UART **exclusively** (`TIOCEXCL`): while it is live, any other
program that opens the same `/dev/ttyX` gets `Device or resource busy`. That is
by design - it prevents two writers from garbling the stream. When you need to
hand the wire to another tool, **suspend** the broker: it closes the port
(dropping the exclusive lock), lets the tool run, then **resume** re-acquires it.

```bash
smolmux-cli suspend        # broker closes the port; clients see [suspended]
avrdude ... / west flash / sb file / minicom ...   # external tool owns the wire
smolmux-cli resume         # broker re-opens; clients resume
```

From an interactive monitor session you don't need a second terminal: press the
prefix key then **`z`** to suspend and **`Z`** to resume (`Ctrl-]` `z` by
default). Suspend/resume is controller-only - press `c` or `t` first if you
connected as an observer.

**For scripts, use `with-port` - it can't leave the port stranded.** It
suspends, runs the command, and **always** resumes, even if the command fails or
is interrupted:

```bash
smolmux-cli with-port west flash            # suspend -> flash -> resume, atomically
smolmux-cli with-port sh -c 'flash && test' # a shell pipeline, if you need one
```

The command inherits your stdio and its exit code becomes `with-port`'s exit
code (so `with-port flash && smolmux-cli send ...` chains correctly). If the
broker can't be suspended, the command is **not** run. This removes the
forget-to-suspend race entirely and is the recommended form for flash-then-test
loops.

**Gotchas:**

- **Suspend first - don't just start the other tool.** If you skip suspend, the
  external tool is correctly blocked with `EBUSY`, but the broker's
  auto-reconnect (2-60 s backoff) will race to reclaim the port the moment the
  tool releases it, which can land mid-workflow. Always `suspend` -> tool ->
  `resume`.
- **GDB targets:** suspending a `--gdb` broker kills its `gdb` process, freeing
  OpenOCD's single gdb-client slot so another front-end can attach. It does
  **not** release the debug probe itself - if you need the *probe* (nrfjprog,
  pyocd, a fresh OpenOCD), you must also stop OpenOCD.

Use `suspend` / `resume` / `with-port` so the broker fully releases the serial
fd (or GDB) while an external flasher owns the wire.

## Success Metrics for This Configuration

- A session starts in under 15 seconds from cold.
- `bootdelay=0` U-Boot interrupt is a broker-side primitive (`smolmux-cli
  break-uboot`), so it wins the race regardless of client/agent latency.
- YMODEM works cleanly via `suspend` -> external `sb`/`sz` -> `resume`.
- No zombie brokers or silent data loss after suspend/resume cycles.
- Logs are always available under your `-l`/`-t` directory (the `smolmux-daily`
  launcher uses `~/smolmux-logs/`) for post-mortems. With no `-l`, the I/O JSONL
  log defaults to `/tmp`; with no `-t`, text logs default to
  `~/.local/share/smolmux/logs`.

## What NOT to do in the daily embedded build

- Do not build in GDB/TCP/WebSocket you will not use - stick with `defconfig.embedded` (UART + MCP + watcher).
- Do not run multiple different smolmux builds at the same time on the same machine (socket name collisions).
- Do not use complex command lines. If you need more than `-b`, `-p`, `-l`, `-t`, `-v` - something is wrong with the defaults or profile.

## Quick Health Check Commands

```bash
# What brokers/boards are up right now?
./build/smolmux-cli brokers
./build/smolmux-cli board list

# Is the broker healthy?
./build/smolmux-cli status

# Check recent incidents (queried from the broker over the socket)
./build/smolmux-cli incidents | tail -20

# Watch real-time text log (filename is <port>-YYYYMMDD.log, no dashes;
# replace the dir with whatever you passed to -t, e.g. ~/smolmux-logs/<run>)
tail -f ~/smolmux-logs/*/ttyUSB0-$(date +%Y%m%d).log
```

This configuration is intentionally opinionated. The goal is to make smolmux *better* than a `minicom`/`screen` + ad-hoc scripts setup for real work, not more flexible in ways that add cognitive load.
