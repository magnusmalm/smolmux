# Persistent Serial Device Names with udev

USB serial dongles (FTDI, CP210x, CH340, etc.) appear as `/dev/ttyUSB0`, `/dev/ttyUSB1`, `/dev/ttyACM0`, etc. The kernel assigns these names based on **enumeration order**, not identity. Unplug one dongle and plug it back in later - it may get a different number. This breaks scripts, smolmux sessions, and muscle memory.

This is the same problem the Linux kernel solved for Ethernet NICs years ago: predictable names (`enp0s31f6`, `wlp2s0`) instead of `eth0` / `wlan0`. We can do the same for UART dongles using **udev rules** + stable symlinks.

> **You cannot rename the actual `/dev/ttyUSB0` nodes** - the kernel owns those. What you *can* do is create predictable symlinks like `/dev/ttyUSB-myboard-console` or `/dev/serial/rpi` that always point to the right device.

## Zero-Config Options (Try These First)

Modern systems (systemd + udev) already create stable symlinks for you under `/dev/serial/`:

```bash
ls -l /dev/serial/by-id/
/dev/serial/by-path/
```

- **`by-id/`** - Uses the USB serial number (or VID:PID if no serial). Best when your dongles have unique serials.
- **`by-path/`** - Uses the physical USB port topology (e.g. `pci-0000:00:14.0-usb-0:2.1:1.0-port0`). Excellent when you always plug "the console dongle" into the same physical port on your laptop.

These often cover most setups without custom udev rules. Write rules only when you want friendlier names (`/dev/serial/rpi` instead of `/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_SERIAL-if00-port0`).

## Step 1: Identify Your Dongles (The Right Way)

### Live monitoring (recommended)

Watch what happens when you plug/unplug in real time:

```bash
sudo udevadm monitor --udev
# or with more detail:
sudo udevadm monitor --udev --property
```

Plug in your dongle. You'll see events like `add /devices/pci.../usb1/1-2/1-2.1/1-2.1:1.0/ttyUSB0/tty/ttyUSB0`.

### Inspect a specific device

```bash
# Once you know the ttyUSB number
udevadm info -a -n /dev/ttyUSB0 | head -80

# Or get just the useful attributes
udevadm info -a -n /dev/ttyUSB0 | grep -E 'idVendor|idProduct|serial|manufacturer|product|devpath|KERNELS'
```

Look for these in order of preference:

| Attribute                                | Where it appears       | Best for                   |
| ---------------------------------------- | ---------------------- | -------------------------- |
| `ATTRS{serial}`                          | Under the USB device   | Unique cross-host identity |
| `KERNELS` / `ATTRS{devpath}`             | Physical port path     | Fixed physical USB port    |
| `ATTRS{idVendor}` + `ATTRS{idProduct}`   | VID/PID                | Rough type filter          |
| `ATTRS{manufacturer}` + `ATTRS{product}` | Human-readable strings | Documentation only         |

Notes:

- `KERNELS` / `ATTRS{devpath}` - best when you always plug a given board into the same physical USB port on your machine ("this USB port on my laptop = this board").
- `ATTRS{idVendor}` + `ATTRS{idProduct}` - a rough filter only; combine it with a serial or port match to disambiguate.

**Critical insight for embedded benches:** Many cheap CH340/CP210x dongles from the same batch share the **same serial number** (or report `0001`). In that case, **physical port (`KERNELS`)** is more reliable than serial.

### Quick helper script

Save this as `~/bin/uart-identify`:

```bash
#!/bin/bash
DEV=${1:-/dev/ttyUSB0}
echo "=== $DEV ==="
udevadm info -a -n "$DEV" | grep -E 'idVendor|idProduct|serial|manufacturer|product|devpath|KERNELS' | head -20
```

Make it executable and use it every time you add a new dongle.

## Step 2: The Three Naming Strategies

Choose based on your reality:

### Strategy 1: By serial number (best when dongles have unique serials)

```udev
# FTDI dongle with real unique serial
SUBSYSTEM=="tty", ATTRS{idVendor}=="0403", ATTRS{idProduct}=="6001", ATTRS{serial}=="YOURSERIAL", SYMLINK+="serial/rpi-console"
```

### Strategy 2: By physical USB port (recommended for embedded development desks)

This is often the **most reliable** approach. You plug "the board on the left" into the same physical USB port on your machine every day.

First find the port:

```bash
udevadm info -a -n /dev/ttyUSB0 | grep -E 'KERNELS|devpath'
```

Example rule:

```udev
# Always the same physical port -> always the same name
SUBSYSTEM=="tty", ATTRS{idVendor}=="1a86", ATTRS{idProduct}=="7523", KERNELS=="1-2.1", SYMLINK+="serial/ch340-left-desk"
```

You can combine VID/PID + port for extra safety:

```udev
SUBSYSTEM=="tty", ATTRS{idVendor}=="0403", ATTRS{idProduct}=="6001", KERNELS=="1-4.2", SYMLINK+="serial/ftdi-jtag"
```

### Strategy 3: By VID/PID only (last resort)

Only use this when you have exactly one device of that type:

```udev
SUBSYSTEM=="tty", ATTRS{idVendor}=="10c4", ATTRS{idProduct}=="ea60", SYMLINK+="serial/esp32"
```

---

## Step 3: Create the Rules File

```bash
sudo mkdir -p /etc/udev/rules.d
sudo nano /etc/udev/rules.d/99-uart-dongles.rules
```

Example complete file for a typical embedded bench:

```udev
# ==============================================================================
# Persistent UART dongle names for embedded development
# ==============================================================================
# Place in /etc/udev/rules.d/99-uart-dongles.rules
# Reload: sudo udevadm control --reload-rules && sudo udevadm trigger
# ==============================================================================

# --- FTDI FT232R with unique serial (reliable identity) ------------------------
SUBSYSTEM=="tty", ATTRS{idVendor}=="0403", ATTRS{idProduct}=="6001", ATTRS{serial}=="YOURSERIAL", SYMLINK+="serial/rpi-console", MODE="0666", TAG+="uaccess"

# --- Same FTDI chip but identified by physical port (no trust in serial) -------
SUBSYSTEM=="tty", ATTRS{idVendor}=="0403", ATTRS{idProduct}=="6001", KERNELS=="1-2.1", SYMLINK+="serial/jtag-left", MODE="0666", TAG+="uaccess"

# --- CH340 (no unique serial, use physical port) -------------------------------
SUBSYSTEM=="tty", ATTRS{idVendor}=="1a86", ATTRS{idProduct}=="7523", KERNELS=="1-4.4", SYMLINK+="serial/power-relay", MODE="0666", TAG+="uaccess"

# --- Silicon Labs CP210x on ESP32 board (serial is often '0001' across batch) --
SUBSYSTEM=="tty", ATTRS{idVendor}=="10c4", ATTRS{idProduct}=="ea60", KERNELS=="1-3", SYMLINK+="serial/esp32-devkit", MODE="0666", TAG+="uaccess"

# --- STM32 Nucleo / Discovery built-in ST-Link ---------------------------------
SUBSYSTEM=="tty", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="374b", SYMLINK+="serial/stm32-nucleo", MODE="0666", TAG+="uaccess"
```

**Permissions:**

- `MODE="0666"` - world read/write (simple but less secure)
- `TAG+="uaccess"` - modern systemd way: gives access to the logged-in user (recommended)
- Or add your user to the `dialout` group once:

  ```bash
  sudo usermod -aG dialout $USER
  # log out and back in
  ```

## Step 4: Reload and Test

```bash
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Or simply unplug the dongle and plug it back in. Then verify:

```bash
ls -la /dev/serial/
# Also check the zero-config paths
ls -la /dev/serial/by-id/ /dev/serial/by-path/
```

---

## Daily Usage

### smolmux

```bash
# Instead of /dev/ttyUSB0 which changes
smolmux /dev/serial/rpi-console -b 115200 -p uboot   # -p <name>: see --list-profiles

# Monitor client (discovers the broker from the port name)
smolmux-monitor /dev/serial/rpi-console
```

For MCP clients, attach the standalone `smolmux-mcp` server to the running
broker - see `MCP-SETUP.md` in this directory.

### Other common terminal programs

```bash
# picocom
picocom -b 115200 --omap crcrlf /dev/serial/esp32-devkit

# tio
tio /dev/serial/rpi-console -b 115200 -l

# minicom
minicom -D /dev/serial/stm32-nucleo -b 115200

# screen
screen /dev/serial/power-relay 115200
```

### Quick aliases (add to ~/.bash_aliases or ~/.zshrc)

```bash
alias uart-list='ls -la /dev/serial/ 2>/dev/null; echo "--- by-id ---"; ls -la /dev/serial/by-id/ 2>/dev/null; echo "--- by-path ---"; ls -la /dev/serial/by-path/ 2>/dev/null'

alias uart-identify='~/bin/uart-identify'

# Common boards
alias rpi-console='picocom -b 115200 /dev/serial/rpi-console'
alias jtag-left='tio /dev/serial/jtag-left'
alias esp32='picocom -b 74880 /dev/serial/esp32-devkit'
```

---

## Troubleshooting

### Multiple identical dongles (no unique serial)

Covered in **Step 2, Strategy 2** above. Use `KERNELS` (physical port path) + VID/PID. The old "serial is 0001" problem is extremely common with CH340 batches.

**Caveat:** Port-based rules break if you physically move the dongle to a different USB port or hub. This is usually acceptable on a fixed development bench.

### Rules not matching or not firing

```bash
# See exactly what udev evaluates for this device
udevadm test $(udevadm info -q path -n /dev/ttyUSB0) 2>&1 | less

# Live event stream (most useful during bring-up)
sudo udevadm monitor --udev --property
```

While the monitor is running, unplug/replug the dongle and watch for your `SYMLINK` in the output.

### Permission denied when opening the symlink

1. Preferred: use `TAG+="uaccess"` in the rule (systemd gives the logged-in user access automatically).
2. Fallback: `sudo usermod -aG dialout $USER` then log out/in.
3. Quick and dirty: `MODE="0666"` in the rule.

### Symlink not created at all

Check these:
- `SYMLINK+="serial/foo"` - do **not** put `/dev/` in the value.
- Parent dir exists: `sudo mkdir -p /dev/serial`
- Rule file name starts with a number (e.g. `99-...`) and ends in `.rules`
- No typos in attribute keys (`ATTRS{serial}` not `ATTR{serial}`)
- The device actually matched the attributes you used - re-run `udevadm info -a -n /dev/ttyUSB0` after plugging

### Want to see what the final symlink resolves to?

```bash
readlink -f /dev/serial/rpi-console
# or
realpath /dev/serial/jtag-left
```

## References & Further Reading

- [udev man page](https://www.freedesktop.org/software/systemd/man/udev.html)
- [Writing udev rules (classic guide)](http://www.reactivated.net/writing_udev_rules.html)
- `man udevadm`
- `udevadm test-builtin path_id /sys/class/tty/ttyUSB0` (see how by-path is computed)

---

## Quick Reference Card

| Task                      | Command                                                       |
| ------------------------- | ------------------------------------------------------------- |
| Live watch plug/unplug    | `sudo udevadm monitor --udev`                                 |
| Inspect device attributes | `udevadm info -a -n /dev/ttyUSB0`                             |
| Identify unknown dongle   | `~/bin/uart-identify /dev/ttyUSB2`                            |
| Reload rules after edit   | `sudo udevadm control --reload-rules && sudo udevadm trigger` |
| List all stable names     | `ls -l /dev/serial/ /dev/serial/by-id/ /dev/serial/by-path/`  |
| Test a specific rule      | `udevadm test $(udevadm info -q path -n /dev/ttyUSB0)`        |
| Kernel name for a symlink | `readlink -f /dev/serial/rpi-console`                         |

Notes:

- Inspect device attributes - filter to just the useful lines with: `udevadm info -a -n /dev/ttyUSB0 | grep -E 'serial|idVendor|KERNELS'`

**Golden rule for embedded work:** When in doubt, use **physical USB port** (`KERNELS`) + VID/PID. Serial numbers on cheap dongles lie.
