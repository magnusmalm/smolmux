# Unknown-board probing (GDB MCP)

How `smolmux-gdb-mcp` helps identify silicon over SWD when you do not yet have
a board profile. For the full bring-up trail, see
[board-exploration-workflow.md](board-exploration-workflow.md) and
[openocd-cold-attach.md](openocd-cold-attach.md).

## Scope today

| Supported now | Not yet |
|---------------|---------|
| **ARM Cortex-M** (CPUID `0xE000ED00`, CoreSight) | RISC-V, Xtensa, other ISAs |
| Vendor IDs: STM32, SAM DSU, nRF FICR (best-effort) | Full peripheral maps without a datasheet |
| Starter `*.gdb-profile.json` from identify | Auto-generated RTOS-perfect profiles |

Other cores are **planned** once the Cortex-M path stays stable. The broker GDB
link itself is architecture-agnostic (it is a byte pipe to gdb); the
**identify/generate** tools are what assume Cortex-M architectural addresses.

## Tools

| Tool / surface | Role |
|----------------|------|
| `gdb_identify_target` | Decode CPUID + probe vendor IDs; return evidence |
| `gdb_generate_profile` | Write a starter profile (optional path under `~/.config/smolmux/`) |
| Prompt `probe_unknown_board` | Guided sequence for agents |
| Resource `smolmux-gdb://board-probing` | Short protocol-discoverable runbook |

## Minimal flow

1. Open a generic SWD session (OpenOCD `cortex_m` target is enough to start).  
2. `smolmux --gdb --gdb-target localhost:3333 -s /tmp/smolmux-gdb.sock`  
3. Attach `smolmux-gdb-mcp`, halt, run `gdb_identify_target`.  
4. `gdb_generate_profile` with a path you will reuse.  
5. Fill peripherals/RTOS from the datasheet; re-run next session with `-p`.

## Hardware notes

- **Cortex-M0/M0+** have no CFSR/HFSR-style configurable fault registers;
  fault tools report an empty set instead of reading reserved addresses.
- Chip-ID path was validated on a **SAM C21 Xplained Pro** (EDBG + OpenOCD).
- For matrix of what is proven on silicon, see
  [hw-validation.md](hw-validation.md).

## Related

- [board-bringup-template.md](board-bringup-template.md) - capture facts per board  
- [MCP-SETUP.md](MCP-SETUP.md) - register gdb-mcp with your agent  
