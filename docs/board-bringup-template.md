<!--
  Board bring-up template - COPY this file per board (e.g. to
  boards/<board>-bringup.md) and fill it in. The how-to for each step lives in
  docs/board-exploration-workflow.md. Delete this comment in your copy.
-->

# `<board>` - Bring-up / Familiarization

|                        |                                                          |
| ---------------------- | -------------------------------------------------------- |
| **Board / product**    | `<name, revision>`                                       |
| **Date started**       | `<YYYY-MM-DD>`                                           |
| **Engineer / agent**   | `<who>`                                                  |
| **Goal**               | ☐ new-product bring-up ☐ bug investigation ☐ new feature |
| **Datasheet / refman** | `<links>`                                                |
| **Schematic**          | `<link/rev>`                                             |

## 1. Pre-known facts (from datasheet - fill before touching hardware)

| Fact                        | Expected                            | Source |
| --------------------------- | ----------------------------------- | ------ |
| SoC / part number           |                                     |        |
| CPU core / arch             | `<e.g. Cortex-M4, ARMv7-M>`         |        |
| Max clock                   |                                     |        |
| RAM size / map              | `<size @ 0x...>`                    |        |
| Flash size / map            | `<size @ 0x...>`                    |        |
| Boot modes / strap pins     |                                     |        |
| Debug transport             | `<SWD/JTAG, connector>`             |        |
| UART(s)                     | `<UARTx: TX/RX pins, default baud>` |        |
| Ethernet / other            |                                     |        |
| Power rails                 |                                     |        |
| Key peripherals of interest |                                     |        |

Profiles written for this board:
- Serial: `configs/<board>.smolmux-profile.json` - ☐ n/a
- GDB target: `configs/<board>.gdb-profile.json` - ☐ n/a

## 2. Wiring & smolmux brokers

One broker per wire, all tagged `--board <board> --role <label>` so
`smolmux-cli boards` shows them as one board. Keep this table current - it is
the board's "socket map".

| Wire (role)      | Adapter / probe | Socket             | Status |
| ---------------- | --------------- | ------------------ | ------ |
| Console UART     | `/dev/ttyUSB0`  | `...-console.sock` | ☐ up   |
| Debug SWD        | OpenOCD :3333   | `...-gdb.sock`     | ☐ up   |
| Aux UART         |                 |                    | ☐ up   |
| Ethernet / other |                 |                    |        |

Broker command per wire (keyed by role):
- **Console UART** - `smolmux /dev/ttyUSB0 -b 115200 --board <board> --role console -p configs/<board>.smolmux-profile.json -s /tmp/smolmux-<board>-console.sock`
- **Debug SWD** - `smolmux --gdb --gdb-path <gdb> --gdb-target localhost:3333 --board <board> --role swd -s /tmp/smolmux-<board>-gdb.sock`
- **Aux UART** - `smolmux ... --board <board> --role aux ...`
- **Ethernet / other** - not a smolmux link - note how accessed.

Verify the grouping: `smolmux-cli boards` (or `smolmux-cli boards --json`).

Or capture these wires as a `*.board.json` manifest and bring them all up with
`smolmux-cli board up <manifest>` (detached; `--foreground` for CI). Stop with
`smolmux-cli board down <board>`. See `configs/samc21.board.json`.

## 3. Discovered facts (confirmed via smolmux)

| Fact                   | Observed                 | How (tool)              | Matches datasheet? |
| ---------------------- | ------------------------ | ----------------------- | ------------------ |
| CPUID (0xE000ED00)     | `<0x......>` -> `<core>` | `gdb_read_memory`       | ☐                  |
| Vendor device ID / rev |                          | `gdb_read_memory`       | ☐                  |
| RAM confirmed          |                          | `gdb_read_memory` probe | ☐                  |
| Flash confirmed        |                          | `gdb_read_memory`       | ☐                  |
| Running firmware / OS  |                          | `serial_output_history` | ☐                  |
| Boot banner            | `<paste key lines>`      | `serial_output_history` | ☐                  |
| Clock config           |                          |                         | ☐                  |
| Peripherals checked    |                          | `gdb_read_peripheral`   | ☐                  |
| RTOS / threads         |                          | `gdb_threads`           | ☐                  |
| Anomalies seen         |                          | `serial_get_incidents`  | -                  |

Extra tools per fact (keyed by fact):
- **Vendor device ID / rev** - also `gdb_command monitor`.
- **Flash confirmed** - also `monitor flash banks`.
- **Running firmware / OS** - also `serial_send_command`.

## 4. Exploration log

```
<YYYY-MM-DD HH:MM>  <what you did / found>
```

## 5. Discrepancies & surprises

- `<datasheet said X, board does Y>`

## 6. Open questions & risks

- `<...>`

## 7. Next steps

- `<bring-up / bug / feature-specific actions>`
