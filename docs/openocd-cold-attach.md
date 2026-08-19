# OpenOCD cold-attach to an unknown board

Getting a gdbserver onto a board whose chip you **don't yet know** is OpenOCD's
job, not smolmux's - but it's the prerequisite for the smolmux GDB broker and
`gdb_identify_target`. This is the missing first rung: the usual
`openocd -f board/<board>.cfg` needs a board/target config that *names the
silicon*, which is exactly what you're trying to discover. This doc gets you
attached generically - far enough to open the `:3333` GDB port - then hands off
to smolmux to identify the part.

Verified against OpenOCD `0.12.0`. Scripts live in
`/usr/(local/)share/openocd/scripts` (`interface/`, `target/swj-dp.tcl`).

## What you must know vs. what you can discover

You cannot avoid knowing two things; everything else is discoverable:

1. **Your debug adapter** - the probe physically wired to the board (on-board
   CMSIS-DAP/EDBG, ST-Link, J-Link, or an FTDI dongle). Pick the matching
   `interface/*.cfg`: `cmsis-dap.cfg`, `stlink.cfg`, `jlink.cfg`, or an FTDI
   file. This is about the *probe*, not the target.
2. **The wire protocol** - SWD or JTAG. Nearly all Cortex-M debug ports are
   **SWD** (2-wire: SWCLK/SWDIO). Try SWD first; fall back to JTAG.

Discoverable once attached: the DP IDCODE (confirms SWD is alive + designer),
the CoreSight ROM table (names the core and debug components), and - via smolmux
 - the CPUID and vendor ID registers (the exact SoC).

## Generic SWD cold-attach (no board file)

A generic Cortex-M DAP, inline so you write no file. Replace the first
`-f interface/...` line with your actual adapter:

```bash
openocd \
  -f interface/cmsis-dap.cfg \
  -c 'transport select swd' \
  -c 'adapter speed 1000' \
  -c 'reset_config none' \
  -c 'swj_newdap chip cpu -expected-id 0' \
  -c 'dap create chip.dap -chain-position chip.cpu' \
  -c 'target create chip.cpu cortex_m -dap chip.dap' \
  -c 'init' \
  -c 'dap info 0'
```

The load-bearing details:

- **`-expected-id 0`** disables the IDCODE match - OpenOCD accepts *whatever* DP
  IDCODE it reads instead of rejecting a mismatch. This is what lets a generic
  config attach to an unknown part.
- **`target create ... cortex_m`** is the generic ARM Cortex-M target type. It's
  correct for M0/M0+/M3/M4/M7/M23/M33 - enough to halt the core and read memory,
  which is all `gdb_identify_target` needs. (It is *not* enough to flash - that
  needs the real target driver once you've identified the part.)
- **`adapter speed 1000`** (1 MHz) is conservative. On long jumper wires or an
  unknown board, drop to `100`-`500` if the connect is flaky, then raise it.
- **`reset_config none`** avoids driving reset lines you haven't confirmed are
  wired. If the board has a working SRST and the target won't halt, try
  `reset_config srst_only` and add `-c 'reset halt'`.

## What OpenOCD tells you at connect

On a successful attach OpenOCD prints the identity it already knows. Read it:

```
Info : SWD DPIDR 0x2ba01477
Info : chip.cpu: hardware has 6 breakpoints, 4 watchpoints
```

- **`SWD DPIDR 0x2ba01477`** - SWD is alive and this is the ARM ADIv5 Debug Port.
  The low 12 bits `0x477` encode JEP106 designer = **ARM** (any `0x...477` DP is
  ARM-designed). The upper bits distinguish DP versions/parts, e.g. `0x0bc11477`
  ~ an M0+-class DP, `0x2ba01477` ~ an M3/M4-class DP - indicative of the core
  family, not the exact SoC.
- **`dap info 0`** walks the CoreSight ROM table and prints each component's
  Peripheral/Component ID, e.g. `Part is Cortex-M4 SCS (System Control Space)`.
  This is OpenOCD's own version of the ROM-table walk - it confirms the core and
  which debug components (SCS/DWT/FPB/ITM/TPIU) are present.
- **`monitor` from gdb** later surfaces the same: `monitor dap info`,
  `monitor mdw 0xE000ED00` (CPUID), `monitor reg`.

If the connect fails with `DAP transaction stalled` / `Could not initialize the
debug port`, it's usually: wrong transport (try JTAG), the target held in reset
or unpowered, SWD pins not muxed to debug yet (some parts gate the AP behind a
power/boot sequence), or the part is read-out-protected (RDP/APPROTECT) - see
gotchas.

## Keep the GDB port open, then hand off to smolmux

The recipe above runs the `-c` commands and, because none of them block, OpenOCD
proceeds to its normal server loop with the GDB port on **`:3333`** and the
telnet/monitor port on `:4444`. (Add `-c 'echo READY'` at the end as a marker.)
Leave it running, then bring up the smolmux GDB broker against it and identify
the part properly:

```bash
smolmux --gdb --gdb-path gdb-multiarch --gdb-target localhost:3333 \
        -s /tmp/smolmux-unknown-gdb.sock
smolmux-gdb-mcp -s /tmp/smolmux-unknown-gdb.sock
#   -> gdb_identify_target {}   (CPUID + vendor ID decode)
#   -> or the probe_unknown_board prompt / smolmux-gdb://board-probing resource
```

OpenOCD's generic view (DPIDR + ROM table) and smolmux's CPUID + vendor-ID
decode corroborate each other. Once the SoC is known, swap the generic
`openocd -c ...` for the real `-f target/<soc>.cfg` (or `board/<board>.cfg`) to
unlock flashing, correct RAM/flash banks, and reset semantics - and capture all
of it in the [board bring-up template](board-bringup-template.md).

## JTAG fallback

If SWD won't come up (no DPIDR), the board may be JTAG-only or strapped to JTAG.
Scan the chain - OpenOCD prints every TAP's IDCODE, which is the discovery:

```bash
openocd \
  -f interface/cmsis-dap.cfg \
  -c 'transport select jtag' \
  -c 'adapter speed 1000' \
  -c 'jtag newtap chip cpu -irlen 4 -expected-id 0' \
  -c 'dap create chip.dap -chain-position chip.cpu' \
  -c 'target create chip.cpu cortex_m -dap chip.dap' \
  -c 'init' \
  -c 'scan_chain'
```

```
Info : JTAG tap: chip.cpu tap/device found: 0x4ba00477 (mfg: 0x23b (ARM Ltd), part: 0xba00, ver: 0x4)
```

`scan_chain` / the `tap/device found` lines list each IDCODE and decode the JEP106
manufacturer. `-irlen 4` is the Cortex-M default; if the scan reports an IR
length mismatch, that number *is* a clue about the chain. Multiple TAPs mean
multiple devices on the chain (e.g. an FPGA + MCU) - each needs its own
`jtag newtap`.

## Gotchas (unknown-board reality)

- **Adapter speed.** The single biggest cause of flaky cold-attach. Start slow
  (`adapter speed 100`), attach, then raise. SWD over long dupont wires is
  unreliable above ~1 MHz.
- **Target in reset / unpowered.** No DPIDR at all usually means no power to the
  target debug logic or SRST asserted. Confirm the board is powered and, if you
  wired SRST, try `reset_config srst_only srst_nogate` + `reset halt`.
- **SWD multidrop (SWDv2).** Some boards (nRF53, multi-core) put several DPs on
  one SWD line addressed by a target-select value. A plain attach hits the
  default; a specific core needs `swd newdap`/`dap create ... -dp-id <IDR>
  -instance-id <N>`. Symptom: DPIDR reads but the AP/ROM table looks empty.
- **Access-port protection.** RDP (STM32), APPROTECT (nRF52/53), DSU security
  (SAM) can block memory reads after connect - you'll see the DP but `dap info`
  or CPUID reads fault. That's not a wiring bug; it's the part refusing debug.
  Recovery is vendor-specific (mass-erase via the vendor's protocol) and out of
  scope here - note it and move on.
- **`cortex_m` vs the real driver.** The generic target halts and reads memory
  but doesn't know the flash banks or the correct reset sequence. Don't try to
  `load`/flash through it - identify the part first, then use the real config.

See also: [board-exploration-workflow.md](board-exploration-workflow.md) (Step 1
wires it in), [board-probing.md](board-probing.md).
