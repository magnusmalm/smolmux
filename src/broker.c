#include "broker.h"
#include "protocol.h"
#include "logger.h"
#include "util/base64.h"
#include "util/json_helpers.h"
#include "util/shared_line.h"
#include "util/timeutil.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/timerfd.h>

#define LOG_TAG "broker"

/* Max link reads per EPOLLIN wakeup — drains kernel buffer without starving clients */
#define SM_LINK_DRAIN_MAX_READS 16
#define SM_LINK_DRAIN_CHUNK     4096   /* per-read buffer; also the rx-filter bound */

/* Sentinel values for epoll_data.ptr — low addresses are never valid heap pointers */
#define EPOLL_TAG_LINK     ((void *)(uintptr_t)1)
#define EPOLL_TAG_LISTEN   ((void *)(uintptr_t)2)
#define EPOLL_TAG_PIPE     ((void *)(uintptr_t)3)
#define EPOLL_TAG_BRKTIMER ((void *)(uintptr_t)4)
#define EPOLL_TAG_LINK_OUT ((void *)(uintptr_t)5)
#define EPOLL_TAG_FLOODTIMER ((void *)(uintptr_t)7)
#define EPOLL_TAG_STALLTIMER ((void *)(uintptr_t)8)
#define EPOLL_TAG_SINK_ID  6u

static void *epoll_sink_ptr(size_t idx)
{
    return (void *)(uintptr_t)(((uint64_t)EPOLL_TAG_SINK_ID << 32) |
                               (idx & 0xFFFFFFFFu));
}

static int epoll_ptr_is_sink(void *ptr, size_t *idx)
{
    uintptr_t v = (uintptr_t)ptr;
    if ((v >> 32) != EPOLL_TAG_SINK_ID)
        return 0;
    if (idx)
        *idx = (size_t)(v & 0xFFFFFFFFu);
    return 1;
}

/* Deferred break state machine states */
enum { SM_BRK_IDLE = 0, SM_BRK_ASSERTED, SM_BRK_DELAY };

/* Constant-time string equality for the auth token: strcmp early-exits on the
 * first differing byte, leaking shared-prefix length via timing. Folds in the
 * length difference and never short-circuits. */
static int ct_str_equal(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    size_t n = la > lb ? la : lb;
    unsigned char diff = (la != lb);
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = i < la ? (unsigned char)a[i] : 0;
        unsigned char cb = i < lb ? (unsigned char)b[i] : 0;
        diff |= (unsigned char)(ca ^ cb);
    }
    return diff == 0;
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* --- Client management --- */

static void remove_client(sm_broker_t *b, sm_client_t *c)
{
    SM_LOG_INFO(LOG_TAG, "client %s (%s) disconnected", c->id, c->name);

    /* Cancel pending expects */
    sm_expect_cancel_client(&b->expect, c->id);

    /* Release takeover if held */
    if (b->takeover_client == c)
        b->takeover_client = NULL;

    /* Remove from epoll */
    if (epoll_ctl(b->epoll_fd, EPOLL_CTL_DEL, c->fd, NULL) < 0)
        SM_LOG_WARN(LOG_TAG, "epoll_ctl del client fd=%d: %s", c->fd, strerror(errno));

    /* Remove from array */
    for (size_t i = 0; i < b->client_count; i++) {
        if (b->clients[i] == c) {
            b->clients[i] = b->clients[b->client_count - 1];
            b->client_count--;
            break;
        }
    }

    sm_client_destroy(c);
}

/* Test hook: sm_msg_encode calls per broadcast fan-out (expect 1). */
size_t sm_broker_test_broadcast_encode_count;

/* Link-health timing (seconds). Mutable only so tests can shrink them and
 * exercise the idle-health path in milliseconds; production never writes. */
double sm_broker_test_health_period_s = 2.0;
double sm_broker_test_idle_degraded_s = 8.0;
double sm_broker_test_idle_recovered_s = 3.0;

int sm_broker_broadcast_clients(sm_client_t **clients, size_t count,
                                cJSON *msg, sm_client_t *exclude)
{
    size_t len;
    char *line = sm_msg_encode(msg, &len);
    if (!line)
        return 0;

    sm_broker_test_broadcast_encode_count++;

    sm_shared_line_t *sl = sm_shared_line_new(line, len);
    if (!sl)
        return 0;

    int queued = 0;
    for (size_t i = 0; i < count; i++) {
        sm_client_t *c = clients[i];
        if (c == exclude || !c->hello_received)
            continue;
        if (sm_client_send_shared(c, sl) == 0)
            queued++;
    }
    sm_shared_line_release(sl);
    return queued;
}

static void broadcast(sm_broker_t *b, cJSON *msg, sm_client_t *exclude)
{
    sm_broker_broadcast_clients(b->clients, b->client_count, msg, exclude);
}

static void send_to_client(sm_client_t *c, cJSON *msg)
{
    sm_client_send(c, msg);
    cJSON_Delete(msg);
}

static int can_send(sm_broker_t *b, sm_client_t *c)
{
    if (strcmp(c->role, "observer") == 0) return 0;
    if (b->takeover_client && b->takeover_client != c) return 0;
    return 1;
}

void sm_broker_add_sink(sm_broker_t *b, sm_sink_t *sink)
{
    void *tmp = realloc(b->sinks, (b->sink_count + 1) * sizeof(sm_sink_t *));
    if (!tmp) {
        SM_LOG_ERROR(LOG_TAG, "realloc failed adding sink");
        return;
    }
    b->sinks = tmp;
    b->sinks[b->sink_count++] = sink;
}

void sm_broker_broadcast_msg(sm_broker_t *b, cJSON *msg)
{
    broadcast(b, msg, NULL);
}

/* --- Deferred BREAK/SysRq state machine --- */

static int break_timer_arm(sm_broker_t *b, int ms)
{
    struct itimerspec its = {0};
    its.it_value.tv_sec = ms / 1000;
    its.it_value.tv_nsec = (long)(ms % 1000) * 1000000L;
    return timerfd_settime(b->break_timer_fd, 0, &its, NULL);
}

static void break_complete(sm_broker_t *b, int rc)
{
    sm_break_done_fn done = b->break_done;
    void *ctx = b->break_done_ctx;

    b->break_state = SM_BRK_IDLE;
    b->break_followup_len = 0;
    b->break_done = NULL;
    b->break_done_ctx = NULL;
    if (b->break_timer_fd >= 0) {
        struct itimerspec its = {0};
        timerfd_settime(b->break_timer_fd, 0, &its, NULL);
    }

    if (done) done(b, ctx, rc);
}

static void broker_update_link_epoll(sm_broker_t *b);

/* Cancel any in-flight break (suspend, link loss, shutdown). */
static void break_cancel(sm_broker_t *b)
{
    if (b->break_state == SM_BRK_IDLE) return;
    if (b->break_state == SM_BRK_ASSERTED)
        b->link->set_param(b->link, "break", "clear");
    break_complete(b, -1);
}

int sm_broker_schedule_break(sm_broker_t *b, int duration_ms,
                             const uint8_t *followup, size_t followup_len,
                             int delay_ms, sm_break_done_fn done, void *ctx)
{
    if (b->suspended || b->link_disconnected) return -1;
    if (b->break_state != SM_BRK_IDLE) return -1;  /* one break at a time */
    if (followup_len > sizeof(b->break_followup)) return -1;

    if (duration_ms <= 0) duration_ms = SM_DEFAULT_BREAK_DURATION_MS;
    if (duration_ms > SM_MAX_BREAK_DURATION_MS)
        duration_ms = SM_MAX_BREAK_DURATION_MS;
    if (delay_ms < 0) delay_ms = 0;
    if (delay_ms > SM_MAX_SYSRQ_DELAY_MS) delay_ms = SM_MAX_SYSRQ_DELAY_MS;

    if (b->link->set_param(b->link, "break", "set") != 0) {
        /* Link without break assert/release (e.g. GDB, where send_break is
         * an instantaneous signal) — run the whole sequence synchronously. */
        int rc = b->link->send_break(b->link, duration_ms);
        if (rc == 0 && followup_len)
            rc = sm_broker_do_write(b, followup, followup_len, "break") < 0
                     ? -1 : 0;
        if (done) done(b, ctx, rc);
        return 0;
    }

    if (b->break_timer_fd < 0) {
        b->break_timer_fd =
            timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (b->break_timer_fd >= 0) {
            struct epoll_event ev = {.events = EPOLLIN,
                                     .data.ptr = EPOLL_TAG_BRKTIMER};
            if (epoll_ctl(b->epoll_fd, EPOLL_CTL_ADD, b->break_timer_fd,
                          &ev) < 0) {
                close(b->break_timer_fd);
                b->break_timer_fd = -1;
            }
        }
        if (b->break_timer_fd < 0) {
            SM_LOG_ERROR(LOG_TAG, "break timerfd: %s", strerror(errno));
            b->link->set_param(b->link, "break", "clear");
            return -1;
        }
    }

    b->break_state = SM_BRK_ASSERTED;
    if (followup_len) memcpy(b->break_followup, followup, followup_len);
    b->break_followup_len = followup_len;
    b->break_delay_ms = delay_ms;
    b->break_done = done;
    b->break_done_ctx = ctx;

    if (break_timer_arm(b, duration_ms) < 0) {
        SM_LOG_ERROR(LOG_TAG, "break timer arm: %s", strerror(errno));
        b->link->set_param(b->link, "break", "clear");
        b->break_state = SM_BRK_IDLE;
        b->break_followup_len = 0;
        b->break_done = NULL;
        b->break_done_ctx = NULL;
        return -1;
    }

    SM_LOG_INFO(LOG_TAG, "break scheduled: %dms%s", duration_ms,
                followup_len ? " + SysRq followup" : "");
    return 0;
}

static void handle_break_timer(sm_broker_t *b)
{
    uint64_t expirations;
    while (read(b->break_timer_fd, &expirations, sizeof(expirations)) ==
           sizeof(expirations))
        ;

    switch (b->break_state) {
    case SM_BRK_ASSERTED: {
        int rc = b->link->set_param(b->link, "break", "clear");
        if (rc != 0) {
            /* Clearing the BREAK failed — it is still physically asserted and
             * would block all further TX on the line. Retry once before
             * giving up. */
            SM_LOG_WARN(LOG_TAG, "break clear failed (rc=%d), retrying", rc);
            rc = b->link->set_param(b->link, "break", "clear");
            if (rc != 0)
                SM_LOG_ERROR(LOG_TAG, "break clear retry failed (rc=%d); "
                             "BREAK may remain asserted on %s", rc, b->port);
        }
        if (rc == 0 && b->break_followup_len) {
            if (b->break_delay_ms > 0 &&
                break_timer_arm(b, b->break_delay_ms) == 0) {
                b->break_state = SM_BRK_DELAY;
            } else {
                rc = sm_broker_do_write(b, b->break_followup,
                                        b->break_followup_len, "break") < 0
                         ? -1 : 0;
                break_complete(b, rc);
            }
        } else {
            break_complete(b, rc);
        }
        break;
    }
    case SM_BRK_DELAY: {
        int rc = sm_broker_do_write(b, b->break_followup,
                                    b->break_followup_len, "break") < 0
                     ? -1 : 0;
        break_complete(b, rc);
        break;
    }
    default:
        break;  /* stale expiry after cancel — nothing to do */
    }
}

/* --- Proactive autoboot-interrupt flood (break into a 0-delay bootloader) --- */

static int flood_arm_timer(sm_broker_t *b, int interval_ms)
{
    struct itimerspec its = {0};
    its.it_value.tv_sec     = interval_ms / 1000;
    its.it_value.tv_nsec    = (long)(interval_ms % 1000) * 1000000L;
    its.it_interval.tv_sec  = interval_ms / 1000;   /* repeating */
    its.it_interval.tv_nsec = (long)(interval_ms % 1000) * 1000000L;
    return timerfd_settime(b->flood_timer_fd, 0, &its, NULL);
}

static void flood_finish(sm_broker_t *b, int matched, const char *reason)
{
    if (!b->flood_active) return;

    if (b->flood_timer_fd >= 0) {
        struct itimerspec off = {0};
        timerfd_settime(b->flood_timer_fd, 0, &off, NULL);   /* disarm */
    }
    if (b->flood_stop_re) { sm_regex_free(b->flood_stop_re); b->flood_stop_re = NULL; }

    /* Safety: if the flood ends while still holding reset (e.g. matched almost
     * immediately), release the line so the device isn't left in reset. */
    if (b->flood_reset_pending) {
        b->link->set_param(b->link, b->flood_reset_pin, b->flood_reset_deassert);
        b->flood_reset_pending = 0;
    }
    b->flood_active = 0;

    int elapsed = (int)((sm_now_monotonic() - b->flood_start) * 1000.0);
    SM_LOG_INFO(LOG_TAG, "autoboot flood done: %s (%zu keys, %dms)",
                reason, b->flood_sent, elapsed);

    cJSON *res = sm_msg_autoboot_result(b->flood_msg_id[0] ? b->flood_msg_id : NULL,
                                        matched, reason, elapsed, (int)b->flood_sent);
    broadcast(b, res, NULL);   /* result carries the request id for correlation */
    cJSON_Delete(res);
}

static void flood_cancel(sm_broker_t *b)
{
    if (b->flood_active) flood_finish(b, 0, "cancelled");
}

/* Feed inbound device bytes to the flood stop-matcher. Runs in the link read
 * path, so a prompt match halts the flood with zero client round-trip. */
static void flood_feed(sm_broker_t *b, const uint8_t *buf, size_t n)
{
    if (!b->flood_active || !b->flood_stop_re || n == 0) return;

    size_t cap = sizeof(b->flood_tail);
    if (n >= cap) {
        memcpy(b->flood_tail, buf + (n - cap), cap);
        b->flood_tail_len = cap;
    } else {
        size_t keep = b->flood_tail_len;
        if (keep + n > cap) keep = cap - n;
        if (keep != b->flood_tail_len)
            memmove(b->flood_tail, b->flood_tail + (b->flood_tail_len - keep), keep);
        memcpy(b->flood_tail + keep, buf, n);
        b->flood_tail_len = keep + n;
    }

    if (sm_regex_exec(b->flood_stop_re, b->flood_tail, b->flood_tail_len, NULL) == 0)
        flood_finish(b, 1, "matched");
}

static void handle_flood_timer(sm_broker_t *b)
{
    uint64_t exp;
    while (read(b->flood_timer_fd, &exp, sizeof(exp)) == sizeof(exp))
        ;
    if (!b->flood_active) return;

    /* Release the reset line once the hold elapses — the device now boots with
     * keystrokes already streaming. */
    if (b->flood_reset_pending && sm_now_monotonic() >= b->flood_reset_deassert_time) {
        b->link->set_param(b->link, b->flood_reset_pin, b->flood_reset_deassert);
        b->flood_reset_pending = 0;
        SM_LOG_INFO(LOG_TAG, "reset_and_interrupt: released reset (%s)",
                    b->flood_reset_pin);
    }

    if (sm_now_monotonic() >= b->flood_deadline) {
        flood_finish(b, 0, "timeout");
        return;
    }
    if (sm_broker_do_write(b, b->flood_key, b->flood_key_len, "autoboot") < 0) {
        flood_finish(b, 0, "aborted");   /* link went down/suspended mid-flood */
        return;
    }
    b->flood_sent++;
}

int sm_broker_start_flood(sm_broker_t *b, const uint8_t *key, size_t key_len,
                          int interval_ms, int duration_ms,
                          const char *stop_pattern,
                          const char *client_id, const char *msg_id,
                          const char *reset_pin, const char *reset_assert,
                          int reset_hold_ms)
{
    if (b->suspended || b->link_disconnected) return -1;
    if (b->flood_active) return -1;                       /* one at a time */
    if (key_len == 0 || key_len > SM_FLOOD_KEY_MAX) return -1;

    if (interval_ms < SM_MIN_FLOOD_INTERVAL_MS)
        interval_ms = SM_DEFAULT_FLOOD_INTERVAL_MS;
    if (duration_ms <= 0) duration_ms = SM_DEFAULT_FLOOD_DURATION_MS;
    if (duration_ms > SM_MAX_FLOOD_DURATION_MS) duration_ms = SM_MAX_FLOOD_DURATION_MS;

    /* Validate reset params up front (before any side effect). */
    int want_reset = reset_pin && reset_pin[0];
    if (want_reset) {
        if (strcmp(reset_pin, "dtr") != 0 && strcmp(reset_pin, "rts") != 0)
            return -1;
        if (reset_hold_ms <= 0) reset_hold_ms = SM_DEFAULT_RESET_HOLD_MS;
        if (reset_hold_ms > SM_MAX_RESET_HOLD_MS) reset_hold_ms = SM_MAX_RESET_HOLD_MS;
        if (reset_hold_ms >= duration_ms) return -1;   /* flood must outlast hold */
    }

    sm_regex_t *re = NULL;
    if (stop_pattern && stop_pattern[0]) {
        char err[128];
        re = sm_regex_compile(stop_pattern, err, sizeof(err));
        if (!re) {
            SM_LOG_WARN(LOG_TAG, "autoboot flood: bad stop_pattern: %s", err);
            return -1;
        }
    }

    if (b->flood_timer_fd < 0) {
        b->flood_timer_fd =
            timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (b->flood_timer_fd >= 0) {
            struct epoll_event ev = {.events = EPOLLIN,
                                     .data.ptr = EPOLL_TAG_FLOODTIMER};
            if (epoll_ctl(b->epoll_fd, EPOLL_CTL_ADD, b->flood_timer_fd, &ev) < 0) {
                close(b->flood_timer_fd);
                b->flood_timer_fd = -1;
            }
        }
        if (b->flood_timer_fd < 0) {
            SM_LOG_ERROR(LOG_TAG, "flood timerfd: %s", strerror(errno));
            if (re) sm_regex_free(re);
            return -1;
        }
    }

    memcpy(b->flood_key, key, key_len);
    b->flood_key_len = key_len;
    b->flood_stop_re = re;
    b->flood_tail_len = 0;
    b->flood_sent = 0;
    b->flood_start = sm_now_monotonic();
    b->flood_deadline = b->flood_start + duration_ms / 1000.0;
    snprintf(b->flood_client_id, sizeof(b->flood_client_id), "%s",
             client_id ? client_id : "");
    snprintf(b->flood_msg_id, sizeof(b->flood_msg_id), "%s", msg_id ? msg_id : "");
    b->flood_reset_pending = 0;
    b->flood_reset_pin[0] = '\0';
    b->flood_active = 1;

    /* Assert the reset line now: the device is held in reset while the flood
     * streams, and the timer releases it after the hold (device then boots with
     * keys already in flight). */
    if (want_reset) {
        const char *assert_act = (reset_assert && reset_assert[0]) ? reset_assert : "clear";
        const char *deassert_act = strcmp(assert_act, "set") == 0 ? "clear" : "set";
        if (b->link->set_param(b->link, reset_pin, assert_act) != 0) {
            SM_LOG_WARN(LOG_TAG, "reset_and_interrupt: cannot assert %s", reset_pin);
            b->flood_active = 0;
            if (b->flood_stop_re) { sm_regex_free(b->flood_stop_re); b->flood_stop_re = NULL; }
            return -1;
        }
        snprintf(b->flood_reset_pin, sizeof(b->flood_reset_pin), "%s", reset_pin);
        snprintf(b->flood_reset_deassert, sizeof(b->flood_reset_deassert), "%s",
                 deassert_act);
        b->flood_reset_pending = 1;
        b->flood_reset_deassert_time = b->flood_start + reset_hold_ms / 1000.0;
    }

    if (flood_arm_timer(b, interval_ms) < 0) {
        SM_LOG_ERROR(LOG_TAG, "flood timer arm: %s", strerror(errno));
        if (b->flood_reset_pending) {
            b->link->set_param(b->link, b->flood_reset_pin, b->flood_reset_deassert);
            b->flood_reset_pending = 0;
        }
        b->flood_active = 0;
        if (b->flood_stop_re) { sm_regex_free(b->flood_stop_re); b->flood_stop_re = NULL; }
        return -1;
    }

    /* Fire the first key at once so the very first interval isn't lost. */
    if (sm_broker_do_write(b, b->flood_key, b->flood_key_len, "autoboot") == 0)
        b->flood_sent++;

    SM_LOG_INFO(LOG_TAG, "autoboot flood started: %dms every %dms%s%s",
                duration_ms, interval_ms, re ? " (stop-on-match)" : "",
                want_reset ? " (reset pulse)" : "");
    return 0;
}

/* --- Boot-stage stall notification (proactive push, one-shot timer) --- */

static void stall_disarm(sm_broker_t *b)
{
    if (b->stall_timer_fd >= 0) {
        struct itimerspec off = {0};
        timerfd_settime(b->stall_timer_fd, 0, &off, NULL);
    }
    b->stall_notified = 0;
}

/* (Re)arm the one-shot stall timer for stall_timeout from now. Called on each
 * stage advance; if the timeout elapses with no further advance the boot is,
 * by construction, stalled. Lazily creates the timerfd like the flood timer. */
static void stall_arm(sm_broker_t *b)
{
    int ms = (int)(b->boot.stall_timeout_s * 1000.0);
    if (ms <= 0) return;   /* stall detection disabled for this profile */

    if (b->stall_timer_fd < 0) {
        b->stall_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (b->stall_timer_fd >= 0) {
            struct epoll_event ev = {.events = EPOLLIN,
                                     .data.ptr = EPOLL_TAG_STALLTIMER};
            if (epoll_ctl(b->epoll_fd, EPOLL_CTL_ADD, b->stall_timer_fd, &ev) < 0) {
                close(b->stall_timer_fd);
                b->stall_timer_fd = -1;
            }
        }
        if (b->stall_timer_fd < 0) {
            SM_LOG_WARN(LOG_TAG, "stall timerfd: %s", strerror(errno));
            return;
        }
    }

    struct itimerspec its = {0};                 /* one-shot: it_interval left 0 */
    its.it_value.tv_sec  = ms / 1000;
    its.it_value.tv_nsec = (long)(ms % 1000) * 1000000L;
    timerfd_settime(b->stall_timer_fd, 0, &its, NULL);
    b->stall_notified = 0;
}

static void handle_stall_timer(sm_broker_t *b)
{
    uint64_t exp;
    while (read(b->stall_timer_fd, &exp, sizeof(exp)) == sizeof(exp))
        ;
    if (b->stall_notified) return;
    if (b->suspended || b->link_disconnected) return;
    if (b->boot.furthest < 0 || sm_boot_terminal_reached(&b->boot)) return;

    b->stall_notified = 1;

    int idx = b->boot.furthest;
    const char *name = ((size_t)idx < b->boot.stage_count)
                       ? b->boot.stages[idx].name : "";
    int ms = (int)(b->boot.stall_timeout_s * 1000.0);

    SM_LOG_WARN(LOG_TAG, "boot stalled at stage '%s' (%d/%zu) after %dms",
                name, idx + 1, b->boot.stage_count, ms);

    cJSON *msg = sm_msg_boot_stall(name, idx, (int)b->boot.stage_count, ms);
    broadcast(b, msg, NULL);
    cJSON_Delete(msg);
}

/* --- Broker operations (usable from handlers and sinks) --- */

int sm_broker_do_suspend(sm_broker_t *b, const char *by_name)
{
    if (b->suspended) return -1;

    break_cancel(b);
    flood_cancel(b);
    stall_disarm(b);

    int link_fd = b->link->read_fd(b->link);
    int write_fd = b->link->write_fd ? b->link->write_fd(b->link) : link_fd;
    if (link_fd >= 0)
        epoll_ctl(b->epoll_fd, EPOLL_CTL_DEL, link_fd, NULL);
    if (write_fd >= 0 && write_fd != link_fd)
        epoll_ctl(b->epoll_fd, EPOLL_CTL_DEL, write_fd, NULL);

    sm_expect_cancel_all(&b->expect);
    b->suspended = 1;
    b->link_connecting = 0;   /* abort any in-flight async reconnect */
    b->link->close(b->link);
    SM_LOG_INFO(LOG_TAG, "suspended by %s", by_name);

    cJSON *smsg = sm_msg_suspended(b->port, by_name);
    broadcast(b, smsg, NULL);
    cJSON_Delete(smsg);
    return 0;
}

int sm_broker_do_resume(sm_broker_t *b, const char *by_name)
{
    if (!b->suspended) return -1;

    int rc = b->link->open(b->link);
    if (rc != 0) return -2;

    b->suspended = 0;

    /* Reset health state after resume (we don't know yet if data will flow).
     * last_link_rx_time is monotonic — it drives only the idle-duration health
     * check, so it must not be perturbed by wall-clock/NTP steps. */
    b->last_link_rx_time = sm_now_monotonic();
    b->link_healthy = 0;   /* Will become healthy again on first real data */

    int link_fd = b->link->read_fd(b->link);
    if (link_fd >= 0) {
        struct epoll_event ev = {.events = EPOLLIN | EPOLLHUP | EPOLLERR,
                                 .data.ptr = EPOLL_TAG_LINK};
        if (epoll_ctl(b->epoll_fd, EPOLL_CTL_ADD, link_fd, &ev) < 0) {
            if (errno == EEXIST)
                epoll_ctl(b->epoll_fd, EPOLL_CTL_MOD, link_fd, &ev);
            else
                SM_LOG_WARN(LOG_TAG, "epoll_ctl add link on resume: %s", strerror(errno));
        }
        int write_fd = b->link->write_fd ? b->link->write_fd(b->link) : link_fd;
        if (write_fd >= 0 && write_fd != link_fd) {
            struct epoll_event wev = {.events = 0, .data.ptr = EPOLL_TAG_LINK_OUT};
            if (epoll_ctl(b->epoll_fd, EPOLL_CTL_ADD, write_fd, &wev) < 0 &&
                errno != EEXIST)
                SM_LOG_WARN(LOG_TAG, "epoll_ctl add link write on resume: %s",
                            strerror(errno));
        }
        broker_update_link_epoll(b);
    }

    /* If a boot was in progress when we suspended, resume watching for a stall
     * (a later stage advance would re-arm anyway, but a hang right after resume
     * should still be caught). */
    if (b->boot.furthest >= 0 && !sm_boot_terminal_reached(&b->boot))
        stall_arm(b);

    SM_LOG_INFO(LOG_TAG, "resumed by %s", by_name);

    cJSON *rmsg = sm_msg_resumed(b->port);
    broadcast(b, rmsg, NULL);
    cJSON_Delete(rmsg);
    return 0;
}

static void broker_update_link_epoll(sm_broker_t *b)
{
    if (!b->link || b->suspended || b->link_disconnected || b->epoll_fd < 0)
        return;

    int read_fd = b->link->read_fd(b->link);
    int write_fd = b->link->write_fd ? b->link->write_fd(b->link) : read_fd;
    int pending = b->link->has_write_pending &&
                  b->link->has_write_pending(b->link);

    if (read_fd >= 0) {
        uint32_t rev = EPOLLIN | EPOLLHUP | EPOLLERR;
        if (read_fd == write_fd && pending)
            rev |= EPOLLOUT;
        struct epoll_event ev = {.events = rev, .data.ptr = EPOLL_TAG_LINK};
        if (epoll_ctl(b->epoll_fd, EPOLL_CTL_MOD, read_fd, &ev) < 0 &&
            errno != ENOENT)
            SM_LOG_WARN(LOG_TAG, "epoll_ctl mod link read fd=%d: %s",
                        read_fd, strerror(errno));
    }

    if (write_fd >= 0 && write_fd != read_fd) {
        struct epoll_event ev = {.events = EPOLLOUT,
                                 .data.ptr = EPOLL_TAG_LINK_OUT};
        if (pending) {
            if (epoll_ctl(b->epoll_fd, EPOLL_CTL_MOD, write_fd, &ev) < 0) {
                if (errno == ENOENT)
                    epoll_ctl(b->epoll_fd, EPOLL_CTL_ADD, write_fd, &ev);
                else
                    SM_LOG_WARN(LOG_TAG, "epoll_ctl mod link write fd=%d: %s",
                                write_fd, strerror(errno));
            }
        } else {
            ev.events = 0;
            epoll_ctl(b->epoll_fd, EPOLL_CTL_MOD, write_fd, &ev);
        }
    }
}

int sm_broker_do_write(sm_broker_t *b, const uint8_t *data, size_t len,
                        const char *sender)
{
    if (b->suspended) return -1;
    if (b->link_disconnected) return -2;

    if (b->link->write_data(b->link, data, len) < 0)
        return -3;

    broker_update_link_epoll(b);

    if (b->io_log) {
        double ts = sm_now_realtime();
        sm_io_log_input(b->io_log, data, len, sender, ts);
    }
    return 0;
}

/* --- Message handlers --- */

static void handle_hello(sm_broker_t *b, sm_client_t *c, sm_msg_t *msg)
{
    /* Reject hello re-sends — prevents observer-to-controller escalation */
    if (c->hello_received) {
        send_to_client(c, sm_msg_error(NULL, "hello already received"));
        return;
    }

    /* Network-origin clients must present the configured token (M4) */
    if (c->requires_auth && b->auth_token[0]) {
        const char *token = sm_json_get_string(msg->root, "token");
        if (!token || !ct_str_equal(token, b->auth_token)) {
            SM_LOG_WARN(LOG_TAG, "client %s: bad or missing auth token", c->id);
            send_to_client(c, sm_msg_error(NULL, "authentication failed"));
            sm_client_flush(c);  /* best effort before dropping */
            c->disconnected = 1;
            return;
        }
    }

    const char *name = sm_json_get_string(msg->root, "name");
    const char *role = sm_json_get_string(msg->root, "role");

    /* Validate role */
    if (role && strcmp(role, "observer") != 0 && strcmp(role, "controller") != 0) {
        send_to_client(c, sm_msg_error(NULL, "invalid role"));
        return;
    }

    if (name) snprintf(c->name, sizeof(c->name), "%s", name);

    /* Check protocol version — reject if missing or mismatched */
    cJSON *proto_ver = cJSON_GetObjectItemCaseSensitive(msg->root, "protocol_version");
    if (!cJSON_IsNumber(proto_ver) || proto_ver->valueint != SM_PROTOCOL_VERSION) {
        send_to_client(c, sm_msg_error(NULL, "protocol version mismatch"));
        return;
    }

    if (role)
        snprintf(c->role, sizeof(c->role), "%s", role);

    c->hello_received = 1;

    SM_LOG_INFO(LOG_TAG, "client %s hello: name=%s role=%s", c->id, c->name, c->role);

    cJSON *welcome = sm_msg_welcome(SM_VERSION, b->port, b->baudrate, c->role);
    send_to_client(c, welcome);
}

static void handle_send(sm_broker_t *b, sm_client_t *c, sm_msg_t *msg)
{
    const char *id = sm_json_get_string(msg->root, "id");
    if (!can_send(b, c)) {
        send_to_client(c, sm_msg_error(id, "not authorized to send"));
        return;
    }

    const char *data_b64 = sm_json_get_string(msg->root, "data");
    if (!data_b64) {
        send_to_client(c, sm_msg_error(id, "missing data field"));
        return;
    }
    if (strlen(data_b64) > SM_MAX_SEND_B64_LEN) {
        send_to_client(c, sm_msg_error(id, "payload too large"));
        return;
    }

    size_t data_len;
    uint8_t *data = sm_base64_decode(data_b64, strlen(data_b64), &data_len);
    if (!data) {
        send_to_client(c, sm_msg_error(id, "invalid base64 data"));
        return;
    }

    int rc = sm_broker_do_write(b, data, data_len, c->name);
    if (rc == 0) {
        /* Input echo to other clients */
        double ts = sm_now_realtime();
        cJSON *echo = sm_msg_input_echo(data, data_len, c->name, ts);
        broadcast(b, echo, c);
        cJSON_Delete(echo);
    }
    free(data);
    if (rc == -1)
        send_to_client(c, sm_msg_error(id, "serial port is suspended"));
    else if (rc == -2)
        send_to_client(c, sm_msg_error(id, "serial port disconnected"));
    else if (rc == -3)
        send_to_client(c, sm_msg_error(id, "write failed"));
}

static void handle_send_expect(sm_broker_t *b, sm_client_t *c, sm_msg_t *msg)
{
    const char *id = sm_json_get_string(msg->root, "id");
    if (!can_send(b, c)) {
        send_to_client(c, sm_msg_error(id, "not authorized to send"));
        return;
    }
    if (b->suspended) {
        send_to_client(c, sm_msg_error(id, "serial port is suspended"));
        return;
    }
    if (b->link_disconnected) {
        send_to_client(c, sm_msg_error(id, "serial port disconnected"));
        return;
    }

    const char *data_b64 = sm_json_get_string(msg->root, "data");
    const char *pattern = sm_json_get_string(msg->root, "pattern");
    int timeout_ms = sm_json_get_int(msg->root, "timeout_ms", SM_DEFAULT_EXPECT_TIMEOUT_MS);

    if (!pattern || !id) {
        send_to_client(c, sm_msg_error(id, "missing id or pattern"));
        return;
    }

    /* Clamp timeout to prevent indefinite resource consumption */
    if (timeout_ms < 100) timeout_ms = 100;
    if (timeout_ms > SM_MAX_EXPECT_TIMEOUT_MS) timeout_ms = SM_MAX_EXPECT_TIMEOUT_MS;

    /* Register expect BEFORE writing data */
    double timeout_s = (double)timeout_ms / 1000.0;
    if (sm_expect_add(&b->expect, id, pattern, timeout_s, c->id) != 0) {
        send_to_client(c, sm_msg_error(id, "invalid regex pattern"));
        return;
    }

    /* Send data to device */
    if (data_b64) {
        if (strlen(data_b64) > SM_MAX_SEND_B64_LEN) {
            sm_expect_cancel_id(&b->expect, id);
            send_to_client(c, sm_msg_error(id, "payload too large"));
            return;
        }
        size_t data_len;
        uint8_t *data = sm_base64_decode(data_b64, strlen(data_b64), &data_len);
        if (!data) {
            sm_expect_cancel_id(&b->expect, id);
            send_to_client(c, sm_msg_error(id, "invalid base64 data"));
            return;
        }
        int rc = sm_broker_do_write(b, data, data_len, c->name);
        if (rc < 0) {
            sm_expect_cancel_id(&b->expect, id);
            send_to_client(c, sm_msg_error(id, "write failed"));
            free(data);
            return;
        }
        double ts = sm_now_realtime();
        cJSON *echo = sm_msg_input_echo(data, data_len, c->name, ts);
        broadcast(b, echo, c);
        cJSON_Delete(echo);
        free(data);
    }
}

static void handle_takeover(sm_broker_t *b, sm_client_t *c, sm_msg_t *msg)
{
    const char *id = sm_json_get_string(msg->root, "id");
    if (strcmp(c->role, "observer") == 0) {
        send_to_client(c, sm_msg_error(id, "observers cannot take over"));
        return;
    }
    if (b->takeover_client && b->takeover_client != c) {
        send_to_client(c, sm_msg_error(id, "another client has takeover"));
        return;
    }

    b->takeover_client = c;
    SM_LOG_INFO(LOG_TAG, "client %s (%s) took over", c->id, c->name);
    send_to_client(c, sm_msg_ack("takeover", id));
}

static void handle_release(sm_broker_t *b, sm_client_t *c, sm_msg_t *msg)
{
    const char *id = sm_json_get_string(msg->root, "id");
    if (b->takeover_client == c) {
        b->takeover_client = NULL;
        SM_LOG_INFO(LOG_TAG, "client %s (%s) released takeover", c->id, c->name);
    }
    send_to_client(c, sm_msg_ack("release", id));
}

static void handle_status(sm_broker_t *b, sm_client_t *c, sm_msg_t *msg)
{
    const char *id = sm_json_get_string(msg->root, "id");

    cJSON *resp = sm_msg_status_response(id ? id : "", b->port, b->baudrate,
                                         b->link->read_fd(b->link) >= 0,
                                         b->suspended);

    /* Add client list */
    cJSON *clients = cJSON_CreateArray();
    for (size_t i = 0; i < b->client_count; i++) {
        sm_client_t *cl = b->clients[i];
        cJSON *ci = cJSON_CreateObject();
        cJSON_AddStringToObject(ci, "id", cl->id);
        cJSON_AddStringToObject(ci, "name", cl->name);
        cJSON_AddStringToObject(ci, "role", cl->role);
        cJSON_AddNumberToObject(ci, "wq_drops", (double)cl->wq_drops);
        cJSON_AddNumberToObject(ci, "wq_count", (double)cl->wq_count);
        cJSON_AddItemToArray(clients, ci);
    }
    cJSON_AddItemToObject(resp, "clients", clients);

    /* Add takeover info */
    if (b->takeover_client)
        cJSON_AddStringToObject(resp, "takeover_client", b->takeover_client->name);
    else
        cJSON_AddNullToObject(resp, "takeover_client");

    /* Board grouping labels (empty string when unset) */
    cJSON_AddStringToObject(resp, "board", b->board);
    cJSON_AddStringToObject(resp, "role", b->role);

    /* Add pin states via link */
    b->link->get_status(b->link, resp);

    /* Add log path */
    if (b->io_log)
        cJSON_AddStringToObject(resp, "log_path", b->io_log->path);

    /* Boot-stage progress (only when a profile declares stages) */
    if (b->boot.stage_count > 0) {
        cJSON *boot = cJSON_CreateObject();
        cJSON_AddNumberToObject(boot, "furthest", b->boot.furthest);
        cJSON_AddNumberToObject(boot, "total", (double)b->boot.stage_count);
        cJSON_AddBoolToObject(boot, "stalled",
                              sm_boot_stalled(&b->boot, sm_now_realtime()));
        cJSON_AddBoolToObject(boot, "terminal_reached",
                              sm_boot_terminal_reached(&b->boot));
        cJSON *stages = cJSON_CreateArray();
        for (size_t i = 0; i < b->boot.stage_count; i++) {
            sm_boot_stage_t *st = &b->boot.stages[i];
            cJSON *sj = cJSON_CreateObject();
            cJSON_AddStringToObject(sj, "name", st->name);
            cJSON_AddBoolToObject(sj, "reached", st->reached);
            if (st->reached)
                cJSON_AddNumberToObject(sj, "timestamp", st->reached_ts);
            cJSON_AddItemToArray(stages, sj);
        }
        cJSON_AddItemToObject(boot, "stages", stages);
        cJSON_AddItemToObject(resp, "boot", boot);
    }

    send_to_client(c, resp);
}

/* Completion context for a wire-protocol break request. The client is
 * remembered by id, not pointer — it may disconnect mid-break. */
typedef struct pin_break_ctx {
    char client_id[32];
    char msg_id[64];
    int has_id;
} pin_break_ctx_t;

static void pin_break_done(sm_broker_t *b, void *ctx_, int rc)
{
    pin_break_ctx_t *ctx = ctx_;
    const char *id = ctx->has_id ? ctx->msg_id : NULL;

    for (size_t i = 0; i < b->client_count; i++) {
        sm_client_t *c = b->clients[i];
        if (strcmp(c->id, ctx->client_id) != 0) continue;
        if (rc != 0)
            send_to_client(c, sm_msg_error(id, "pin control failed"));
        else
            send_to_client(c, sm_msg_ack("pin_control", id));
        break;
    }
    free(ctx);
}

static void handle_interrupt_autoboot(sm_broker_t *b, sm_client_t *c, sm_msg_t *msg)
{
    const char *id = sm_json_get_string(msg->root, "id");
    if (!can_send(b, c)) {
        send_to_client(c, sm_msg_error(id, "not authorized to send"));
        return;
    }

    /* Key bytes: prefer key_b64 (allows control chars); else literal `key`
     * string; default a single space (the usual U-Boot break key). */
    uint8_t keybuf[SM_FLOOD_KEY_MAX];
    const uint8_t *key = (const uint8_t *)" ";
    size_t key_len = 1;
    uint8_t *decoded = NULL;

    const char *kb64 = sm_json_get_string(msg->root, "key_b64");
    const char *kstr = sm_json_get_string(msg->root, "key");
    if (kb64 && kb64[0]) {
        size_t dlen = 0;
        decoded = sm_base64_decode(kb64, strlen(kb64), &dlen);
        if (!decoded || dlen == 0 || dlen > SM_FLOOD_KEY_MAX) {
            free(decoded);
            send_to_client(c, sm_msg_error(id, "invalid key_b64"));
            return;
        }
        key = decoded;
        key_len = dlen;
    } else if (kstr && kstr[0]) {
        size_t slen = strlen(kstr);
        if (slen > SM_FLOOD_KEY_MAX) slen = SM_FLOOD_KEY_MAX;
        memcpy(keybuf, kstr, slen);
        key = keybuf;
        key_len = slen;
    }

    int interval_ms = sm_json_get_int(msg->root, "interval_ms",
                                      SM_DEFAULT_FLOOD_INTERVAL_MS);
    int duration_ms = sm_json_get_int(msg->root, "duration_ms",
                                      SM_DEFAULT_FLOOD_DURATION_MS);
    const char *stop = sm_json_get_string(msg->root, "stop_pattern");

    /* Optional reset pulse (reset_and_interrupt). */
    const char *reset_pin = sm_json_get_string(msg->root, "reset_pin");
    const char *reset_assert = sm_json_get_string(msg->root, "reset_assert");
    int reset_hold_ms = sm_json_get_int(msg->root, "reset_hold_ms",
                                        SM_DEFAULT_RESET_HOLD_MS);

    int rc = sm_broker_start_flood(b, key, key_len, interval_ms, duration_ms,
                                   stop, c->id, id,
                                   reset_pin, reset_assert, reset_hold_ms);
    free(decoded);
    if (rc != 0)
        send_to_client(c, sm_msg_error(id, "autoboot flood rejected (busy, "
                       "suspended, link down, or bad args)"));
    /* On success the autoboot_result is broadcast later by flood_finish. */
}

static void handle_pin_control(sm_broker_t *b, sm_client_t *c, sm_msg_t *msg)
{
    const char *id = sm_json_get_string(msg->root, "id");
    if (!can_send(b, c)) {
        send_to_client(c, sm_msg_error(id, "not authorized"));
        return;
    }

    const char *pin = sm_json_get_string(msg->root, "pin");
    const char *action = sm_json_get_string(msg->root, "action");
    int duration_ms = sm_json_get_int(msg->root, "duration_ms", 250);

    if (!pin || !action) {
        send_to_client(c, sm_msg_error(id, "missing pin or action"));
        return;
    }

    if (strcmp(pin, "break") == 0) {
        pin_break_ctx_t *ctx = calloc(1, sizeof(*ctx));
        if (!ctx) {
            send_to_client(c, sm_msg_error(id, "out of memory"));
            return;
        }
        snprintf(ctx->client_id, sizeof(ctx->client_id), "%s", c->id);
        if (id) {
            ctx->has_id = 1;
            snprintf(ctx->msg_id, sizeof(ctx->msg_id), "%s", id);
        }
        /* Ack is sent by pin_break_done when the break completes */
        if (sm_broker_schedule_break(b, duration_ms, NULL, 0, 0,
                                     pin_break_done, ctx) != 0) {
            free(ctx);
            send_to_client(c, sm_msg_error(id, "break busy or unavailable"));
        }
        return;
    }

    if (b->link->set_param(b->link, pin, action) != 0)
        send_to_client(c, sm_msg_error(id, "pin control failed"));
    else
        send_to_client(c, sm_msg_ack("pin_control", id));
}

static void handle_set_baud(sm_broker_t *b, sm_client_t *c, sm_msg_t *msg)
{
    const char *id = sm_json_get_string(msg->root, "id");
    if (!can_send(b, c)) {
        send_to_client(c, sm_msg_error(id, "not authorized"));
        return;
    }

    int baud = sm_json_get_int(msg->root, "baud", 0);
    if (baud <= 0) {
        send_to_client(c, sm_msg_error(id, "invalid baud rate"));
        return;
    }

    char baud_str[16];
    snprintf(baud_str, sizeof(baud_str), "%d", baud);
    int rc = b->link->set_param(b->link, "baud", baud_str);
    if (rc == 0) {
        b->baudrate = baud;
        cJSON *ack = sm_msg_ack("set_baud", id);
        cJSON_AddNumberToObject(ack, "baud", baud);
        send_to_client(c, ack);
    } else {
        send_to_client(c, sm_msg_error(id, "failed to set baud rate"));
    }
}

static void handle_suspend(sm_broker_t *b, sm_client_t *c, sm_msg_t *msg)
{
    const char *id = sm_json_get_string(msg->root, "id");
    if (!can_send(b, c)) {
        send_to_client(c, sm_msg_error(id, "not authorized to suspend"));
        return;
    }

    int rc = sm_broker_do_suspend(b, c->name);
    if (rc < 0)
        send_to_client(c, sm_msg_error(id, "already suspended"));
}

static void handle_resume(sm_broker_t *b, sm_client_t *c, sm_msg_t *msg)
{
    const char *id = sm_json_get_string(msg->root, "id");
    if (!can_send(b, c)) {
        send_to_client(c, sm_msg_error(id, "not authorized to resume"));
        return;
    }

    int rc = sm_broker_do_resume(b, c->name);
    if (rc == -1)
        send_to_client(c, sm_msg_error(id, "not suspended"));
    else if (rc == -2)
        send_to_client(c, sm_msg_error(id, "failed to reopen serial port"));
}

static void history_pending_clear(sm_broker_t *b)
{
    if (b->history_pending.chunks) {
        for (size_t i = 0; i < b->history_pending.chunk_count; i++)
            free(b->history_pending.chunks[i].data);
        free(b->history_pending.chunks);
    }
    if (b->history_pending.response_arr)
        cJSON_Delete(b->history_pending.response_arr);
    memset(&b->history_pending, 0, sizeof(b->history_pending));
}

static void process_history_pending(sm_broker_t *b)
{
    if (!b->history_pending.active)
        return;

    size_t chunks_done = 0;
    size_t bytes_done = 0;

    while (b->history_pending.encode_index < b->history_pending.chunk_count &&
           chunks_done < SM_HISTORY_ENCODE_MAX_CHUNKS &&
           bytes_done < SM_HISTORY_ENCODE_MAX_BYTES) {
        sm_rb_chunk_t *ch =
            &b->history_pending.chunks[b->history_pending.encode_index];
        cJSON *chunk = cJSON_CreateObject();
        char *b64 = sm_base64_encode(ch->data, ch->len);
        cJSON_AddStringToObject(chunk, "data", b64 ? b64 : "");
        cJSON_AddNumberToObject(chunk, "timestamp", ch->timestamp);
        cJSON_AddItemToArray(b->history_pending.response_arr, chunk);
        free(b64);
        bytes_done += ch->len;
        chunks_done++;
        b->history_pending.encode_index++;
    }

    if (b->history_pending.encode_index < b->history_pending.chunk_count)
        return;

    sm_client_t *target = NULL;
    for (size_t i = 0; i < b->client_count; i++) {
        if (strcmp(b->clients[i]->id, b->history_pending.client_id) == 0) {
            target = b->clients[i];
            break;
        }
    }

    if (target) {
        cJSON *resp = sm_msg_history_response(b->history_pending.msg_id,
                                              b->history_pending.response_arr);
        b->history_pending.response_arr = NULL;
        if (b->history_pending.truncated)
            cJSON_AddBoolToObject(resp, "truncated", 1);
        send_to_client(target, resp);
    }

    history_pending_clear(b);
}

static void handle_history_request(sm_broker_t *b, sm_client_t *c, sm_msg_t *msg)
{
    const char *id = sm_json_get_string(msg->root, "id");

    if (b->history_pending.active) {
        send_to_client(c, sm_msg_error(id, "history request already in progress"));
        return;
    }

    double since_ts = sm_json_get_double(msg->root, "since_ts", 0.0);
    int last_bytes = sm_json_get_int(msg->root, "last_bytes", 0);

    sm_rb_chunk_t *chunks = NULL;
    size_t count;

    if (last_bytes > 0)
        count = sm_rb_get_last_n_bytes(&b->history, (size_t)last_bytes, &chunks);
    else if (since_ts > 0)
        count = sm_rb_get_since(&b->history, since_ts, &chunks);
    else
        count = sm_rb_get_all(&b->history, &chunks);

    size_t start = count, total = 0;
    while (start > 0 &&
           total + chunks[start - 1].len <= SM_MAX_HISTORY_RESPONSE_BYTES) {
        total += chunks[start - 1].len;
        start--;
    }
    int truncated = (start > 0);
    if (truncated)
        SM_LOG_WARN(LOG_TAG,
                    "history_request truncated: %zu of %zu chunks returned",
                    count - start, count);

    size_t sel_count = count - start;
    if (sel_count == 0) {
        free(chunks);
        cJSON *resp = sm_msg_history_response(id ? id : "", cJSON_CreateArray());
        if (truncated)
            cJSON_AddBoolToObject(resp, "truncated", 1);
        send_to_client(c, resp);
        return;
    }

    sm_rb_chunk_t *owned = calloc(sel_count, sizeof(*owned));
    if (!owned) {
        free(chunks);
        send_to_client(c, sm_msg_error(id, "out of memory"));
        return;
    }

    for (size_t i = 0; i < sel_count; i++) {
        sm_rb_chunk_t *src = &chunks[start + i];
        owned[i].timestamp = src->timestamp;
        owned[i].len = src->len;
        owned[i].alloc = src->len;
        owned[i].data = malloc(src->len);
        if (!owned[i].data) {
            for (size_t j = 0; j < i; j++)
                free(owned[j].data);
            free(owned);
            free(chunks);
            send_to_client(c, sm_msg_error(id, "out of memory"));
            return;
        }
        memcpy(owned[i].data, src->data, src->len);
    }
    free(chunks);

    snprintf(b->history_pending.client_id, sizeof(b->history_pending.client_id),
             "%s", c->id);
    snprintf(b->history_pending.msg_id, sizeof(b->history_pending.msg_id),
             "%s", id ? id : "");
    b->history_pending.chunks = owned;
    b->history_pending.chunk_count = sel_count;
    b->history_pending.encode_index = 0;
    b->history_pending.response_arr = cJSON_CreateArray();
    b->history_pending.truncated = truncated;
    b->history_pending.active = 1;
}

static void handle_incidents_request(sm_broker_t *b, sm_client_t *c, sm_msg_t *msg)
{
    const char *id = sm_json_get_string(msg->root, "id");
    double since_ts = sm_json_get_double(msg->root, "since_ts", 0.0);

    size_t count;
    const sm_anomaly_incident_t *incidents = sm_anomaly_get_incidents(&b->anomaly, &count);

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) {
        if (since_ts > 0 && incidents[i].timestamp < since_ts) continue;
        cJSON *inc = cJSON_CreateObject();
        cJSON_AddStringToObject(inc, "incident_id", incidents[i].id);
        cJSON_AddStringToObject(inc, "pattern_name", incidents[i].pattern_name);
        cJSON_AddStringToObject(inc, "severity", incidents[i].severity);
        cJSON_AddNumberToObject(inc, "timestamp", incidents[i].timestamp);
        cJSON_AddStringToObject(inc, "match_text", incidents[i].match_text);
        cJSON_AddStringToObject(inc, "pre_context", incidents[i].pre_context);
        cJSON_AddItemToArray(arr, inc);
    }

    cJSON *resp = sm_msg_incidents_response(id ? id : "", arr);
    send_to_client(c, resp);
}

static void handle_configure_anomaly(sm_broker_t *b, sm_client_t *c, sm_msg_t *msg)
{
    const char *id = sm_json_get_string(msg->root, "id");

    if (!can_send(b, c)) {
        send_to_client(c, sm_msg_error(id, "not authorized"));
        return;
    }

    cJSON *patterns = cJSON_GetObjectItemCaseSensitive(msg->root, "patterns");
    if (!cJSON_IsArray(patterns)) {
        send_to_client(c, sm_msg_error(id, "patterns must be an array"));
        return;
    }

    int added = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, patterns) {
        const char *name = sm_json_get_string(item, "name");
        const char *pat = sm_json_get_string(item, "pattern");
        const char *sev = sm_json_get_string(item, "severity");
        if (name && pat) {
            if (sm_anomaly_add_pattern(&b->anomaly, name, pat,
                                        sev ? sev : "warning") == 0)
                added++;
        }
    }

    cJSON *ack = sm_msg_ack("configure_anomaly", id);
    cJSON_AddNumberToObject(ack, "patterns_added", added);
    send_to_client(c, ack);
}

static void handle_configure_autoresponder(sm_broker_t *b, sm_client_t *c,
                                           sm_msg_t *msg)
{
    const char *id = sm_json_get_string(msg->root, "id");
    if (!can_send(b, c)) {
        send_to_client(c, sm_msg_error(id, "not authorized"));
        return;
    }

    const char *name = sm_json_get_string(msg->root, "name");
    if (!name || !name[0]) {
        send_to_client(c, sm_msg_error(id, "missing name"));
        return;
    }

    /* Remove a rule by name. */
    if (sm_json_get_bool(msg->root, "remove", 0)) {
        int removed = sm_autoresponder_remove(&b->autoresponder, name);
        cJSON *ack = sm_msg_ack("configure_autoresponder", id);
        cJSON_AddBoolToObject(ack, "removed", removed);
        send_to_client(c, ack);
        return;
    }

    const char *pattern = sm_json_get_string(msg->root, "pattern");
    if (!pattern || !pattern[0]) {
        send_to_client(c, sm_msg_error(id, "missing pattern"));
        return;
    }

    /* Response bytes: response_b64 (allows control chars) or literal response. */
    uint8_t respbuf[SM_AR_RESPONSE_MAX];
    const uint8_t *response = NULL;
    size_t response_len = 0;
    uint8_t *decoded = NULL;

    const char *rb64 = sm_json_get_string(msg->root, "response_b64");
    const char *rstr = sm_json_get_string(msg->root, "response");
    if (rb64 && rb64[0]) {
        size_t dlen = 0;
        decoded = sm_base64_decode(rb64, strlen(rb64), &dlen);
        if (!decoded || dlen > SM_AR_RESPONSE_MAX) {
            free(decoded);
            send_to_client(c, sm_msg_error(id, "invalid response_b64"));
            return;
        }
        response = decoded;
        response_len = dlen;
    } else if (rstr && rstr[0]) {
        size_t slen = strlen(rstr);
        if (slen > SM_AR_RESPONSE_MAX) slen = SM_AR_RESPONSE_MAX;
        memcpy(respbuf, rstr, slen);
        response = respbuf;
        response_len = slen;
    }

    int once = sm_json_get_bool(msg->root, "once", 0);
    int cooldown_ms = sm_json_get_int(msg->root, "cooldown_ms",
                                      SM_AR_DEFAULT_COOLDOWN_MS);

    int rc = sm_autoresponder_add(&b->autoresponder, name, pattern,
                                  response, response_len, once, cooldown_ms);
    free(decoded);
    if (rc != 0) {
        send_to_client(c, sm_msg_error(id, "autoresponder rejected (bad regex, "
                       "full, or response too long)"));
        return;
    }

    cJSON *ack = sm_msg_ack("configure_autoresponder", id);
    cJSON_AddStringToObject(ack, "name", name);
    send_to_client(c, ack);
}

static void handle_autoresponders_request(sm_broker_t *b, sm_client_t *c,
                                          sm_msg_t *msg)
{
    const char *id = sm_json_get_string(msg->root, "id");

    cJSON *rules = cJSON_CreateArray();
    for (size_t i = 0; i < b->autoresponder.rule_count; i++) {
        sm_ar_rule_t *r = &b->autoresponder.rules[i];
        cJSON *ro = cJSON_CreateObject();
        cJSON_AddStringToObject(ro, "name", r->name);
        cJSON_AddStringToObject(ro, "pattern", r->pattern);
        char *rb64 = sm_base64_encode(r->response, r->response_len);
        cJSON_AddStringToObject(ro, "response_b64", rb64 ? rb64 : "");
        free(rb64);
        cJSON_AddBoolToObject(ro, "once", r->once);
        cJSON_AddBoolToObject(ro, "enabled", r->enabled);
        cJSON_AddNumberToObject(ro, "cooldown_ms", (int)(r->cooldown_s * 1000.0));
        cJSON_AddItemToArray(rules, ro);
    }

    send_to_client(c, sm_msg_autoresponders_response(id ? id : "", rules));
}

static void handle_client_message(sm_broker_t *b, sm_client_t *c, sm_msg_t *msg)
{
    /* Require hello before any other message */
    if (!c->hello_received && msg->type != SM_MSG_HELLO) {
        send_to_client(c, sm_msg_error(NULL, "hello required"));
        return;
    }

    switch (msg->type) {
    case SM_MSG_HELLO:              handle_hello(b, c, msg); break;
    case SM_MSG_SEND:               handle_send(b, c, msg); break;
    case SM_MSG_SEND_EXPECT:        handle_send_expect(b, c, msg); break;
    case SM_MSG_TAKEOVER:           handle_takeover(b, c, msg); break;
    case SM_MSG_RELEASE:            handle_release(b, c, msg); break;
    case SM_MSG_STATUS:             handle_status(b, c, msg); break;
    case SM_MSG_PIN_CONTROL:        handle_pin_control(b, c, msg); break;
    case SM_MSG_SET_BAUD:           handle_set_baud(b, c, msg); break;
    case SM_MSG_SUSPEND:            handle_suspend(b, c, msg); break;
    case SM_MSG_RESUME:             handle_resume(b, c, msg); break;
    case SM_MSG_HISTORY_REQUEST:    handle_history_request(b, c, msg); break;
    case SM_MSG_INCIDENTS_REQUEST:  handle_incidents_request(b, c, msg); break;
    case SM_MSG_CONFIGURE_ANOMALY:  handle_configure_anomaly(b, c, msg); break;
    case SM_MSG_CONFIGURE_AUTORESPONDER:
        handle_configure_autoresponder(b, c, msg); break;
    case SM_MSG_AUTORESPONDERS_REQUEST:
        handle_autoresponders_request(b, c, msg); break;
    case SM_MSG_INTERRUPT_AUTOBOOT: handle_interrupt_autoboot(b, c, msg); break;
    default: {
        const char *id = sm_json_get_string(msg->root, "id");
        send_to_client(c, sm_msg_error(id, "unknown message type"));
        break;
    }
    }
}

/* --- Broker lifecycle --- */

int sm_broker_init(sm_broker_t *b, sm_link_t *link, const char *socket_path)
{
    memset(b, 0, sizeof(*b));
    b->link = link;
    snprintf(b->socket_path, sizeof(b->socket_path), "%s", socket_path);

    sm_expect_init(&b->expect);
    sm_rb_init(&b->history, SM_RING_BUFFER_MAX_BYTES);
    sm_anomaly_init(&b->anomaly);
    sm_anomaly_add_builtins(&b->anomaly);
    sm_boot_init(&b->boot);
    sm_autoresponder_init(&b->autoresponder);

    b->client_cap = SM_MAX_CLIENTS;
    b->clients = calloc(b->client_cap, sizeof(sm_client_t *));
    if (!b->clients) {
        SM_LOG_ERROR(LOG_TAG, "failed to allocate client array");
        sm_expect_destroy(&b->expect);
        sm_rb_destroy(&b->history);
        sm_anomaly_destroy(&b->anomaly);
        sm_boot_destroy(&b->boot);
        sm_autoresponder_destroy(&b->autoresponder);
        return -1;
    }

    b->listen_fd = -1;
    b->epoll_fd = -1;
    b->reg_pipe[0] = -1;
    b->reg_pipe[1] = -1;
    b->break_timer_fd = -1;
    b->flood_timer_fd = -1;
    b->stall_timer_fd = -1;
    b->stall_notified = 0;
    b->reconnect = 1;

    if (pipe2(b->reg_pipe, O_CLOEXEC | O_NONBLOCK) < 0) {
        SM_LOG_ERROR(LOG_TAG, "pipe2: %s", strerror(errno));
        free(b->clients);
        b->clients = NULL;
        sm_expect_destroy(&b->expect);
        sm_rb_destroy(&b->history);
        sm_anomaly_destroy(&b->anomaly);
        sm_boot_destroy(&b->boot);
        sm_autoresponder_destroy(&b->autoresponder);
        return -1;
    }

    return 0;
}

static int create_listen_socket(sm_broker_t *b)
{
    /* Check for symlink attack before unlinking */
    struct stat st;
    if (lstat(b->socket_path, &st) == 0) {
        if (S_ISLNK(st.st_mode)) {
            SM_LOG_ERROR(LOG_TAG, "socket path %s is a symlink, refusing to unlink",
                         b->socket_path);
            return -1;
        }
        /* Check if another instance is already running on this socket */
        if (S_ISSOCK(st.st_mode)) {
            int test_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (test_fd >= 0) {
                struct sockaddr_un test_addr;
                memset(&test_addr, 0, sizeof(test_addr));
                test_addr.sun_family = AF_UNIX;
                snprintf(test_addr.sun_path, sizeof(test_addr.sun_path),
                         "%s", b->socket_path);
                if (connect(test_fd, (struct sockaddr *)&test_addr,
                            sizeof(test_addr)) == 0) {
                    close(test_fd);
                    SM_LOG_ERROR(LOG_TAG,
                                 "another instance is already running on %s",
                                 b->socket_path);
                    return -1;
                }
                close(test_fd);
            }
        }
    }

    /* Bind to a unique temp name, then rename() over the final path.
     * rename replaces whatever sits there — including a symlink planted
     * after the lstat above — without following it, so there is no
     * check-to-unlink race window. */
    char tmp_path[sizeof(b->socket_path) + 16];
    int tlen = snprintf(tmp_path, sizeof(tmp_path), "%s.%d.tmp",
                        b->socket_path, (int)getpid());

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (tlen < 0 || (size_t)tlen >= sizeof(addr.sun_path)) {
        SM_LOG_ERROR(LOG_TAG, "socket path too long for temp bind: %s",
                     b->socket_path);
        return -1;
    }
    memcpy(addr.sun_path, tmp_path, (size_t)tlen + 1);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    unlink(tmp_path);  /* stale leftover from a crashed run */

    /* Set restrictive umask before bind to prevent permission race */
    mode_t old_umask = umask(0077);
    int bind_rc = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    umask(old_umask);
    if (bind_rc < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 8) < 0) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }
    if (rename(tmp_path, b->socket_path) < 0) {
        SM_LOG_ERROR(LOG_TAG, "rename %s -> %s: %s", tmp_path,
                     b->socket_path, strerror(errno));
        close(fd);
        unlink(tmp_path);
        return -1;
    }

    set_nonblocking(fd);
    return fd;
}

sm_client_t *sm_broker_register_client(sm_broker_t *b, int fd)
{
    set_nonblocking(fd);

    if (b->client_count >= b->client_cap) {
        SM_LOG_WARN(LOG_TAG, "max clients reached, rejecting");
        close(fd);
        return NULL;
    }

    sm_client_t *c = sm_client_new(fd, b->next_client_num++);
    if (!c) {
        SM_LOG_ERROR(LOG_TAG, "failed to allocate client");
        close(fd);
        return NULL;
    }
    c->connected_at = sm_now_monotonic();
    b->clients[b->client_count++] = c;

    struct epoll_event ev = {.events = EPOLLIN | EPOLLHUP | EPOLLERR, .data.ptr = c};
    if (epoll_ctl(b->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        SM_LOG_ERROR(LOG_TAG, "epoll_ctl add client fd=%d: %s", fd, strerror(errno));
        b->client_count--;
        sm_client_destroy(c);  /* also closes fd */
        return NULL;
    }

    SM_LOG_INFO(LOG_TAG, "new client %s (fd=%d)", c->id, fd);
    return c;
}

static void accept_client(sm_broker_t *b)
{
    int fd = accept4(b->listen_fd, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0) return;
    sm_broker_register_client(b, fd);
}

static void handle_link_disconnect(sm_broker_t *b);

static sm_drain_read_hook_fn g_drain_read_hook;
static sm_drain_avail_hook_fn g_drain_avail_hook;

void sm_broker_test_set_drain_hooks(sm_drain_read_hook_fn read_fn,
                                    sm_drain_avail_hook_fn avail_fn)
{
    g_drain_read_hook = read_fn;
    g_drain_avail_hook = avail_fn;
}

void sm_broker_test_reset_drain_hooks(void)
{
    g_drain_read_hook = NULL;
    g_drain_avail_hook = NULL;
}

static ssize_t drain_read(int fd, void *buf, size_t len)
{
    if (g_drain_read_hook)
        return g_drain_read_hook(fd, buf, len);
    return read(fd, buf, len);
}

static int drain_avail(int fd, int *avail)
{
    if (g_drain_avail_hook)
        return g_drain_avail_hook(fd, avail);
    return ioctl(fd, FIONREAD, avail);
}

/* Returns 0 = idle/drained, 1 = disconnect */
static int drain_link_fd(int fd, int max_reads, sm_drain_chunk_fn chunk_cb, void *ctx)
{
    uint8_t buf[SM_LINK_DRAIN_CHUNK];
    int reads = 0;

    while (reads < max_reads) {
        ssize_t n = drain_read(fd, buf, sizeof(buf));
        reads++;

        if (n == 0) {
            int avail = 0;
            int have_avail = (drain_avail(fd, &avail) == 0 && avail > 0);
            if (have_avail)
                continue;
            if (reads == 1)
                return 1;
            return 0;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0;
            if (errno == EIO || errno == ENXIO || errno == ENODEV ||
                errno == ECONNRESET || errno == EPIPE)
                return 1;
            return 0;
        }
        if (chunk_cb)
            chunk_cb(ctx, buf, (size_t)n);
    }
    return 0;
}

int sm_broker_test_drain_link_fd(int fd, int max_reads,
                                 sm_drain_chunk_fn chunk_cb, void *ctx)
{
    return drain_link_fd(fd, max_reads, chunk_cb, ctx);
}

static void process_link_chunk(sm_broker_t *b, const uint8_t *buf, size_t n, double ts)
{
    sm_expect_feed(&b->expect, buf, n);
    flood_feed(b, buf, n);   /* stop an active autoboot flood on prompt match */
    sm_rb_append(&b->history, buf, n, ts);

    char *b64 = sm_base64_encode(buf, n);

    if (b->io_log)
        sm_io_log_output_b64(b->io_log, b64, ts);

    if (b->text_log)
        sm_text_log_write(b->text_log, buf, n, ts);

    size_t new_incidents = sm_anomaly_feed(&b->anomaly, buf, n, ts);
    if (new_incidents > 0) {
        size_t count;
        const sm_anomaly_incident_t *incidents = sm_anomaly_get_incidents(&b->anomaly, &count);
        for (size_t i = count - new_incidents; i < count; i++) {
            cJSON *amsg = sm_msg_anomaly(incidents[i].id, incidents[i].pattern_name,
                                          incidents[i].severity, incidents[i].timestamp,
                                          incidents[i].match_text, incidents[i].pre_context);
            broadcast(b, amsg, NULL);
            cJSON_Delete(amsg);

            if (b->io_log)
                sm_io_log_incident(b->io_log, incidents[i].id,
                                   incidents[i].pattern_name,
                                   incidents[i].severity, ts);

            for (size_t s = 0; s < b->sink_count; s++) {
                if (b->sinks[s]->on_event)
                    b->sinks[s]->on_event(b->sinks[s], "anomaly",
                                           incidents[i].pattern_name);
            }
        }
    }

    /* Boot-stage progress: advance the tracker and announce any stage newly
     * reached (broadcast once per stage, like anomalies). */
    if (sm_boot_feed(&b->boot, buf, n, ts) > 0) {
        for (size_t i = 0; i < b->boot.stage_count; i++) {
            sm_boot_stage_t *st = &b->boot.stages[i];
            if (st->reached && !st->announced) {
                st->announced = 1;
                cJSON *bmsg = sm_msg_boot_stage(st->name, (int)i,
                                                (int)b->boot.stage_count,
                                                st->reached_ts);
                broadcast(b, bmsg, NULL);
                cJSON_Delete(bmsg);
            }
        }
        /* Re-arm the stall watchdog on each advance; disarm once fully booted. */
        if (sm_boot_terminal_reached(&b->boot))
            stall_disarm(b);
        else
            stall_arm(b);
    }

    /* Autoresponder: fire standing expect->send rules straight from the read
     * path (menus, y/n prompts, login) with zero client round-trip. */
    sm_ar_fired_t ar_fired[SM_AR_MAX_FIRED_PER_FEED];
    size_t nar = sm_autoresponder_feed(&b->autoresponder, buf, n, ts,
                                       ar_fired, SM_AR_MAX_FIRED_PER_FEED);
    for (size_t i = 0; i < nar; i++) {
        if (sm_broker_do_write(b, ar_fired[i].response,
                               ar_fired[i].response_len, "autoresponder") == 0) {
            SM_LOG_INFO(LOG_TAG, "autoresponder '%s' fired (%zu bytes sent)",
                        ar_fired[i].name, ar_fired[i].response_len);
            cJSON *fmsg = sm_msg_autoresponder_fired(ar_fired[i].name,
                                                     ar_fired[i].matched_text,
                                                     (int)ar_fired[i].response_len);
            broadcast(b, fmsg, NULL);
            cJSON_Delete(fmsg);
        }
    }

    for (size_t s = 0; s < b->sink_count; s++) {
        if (b->sinks[s]->on_output)
            b->sinks[s]->on_output(b->sinks[s], buf, n, ts);
    }

    cJSON *out = sm_msg_output_b64(b64, ts);
    free(b64);
    broadcast(b, out, NULL);
    cJSON_Delete(out);
}

static void link_drain_chunk(void *ctx, const uint8_t *buf, size_t n)
{
    sm_broker_t *b = ctx;
    double ts = sm_now_realtime();          /* wall-clock: user-facing chunk timestamp */
    b->last_link_rx_time = sm_now_monotonic();   /* monotonic: drives idle health check */
    b->link_healthy = 1;

    /* Optional per-link receive filter (e.g. telnet IAC stripping). The filter
     * only removes bytes, so a same-size buffer suffices; drain reads at most
     * SM_LINK_DRAIN_CHUNK per chunk. */
    if (b->link->filter_rx) {
        uint8_t filtered[SM_LINK_DRAIN_CHUNK];
        if (n > sizeof(filtered)) n = sizeof(filtered);
        size_t fn = b->link->filter_rx(b->link, buf, n, filtered);
        if (fn > 0)
            process_link_chunk(b, filtered, fn, ts);
        return;
    }

    process_link_chunk(b, buf, n, ts);
}

static void process_link_data(sm_broker_t *b)
{
    int link_fd = b->link->read_fd(b->link);
    if (link_fd < 0) return;

    if (drain_link_fd(link_fd, SM_LINK_DRAIN_MAX_READS, link_drain_chunk, b) == 1)
        handle_link_disconnect(b);
}

static void handle_link_disconnect(sm_broker_t *b)
{
    if (b->link_disconnected) return;  /* already handling */

    /* No strerror here: this is also reached on EPOLLHUP where errno is
     * not meaningful (L6). The triggering read error is logged at its
     * own site. */
    SM_LOG_WARN(LOG_TAG, "link disconnected");

    break_cancel(b);
    flood_cancel(b);
    stall_disarm(b);   /* don't fire a stall for a deliberate/observed outage */

    int link_fd = b->link->read_fd(b->link);
    int write_fd = b->link->write_fd ? b->link->write_fd(b->link) : link_fd;
    if (link_fd >= 0)
        epoll_ctl(b->epoll_fd, EPOLL_CTL_DEL, link_fd, NULL);
    if (write_fd >= 0 && write_fd != link_fd)
        epoll_ctl(b->epoll_fd, EPOLL_CTL_DEL, write_fd, NULL);

    b->link->close(b->link);
    b->link_disconnected = 1;
    b->reconnect_delay_s = SM_RECONNECT_BASE_S;
    b->reconnect_next = sm_now_monotonic() + b->reconnect_delay_s;

    /* Notify clients */
    cJSON *msg = sm_msg_link_down(b->port, "device disconnected");
    broadcast(b, msg, NULL);
    cJSON_Delete(msg);
}

static void process_expect_results(sm_broker_t *b)
{
    sm_expect_result_t results[16];
    double now = sm_now_monotonic();
    size_t count = sm_expect_collect(&b->expect, now, results, 16);

    for (size_t i = 0; i < count; i++) {
        sm_expect_result_t *r = &results[i];

        /* Check if this expect belongs to a sink */
        int routed_to_sink = 0;
        for (size_t s = 0; s < b->sink_count; s++) {
            if (b->sinks[s]->on_expect_result &&
                strcmp(r->client_id, SM_MCP_CLIENT_ID) == 0) {
                b->sinks[s]->on_expect_result(b->sinks[s], r->id, r->matched,
                                               r->data, r->data_len, r->pattern);
                routed_to_sink = 1;
                break;
            }
        }

        if (!routed_to_sink) {
            /* Find the client that owns this expect */
            for (size_t j = 0; j < b->client_count; j++) {
                sm_client_t *c = b->clients[j];
                if (strcmp(c->id, r->client_id) == 0) {
                    cJSON *msg = sm_msg_expect_result(r->id, r->matched,
                                                       r->data, r->data_len,
                                                       r->pattern);
                    send_to_client(c, msg);
                    break;
                }
            }
        }
        free(r->data);
    }
}

/* --- Extracted helpers for sm_broker_run --- */

static int broker_setup(sm_broker_t *b)
{
    if (b->link->open(b->link) != 0) {
        SM_LOG_ERROR(LOG_TAG, "failed to open link");
        return -1;
    }

    /* Initial health state after successful link open */
    b->last_link_rx_time = sm_now_monotonic();
    b->link_healthy = 1;

    if (b->log_dir[0]) {
        char path[512];
        snprintf(path, sizeof(path), "%s/smolmux-io.jsonl", b->log_dir);
        b->io_log = sm_io_log_open(path);
    }

    if (!b->no_text_log && b->text_log_dir[0])
        b->text_log = sm_text_log_open(b->text_log_dir, b->port);

    b->listen_fd = create_listen_socket(b);
    if (b->listen_fd < 0) {
        SM_LOG_ERROR(LOG_TAG, "failed to create socket %s: %s",
                     b->socket_path, strerror(errno));
        return -1;
    }

    b->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (b->epoll_fd < 0) return -1;

    int link_fd = b->link->read_fd(b->link);
    if (link_fd >= 0) {
        struct epoll_event ev = {.events = EPOLLIN | EPOLLHUP | EPOLLERR,
                                 .data.ptr = EPOLL_TAG_LINK};
        if (epoll_ctl(b->epoll_fd, EPOLL_CTL_ADD, link_fd, &ev) < 0) {
            SM_LOG_ERROR(LOG_TAG, "epoll_ctl add link: %s", strerror(errno));
            return -1;
        }
        int write_fd = b->link->write_fd ? b->link->write_fd(b->link) : link_fd;
        if (write_fd >= 0 && write_fd != link_fd) {
            struct epoll_event wev = {.events = 0, .data.ptr = EPOLL_TAG_LINK_OUT};
            if (epoll_ctl(b->epoll_fd, EPOLL_CTL_ADD, write_fd, &wev) < 0)
                SM_LOG_WARN(LOG_TAG, "epoll_ctl add link write: %s", strerror(errno));
        }
    }

    struct epoll_event ev = {.events = EPOLLIN, .data.ptr = EPOLL_TAG_LISTEN};
    if (epoll_ctl(b->epoll_fd, EPOLL_CTL_ADD, b->listen_fd, &ev) < 0) {
        SM_LOG_ERROR(LOG_TAG, "epoll_ctl add listen: %s", strerror(errno));
        return -1;
    }

    ev = (struct epoll_event){.events = EPOLLIN, .data.ptr = EPOLL_TAG_PIPE};
    if (epoll_ctl(b->epoll_fd, EPOLL_CTL_ADD, b->reg_pipe[0], &ev) < 0) {
        SM_LOG_ERROR(LOG_TAG, "epoll_ctl add reg_pipe: %s", strerror(errno));
        return -1;
    }

    SM_LOG_INFO(LOG_TAG, "listening on %s", b->socket_path);

    for (size_t s = 0; s < b->sink_count; s++) {
        sm_sink_t *sink = b->sinks[s];
        if (sink->start) sink->start(sink, b);
        if (sink->fd >= 0) {
            struct epoll_event sev = {.events = EPOLLIN,
                                      .data.ptr = epoll_sink_ptr(s)};
            if (epoll_ctl(b->epoll_fd, EPOLL_CTL_ADD, sink->fd, &sev) < 0)
                SM_LOG_WARN(LOG_TAG, "epoll_ctl add sink fd=%d: %s", sink->fd, strerror(errno));
        }
    }

    return 0;
}

/* Schedule the next reconnect attempt with exponential backoff. */
static void link_schedule_retry(sm_broker_t *b)
{
    b->reconnect_delay_s *= 2;
    if (b->reconnect_delay_s > SM_RECONNECT_MAX_S)
        b->reconnect_delay_s = SM_RECONNECT_MAX_S;
    b->reconnect_next = sm_now_monotonic() + b->reconnect_delay_s;
    SM_LOG_DEBUG(LOG_TAG, "reconnect failed, retry in %ds", b->reconnect_delay_s);
}

/* A link (re)connected: register it for I/O, reset health, clear the
 * disconnected/connecting flags, and optionally announce link_up. */
static void link_bring_up(sm_broker_t *b, int broadcast_up)
{
    int new_fd = b->link->read_fd(b->link);
    if (new_fd >= 0) {
        struct epoll_event rev = {.events = EPOLLIN | EPOLLHUP | EPOLLERR,
                                  .data.ptr = EPOLL_TAG_LINK};
        if (epoll_ctl(b->epoll_fd, EPOLL_CTL_ADD, new_fd, &rev) < 0 &&
            errno == EEXIST)
            epoll_ctl(b->epoll_fd, EPOLL_CTL_MOD, new_fd, &rev);
        int write_fd = b->link->write_fd ? b->link->write_fd(b->link) : new_fd;
        if (write_fd >= 0 && write_fd != new_fd) {
            struct epoll_event wev = {.events = 0,
                                      .data.ptr = EPOLL_TAG_LINK_OUT};
            if (epoll_ctl(b->epoll_fd, EPOLL_CTL_ADD, write_fd, &wev) < 0 &&
                errno != EEXIST)
                SM_LOG_WARN(LOG_TAG, "epoll_ctl add link write on reconnect: %s",
                            strerror(errno));
        }
        broker_update_link_epoll(b);
    }
    b->link_connecting = 0;
    b->link_disconnected = 0;

    /* Reset health state like do_resume: the outage interval must not count as
     * receive idle, or housekeeping would immediately flag the freshly-
     * reconnected link as degraded until data next flows. */
    b->last_link_rx_time = sm_now_monotonic();
    b->link_healthy = 0;

    /* A reconnect usually means the device was reset/replugged: start boot
     * progress over so stages re-detect from the new boot. */
    sm_boot_reset(&b->boot);
    stall_disarm(b);   /* fresh boot; the stall timer re-arms when stage 0 hits */

    SM_LOG_INFO(LOG_TAG, "link reconnected");

    if (broadcast_up) {
        cJSON *up = sm_msg_link_up(b->port);
        broadcast(b, up, NULL);
        cJSON_Delete(up);
    }
}

/* Watch an in-progress async connect fd for completion (writable) or error,
 * bounded by a deadline the loop enforces. */
static void link_watch_connecting(sm_broker_t *b)
{
    int fd = b->link->read_fd(b->link);
    if (fd >= 0) {
        struct epoll_event ev = {.events = EPOLLOUT | EPOLLHUP | EPOLLERR,
                                 .data.ptr = EPOLL_TAG_LINK};
        if (epoll_ctl(b->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0 && errno == EEXIST)
            epoll_ctl(b->epoll_fd, EPOLL_CTL_MOD, fd, &ev);
    }
    b->link_connecting = 1;
    b->link_connect_deadline = sm_now_monotonic() + SM_LINK_CONNECT_TIMEOUT_S;
}

/* An async connect failed or timed out: close it and back off. */
static void link_connect_give_up(sm_broker_t *b)
{
    b->link_connecting = 0;
    b->link->close(b->link);
    b->link_disconnected = 1;
    link_schedule_retry(b);
}

/* Handle an epoll event on a link fd that is mid-connect. */
static void handle_link_connecting(sm_broker_t *b, uint32_t ev)
{
    if (ev & (EPOLLHUP | EPOLLERR)) { link_connect_give_up(b); return; }
    if (ev & EPOLLOUT) {
        int rc = b->link->connect_poll ? b->link->connect_poll(b->link) : -1;
        if (rc == 1)       link_bring_up(b, 1);
        else if (rc < 0)   link_connect_give_up(b);
        /* rc == 0: still pending, keep waiting */
    }
}

static void attempt_reconnect(sm_broker_t *b)
{
    if (b->link_connecting) return;   /* an async connect is already in flight */
    if (!b->link_disconnected || !b->reconnect) return;
    if (sm_now_monotonic() < b->reconnect_next) return;

    /* Async path (e.g. serial-tcp): initiate a non-blocking connect so a slow
     * or unreachable server never stalls the event loop. */
    if (b->link->connect_begin) {
        int rc = b->link->connect_begin(b->link);
        if (rc == 0)       link_bring_up(b, 1);       /* immediate (loopback) */
        else if (rc == 1)  link_watch_connecting(b);  /* completes async */
        else               link_schedule_retry(b);    /* immediate failure */
        return;
    }

    /* Blocking fallback (uart/gdb): open() at startup/reconnect is quick. */
    if (b->link->open(b->link) == 0)
        link_bring_up(b, 1);
    else
        link_schedule_retry(b);
}

static int is_client_live(sm_broker_t *b, sm_client_t *c)
{
    for (size_t ci = 0; ci < b->client_count; ci++) {
        if (b->clients[ci] == c) return 1;
    }
    return 0;
}

static void process_client_event(sm_broker_t *b, sm_client_t *c, uint32_t ev)
{
    /* Drain readable input before honoring a hangup: the kernel delivers
     * EPOLLIN|EPOLLHUP together when the peer sent a final command then
     * closed, so handling HUP first would silently drop that command. */
    if (ev & EPOLLIN) {
        sm_msg_t msgs[16];
        size_t count = sm_client_feed(c, msgs, 16);
        for (size_t m = 0; m < count; m++) {
            /* A handler may disconnect the client; drop queued messages */
            if (!c->disconnected)
                handle_client_message(b, c, &msgs[m]);
            sm_msg_free(&msgs[m]);
        }
        if (c->disconnected) {
            remove_client(b, c);
            return;
        }
    }

    if (ev & (EPOLLHUP | EPOLLERR)) {
        remove_client(b, c);
        return;
    }

    /* Re-validate liveness after message handling */
    if (!is_client_live(b, c)) return;

    if (ev & EPOLLOUT) {
        int rc = sm_client_flush(c);
        if (rc == 0) {
            struct epoll_event nev = {.events = EPOLLIN | EPOLLHUP | EPOLLERR,
                                      .data.ptr = c};
            if (epoll_ctl(b->epoll_fd, EPOLL_CTL_MOD, c->fd, &nev) < 0)
                SM_LOG_WARN(LOG_TAG, "epoll_ctl mod fd=%d: %s", c->fd, strerror(errno));
            c->write_pending = 0;
        } else if (rc < 0) {
            remove_client(b, c);
            return;
        }
    }

    if (c->wq_count > 0 && !c->write_pending) {
        struct epoll_event nev = {.events = EPOLLIN | EPOLLOUT | EPOLLHUP | EPOLLERR,
                                  .data.ptr = c};
        if (epoll_ctl(b->epoll_fd, EPOLL_CTL_MOD, c->fd, &nev) < 0)
            SM_LOG_WARN(LOG_TAG, "epoll_ctl mod fd=%d: %s", c->fd, strerror(errno));
        c->write_pending = 1;
    }
}

static int any_client_write_pending(sm_broker_t *b)
{
    for (size_t i = 0; i < b->client_count; i++) {
        if (b->clients[i]->wq_count > 0)
            return 1;
    }
    return 0;
}

static void flush_client_queues(sm_broker_t *b)
{
    for (size_t i = 0; i < b->client_count; ) {
        sm_client_t *c = b->clients[i];
        if (c->wq_count > 0) {
            int rc = sm_client_flush(c);
            if (rc < 0) {
                remove_client(b, c);
                continue;
            }
        }
        i++;
    }
}

int sm_broker_run(sm_broker_t *b)
{
    if (broker_setup(b) != 0)
        return -1;

    struct epoll_event events[SM_MAX_EPOLL_EVENTS];

    while (!b->stopped) {
        int nfds = epoll_wait(b->epoll_fd, events, SM_MAX_EPOLL_EVENTS,
                              SM_EPOLL_TIMEOUT_MS);

        if (nfds < 0) {
            if (errno == EINTR) continue;
            SM_LOG_ERROR(LOG_TAG, "epoll_wait: %s", strerror(errno));
            break;
        }

        if (b->expect.count > 0)
            process_expect_results(b);
        if (b->history_pending.active)
            process_history_pending(b);
        attempt_reconnect(b);
        if (b->link_connecting && sm_now_monotonic() > b->link_connect_deadline)
            link_connect_give_up(b);   /* connect took too long */

        for (int i = 0; i < nfds; i++) {
            void *ptr = events[i].data.ptr;
            uint32_t ev = events[i].events;

            if (ptr == EPOLL_TAG_LISTEN) {
                accept_client(b);
                continue;
            }
            if (ptr == EPOLL_TAG_PIPE) {
                int client_fd;
                while (read(b->reg_pipe[0], &client_fd, sizeof(client_fd)) == sizeof(client_fd))
                    sm_broker_register_client(b, client_fd);
                continue;
            }
            if (ptr == EPOLL_TAG_BRKTIMER) {
                handle_break_timer(b);
                continue;
            }
            if (ptr == EPOLL_TAG_FLOODTIMER) {
                handle_flood_timer(b);
                continue;
            }
            if (ptr == EPOLL_TAG_STALLTIMER) {
                handle_stall_timer(b);
                continue;
            }
            if (ptr == EPOLL_TAG_LINK && b->link_connecting && !b->suspended) {
                handle_link_connecting(b, ev);
                continue;
            }
            if (ptr == EPOLL_TAG_LINK && !b->suspended && !b->link_disconnected) {
                if (ev & (EPOLLHUP | EPOLLERR))
                    handle_link_disconnect(b);
                else {
                    if (ev & EPOLLIN)
                        process_link_data(b);
                    if ((ev & EPOLLOUT) && b->link->flush_write_queue) {
                        int rc = b->link->flush_write_queue(b->link);
                        if (rc < 0)
                            handle_link_disconnect(b);
                        else
                            broker_update_link_epoll(b);
                    }
                }
                continue;
            }
            if (ptr == EPOLL_TAG_LINK_OUT && !b->suspended &&
                !b->link_disconnected && (ev & EPOLLOUT) &&
                b->link->flush_write_queue) {
                int rc = b->link->flush_write_queue(b->link);
                if (rc < 0)
                    handle_link_disconnect(b);
                else
                    broker_update_link_epoll(b);
                continue;
            }

            size_t sink_idx;
            if (epoll_ptr_is_sink(ptr, &sink_idx)) {
                if (sink_idx < b->sink_count && (ev & EPOLLIN) &&
                    b->sinks[sink_idx]->on_readable)
                    b->sinks[sink_idx]->on_readable(b->sinks[sink_idx]);
                continue;
            }

            /* Client event */
            sm_client_t *c = ptr;
            if (!is_client_live(b, c)) continue;
            process_client_event(b, c, ev);
        }

        if (any_client_write_pending(b))
            flush_client_queues(b);

        /* Periodic housekeeping (~2s): link health + hello timeout. Uses the
         * monotonic clock so an NTP step can't stall the cadence (backward
         * step) or fire a spurious degraded alarm (forward step). */
        static double last_health_check = 0;
        double now = sm_now_monotonic();
        if (now - last_health_check > sm_broker_test_health_period_s) {
            last_health_check = now;

            /* Drop clients that connected but never sent hello — they
             * would otherwise consume client slots forever (M4) */
            double mono = now;
            for (size_t i = b->client_count; i-- > 0; ) {
                sm_client_t *cl = b->clients[i];
                if (!cl->hello_received && cl->connected_at > 0 &&
                    mono - cl->connected_at > SM_HELLO_TIMEOUT_S) {
                    SM_LOG_WARN(LOG_TAG,
                                "client %s: no hello within %ds, dropping",
                                cl->id, SM_HELLO_TIMEOUT_S);
                    remove_client(b, cl);
                }
            }

            if (!b->suspended && !b->link_disconnected && b->link &&
                !b->link->silence_normal) {
                double idle = now - b->last_link_rx_time;
                if (b->last_link_rx_time > 0 &&
                    idle > sm_broker_test_idle_degraded_s) {
                    if (b->link_healthy) {
                        SM_LOG_WARN(LOG_TAG,
                            "LINK HEALTH WARNING: No data received for %.1fs (port=%s). "
                            "Link may be stuck or disconnected. Clients may see stale data.",
                            idle, b->port);
                        b->link_healthy = 0;
                    }
                    /* Broadcast health status so MCP/monitor clients can react */
                    cJSON *health = cJSON_CreateObject();
                    cJSON_AddStringToObject(health, "type", "link_health");
                    cJSON_AddStringToObject(health, "status", "degraded");
                    cJSON_AddNumberToObject(health, "idle_seconds", idle);
                    cJSON_AddStringToObject(health, "port", b->port);
                    sm_broker_broadcast_msg(b, health);
                    cJSON_Delete(health);
                } else if (idle < sm_broker_test_idle_recovered_s &&
                           !b->link_healthy) {
                    /* Recovered */
                    b->link_healthy = 1;
                    SM_LOG_INFO(LOG_TAG, "Link health recovered (data flowing again)");
                }
            }
        }
    }

    /* Cleanup */
    SM_LOG_INFO(LOG_TAG, "shutting down");

    /* Cancel pending break while sinks can still deliver the result */
    break_cancel(b);

    /* Stop sinks */
    for (size_t s = 0; s < b->sink_count; s++) {
        if (b->sinks[s]->stop)
            b->sinks[s]->stop(b->sinks[s]);
    }

    if (b->break_timer_fd >= 0) { close(b->break_timer_fd); b->break_timer_fd = -1; }
    b->flood_active = 0;
    if (b->flood_stop_re) { sm_regex_free(b->flood_stop_re); b->flood_stop_re = NULL; }
    if (b->flood_timer_fd >= 0) { close(b->flood_timer_fd); b->flood_timer_fd = -1; }
    if (b->stall_timer_fd >= 0) { close(b->stall_timer_fd); b->stall_timer_fd = -1; }
    if (b->epoll_fd >= 0) { close(b->epoll_fd); b->epoll_fd = -1; }
    if (b->listen_fd >= 0) { close(b->listen_fd); b->listen_fd = -1; }
    if (b->reg_pipe[0] >= 0) { close(b->reg_pipe[0]); b->reg_pipe[0] = -1; }
    if (b->reg_pipe[1] >= 0) { close(b->reg_pipe[1]); b->reg_pipe[1] = -1; }
    /* Clear so sm_broker_destroy doesn't unlink a second time (L7) */
    if (b->socket_path[0]) { unlink(b->socket_path); b->socket_path[0] = '\0'; }

    return 0;
}

void sm_broker_register_client_async(sm_broker_t *b, int fd)
{
    if (write(b->reg_pipe[1], &fd, sizeof(fd)) != sizeof(fd))
        SM_LOG_WARN(LOG_TAG, "reg_pipe write failed: %s", strerror(errno));
}

/* Called from signal handlers — must stay a single atomic store; no
 * locks, allocation, or I/O may ever be added here. */
void sm_broker_stop(sm_broker_t *b)
{
    b->stopped = 1;
}

void sm_broker_destroy(sm_broker_t *b)
{
    /* Cancel pending break/flood before the link goes away */
    break_cancel(b);
    flood_cancel(b);
    stall_disarm(b);
    if (b->break_timer_fd >= 0) { close(b->break_timer_fd); b->break_timer_fd = -1; }
    if (b->flood_stop_re) { sm_regex_free(b->flood_stop_re); b->flood_stop_re = NULL; }
    if (b->flood_timer_fd >= 0) { close(b->flood_timer_fd); b->flood_timer_fd = -1; }
    if (b->stall_timer_fd >= 0) { close(b->stall_timer_fd); b->stall_timer_fd = -1; }

    /* Disconnect all clients */
    while (b->client_count > 0) {
        sm_client_destroy(b->clients[--b->client_count]);
    }
    free(b->clients);

    /* Close link */
    if (b->link) {
        b->link->close(b->link);
        b->link->destroy(b->link);
    }

    /* Destroy sinks */
    for (size_t s = 0; s < b->sink_count; s++) {
        if (b->sinks[s]->destroy)
            b->sinks[s]->destroy(b->sinks[s]);
    }
    free(b->sinks);
    b->sinks = NULL;
    b->sink_count = 0;

    /* Close fds that may be left from partial sm_broker_run */
    if (b->epoll_fd >= 0) { close(b->epoll_fd); b->epoll_fd = -1; }
    if (b->listen_fd >= 0) { close(b->listen_fd); b->listen_fd = -1; }
    if (b->reg_pipe[0] >= 0) { close(b->reg_pipe[0]); b->reg_pipe[0] = -1; }
    if (b->reg_pipe[1] >= 0) { close(b->reg_pipe[1]); b->reg_pipe[1] = -1; }
    if (b->socket_path[0]) { unlink(b->socket_path); b->socket_path[0] = '\0'; }

    history_pending_clear(b);

    /* Cleanup modules */
    sm_expect_destroy(&b->expect);
    sm_rb_destroy(&b->history);
    sm_anomaly_destroy(&b->anomaly);
    sm_boot_destroy(&b->boot);
    sm_autoresponder_destroy(&b->autoresponder);
    sm_profile_destroy(&b->profile);
    sm_io_log_close(b->io_log);
    sm_text_log_close(b->text_log);
}
