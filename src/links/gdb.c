#include "links/gdb.h"
#include "links/link_wq.h"
#include "logger.h"
#include "constants.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <signal.h>
#include <sys/wait.h>

#define LOG_TAG "gdb"

typedef struct gdb_data {
    char gdb_path[256];
    char target_spec[256];
    pid_t pid;        /* GDB child PID, -1 when not running */
    int stdin_fd;     /* write end -> GDB's stdin */
    int stdout_fd;    /* read end <- GDB's stdout */
    int allow_shell;  /* permit shell-invoking GDB commands (default off) */
    sm_link_wq_t wq;
} gdb_data_t;

/* Console commands that run arbitrary code on the broker host. GDB's MI
 * interpreter also accepts console commands, and any of these lands host code
 * execution for a controller client:
 *   shell / !   host shell escape          pipe / |   pipe output to a shell
 *   python      run arbitrary Python       guile      run arbitrary Guile
 *   make        spawns make (-> shell)
 *
 * BEST-EFFORT GUARD, NOT A SECURITY BOUNDARY (M5). A blocklist cannot be
 * complete for GDB — python/guile/make are only the obvious surfaces; source,
 * define/commands, dprintf, add-symbol-file scripts and future console verbs
 * all reach code execution too. When a GDB link is exposed to untrusted
 * controllers, confine the broker at the OS level (seccomp/namespaces) rather
 * than relying on this. Bypassed by design once set_param("allow_shell","1")
 * opts in. */
static const char *const gdb_code_exec_cmds[] = {
    "shell", "pipe", "python", "guile", "make",
};

/* GDB resolves unambiguous prefix abbreviations, so "py" runs python, "gu"
 * runs guile, "pi" runs pipe. Match the typed word against each name on their
 * common prefix (>= 2 chars): this flags both abbreviations ("py") and longer
 * spellings that start with a dangerous name ("python-interactive"). */
static int word_is_code_exec(const uint8_t *word, size_t wlen)
{
    if (wlen < 2) return 0;
    for (size_t d = 0; d < sizeof(gdb_code_exec_cmds) / sizeof(gdb_code_exec_cmds[0]); d++) {
        size_t fl = strlen(gdb_code_exec_cmds[d]);
        size_t m = wlen < fl ? wlen : fl;
        if (memcmp(word, gdb_code_exec_cmds[d], m) == 0)
            return 1;
    }
    return 0;
}

static int line_invokes_shell(const uint8_t *line, size_t len)
{
    size_t j = 0;
    while (j < len && line[j] >= '0' && line[j] <= '9') j++;  /* MI token */
    while (j < len && (line[j] == ' ' || line[j] == '\t')) j++;
    if (j >= len) return 0;

    if (line[j] == '!' || line[j] == '|') return 1;

    /* Extract the first word and test it against the code-exec commands. */
    size_t wend = j;
    while (wend < len && line[wend] != ' ' && line[wend] != '\t' &&
           line[wend] != '\r' && line[wend] != '\n')
        wend++;
    if (word_is_code_exec(line + j, wend - j))
        return 1;

    /* -interpreter-exec <interp> "<cmd>" smuggles a console command; scan the
     * argument for any code-exec verb or shell metacharacter. */
    if (line[j] == '-' && len - j >= 17 &&
        memcmp(line + j, "-interpreter-exec", 17) == 0) {
        for (size_t k = j + 17; k < len; k++) {
            if (line[k] == '!' || line[k] == '|') return 1;
            for (size_t d = 0;
                 d < sizeof(gdb_code_exec_cmds) / sizeof(gdb_code_exec_cmds[0]); d++) {
                size_t n = strlen(gdb_code_exec_cmds[d]);
                if (k + n <= len && memcmp(line + k, gdb_code_exec_cmds[d], n) == 0)
                    return 1;
            }
        }
    }
    return 0;
}

static int contains_shell_command(const uint8_t *data, size_t len)
{
    size_t i = 0;
    while (i < len) {
        size_t eol = i;
        while (eol < len && data[eol] != '\n') eol++;
        if (line_invokes_shell(data + i, eol - i)) return 1;
        i = eol + 1;
    }
    return 0;
}

static int gdb_write_str(sm_link_t *self, const char *s)
{
    return self->write_data(self, (const uint8_t *)s, strlen(s));
}

static int gdb_open(sm_link_t *self)
{
    gdb_data_t *gd = self->data;

    /* Validate target_spec before forking — failing after the fork would
     * leak the child process and pipe fds (M14) */
    for (const char *p = gd->target_spec; *p; p++) {
        if (*p == '\n' || *p == '\r' || (*p > 0 && *p < 0x20)) {
            SM_LOG_ERROR(LOG_TAG, "target_spec contains control characters");
            return -1;
        }
    }

    int to_child[2];   /* parent writes to_child[1], child reads to_child[0] */
    int from_child[2]; /* child writes from_child[1], parent reads from_child[0] */

    if (pipe2(to_child, O_CLOEXEC) < 0) {
        SM_LOG_ERROR(LOG_TAG, "pipe (stdin): %s", strerror(errno));
        return -1;
    }
    if (pipe2(from_child, O_CLOEXEC) < 0) {
        SM_LOG_ERROR(LOG_TAG, "pipe (stdout): %s", strerror(errno));
        close(to_child[0]);
        close(to_child[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        SM_LOG_ERROR(LOG_TAG, "fork: %s", strerror(errno));
        close(to_child[0]);
        close(to_child[1]);
        close(from_child[0]);
        close(from_child[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child process */
        close(to_child[1]);
        close(from_child[0]);

        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        /* Merge stderr into stdout so GDB errors are visible */
        dup2(from_child[1], STDERR_FILENO);
        close(to_child[0]);
        close(from_child[1]);

        execvp(gd->gdb_path, (char *const[]){
            (char *)gd->gdb_path, "--interpreter=mi3", "--quiet", NULL
        });
        _exit(127);
    }

    /* Parent */
    close(to_child[0]);
    close(from_child[1]);

    gd->stdin_fd = to_child[1];
    gd->stdout_fd = from_child[0];
    gd->pid = pid;

    /* Non-blocking fds: stdout for epoll reads, stdin so writes get EAGAIN */
    int flags = fcntl(gd->stdout_fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(gd->stdout_fd, F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(gd->stdin_fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(gd->stdin_fd, F_SETFL, flags | O_NONBLOCK);

    SM_LOG_INFO(LOG_TAG, "started %s (pid %d)", gd->gdb_path, (int)pid);

    /* Stagger MI bootstrap without consuming GDB stdout (broker history and
     * tests must still see the banner). Fire-and-forget of both mi-async and
     * CLI "target remote" raced on real OpenOCD; short delays + MI
     * extended-remote are enough in practice and keep open() < ~300ms.
     * mi-async must be set BEFORE attach. */
    {
        struct pollfd pfd = { .fd = gd->stdout_fd, .events = POLLIN };
        (void)poll(&pfd, 1, 150); /* wait up to 150ms for GDB to start */
    }

    gdb_write_str(self, "-gdb-set mi-async on\n");
    {
        struct pollfd pfd = { .fd = gd->stdout_fd, .events = POLLIN };
        (void)poll(&pfd, 1, 80); /* let mi-async complete before attach */
    }

    if (gd->target_spec[0]) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "-target-select extended-remote %s\n", gd->target_spec);
        gdb_write_str(self, cmd);
        SM_LOG_INFO(LOG_TAG, "queued: -target-select extended-remote %s",
                    gd->target_spec);
    }

    return 0;
}

static void gdb_close(sm_link_t *self)
{
    gdb_data_t *gd = self->data;

    sm_link_wq_clear(&gd->wq);

    if (gd->stdin_fd >= 0) {
        gdb_write_str(self, "-gdb-exit\n");
        close(gd->stdin_fd);
        gd->stdin_fd = -1;
    }

    if (gd->pid > 0) {
        /* Wait up to 2 seconds for GDB to exit */
        int status;
        int waited = 0;

        while (waited < 20) {
            pid_t ret = waitpid(gd->pid, &status, WNOHANG);
            if (ret > 0 || (ret < 0 && errno == ECHILD))
                break;
            struct timespec ts = {0, 100000000L};  /* 100ms */
            nanosleep(&ts, NULL);
            waited++;
        }

        if (waited >= 20) {
            SM_LOG_WARN(LOG_TAG, "GDB pid %d not exiting, sending SIGKILL",
                        (int)gd->pid);
            kill(gd->pid, SIGKILL);
            waitpid(gd->pid, &status, 0);
        }

        gd->pid = -1;
    }

    if (gd->stdout_fd >= 0) {
        close(gd->stdout_fd);
        gd->stdout_fd = -1;
    }

    SM_LOG_INFO(LOG_TAG, "closed");
}

static int gdb_read_fd(sm_link_t *self)
{
    gdb_data_t *gd = self->data;
    return gd->stdout_fd;
}

static int gdb_write_fd_vt(sm_link_t *self)
{
    gdb_data_t *gd = self->data;
    return gd->stdin_fd;
}

static int gdb_has_write_pending(sm_link_t *self)
{
    gdb_data_t *gd = self->data;
    return sm_link_wq_has_pending(&gd->wq);
}

static int gdb_flush_write_queue(sm_link_t *self)
{
    gdb_data_t *gd = self->data;
    if (gd->stdin_fd < 0) return -1;
    return sm_link_wq_flush(gd->stdin_fd, &gd->wq);
}

static int gdb_write_data(sm_link_t *self, const uint8_t *data, size_t len)
{
    gdb_data_t *gd = self->data;
    if (gd->stdin_fd < 0) return -1;

    if (!gd->allow_shell && contains_shell_command(data, len)) {
        SM_LOG_WARN(LOG_TAG, "blocked shell-invoking GDB command "
                             "(set_param allow_shell=1 to permit)");
        return -1;
    }

    size_t written = 0;
    while (written < len) {
        ssize_t n = write(gd->stdin_fd, data + written, len - written);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (sm_link_wq_enqueue(&gd->wq, data + written,
                                       len - written) != 0)
                    return -1;
                return 0;
            }
            return -1;
        }
        if (n == 0)
            return -1;
        written += (size_t)n;
    }
    return 0;
}

static int gdb_send_break(sm_link_t *self, int duration_ms)
{
    (void)duration_ms;
    gdb_data_t *gd = self->data;
    if (gd->stdin_fd < 0) return -1;

    /* Only effective because gdb_open enables mi-async: in sync-mode MI gdb
     * does not read stdin while the target runs, so this command would sit
     * unprocessed in the pipe until the next stop (found live against
     * OpenOCD). Signalling SIGINT instead is NOT a substitute — under
     * mi-async gdb answers it with "Quit" at the MI prompt and never
     * interrupts the target (also found live). */
    return gdb_write_str(self, "-exec-interrupt\n");
}

static int gdb_set_param(sm_link_t *self, const char *key, const char *value)
{
    gdb_data_t *gd = self->data;
    if (gd->stdin_fd < 0) return -1;

    if (strcmp(key, "target") == 0) {
        /* Reject values with newlines or control chars to prevent MI injection */
        for (const char *p = value; *p; p++) {
            if (*p == '\n' || *p == '\r' || (*p > 0 && *p < 0x20))
                return -1;
        }
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "-target-select extended-remote %s\n", value);
        snprintf(gd->target_spec, sizeof(gd->target_spec), "%s", value);
        return gdb_write_str(self, cmd);
    }

    if (strcmp(key, "allow_shell") == 0) {
        gd->allow_shell = (strcmp(value, "1") == 0 ||
                           strcmp(value, "true") == 0);
        SM_LOG_WARN(LOG_TAG, "shell commands %s",
                    gd->allow_shell ? "PERMITTED" : "blocked");
        return 0;
    }

    return -1;
}

static int gdb_get_status(sm_link_t *self, cJSON *out)
{
    gdb_data_t *gd = self->data;
    cJSON_AddStringToObject(out, "link_type", "gdb");
    cJSON_AddStringToObject(out, "gdb_path", gd->gdb_path);
    if (gd->target_spec[0])
        cJSON_AddStringToObject(out, "target", gd->target_spec);
    cJSON_AddBoolToObject(out, "connected", gd->pid > 0);
    if (gd->pid > 0)
        cJSON_AddNumberToObject(out, "gdb_pid", gd->pid);
    return 0;
}

static void gdb_destroy(sm_link_t *self)
{
    gdb_data_t *gd = self->data;
    if (gd->stdin_fd >= 0 || gd->stdout_fd >= 0)
        gdb_close(self);
    free(gd);
    free(self);
}

sm_link_t *sm_gdb_new(const char *gdb_path, const char *target_spec)
{
    sm_link_t *link = calloc(1, sizeof(*link));
    gdb_data_t *gd = calloc(1, sizeof(*gd));
    if (!link || !gd) { free(link); free(gd); return NULL; }

    snprintf(gd->gdb_path, sizeof(gd->gdb_path), "%s",
             gdb_path ? gdb_path : "gdb");
    if (target_spec)
        snprintf(gd->target_spec, sizeof(gd->target_spec), "%s", target_spec);
    gd->pid = -1;
    gd->stdin_fd = -1;
    gd->stdout_fd = -1;

    link->name = "gdb";
    link->open = gdb_open;
    link->close = gdb_close;
    link->read_fd = gdb_read_fd;
    link->write_fd = gdb_write_fd_vt;
    link->write_data = gdb_write_data;
    link->has_write_pending = gdb_has_write_pending;
    link->flush_write_queue = gdb_flush_write_queue;
    link->send_break = gdb_send_break;
    link->set_param = gdb_set_param;
    link->get_status = gdb_get_status;
    link->destroy = gdb_destroy;
    link->silence_normal = 1;
    link->data = gd;

    return link;
}

/* Test-only constructor: create link with pre-assigned pipe fds.
 * pid=0 means close() skips waitpid. Not declared in gdb.h. */
sm_link_t *sm_gdb_new_test(int stdin_fd, int stdout_fd)
{
    sm_link_t *link = calloc(1, sizeof(*link));
    gdb_data_t *gd = calloc(1, sizeof(*gd));

    snprintf(gd->gdb_path, sizeof(gd->gdb_path), "gdb");
    gd->pid = 0;
    gd->stdin_fd = stdin_fd;
    gd->stdout_fd = stdout_fd;

    int flags = fcntl(gd->stdin_fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(gd->stdin_fd, F_SETFL, flags | O_NONBLOCK);

    link->name = "gdb";
    link->open = gdb_open;
    link->close = gdb_close;
    link->read_fd = gdb_read_fd;
    link->write_fd = gdb_write_fd_vt;
    link->write_data = gdb_write_data;
    link->has_write_pending = gdb_has_write_pending;
    link->flush_write_queue = gdb_flush_write_queue;
    link->send_break = gdb_send_break;
    link->set_param = gdb_set_param;
    link->get_status = gdb_get_status;
    link->destroy = gdb_destroy;
    link->silence_normal = 1;
    link->data = gd;

    return link;
}
