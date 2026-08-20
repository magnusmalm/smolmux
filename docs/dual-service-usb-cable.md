# One USB cable, two services (dual FTDI / dual interface)

**Audience:** operator or agent bringing up a board where **one physical USB
plug** exposes **two** endpoints (UART + JTAG, dual UART, …).

**Product rule:** smolmux is **one process → one link**. Multi-service cables
are **N brokers** under one `--board` / `*.board.json`, not a composite
USB object inside the broker.

---

## 1. What dual service means

| Piece          | Typical owner    | smolmux role     |
| -------------- | ---------------- | ---------------- |
| USB ifNN       | kernel / OpenOCD | not owned as USB |
| tty (ftdi_sio) | UART broker      | TIOCEXCL on tty  |
| JTAG (libftdi) | OpenOCD external | GDB → host:port  |

**Channel map is board-specific.** Never assume if00 = JTAG and if01 = UART
for every FTDI board — check vendor docs and `ls -l /dev/serial/by-id`.

---

## 2. FT2232-class recipe (example)

Trigger class: FTDI Dual (VID:PID `0403:6010`) with a unique USB serial.

```bash
ls -l /dev/serial/by-id/
# two by-id names sharing the serial; differ in -if00 vs -if01
```

1. Start **OpenOCD on the JTAG interface only** (board-specific channel —
   do not claim the UART interface from OpenOCD).
2. Copy `configs/ft2232-dual.board.json` (Pro zip: `profiles/`) and fill:
   - `console.device` = by-id for the **UART** interface (prefer STRONG
     serial; see weak by-id in `persistent-serial-devices.md`).
   - `swd.target` = OpenOCD GDB port (usually `localhost:3333`).
   - Optional short `"socket"` keys if paths get long.
3. Bring the board up:

```bash
smolmux-cli board up configs/ft2232-dual.board.json   # edit paths first
smolmux-cli boards --json
```

4. Pin agents explicitly (never first-glob with multi-wire):

```bash
# $XDG_RUNTIME_DIR/smolmux-<board>-console.sock and …-swd.sock
smolmux-mcp -s "$XDG_RUNTIME_DIR/smolmux-ft2232-dual-example-console.sock"
smolmux-gdb-mcp -s "$XDG_RUNTIME_DIR/smolmux-ft2232-dual-example-swd.sock"
```

5. Flash tools that need the console tty: suspend **console only**
   (`with-port` / `serial_suspend` on that socket). Stop OpenOCD separately
   if the probe tool needs JTAG.

6. Stop: `smolmux-cli board down ft2232-dual-example`.

Daily single-tty (`smolmux /dev/ttyUSB0`) is unchanged.

---

## 3. Hard rules

1. **One smolmux process holds one link** (UART or GDB, not both).
2. **OpenOCD is external** — GDB wire is host:port only.
3. **Do not open the same USB interface** from OpenOCD and ftdi_sio.
4. **Multi-wire agents always use `-s`** — first-glob is a footgun.
5. Prefer **board-role sockets** over long by-id basename sockets.
6. Weak class-only by-id: seat risk; list-ports `[WEAK]` tags apply.

---

## 4. Related

| Doc                                | Why                       |
| ---------------------------------- | ------------------------- |
| configs/ft2232-dual.board.json     | Example two-wire manifest |
| configs/samc21.board.json          | Same model, ACM + OpenOCD |
| docs/openocd-cold-attach.md        | Probe before GDB broker   |
| docs/board-exploration-workflow.md | One broker per wire       |
| docs/persistent-serial-devices.md  | by-id / weak id           |
