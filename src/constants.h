#ifndef SM_CONSTANTS_H
#define SM_CONSTANTS_H

#define SM_NAME "smolmux"
#define SM_VERSION "0.1.2"
#define SM_PROTOCOL_VERSION 1

/* Limits */
#define SM_MAX_CLIENTS 32
#define SM_MAX_EXPECT_PENDING 256
#define SM_MAX_EXPECT_PER_CLIENT 16
#define SM_MAX_EXPECT_TIMEOUT_MS 3600000  /* 1 hour */
#define SM_MAX_ANOMALY_PATTERNS 64
#define SM_MAX_ANOMALY_INCIDENTS 1024
#define SM_MAX_ANOMALY_WINDOW 65536

/* Boot-stage progress tracking: ordered boot markers matched on the device
 * output stream (reached / furthest / stalled). */
#define SM_MAX_BOOT_STAGES        32
#define SM_BOOT_WINDOW_BYTES      8192   /* fixed sliding window, drops oldest */
#define SM_BOOT_STALL_TIMEOUT_MS  10000  /* default: flag stall after 10s idle */

/* Autoresponder: standing expect->send rules matched in the read path (menus,
 * y/n prompts, login). */
#define SM_MAX_AUTORESPONDERS       32
#define SM_AR_RESPONSE_MAX          256
#define SM_AR_WINDOW_BYTES          8192  /* fixed sliding window, drops oldest */
#define SM_AR_DEFAULT_COOLDOWN_MS   1000  /* re-fire guard on still-visible text */
#define SM_AR_MAX_FIRED_PER_FEED    8

#define SM_MAX_BREAK_DURATION_MS 2000
#define SM_DEFAULT_BREAK_DURATION_MS 250
#define SM_MAX_SYSRQ_DELAY_MS 500

/* Proactive autoboot-interrupt flood ("break into U-Boot at bootdelay=0"):
 * stream a key at a tight interval from inside the event loop, stopping on a
 * broker-side prompt match or a hard duration cap. */
#define SM_FLOOD_KEY_MAX          8      /* max bytes in the flooded key */
#define SM_FLOOD_TAIL_LEN         256    /* rolling window for the stop match */
#define SM_MIN_FLOOD_INTERVAL_MS  1      /* floor: avoid a busy-spin timer */
#define SM_DEFAULT_FLOOD_INTERVAL_MS 10
#define SM_DEFAULT_FLOOD_DURATION_MS 2000
#define SM_MAX_FLOOD_DURATION_MS  15000
/* Optional reset pulse for reset_and_interrupt: assert a modem line, flood,
 * then release the line after the hold so keys are already streaming when the
 * device boots. */
#define SM_DEFAULT_RESET_HOLD_MS  100
#define SM_MAX_RESET_HOLD_MS      2000

/* Expect scan lookback for search offset optimization */
#define SM_EXPECT_SCAN_LOOKBACK 256

/* Buffer sizes */
#define SM_CLIENT_READ_BUF_SIZE (64 * 1024)
#define SM_CLIENT_WRITE_QUEUE_SIZE 256
/* Stop coalescing once the merged output entry's encoded size reaches this,
 * so a backlogged client's decode+re-encode cost stays O(cap) per line
 * instead of growing with the whole accumulated head (O(K^2) in backlog). */
#define SM_CLIENT_COALESCE_MAX_BYTES (16 * 1024)
#define SM_RING_BUFFER_MAX_BYTES (2 * 1024 * 1024)
#define SM_MAX_HISTORY_RESPONSE_BYTES (512 * 1024)  /* raw bytes per history_response */
#define SM_HISTORY_ENCODE_MAX_CHUNKS  4
#define SM_HISTORY_ENCODE_MAX_BYTES   (32 * 1024)
#define SM_MAX_EXPECT_RESULT_BYTES    (64 * 1024)
#define SM_LINK_WRITE_QUEUE_MAX_BYTES (512 * 1024)
#define SM_RB_CHUNK_TARGET            4096
#define SM_RB_POOL_SLOTS              16
#define SM_EXPECT_BUF_INITIAL 4096
#define SM_MAX_EXPECT_BUF_SIZE (256 * 1024)
#define SM_ANOMALY_CONTEXT_SIZE 1024
#define SM_MAX_PROFILE_FILE_BYTES (1024 * 1024)  /* reject larger profile files */

/* Client send payload limits (raw bytes after base64 decode) */
#define SM_MAX_SEND_PAYLOAD       (40 * 1024)
#define SM_MAX_SEND_B64_LEN       (((SM_MAX_SEND_PAYLOAD + 2) / 3) * 4 + 4)

/* Logging flush batching */
#define SM_IO_LOG_FLUSH_RECORDS   16
/* Text log flushes every completed line (see text_log.c). This constant is
 * retained only for documentation / callers that still mention the old
 * batching policy; it is no longer used to gate fflush. */
#define SM_TEXT_LOG_FLUSH_LINES   1

/* UART write */
#define SM_UART_WRITE_POLL_MS 50
#define SM_UART_WRITE_MAX_RETRIES 20  /* 50ms * 20 = 1s max block */

/* GDB write */
#define SM_GDB_WRITE_MAX_RETRIES 20   /* 100ms poll * 20 = 2s max block */

/* Timeouts (ms) */
#define SM_DEFAULT_EXPECT_TIMEOUT_MS 5000
#define SM_EPOLL_TIMEOUT_MS 100
#define SM_HELLO_TIMEOUT_S 10  /* drop clients that never send hello */
#define SM_ANOMALY_COOLDOWN_MS 5000

/* Reconnect */
#define SM_RECONNECT_BASE_S  2
#define SM_RECONNECT_MAX_S   60
#define SM_LINK_CONNECT_TIMEOUT_S 5   /* broker deadline for an async link connect */

/* Defaults */
#define SM_DEFAULT_BAUD 115200
#define SM_SOCKET_PATH_FMT "/tmp/smolmux-%s.sock"
#define SM_LOG_DIR "/tmp"
#define SM_TEXT_LOG_DIR_FMT "%s/.local/share/smolmux/logs"

/* Epoll */
#define SM_MAX_EPOLL_EVENTS 64

/* Device profiles */
#define SM_PROFILE_ENV "SMOLMUX_DEVICE_PROFILE"
#define SM_PROFILE_CONFIG_DIR_FMT "%s/.config/smolmux"
#define SM_PROFILE_FILE_SUFFIX ".smolmux-profile.json"
#define SM_GDB_PROFILE_ENV "SMOLMUX_GDB_PROFILE"
#define SM_GDB_PROFILE_FILE_SUFFIX ".gdb-profile.json"
#define SM_PROFILE_DEFAULT_PROMPT "\\$\\s*$|\\#\\s*$|>\\s*$"
#define SM_PROFILE_DEFAULT_TIMEOUT 5000

/* Monitor */
#define SM_MONITOR_READ_BUF_SIZE 8192
#define SM_MONITOR_ESCAPE_CHAR 0x1D  /* Ctrl-] */
#define SM_SOCKET_ENV "SMOLMUX_SOCKET"
#define SM_SOCKET_GLOB "/tmp/smolmux-*.sock"
#define SM_SOCKET_GLOB_XDG_FMT "%s/smolmux-*.sock"

/* TCP sink */
#define SM_TCP_DEFAULT_PORT 5555

/* serial-over-TCP device link */
#define SM_SERIAL_TCP_DEFAULT_PORT 23        /* telnet/console default */
#define SM_SERIAL_TCP_CONNECT_TIMEOUT_MS 5000 /* bound the connect stall in open() */

/* WebSocket sink */
#define SM_WS_DEFAULT_PORT 5556
#define SM_WS_MAX_CLIENTS 16
#define SM_WS_READ_BUF_SIZE 8192

/* Watcher */
#define SM_WATCHER_DEFAULT_OUTPUT_DIR "./smolmux-incidents"
#define SM_WATCHER_DEFAULT_CONTEXT_BYTES 8192
#define SM_WATCHER_RECONNECT_BASE_S 2
#define SM_WATCHER_RECONNECT_MAX_S 60
#define SM_WATCHER_HISTORY_TIMEOUT_S 5

/* MCP sink */
#define SM_MCP_OUTPUT_BUFFER_MAX (1024 * 1024)
#define SM_MCP_READ_BUF_MAX (1024 * 1024)
#define SM_MCP_MAX_PENDING_CALLS 64
#define SM_MCP_PROTOCOL_VERSION "2024-11-05"
#define SM_MCP_CLIENT_ID "mcp"

#endif /* SM_CONSTANTS_H */
