# MCP setup - smolmux Pro bundle

This is the Pro-bundle copy of [`MCP-SETUP.md`](MCP-SETUP.md) with the zip's
install paths filled in. Everything in the public guide applies; only the
binary locations differ.

## Install from the Pro zip

Unzip, then follow **Install** in the zip README. That puts the six tools
in `~/.local/bin`. Put that directory first on `PATH`. Do not `sudo install`
into `/usr/local/bin`, that fights the installer.

```bash
export PATH="$HOME/.local/bin:$PATH"
command -v smolmux-mcp   # should be .../.local/bin/smolmux-mcp
```

The installer copies packed `*.json` into `~/.config/smolmux/` with `cp -n`.

The binaries are fully static (musl) - no runtime dependencies, any modern
Linux.

### Zip-only (no install)

You can point agents at the unpacked tree:

```bash
cd smolmux-pro-*
ARCH=$(uname -m)
# example Claude Code:
claude mcp add serial -- "$(pwd)/bin/$ARCH/smolmux-mcp"
```

Start a broker with an absolute path to a profile in this zip:

```bash
./bin/$ARCH/smolmux /dev/ttyACM0 -b 115200 \
  -p "$(pwd)/profiles/linux-shell.smolmux-profile.json"
```

## Register with your agent

**Claude Code (after host install, tools on `PATH`):**

```bash
# Read-only tools by default. For console writes, set MUTATE:
claude mcp add serial --env SMOLMUX_MCP_MUTATE=1 -- "$HOME/.local/bin/smolmux-mcp"
claude mcp add gdb    -- "$HOME/.local/bin/smolmux-gdb-mcp" -s /tmp/smolmux-gdb.sock
```

**Claude Desktop / Cursor:**

Use a real absolute path under your home directory (many GUI clients do not
expand `~`). After the host installer that is `$HOME/.local/bin/...`.

```json
{
  "mcpServers": {
    "serial": {
      "command": "/absolute/path/to/.local/bin/smolmux-mcp",
      "args": ["-p", "/absolute/path/to/.config/smolmux/uboot.smolmux-profile.json"],
      "env": { "SMOLMUX_MCP_MUTATE": "1" }
    },
    "gdb": {
      "command": "/absolute/path/to/.local/bin/smolmux-gdb-mcp",
      "args": ["-s", "/tmp/smolmux-gdb.sock"]
    }
  }
}
```

## Profile pack

`profiles/` in the zip ships the generic Linux files (`uboot`, `linux-shell`,
`gdb`, `newboard`) and the named MCU and FPGA JSON (`esp-idf-uart`,
`esp32-arduino-lvgl`, `nrf9151-zephyr`, `nrf9151.gdb-profile`, `samc21`,
`esp32-uart`, FT2232 dual). The host installer copies them with `cp -n`.
Brokers and MCP servers look in `~/.config/smolmux/` for short names.
`-p path/to/file.json` always works from the zip tree.

## Running the broker as a service

`systemd/smolmux@.service.example` in the zip runs a broker per port (ExecStart
is `%h/.local/bin/smolmux`, matching the host installer):

```bash
cp systemd/smolmux@.service.example ~/.config/systemd/user/smolmux@.service
systemctl --user enable --now smolmux@ttyUSB0
# or: smolmux@ttyACM0
```

Tool lists, remote-broker notes, and troubleshooting: see
[`MCP-SETUP.md`](MCP-SETUP.md) - identical behavior, only paths differ.
