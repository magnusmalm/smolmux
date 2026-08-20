#include "test_main.h"
#include "io_log.h"

#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

/* Scratch path under $TMPDIR. The suite must never write to bare /tmp. */
static void scratch_path(char *out, size_t out_len, const char *name)
{
    const char *tmp = getenv("TMPDIR");
    snprintf(out, out_len, "%s/%s", tmp && tmp[0] ? tmp : ".", name);
}

static void test_create_and_write(void)
{
    const char *tmp = getenv("TMPDIR");
    char path[512];
    snprintf(path, sizeof(path), "%s/smolmux-test-io.jsonl",
             tmp && tmp[0] ? tmp : "/tmp");
    unlink(path);

    sm_io_log_t *log = sm_io_log_open(path);
    ASSERT_NOT_NULL(log);

    sm_io_log_output(log, (const uint8_t *)"hello\n", 6, 1234567890.123);
    sm_io_log_input(log, (const uint8_t *)"cmd\n", 4, "test-client", 1234567891.0);
    sm_io_log_incident(log, "inc-001", "kernel_panic", "critical", 1234567892.0);

    sm_io_log_close(log);

    /* Verify file content */
    FILE *fp = fopen(path, "r");
    ASSERT_NOT_NULL(fp);
    if (fp) {
        char line[1024];
        /* Line 1: output */
        ASSERT_NOT_NULL(fgets(line, sizeof(line), fp));
        ASSERT(strstr(line, "\"type\":\"output\"") != NULL, "output line");
        ASSERT(strstr(line, "\"timestamp\":") != NULL, "has timestamp");
        ASSERT(strstr(line, "\"data\":") != NULL, "has data");

        /* Line 2: input */
        ASSERT_NOT_NULL(fgets(line, sizeof(line), fp));
        ASSERT(strstr(line, "\"type\":\"input\"") != NULL, "input line");
        ASSERT(strstr(line, "\"sender\":\"test-client\"") != NULL, "has sender");

        /* Line 3: incident */
        ASSERT_NOT_NULL(fgets(line, sizeof(line), fp));
        ASSERT(strstr(line, "\"type\":\"incident\"") != NULL, "incident line");
        ASSERT(strstr(line, "\"pattern_name\":\"kernel_panic\"") != NULL, "has pattern");

        fclose(fp);
    }
    unlink(path);
}

/* The I/O log holds console credentials, so it must never be world-readable.
 * Pre-fix it was created by fopen(path,"a") => 0666 & ~umask, i.e. 0644 under
 * the usual umask 022, in a directory that defaulted to /tmp. */
static void test_log_created_private(void)
{
    char path[512];
    scratch_path(path, sizeof(path), "smolmux-test-io-mode.jsonl");
    unlink(path);

    mode_t old = umask(022);   /* the umask that produced 0644 pre-fix */
    sm_io_log_t *log = sm_io_log_open(path);
    umask(old);
    ASSERT_NOT_NULL(log);

    struct stat st;
    ASSERT_INT_EQ(stat(path, &st), 0);
    ASSERT_INT_EQ((int)(st.st_mode & 07777), 0600);

    sm_io_log_close(log);
    unlink(path);
}

/* An existing log from an older build can already be world-readable. Opening
 * it must tighten it rather than append more console traffic to it. */
static void test_existing_loose_log_tightened(void)
{
    char path[512];
    scratch_path(path, sizeof(path), "smolmux-test-io-loose.jsonl");
    unlink(path);

    FILE *seed = fopen(path, "w");
    ASSERT_NOT_NULL(seed);
    fclose(seed);
    ASSERT_INT_EQ(chmod(path, 0644), 0);

    sm_io_log_t *log = sm_io_log_open(path);
    ASSERT_NOT_NULL(log);

    struct stat st;
    ASSERT_INT_EQ(stat(path, &st), 0);
    ASSERT_INT_EQ((int)(st.st_mode & 07777), 0600);

    sm_io_log_close(log);
    unlink(path);
}

/* A symlink planted at the log path let anyone who could write the log
 * directory redirect console output into a file of their choosing. The open
 * must fail closed, and the target must be untouched. */
static void test_symlinked_path_refused(void)
{
    char link_path[512], victim[512];
    scratch_path(link_path, sizeof(link_path), "smolmux-test-io-link.jsonl");
    scratch_path(victim, sizeof(victim), "smolmux-test-io-victim.txt");
    unlink(link_path);
    unlink(victim);

    FILE *v = fopen(victim, "w");
    ASSERT_NOT_NULL(v);
    fputs("ORIGINAL\n", v);
    fclose(v);
    ASSERT_INT_EQ(symlink(victim, link_path), 0);

    sm_io_log_t *log = sm_io_log_open(link_path);
    ASSERT(log == NULL, "refuses to open a symlinked log path");

    /* Nothing was appended through the link. */
    struct stat st;
    ASSERT_INT_EQ(stat(victim, &st), 0);
    ASSERT_INT_EQ((int)st.st_size, 9);

    /* And the link itself was not replaced by a regular file. */
    struct stat lst;
    ASSERT_INT_EQ(lstat(link_path, &lst), 0);
    ASSERT(S_ISLNK(lst.st_mode), "symlink left in place, not clobbered");

    unlink(link_path);
    unlink(victim);
}

/* A hardlink farmed onto the log path aims at the same redirection as the
 * symlink, past O_NOFOLLOW. The link count check catches it. */
static void test_hardlinked_path_refused(void)
{
    char link_path[512], victim[512];
    scratch_path(link_path, sizeof(link_path), "smolmux-test-io-hard.jsonl");
    scratch_path(victim, sizeof(victim), "smolmux-test-io-hardtarget.txt");
    unlink(link_path);
    unlink(victim);

    FILE *v = fopen(victim, "w");
    ASSERT_NOT_NULL(v);
    fputs("ORIGINAL\n", v);
    fclose(v);
    ASSERT_INT_EQ(link(victim, link_path), 0);

    sm_io_log_t *log = sm_io_log_open(link_path);
    ASSERT(log == NULL, "refuses a log path with extra hard links");

    struct stat st;
    ASSERT_INT_EQ(stat(victim, &st), 0);
    ASSERT_INT_EQ((int)st.st_size, 9);

    unlink(link_path);
    unlink(victim);
}

/* Missing directories are created, and our own leaf directory is private. */
static void test_parent_dir_created_private(void)
{
    char dir[256], path[512];
    scratch_path(dir, sizeof(dir), "smolmux-test-io-newdir");
    snprintf(path, sizeof(path), "%s/smolmux-ttyTEST-io.jsonl", dir);
    unlink(path);
    rmdir(dir);

    sm_io_log_t *log = sm_io_log_open(path);
    ASSERT_NOT_NULL(log);

    struct stat st;
    ASSERT_INT_EQ(stat(dir, &st), 0);
    ASSERT_INT_EQ((int)(st.st_mode & 07777), 0700);

    sm_io_log_close(log);
    unlink(path);
    rmdir(dir);
}

static void test_null_log(void)
{
    /* Should not crash with NULL log */
    sm_io_log_output(NULL, (const uint8_t *)"test", 4, 0.0);
    sm_io_log_input(NULL, (const uint8_t *)"test", 4, "x", 0.0);
    sm_io_log_incident(NULL, "id", "pat", "sev", 0.0);
    sm_io_log_close(NULL);
    ASSERT(1, "null operations don't crash");
}

int main(void)
{
    printf("test_io_log\n");

    RUN_TEST(test_create_and_write);
    RUN_TEST(test_log_created_private);
    RUN_TEST(test_existing_loose_log_tightened);
    RUN_TEST(test_symlinked_path_refused);
    RUN_TEST(test_hardlinked_path_refused);
    RUN_TEST(test_parent_dir_created_private);
    RUN_TEST(test_null_log);

    TEST_REPORT();
}
