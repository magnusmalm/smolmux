# Start here

One-screen router for smolmux. Find your intent and follow the trail.

## Mental model

- A **broker** holds exactly **one wire** open (UART, GDB/SWD, or serial-over-TCP)
  and multiplexes it to many clients over a Unix socket. A board with console +
  SWD is **two brokers**, grouped by `--board` or a `*.board.json`.
- **Clients** use a newline-delimited JSON protocol: `smolmux-monitor`,
  `smolmux-cli`, `smolmux-watcher`, plus MCP servers `smolmux-mcp` (serial) and
  `smolmux-gdb-mcp` (GDB; unknown-board probing is **ARM Cortex-M** today).
- The broker stays a **dumb byte pipe**; protocol/MI intelligence is client-side.

## Find your intent

| I want to...                              | Start at                                     |
| ----------------------------------------- | -------------------------------------------- |
| **Build and run for daily use**           | [daily-driver.md](daily-driver.md)           |
| **Bring up a new board with an AI agent** | [workflow](board-exploration-workflow.md)    |
| **Attach a debugger to an unknown chip**  | [cold-attach](openocd-cold-attach.md)        |
| **Understand the architecture**           | [../DESIGN.md](../DESIGN.md)                 |
| **Hack on the code / add a tool or link** | [../CLAUDE.md](../CLAUDE.md)                 |
| **What is proven on HW**                  | [hw-validation.md](hw-validation.md)         |
| **Point an AI agent at a board (MCP)**    | [MCP-SETUP.md](MCP-SETUP.md)                 |
| **Give serial dongles stable names**      | [serial-names](persistent-serial-devices.md) |
| **Linux login / multi-console**          | [daily-driver](daily-driver.md)              |
| **Demo: SAM C21 unknown-board probe**    | [probe transcript](demo-samc21-probe-transcript.md) |

Then:

- **Daily use** - [../README.md](../README.md) for quick start and Free vs Pro.
- **Linux login / multi-wire** - same [daily-driver.md](daily-driver.md)
  (section *Linux console login and multi-wire*).
- **Agent probe demo** - [demo-samc21-probe-transcript.md](demo-samc21-probe-transcript.md).
- **Unknown chip** - cold-attach, then [board-probing.md](board-probing.md).
- **Architecture** - [../CLAUDE.md](../CLAUDE.md) file map after DESIGN.

## New board on Monday

For an unfamiliar board over console + SWD, with an AI agent as assistant:

1. **Wire it up.** Copy [`../configs/newboard.board.json`](../configs/newboard.board.json),
   set board name, console `device` (prefer `/dev/serial/by-id/...`), `baud`,
   and GDB `target`. Procedure:
   [board-exploration-workflow.md](board-exploration-workflow.md) Step 1.
2. **Unknown chip?** Generic-SWD cold-attach:
   [openocd-cold-attach.md](openocd-cold-attach.md) (Cortex-M DAP is enough to start).
3. **Point the agent at it.** Register MCP servers ([MCP-SETUP.md](MCP-SETUP.md)),
   then run the **`probe_unknown_board`** prompt (`gdb_identify_target` + banner
   correlation). Details: board-exploration-workflow Steps 2-4;
   scope: [board-probing.md](board-probing.md).
4. **Capture knowledge.** `gdb_generate_profile` writes a starter profile; fill
   peripherals from the datasheet and
   [board-bringup-template.md](board-bringup-template.md).

## Reference (deeper)

- [../CLAUDE.md](../CLAUDE.md) - codebase map for contributors and agents
- [../DESIGN.md](../DESIGN.md) - architecture and wire protocol
- [../CHANGELOG.md](../CHANGELOG.md) - release notes
- [MCP-SETUP.md](MCP-SETUP.md) - Claude Code / Desktop / Cursor
- [board-bringup-template.md](board-bringup-template.md) - per-board facts
- [board-probing.md](board-probing.md) - Cortex-M identify / generate profile

## Known limitations

- [issue-ws-hardening-deferred.md](issue-ws-hardening-deferred.md) - WebSocket
  sink is loopback-oriented; further hardening is deferred on purpose.
