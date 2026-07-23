# Demo transcript: unknown-board probe (SAM C21)

**Board:** Microchip **SAM C21 Xplained Pro** (eval kit, Cortex-M0+).  
**Date:** 2026-07-07 (on hardware).  
**Tools:** OpenOCD 0.12 + `gdb-multiarch`, `smolmux --gdb`, `smolmux-gdb-mcp`.

Example session: halt a board with no profile yet, run `gdb_identify_target`,
then `gdb_generate_profile`. Register values below are from that run.

Identify/generate is **ARM Cortex-M** only today (see
[board-probing.md](board-probing.md)); other architectures are planned. Full SoC
peripheral maps still need a datasheet. The nRF FICR path is not HW-proven yet
([hw-validation.md](hw-validation.md)).

---

## Setup (human)

```bash
# Terminal A - OpenOCD against the Xplained Pro EDBG (example config)
openocd -f board/atmel_samc21_xplained_pro.cfg
# listens gdb on :3333

# Terminal B - smolmux GDB broker
smolmux --gdb --gdb-path gdb-multiarch --gdb-target localhost:3333 \
  -s /tmp/smolmux-gdb.sock

# Terminal C - agent MCP (or drive tools by hand)
smolmux-gdb-mcp -s /tmp/smolmux-gdb.sock
```

Agent (or human) uses the **`probe_unknown_board`** prompt, or the same tools
directly.

---

## Step 1 - Halt

```text
tool: gdb_reset
args: { "mode": "halt" }
# or: gdb_interrupt + gdb_wait_stop
```

Target stopped under SWD; ready for memory probes.

---

## Step 2 - Identify (live values from HW)

```text
tool: gdb_identify_target
args: {}
```

**Structured result (essence of the live JSON):**

| Field | Value (this board) |
| ----- | ------------------ |
| CPUID @ `0xE000ED00` | `0x410cc601` |
| Core decode | **Cortex-M0+** r0p1 |
| SAM DSU DID @ `0x41002018` | `0x11010500` |
| DSU fields | PROCESSOR=1, FAMILY=2, SERIES=1 |
| Vendor / family guess | **Microchip/Atmel SAM** (C-series class) |
| STM32 DBGMCU @ `0xE0042000` | reads `0x00000000` (not a fault on this core) |
| STM32 match | **rejected** (DEV_ID must be nonzero) |
| Fault registers | none on M0+ (empty set) |

**Evidence trail (paraphrased from the tool):**

```text
SCB CPUID 0xE000ED00 = 0x410cc601 -> ARM Cortex-M0+ r0p1
SAM DSU DID 0x41002018 = 0x11010500 -> PROCESSOR=1 FAMILY=2 SERIES=1
STM32 DBGMCU_IDCODE 0xE0042000 = 0x00000000 -> ignored (zero DEV_ID)
```

OpenOCD's own scan agreed on Cortex-M0+; smolmux's vendor path named the SAM
family via DSU DID rather than model-recall alone.

---

## Step 3 - Generate a starter profile

```text
tool: gdb_generate_profile
args: {
  "path": "~/.config/smolmux/samc21.gdb-profile.json"
}
```

**What was written (shape; see also `configs/samc21.gdb-profile.json`):**

- `arch`: arm  
- `important_registers`: r0-r12, sp, lr, pc, xpsr, msp, psp, control, primask  
- `fault_registers`: **[]** (M0+ has no CFSR/HFSR-class block)  
- `peripheral_map` seeded with architectural Cortex-M blocks + **DSU**  
  (`0x41002000`) because identify confirmed that vendor block  

Application UART/SPI/GPIO bases are **not** guessed - fill from the datasheet
afterward (see [board-bringup-template.md](board-bringup-template.md)).

---

## Step 4 - Next session

```bash
smolmux --gdb --gdb-target localhost:3333 -s /tmp/smolmux-gdb.sock \
  -p ~/.config/smolmux/samc21.gdb-profile.json
# or short name after install: -p samc21
```

Named registers and DSU-aware peripherals are available without re-probing.

---

## Reproduce the flow without this board

Unit tests drive the same decode path with `tests/fake_gdb.c` (CPUID + DSU DID
answers modeled on this silicon). Cold-attach without a board cfg:
[openocd-cold-attach.md](openocd-cold-attach.md). Full runbook:
[board-exploration-workflow.md](board-exploration-workflow.md).
