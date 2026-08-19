/*
 * End-to-end test for smolmux-gdb-mcp: a real broker holding a GDB link (the
 * fake_gdb helper stands in for gdb-multiarch) runs in-process, the actual
 * smolmux-gdb-mcp binary is spawned with stdio pipes, and MCP JSON-RPC is
 * driven through it. Exercises the full path: MCP tool call -> token-prefixed
 * MI command -> broker -> fake gdb -> output stream -> MI parse -> tool text.
 *
 * argv[1] = path to smolmux-gdb-mcp, argv[2] = path to fake_gdb.
 */
#include "test_main.h"
#include "broker.h"
#include "links/gdb.h"
#include "protocol.h"
#include "util/json_helpers.h"

#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

#define TEST_SOCK "/tmp/smolmux-test-gdbmcp.sock"
#define STARTUP_DELAY 150000

static const char *g_gdbmcp_bin;
static const char *g_fake_gdb;

/* --- Broker fixture (GDB link) --- */

static void *broker_thread(void *arg)
{
    sm_broker_run((sm_broker_t *)arg);
    return NULL;
}

typedef struct fixture {
    sm_broker_t broker;
    sm_link_t *link;
    pthread_t tid;

    pid_t pid;
    int in;          /* -> gdb-mcp stdin */
    int out;         /* <- gdb-mcp stdout */
    char buf[65536];
    size_t len;
} fixture_t;

static void spawn_gdbmcp(fixture_t *fx)
{
    int to_child[2], from_child[2];
    (void)!pipe(to_child);
    (void)!pipe(from_child);

    fx->pid = fork();
    if (fx->pid == 0) {
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        const char *tmp = getenv("TMPDIR");
        setenv("HOME", tmp && tmp[0] ? tmp : "/tmp", 1);
        unsetenv("SMOLMUX_GDB_PROFILE");
        unsetenv("SMOLMUX_SOCKET");
        execl(g_gdbmcp_bin, g_gdbmcp_bin, "-s", TEST_SOCK, "-n", "gdbmcp-e2e", NULL);
        _exit(127);
    }
    close(to_child[0]);
    close(from_child[1]);
    fx->in = to_child[1];
    fx->out = from_child[0];
    int flags = fcntl(fx->out, F_GETFL, 0);
    fcntl(fx->out, F_SETFL, flags | O_NONBLOCK);
}

static void setup(fixture_t *fx)
{
    memset(fx, 0, sizeof(*fx));
    fx->link = sm_gdb_new(g_fake_gdb, "localhost:3333");
    sm_broker_init(&fx->broker, fx->link, TEST_SOCK);
    snprintf(fx->broker.port, sizeof(fx->broker.port), "fake-gdb");
    pthread_create(&fx->tid, NULL, broker_thread, &fx->broker);
    usleep(STARTUP_DELAY);

    spawn_gdbmcp(fx);
    usleep(STARTUP_DELAY);
}

static void teardown(fixture_t *fx)
{
    close(fx->in);
    int status = -1;
    for (int i = 0; i < 200; i++) {
        if (waitpid(fx->pid, &status, WNOHANG) == fx->pid) break;
        usleep(10000);
    }
    if (status == -1) { kill(fx->pid, SIGKILL); waitpid(fx->pid, &status, 0); }
    ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "smolmux-gdb-mcp exited cleanly on stdin EOF");
    close(fx->out);

    sm_broker_stop(&fx->broker);
    pthread_join(fx->tid, NULL);
    sm_broker_destroy(&fx->broker);
}

/* --- JSON-RPC over the pipe --- */

static void rpc_send(fixture_t *fx, const char *json)
{
    (void)!write(fx->in, json, strlen(json));
    (void)!write(fx->in, "\n", 1);
}

static char *next_line(fixture_t *fx, int attempts)
{
    static char line[32768];
    for (int i = 0; i < attempts; i++) {
        char *nl = memchr(fx->buf, '\n', fx->len);
        if (nl) {
            size_t full = (size_t)(nl - fx->buf);
            size_t n = full >= sizeof(line) ? sizeof(line) - 1 : full;
            memcpy(line, fx->buf, n);
            line[n] = '\0';
            memmove(fx->buf, nl + 1, fx->len - full - 1);
            fx->len -= full + 1;
            return line;
        }
        ssize_t r = read(fx->out, fx->buf + fx->len, sizeof(fx->buf) - fx->len);
        if (r > 0) fx->len += (size_t)r;
        else usleep(10000);
    }
    return NULL;
}

static cJSON *rpc_call(fixture_t *fx, int id, const char *json, int attempts)
{
    rpc_send(fx, json);
    for (int i = 0; i < 50; i++) {
        char *l = next_line(fx, attempts);
        if (!l) return NULL;
        cJSON *resp = cJSON_Parse(l);
        if (!resp) continue;
        cJSON *rid = cJSON_GetObjectItem(resp, "id");
        if (cJSON_IsNumber(rid) && (int)rid->valuedouble == id)
            return resp;
        cJSON_Delete(resp);
    }
    return NULL;
}

static const char *tool_text(cJSON *resp)
{
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    cJSON *content = cJSON_GetObjectItem(result, "content");
    cJSON *item = cJSON_GetArrayItem(content, 0);
    return item ? sm_json_get_string(item, "text") : NULL;
}

/* --- Test --- */

static void test_gdb_mcp_e2e(void)
{
    fixture_t fx;
    setup(&fx);

    /* initialize */
    cJSON *resp = rpc_call(&fx, 1,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}", 200);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        cJSON *info = cJSON_GetObjectItem(cJSON_GetObjectItem(resp, "result"), "serverInfo");
        ASSERT_STR_EQ(sm_json_get_string(info, "name"), "smolmux-gdb-mcp");
        cJSON_Delete(resp);
    }

    /* tools/list — gdb tools */
    resp = rpc_call(&fx, 2, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}", 500);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        cJSON *tools = cJSON_GetObjectItem(cJSON_GetObjectItem(resp, "result"), "tools");
        /* Base GDB tools + gdb_interrupt (HW dogfooding) +
         * gdb_identify_target + gdb_generate_profile (unknown-board probing). */
        ASSERT_INT_EQ(cJSON_GetArraySize(tools), 21);
        int found_fault = 0, found_interrupt = 0, found_identify = 0, found_gen = 0;
        cJSON *t;
        cJSON_ArrayForEach(t, tools) {
            const char *nm = sm_json_get_string(t, "name");
            if (strcmp(nm, "gdb_read_fault_registers") == 0) found_fault = 1;
            if (strcmp(nm, "gdb_interrupt") == 0) found_interrupt = 1;
            if (strcmp(nm, "gdb_identify_target") == 0) found_identify = 1;
            if (strcmp(nm, "gdb_generate_profile") == 0) found_gen = 1;
        }
        ASSERT(found_fault, "gdb_read_fault_registers advertised");
        ASSERT(found_interrupt, "gdb_interrupt advertised");
        ASSERT(found_identify, "gdb_identify_target advertised");
        ASSERT(found_gen, "gdb_generate_profile advertised");
        cJSON_Delete(resp);
    }

    /* gdb_backtrace: MCP -> MI -> fake gdb -> parsed frames */
    resp = rpc_call(&fx, 3,
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":"
        "{\"name\":\"gdb_backtrace\",\"arguments\":{}}}", 500);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        const char *text = tool_text(resp);
        ASSERT(text && strstr(text, "#0") && strstr(text, "main") &&
               strstr(text, "main.c:42"), "backtrace formatted from MI");
        cJSON_Delete(resp);
    }

    /* gdb_evaluate: token-correlated request/response */
    resp = rpc_call(&fx, 4,
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":"
        "{\"name\":\"gdb_evaluate\",\"arguments\":{\"expression\":\"1+1\"}}}", 500);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        ASSERT_STR_EQ(tool_text(resp), "42");
        cJSON_Delete(resp);
    }

    /* gdb_read_registers: name->index mapping then value read */
    resp = rpc_call(&fx, 5,
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":"
        "{\"name\":\"gdb_read_registers\",\"arguments\":{}}}", 500);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        const char *text = tool_text(resp);
        ASSERT(text && strstr(text, "0xff"), "register values returned");
        cJSON_Delete(resp);
    }

    /* names as JSON array (schema type) */
    resp = rpc_call(&fx, 51,
        "{\"jsonrpc\":\"2.0\",\"id\":51,\"method\":\"tools/call\",\"params\":"
        "{\"name\":\"gdb_read_registers\",\"arguments\":"
        "{\"names\":[\"pc\",\"sp\"]}}}", 500);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        const char *text = tool_text(resp);
        ASSERT(text && strstr(text, "0x28c") && strstr(text, "pc"),
               "names array filters to pc");
        ASSERT(text && !strstr(text, "0xff"),
               "names array does not dump unrequested r1");
        cJSON_Delete(resp);
    }

    /* names as CSV string (legacy / agent-friendly) */
    resp = rpc_call(&fx, 52,
        "{\"jsonrpc\":\"2.0\",\"id\":52,\"method\":\"tools/call\",\"params\":"
        "{\"name\":\"gdb_read_registers\",\"arguments\":"
        "{\"names\":\"pc,sp\"}}}", 500);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        const char *text = tool_text(resp);
        ASSERT(text && strstr(text, "0x28c"), "CSV names filter works");
        cJSON_Delete(resp);
    }

    /* gdb_read_fault_registers: memory read + CFSR bit decode */
    resp = rpc_call(&fx, 6,
        "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\",\"params\":"
        "{\"name\":\"gdb_read_fault_registers\",\"arguments\":{}}}", 500);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        const char *text = tool_text(resp);
        ASSERT(text && strstr(text, "CFSR") && strstr(text, "DIVBYZERO"),
               "fault registers decoded (CFSR bit 25)");
        cJSON_Delete(resp);
    }

    /* gdb_command: raw MI passthrough */
    resp = rpc_call(&fx, 7,
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":"
        "{\"name\":\"gdb_command\",\"arguments\":{\"command\":\"-break-list\"}}}", 500);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        const char *text = tool_text(resp);
        ASSERT(text && strstr(text, "^done"), "raw MI command returned result");
        cJSON_Delete(resp);
    }

    /* gdb_command must refuse a host-code-execution command up front */
    resp = rpc_call(&fx, 8,
        "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\",\"params\":"
        "{\"name\":\"gdb_command\",\"arguments\":{\"command\":\"shell rm -rf /\"}}}", 500);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        const char *text = tool_text(resp);
        ASSERT(text && strstr(text, "blocked"), "shell command blocked client-side");
        cJSON_Delete(resp);
    }

    /* gdb_interrupt: halt round trip (fake gdb answers ^done + *stopped) */
    resp = rpc_call(&fx, 9,
        "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":"
        "{\"name\":\"gdb_interrupt\",\"arguments\":{}}}", 500);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        const char *text = tool_text(resp);
        ASSERT(text && strstr(text, "Interrupt requested"), "interrupt acknowledged");
        cJSON_Delete(resp);
    }

    /* gdb_identify_target: CPUID decode + vendor probe. fake_gdb answers CPUID
     * 0x410CC601 (M0+ r0p1), SAM DSU DID 0x11010405, and faults STM32/nRF regs. */
    resp = rpc_call(&fx, 16,
        "{\"jsonrpc\":\"2.0\",\"id\":16,\"method\":\"tools/call\",\"params\":"
        "{\"name\":\"gdb_identify_target\",\"arguments\":{}}}", 500);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        const char *text = tool_text(resp);
        ASSERT_NOT_NULL(text);
        cJSON *idj = text ? cJSON_Parse(text) : NULL;
        ASSERT_NOT_NULL(idj);   /* the tool returns structured JSON */
        if (idj) {
            ASSERT_STR_EQ(sm_json_get_string(idj, "core"), "Cortex-M0+");
            ASSERT_STR_EQ(sm_json_get_string(idj, "revision"), "r0p1");
            const char *vendor = sm_json_get_string(idj, "vendor_guess");
            ASSERT(vendor && strstr(vendor, "SAM"), "SAM vendor guessed from DSU DID");
            ASSERT_STR_EQ(sm_json_get_string(idj, "cpuid"), "0x410cc601");
            cJSON_Delete(idj);
        }
        cJSON_Delete(resp);
    }

    /* gdb_generate_profile (no path): returns a starter profile JSON. The fake
     * is a Cortex-M0+ (baseline) -> fault_registers must be empty, name derived
     * from the SAM vendor guess, arch "arm". */
    resp = rpc_call(&fx, 19,
        "{\"jsonrpc\":\"2.0\",\"id\":19,\"method\":\"tools/call\",\"params\":"
        "{\"name\":\"gdb_generate_profile\",\"arguments\":{}}}", 500);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        const char *text = tool_text(resp);
        ASSERT_NOT_NULL(text);
        cJSON *prof = text ? cJSON_Parse(text) : NULL;
        ASSERT_NOT_NULL(prof);   /* the tool returns a valid profile JSON */
        if (prof) {
            ASSERT_STR_EQ(sm_json_get_string(prof, "arch"), "arm");
            const char *nm = sm_json_get_string(prof, "name");
            ASSERT(nm && strstr(nm, "sam") && strstr(nm, "cortex-m0plus"),
                   "profile name derived from vendor + core");
            cJSON *fr = cJSON_GetObjectItem(prof, "fault_registers");
            ASSERT(cJSON_IsArray(fr) && cJSON_GetArraySize(fr) == 0,
                   "M0+ baseline profile has no fault registers");
            cJSON *regs = cJSON_GetObjectItem(prof, "important_registers");
            ASSERT(cJSON_IsArray(regs) && cJSON_GetArraySize(regs) > 0,
                   "profile has an important_registers set");
            /* peripheral_map: architectural blocks + the SAM vendor block, but
             * no mainline-only blocks on a baseline M0+. */
            cJSON *pm = cJSON_GetObjectItem(prof, "peripheral_map");
            ASSERT(cJSON_IsObject(pm), "peripheral_map is an object");
            ASSERT_NOT_NULL(cJSON_GetObjectItem(pm, "SCB"));
            ASSERT_NOT_NULL(cJSON_GetObjectItem(pm, "NVIC"));
            ASSERT_NOT_NULL(cJSON_GetObjectItem(pm, "DSU"));   /* SAM vendor block */
            ASSERT(cJSON_GetObjectItem(pm, "MPU") == NULL &&
                   cJSON_GetObjectItem(pm, "DWT") == NULL,
                   "no mainline-only peripherals on baseline M0+");
            cJSON_Delete(prof);
        }
        cJSON_Delete(resp);
    }

    /* gdb_generate_profile (with path): writes the file; the parent reads it
     * back and confirms it is a valid, loadable profile. */
    const char *tmp = getenv("TMPDIR");
    char gen_path[256];
    snprintf(gen_path, sizeof(gen_path), "%s/smolmux-gen.gdb-profile.json",
             tmp && tmp[0] ? tmp : "/tmp");
    unlink(gen_path);
    char gen_req[512];
    snprintf(gen_req, sizeof(gen_req),
        "{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"tools/call\",\"params\":"
        "{\"name\":\"gdb_generate_profile\",\"arguments\":"
        "{\"path\":\"%s\",\"rtos\":\"zephyr\"}}}",
        gen_path);
    resp = rpc_call(&fx, 20, gen_req, 500);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        const char *text = tool_text(resp);
        ASSERT(text && strstr(text, "Saved"), "write acknowledged");
        cJSON_Delete(resp);

        FILE *gf = fopen(gen_path, "rb");
        ASSERT_NOT_NULL(gf);
        if (gf) {
            char fbuf[8192];
            size_t rd = fread(fbuf, 1, sizeof(fbuf) - 1, gf);
            fclose(gf);
            fbuf[rd] = '\0';
            cJSON *disk = cJSON_Parse(fbuf);
            ASSERT_NOT_NULL(disk);   /* the written file is valid JSON */
            if (disk) {
                ASSERT_STR_EQ(sm_json_get_string(disk, "arch"), "arm");
                ASSERT_NOT_NULL(cJSON_GetObjectItem(disk, "important_registers"));
                ASSERT_STR_EQ(sm_json_get_string(disk, "rtos"), "zephyr");
                cJSON *rc = cJSON_GetObjectItem(disk, "rtos_commands");
                ASSERT(cJSON_IsArray(rc) && cJSON_GetArraySize(rc) > 0,
                       "rtos arg stamped rtos_commands");
                cJSON_Delete(disk);
            }
        }
        unlink(gen_path);
    }

    /* resources/list — target-profile + board-probing runbook resources. */
    resp = rpc_call(&fx, 10,
        "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"resources/list\"}", 500);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        cJSON *rs = cJSON_GetObjectItem(cJSON_GetObjectItem(resp, "result"), "resources");
        ASSERT_INT_EQ(cJSON_GetArraySize(rs), 2);
        int found_profile = 0, found_probing = 0;
        cJSON *r;
        cJSON_ArrayForEach(r, rs) {
            const char *uri = sm_json_get_string(r, "uri");
            if (uri && strcmp(uri, "smolmux-gdb://target/profile") == 0) found_profile = 1;
            if (uri && strcmp(uri, "smolmux-gdb://board-probing") == 0) found_probing = 1;
        }
        ASSERT(found_profile && found_probing, "both resources advertised");
        cJSON_Delete(resp);
    }

    /* resources/read — profile JSON round-trips (has the "arch" field). */
    resp = rpc_call(&fx, 11,
        "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"resources/read\",\"params\":"
        "{\"uri\":\"smolmux-gdb://target/profile\"}}", 500);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        cJSON *contents = cJSON_GetObjectItem(cJSON_GetObjectItem(resp, "result"), "contents");
        cJSON *c0 = cJSON_GetArrayItem(contents, 0);
        const char *text = sm_json_get_string(c0, "text");
        ASSERT_NOT_NULL(text);
        cJSON *prof = text ? cJSON_Parse(text) : NULL;
        ASSERT_NOT_NULL(prof);   /* the resource body is valid JSON */
        if (prof) {
            ASSERT_NOT_NULL(cJSON_GetObjectItem(prof, "arch"));
            cJSON_Delete(prof);
        }
        cJSON_Delete(resp);
    }

    /* resources/read — unknown uri is an error. */
    resp = rpc_call(&fx, 12,
        "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"resources/read\",\"params\":"
        "{\"uri\":\"smolmux-gdb://nope\"}}", 500);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        ASSERT_NOT_NULL(cJSON_GetObjectItem(resp, "error"));
        cJSON_Delete(resp);
    }

    /* prompts/list — diagnose_fault + analyze_crash + probe_unknown_board. */
    resp = rpc_call(&fx, 13,
        "{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"prompts/list\"}", 500);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        cJSON *ps = cJSON_GetObjectItem(cJSON_GetObjectItem(resp, "result"), "prompts");
        ASSERT_INT_EQ(cJSON_GetArraySize(ps), 3);
        int found_df = 0, found_ac = 0, found_pb = 0;
        cJSON *p;
        cJSON_ArrayForEach(p, ps) {
            const char *nm = sm_json_get_string(p, "name");
            if (nm && strcmp(nm, "diagnose_fault") == 0) found_df = 1;
            if (nm && strcmp(nm, "analyze_crash") == 0) found_ac = 1;
            if (nm && strcmp(nm, "probe_unknown_board") == 0) found_pb = 1;
        }
        ASSERT(found_df && found_ac && found_pb, "all three prompts advertised");
        cJSON_Delete(resp);
    }

    /* prompts/get diagnose_fault — the symptom is interpolated into the guide. */
    resp = rpc_call(&fx, 14,
        "{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"prompts/get\",\"params\":"
        "{\"name\":\"diagnose_fault\",\"arguments\":{\"symptom\":\"stack overflow\"}}}", 500);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        cJSON *msgs = cJSON_GetObjectItem(cJSON_GetObjectItem(resp, "result"), "messages");
        cJSON *m0 = cJSON_GetArrayItem(msgs, 0);
        cJSON *content = cJSON_GetObjectItem(m0, "content");
        const char *text = sm_json_get_string(content, "text");
        ASSERT(text && strstr(text, "stack overflow"), "symptom interpolated");
        ASSERT(text && strstr(text, "gdb_backtrace"), "guide references the tools");
        cJSON_Delete(resp);
    }

    /* prompts/get probe_unknown_board — the probing playbook. */
    resp = rpc_call(&fx, 17,
        "{\"jsonrpc\":\"2.0\",\"id\":17,\"method\":\"prompts/get\",\"params\":"
        "{\"name\":\"probe_unknown_board\"}}", 500);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        cJSON *msgs = cJSON_GetObjectItem(cJSON_GetObjectItem(resp, "result"), "messages");
        cJSON *m0 = cJSON_GetArrayItem(msgs, 0);
        cJSON *content = cJSON_GetObjectItem(m0, "content");
        const char *text = sm_json_get_string(content, "text");
        ASSERT(text && strstr(text, "gdb_identify_target"),
               "playbook references the identify tool");
        ASSERT(text && strstr(text, "CPUID"), "playbook mentions CPUID");
        cJSON_Delete(resp);
    }

    /* resources/read board-probing — the runbook markdown. */
    resp = rpc_call(&fx, 18,
        "{\"jsonrpc\":\"2.0\",\"id\":18,\"method\":\"resources/read\",\"params\":"
        "{\"uri\":\"smolmux-gdb://board-probing\"}}", 500);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        cJSON *contents = cJSON_GetObjectItem(cJSON_GetObjectItem(resp, "result"), "contents");
        cJSON *c0 = cJSON_GetArrayItem(contents, 0);
        ASSERT_STR_EQ(sm_json_get_string(c0, "mimeType"), "text/markdown");
        const char *text = sm_json_get_string(c0, "text");
        ASSERT(text && strstr(text, "gdb_identify_target"), "runbook body served");
        cJSON_Delete(resp);
    }

    /* prompts/get unknown — error. */
    resp = rpc_call(&fx, 15,
        "{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"prompts/get\",\"params\":"
        "{\"name\":\"nope\"}}", 500);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        ASSERT_NOT_NULL(cJSON_GetObjectItem(resp, "error"));
        cJSON_Delete(resp);
    }

    teardown(&fx);
}

int main(int argc, char *argv[])
{
    printf("test_gdb_mcp\n");
    signal(SIGPIPE, SIG_IGN);
    unlink(TEST_SOCK);

    if (argc < 3) {
        fprintf(stderr, "usage: %s <smolmux-gdb-mcp> <fake_gdb>\n", argv[0]);
        return 1;
    }
    g_gdbmcp_bin = argv[1];
    g_fake_gdb = argv[2];

    RUN_TEST(test_gdb_mcp_e2e);
    TEST_REPORT();
}
