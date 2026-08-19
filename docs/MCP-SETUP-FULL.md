# MCP setup - smolmux Pro bundle

This is the Pro-bundle copy of [`MCP-SETUP.md`](MCP-SETUP.md) with the zip's
install paths filled in. Everything in the public guide applies; only the
binary locations differ.

## Install from the Pro zip

```bash
unzip smolmux-pro-*.zip && cd smolmux-pro-*
ARCH=$(uname -m)                      # x86_64 or aarch64
sudo install -m755 bin/$ARCH/* /usr/local/bin/
mkdir -p ~/.config/smolmux && cp -n profiles/*.json ~/.config/smolmux/
```

The binaries are fully static (musl) - no runtime dependencies, any modern
Linux.

### Zip-only (no `sudo install`)

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
  -p "$(pwd)/profiles/esp-idf-uart.smolmux-profile.json"
```

## Register with your agent

**Claude Code (after install to `/usr/local/bin`):**

```bash
claude mcp add serial -- /usr/local/bin/smolmux-mcp
claude mcp add gdb    -- /usr/local/bin/smolmux-gdb-mcp -s /tmp/smolmux-gdb.sock
```

**Claude Desktop / Cursor:**

```json
{
  "mcpServers": {
    "serial": {
      "command": "/usr/local/bin/smolmux-mcp",
      "args": ["-p", "/absolute/path/to/.config/smolmux/uboot.smolmux-profile.json"]
    },
    "gdb": {
      "command": "/usr/local/bin/smolmux-gdb-mcp",
      "args": ["-s", "/tmp/smolmux-gdb.sock"]
    }
  }
}
```

Use a real absolute path under your home directory (many GUI clients do not
expand `~`).

## Profile pack

`profiles/` in the zip ships the generic profiles (`uboot`, `linux-shell`,
`esp-idf-uart`, `esp32-arduino-lvgl`, `nrf9151-zephyr` + `nrf9151.gdb-profile`,
`samc21` / `esp32-uart` / `newboard` manifests) with a per-profile README. Copy
the ones you use to `~/.config/smolmux/` - brokers and MCP servers look there
for short names; `-p path/to/file.json` always works from the zip tree.

## Running the broker as a service

`systemd/smolmux@.service.example` in the zip runs a broker per port (assumes
binaries in `/usr/local/bin`):

```bash
cp systemd/smolmux@.service.example ~/.config/systemd/user/smolmux@.service
systemctl --user enable --now smolmux@ttyUSB0
# or: smolmux@ttyACM0
```

Tool lists, remote-broker notes, and troubleshooting: see
[`MCP-SETUP.md`](MCP-SETUP.md) - identical behavior, only paths differ.
