# Hardware validation matrix

**Purpose:** what has been proven on **real hardware** vs sim/unit only. Unit
tests (PTY / fake-gdb) are necessary but not sufficient.

Date: 2026-07-15. Boards referenced below are development kits and lab
setups named in the table keys (e.g. SAM C21 Xplained Pro).

See also: [`board-exploration-workflow.md`](board-exploration-workflow.md),
[`board-probing.md`](board-probing.md),
[`openocd-cold-attach.md`](openocd-cold-attach.md).

---

## 1. Proven on silicon vs sim/unit

| Capability                          | Real HW? |
| ----------------------------------- | -------- |
| UART link (round-trip)              | yes (a)  |
| GDB link + most gdb-mcp tools       | yes (a)  |
| `gdb_interrupt`, named registers    | yes (a)  |
| Chip-ID / profile auto-gen          | yes (a)  |
| Chip-ID nRF FICR path over SWD      | no       |
| suspend/resume (fd release)         | yes (a)  |
| Fault-register decode (M3/M33)      | no (b)   |
| serial-over-TCP / telnet / RFC2217  | yes (c)  |
| Board lifecycle two real wires      | no (d)   |
| `with-port` + real flasher          | partial  |
| Remote mon over TCP/WS sink         | no (d)   |
| Zephyr RTOS threads (`gdb_threads`) | no (d)   |
| Text-log sink (`-t`) live UART      | yes      |
| `set_baud` / flow / pins / break    | partial  |
| Anomaly / watcher on real crash     | no       |
| YMODEM suspend -> sb/sz -> resume   | no (e)   |

**Keys:** (a) SAM C21 Xplained Pro. (b) needs M3/M33-class (e.g. nRF9151 DK).
(c) ser2net 4.3.11 lab. (d) PTY / fake-gdb only. (e) documented, not HW-proven.
partial = CI `/bin/true` or incomplete copper proof.

**Notes:**

- **SAM C21 Xplained Pro:** EDBG virtual COM; OpenOCD
  `board/atmel_samc21_xplained_pro.cfg` on `:3333`; gdb-multiarch. Cortex-M0+
  (no CFSR/HFSR). Chip-ID check 2026-07-07 matched OpenOCD; fixed STM32
  false-match when `0xE0042000` reads as zero. Tools covered: identify +
  generate profile (DSU DID path); suspend on `/dev/ttyACM1`.
- **nRF9151-class (generic DK):** preferred for M33 fault registers and nRF
  FICR probing - not HW-proven on SWD in this matrix.
- **serial-over-TCP:** lab validation with ser2net; physical baud/pin effect on
  copper still separate from "link speaks RFC2217."
- **`with-port`:** exercised with `/bin/true` in CI; a real flasher on hardware
  is not yet in this matrix.

---

## 2. Not yet proven on hardware

Highest-value gaps relative to the matrix above (ordered roughly by impact):

1. **nRF9151 DK (or other M33) SWD** - fault-register decode + FICR identify.
2. **`with-port` + real flasher** - west/nrfjprog/OpenOCD exclusive ownership.
3. **Two-wire board lifecycle** - console + SWD `board up/down` on hardware.
4. **YMODEM path** - suspend -> `sb`/`sz` -> resume on a real receiver.
5. **Watcher on a real panic** - forced crash and incident-file confirmation.

---

## 3. What "validated" means here

A row is **yes** only if exercised on physical silicon (or a real external
service like ser2net on a lab host), not only PTY/`fake_gdb` unit tests. When
in doubt, treat the capability as unproven for production claims.
