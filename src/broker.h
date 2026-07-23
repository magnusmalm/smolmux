#ifndef SM_BROKER_H
#define SM_BROKER_H

#include <signal.h>
#include <stdatomic.h>
#include "links/link.h"
#include "expect.h"
#include "ring_buffer.h"
#include "anomaly.h"
#include "autoresponder.h"
#include "io_log.h"
#include "text_log.h"
#include "client.h"
#include "constants.h"
#include "sink.h"
#include "device_profile.h"
#include "regex_engine.h"

typedef struct sm_broker {
    sm_link_t *link;
    sm_expect_engine_t expect;
    sm_ring_buffer_t history;
    sm_anomaly_detector_t anomaly;
    sm_boot_tracker_t boot;
    sm_autoresponder_t autoresponder;
    sm_io_log_t *io_log;
    sm_text_log_t *text_log;

    int listen_fd;
    char socket_path[108];
    int epoll_fd;
    int reg_pipe[2];

    sm_client_t **clients;
    size_t client_count;
    size_t client_cap;
    unsigned int next_client_num;  /* unsigned: wraps cleanly, no UB (L3) */
    sm_client_t *takeover_client;

    int suspended;
    int reconnect;
    atomic_int stopped;
    int link_disconnected;
    double reconnect_next;
    int reconnect_delay_s;
    int link_connecting;            /* async (non-blocking) reconnect in progress */
    double link_connect_deadline;   /* monotonic; give up the connect past this */

    /* Link health monitoring (for dogfooding reliability) */
    double last_link_rx_time;   /* Last successful byte received from link */
    int link_healthy;           /* 1 if we believe the link is still delivering data */

    /* Deferred BREAK/SysRq state machine (timerfd-driven, never blocks the
     * event loop). One operation in flight at a time. */
    int break_timer_fd;
    int break_state;
    uint8_t break_followup[8];  /* bytes to send after break (SysRq key) */
    size_t break_followup_len;
    int break_delay_ms;         /* pause between break release and followup */
    void (*break_done)(struct sm_broker *b, void *ctx, int rc);
    void *break_done_ctx;

    /* Proactive autoboot-interrupt flood (timerfd-driven). Streams a key at a
     * tight interval and stops on a broker-side prompt match or a duration cap
     * — all inside the event loop, no client/agent round-trip, so it wins the
     * bootdelay=0 race. One flood in flight at a time. */
    int         flood_timer_fd;
    int         flood_active;
    uint8_t     flood_key[SM_FLOOD_KEY_MAX];
    size_t      flood_key_len;
    double      flood_deadline;                 /* monotonic; stop past this */
    sm_regex_t *flood_stop_re;                  /* optional; stop on match */
    char        flood_tail[SM_FLOOD_TAIL_LEN];  /* rolling window for the match */
    size_t      flood_tail_len;
    size_t      flood_sent;                     /* keystrokes written so far */
    double      flood_start;                    /* monotonic */
    char        flood_client_id[32];            /* requester, for the result */
    char        flood_msg_id[64];               /* request id, to correlate */

    /* Optional reset pulse (reset_and_interrupt): assert a modem line, flood,
     * release the line after the hold. "" pin = no reset. */
    char        flood_reset_pin[8];             /* "dtr"/"rts"; "" = none */
    char        flood_reset_deassert[8];        /* action to release the line */
    int         flood_reset_pending;            /* 1 = holding reset, release due */
    double      flood_reset_deassert_time;      /* monotonic */

    /* Boot-stage stall notification (one-shot timerfd). Re-armed on each stage
     * advance; fires a single boot_stall event if the timeout elapses with the
     * terminal stage unreached. */
    int         stall_timer_fd;
    int         stall_notified;                 /* 1 = fired for this episode */

    char port[256];
    int baudrate;
    char board[64];   /* board this wire belongs to (--board); "" if unset */
    char role[32];    /* this wire's role on the board (--role): console/swd/... */
    char log_dir[256];
    char text_log_dir[256];
    int no_text_log;
    char auth_token[64];    /* required in hello from network clients if set */

    sm_sink_t **sinks;
    size_t sink_count;
    sm_device_profile_t profile;

    /* Incremental history_request encoding (budgeted per event-loop turn). */
    struct {
        int active;
        char client_id[32];
        char msg_id[64];
        sm_rb_chunk_t *chunks;
        size_t chunk_count;
        size_t encode_index;
        cJSON *response_arr;
        int truncated;
    } history_pending;
} sm_broker_t;

int  sm_broker_init(sm_broker_t *b, sm_link_t *link, const char *socket_path);
int  sm_broker_run(sm_broker_t *b);
void sm_broker_stop(sm_broker_t *b);
void sm_broker_destroy(sm_broker_t *b);
void sm_broker_add_sink(sm_broker_t *b, sm_sink_t *sink);
void sm_broker_broadcast_msg(sm_broker_t *b, cJSON *msg);
sm_client_t *sm_broker_register_client(sm_broker_t *b, int fd);
void sm_broker_register_client_async(sm_broker_t *b, int fd);

/* Broker operations — safe to call from handlers or sinks.
 * Return 0 on success, negative on error. */
int  sm_broker_do_suspend(sm_broker_t *b, const char *by_name);
int  sm_broker_do_resume(sm_broker_t *b, const char *by_name);
int  sm_broker_do_write(sm_broker_t *b, const uint8_t *data, size_t len,
                         const char *sender);

/* Completion callback for a scheduled break: rc 0 = success, -1 = failed
 * or cancelled (suspend, link loss, shutdown). */
typedef void (*sm_break_done_fn)(struct sm_broker *b, void *ctx, int rc);

/* Schedule a BREAK without blocking the event loop: assert break, release
 * after duration_ms, then optionally wait delay_ms and send followup bytes
 * (SysRq key). done fires exactly once on completion or cancellation.
 * Returns 0 if accepted (done may already have fired for links that
 * complete synchronously), -1 if rejected (busy, suspended, link down);
 * done is never called on rejection. */
int  sm_broker_schedule_break(sm_broker_t *b, int duration_ms,
                              const uint8_t *followup, size_t followup_len,
                              int delay_ms, sm_break_done_fn done, void *ctx);

/* Start a proactive keystroke flood (break into a 0-delay bootloader). Streams
 * `key` every interval_ms until `stop_pattern` (optional regex) matches the
 * device output or duration_ms elapses; an `autoboot_result` is then sent to
 * client_id/msg_id. Returns 0 if started, -1 on reject (busy, suspended, link
 * down, bad args). One flood at a time.
 *
 * reset_and_interrupt: when reset_pin is non-NULL/non-empty ("dtr"/"rts"), the
 * broker asserts that modem line (reset_assert = "set"/"clear"), starts the
 * flood, and releases the line reset_hold_ms later — so keystrokes are already
 * streaming when the device comes out of reset. reset_pin NULL = plain flood. */
int  sm_broker_start_flood(sm_broker_t *b, const uint8_t *key, size_t key_len,
                           int interval_ms, int duration_ms,
                           const char *stop_pattern,
                           const char *client_id, const char *msg_id,
                           const char *reset_pin, const char *reset_assert,
                           int reset_hold_ms);

/* Test hooks for bounded link-fd drain (used by tests/test_broker.c). */
typedef ssize_t (*sm_drain_read_hook_fn)(int fd, void *buf, size_t len);
typedef int (*sm_drain_avail_hook_fn)(int fd, int *avail);
typedef void (*sm_drain_chunk_fn)(void *ctx, const uint8_t *buf, size_t n);

void sm_broker_test_set_drain_hooks(sm_drain_read_hook_fn read_fn,
                                    sm_drain_avail_hook_fn avail_fn);
void sm_broker_test_reset_drain_hooks(void);
int sm_broker_test_drain_link_fd(int fd, int max_reads,
                                 sm_drain_chunk_fn chunk_cb, void *ctx);

/* Fan-out to connected clients: one sm_msg_encode, shared-line queue per client. */
int sm_broker_broadcast_clients(sm_client_t **clients, size_t count,
                                cJSON *msg, sm_client_t *exclude);

/* Test hook: sm_msg_encode invocations in sm_broker_broadcast_clients. */
extern size_t sm_broker_test_broadcast_encode_count;

/* Test hooks: link-health timing (seconds). Defaults 2.0 / 8.0 / 3.0;
 * tests shrink them to exercise idle-health behavior without real waits. */
extern double sm_broker_test_health_period_s;
extern double sm_broker_test_idle_degraded_s;
extern double sm_broker_test_idle_recovered_s;

#endif /* SM_BROKER_H */
