/*
 * bench_coalesce — measure the cost of the client output coalescer
 * (try_coalesce_output in src/client.c) as a function of backlog depth.
 *
 * Why: the R4 deep-dive flagged the coalescer as O(K^2) for a backlogged
 * client — each new output line decodes the queued (merged) head, appends,
 * and re-encodes+re-base64s the whole accumulated head, so a client K lines
 * behind pays ~sum(1..K) work. This harness measures whether that actually
 * matters before anyone optimizes it (mantra: don't tune what you haven't
 * measured).
 *
 * It drives the real public API sm_client_send_shared(), which invokes the
 * (static) coalescer, in two scenarios:
 *
 *   STALLED  — the client socket is never drained, so the write queue never
 *              flushes (offset stays 0) and every new line coalesces into one
 *              ever-growing entry. This is the worst case the finding
 *              describes (a fully stalled slow client while the link is busy).
 *
 *   DRAINED  — the client is fully flushed+drained after each line, so the
 *              queue is empty at each send and coalescing is skipped
 *              (encode-once-and-share, O(1) per line). This is the normal
 *              "clients keep up" path, shown as the baseline.
 *
 * Build:  cmake -B build -DSM_BUILD_BENCH=ON && cmake --build build --target bench_coalesce
 * Run:    ./build/bench_coalesce [max_backlog] [payload_bytes]
 */

#include "client.h"
#include "protocol.h"
#include "util/shared_line.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* One device-output message of `payload` raw bytes, wrapped as a shared line
 * exactly as the broker broadcast path builds it. Caller owns one ref. */
static sm_shared_line_t *make_output_line(size_t payload, double ts)
{
    uint8_t *buf = malloc(payload);
    if (!buf) return NULL;
    memset(buf, 'x', payload);
    cJSON *msg = sm_msg_output(buf, payload, ts);
    free(buf);
    if (!msg) return NULL;
    size_t mlen = 0;
    char *line = sm_msg_encode(msg, &mlen);   /* owns line; includes '\n' */
    cJSON_Delete(msg);
    if (!line) return NULL;
    return sm_shared_line_new(line, mlen);    /* takes ownership of line */
}

/* Send N output lines into a never-drained client. Returns elapsed seconds.
 * On return *final_wq holds the queue depth (should be 1 if coalescing
 * engaged the whole way). */
static double run_stalled(int n, size_t payload, size_t *final_wq)
{
    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) return -1;
    sm_client_t *c = sm_client_new(sp[0], 0);
    c->hello_received = 1;

    double t0 = now_s();
    for (int i = 0; i < n; i++) {
        sm_shared_line_t *sl = make_output_line(payload, 1000.0 + i);
        sm_client_send_shared(c, sl);   /* coalesces after the first line */
        sm_shared_line_release(sl);     /* drop our ref (send took its own) */
    }
    double dt = now_s() - t0;

    if (final_wq) *final_wq = c->wq_count;
    sm_client_destroy(c);               /* closes sp[0] */
    close(sp[1]);
    return dt;
}

/* Send N output lines into a client that is fully flushed+drained after each
 * line, so the queue is empty at each send and coalescing is skipped. */
static double run_drained(int n, size_t payload)
{
    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) return -1;
    /* Non-blocking peer so we can drain to EAGAIN. */
    int fl = fcntl(sp[1], F_GETFL, 0);
    fcntl(sp[1], F_SETFL, fl | O_NONBLOCK);
    sm_client_t *c = sm_client_new(sp[0], 0);
    c->hello_received = 1;

    char drain[65536];
    double t0 = now_s();
    for (int i = 0; i < n; i++) {
        sm_shared_line_t *sl = make_output_line(payload, 1000.0 + i);
        sm_client_send_shared(c, sl);
        sm_shared_line_release(sl);
        sm_client_flush(c);
        while (read(sp[1], drain, sizeof(drain)) > 0)
            ;                           /* drain the peer so wq empties */
    }
    double dt = now_s() - t0;

    sm_client_destroy(c);
    close(sp[1]);
    return dt;
}

int main(int argc, char **argv)
{
    int max_backlog = argc > 1 ? atoi(argv[1]) : 8000;
    size_t payload = argc > 2 ? (size_t)atol(argv[2]) : 64;
    if (max_backlog < 1000) max_backlog = 1000;

    /* Encoded line size for context. */
    sm_shared_line_t *probe = make_output_line(payload, 0.0);
    size_t line_bytes = probe ? probe->len : 0;
    if (probe) sm_shared_line_release(probe);

    /* Realistic-load context: 115200 8N1 ~= 11520 bytes/s. */
    double bytes_per_s = 11520.0;
    double lines_per_s = line_bytes ? bytes_per_s / (double)line_bytes : 0.0;

    printf("smolmux coalescer benchmark\n");
    printf("  payload      = %zu raw bytes/output msg\n", payload);
    printf("  encoded line = %zu bytes (JSON+base64+newline)\n", line_bytes);
    printf("  context      = at 115200 baud (~%.0f B/s) that's ~%.0f lines/s;\n",
           bytes_per_s, lines_per_s);
    printf("                 filling an %d-line backlog at that rate takes ~%.0f s of\n",
           max_backlog, lines_per_s ? max_backlog / lines_per_s : 0.0);
    printf("                 continuous stall; a fast link (USB-CDC/GDB, ~MB/s) fills\n");
    printf("                 the same backlog in well under a second.\n\n");

    int checkpoints[] = {1000, 2000, 4000, 8000, 16000, 32000};
    size_t ncp = sizeof(checkpoints) / sizeof(checkpoints[0]);

    printf("STALLED client (never drains -> coalescing merges into one growing entry):\n");
    printf("  %10s %12s %12s %10s %10s\n",
           "backlog N", "elapsed ms", "us/line", "ratio", "wq_count");
    double prev = 0.0;
    for (size_t i = 0; i < ncp; i++) {
        int n = checkpoints[i];
        if (n > max_backlog) break;
        size_t wq = 0;
        double dt = run_stalled(n, payload, &wq);
        double ratio = prev > 0 ? dt / prev : 0.0;
        printf("  %10d %12.1f %12.2f %9.2fx %10zu\n",
               n, dt * 1e3, dt * 1e6 / n, ratio, wq);
        fflush(stdout);
        prev = dt;
    }

    printf("\nDRAINED client (keeps up -> queue empty each send -> coalescing skipped):\n");
    printf("  %10s %12s %12s %10s\n",
           "backlog N", "elapsed ms", "us/line", "ratio");
    prev = 0.0;
    for (size_t i = 0; i < ncp; i++) {
        int n = checkpoints[i];
        if (n > max_backlog) break;
        double dt = run_drained(n, payload);
        double ratio = prev > 0 ? dt / prev : 0.0;
        printf("  %10d %12.1f %12.2f %9.2fx\n",
               n, dt * 1e3, dt * 1e6 / n, ratio);
        fflush(stdout);
        prev = dt;
    }

    printf("\nReading the table (per column, per doubling of N):\n");
    printf("  - elapsed ratio ~2x with flat us/line  => O(N), healthy.\n");
    printf("  - elapsed ratio ~4x with rising us/line => O(K^2) in backlog\n");
    printf("    depth; the uncapped coalescer behaved this way before the\n");
    printf("    SM_CLIENT_COALESCE_MAX_BYTES head cap. With the cap, STALLED\n");
    printf("    us/line flattens (~O(cap)/line) and wq_count grows instead of\n");
    printf("    staying at 1 (merging restarts in a fresh entry past the cap).\n");
    printf("  - DRAINED is the encode-once-and-share path when clients keep up.\n");
    return 0;
}
