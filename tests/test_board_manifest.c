#include "test_main.h"
#include "board_manifest.h"
#include "constants.h"
#include "util/sock_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_valid_manifest(void)
{
    const char *json =
        "{"
        "\"board\":\"samc21-bench\","
        "\"wires\":["
        "  {\"role\":\"console\",\"link\":\"uart\",\"device\":\"/dev/ttyACM1\","
        "   \"baud\":115200,\"profile\":\"configs/x.smolmux-profile.json\"},"
        "  {\"role\":\"swd\",\"link\":\"gdb\",\"gdb_path\":\"gdb-multiarch\","
        "   \"target\":\"localhost:3333\"}"
        "]}";

    sm_board_manifest_t m;
    ASSERT_INT_EQ(sm_board_manifest_from_json(json, &m), 0);
    ASSERT_STR_EQ(m.board, "samc21-bench");
    ASSERT_INT_EQ((int)m.wire_count, 2);

    ASSERT_STR_EQ(m.wires[0].role, "console");
    ASSERT_STR_EQ(m.wires[0].link, "uart");
    ASSERT_STR_EQ(m.wires[0].device, "/dev/ttyACM1");
    ASSERT_INT_EQ(m.wires[0].baud, 115200);
    ASSERT_STR_EQ(m.wires[0].profile, "configs/x.smolmux-profile.json");

    ASSERT_STR_EQ(m.wires[1].role, "swd");
    ASSERT_STR_EQ(m.wires[1].link, "gdb");
    ASSERT_STR_EQ(m.wires[1].gdb_path, "gdb-multiarch");
    ASSERT_STR_EQ(m.wires[1].target, "localhost:3333");
}

static void test_defaults(void)
{
    /* baud defaults to SM_DEFAULT_BAUD; gdb_path defaults to "gdb". */
    const char *json =
        "{\"board\":\"b\",\"wires\":["
        "{\"role\":\"c\",\"link\":\"uart\",\"device\":\"/dev/x\"},"
        "{\"role\":\"d\",\"link\":\"gdb\",\"target\":\"h:1\"}]}";
    sm_board_manifest_t m;
    ASSERT_INT_EQ(sm_board_manifest_from_json(json, &m), 0);
    ASSERT_INT_EQ(m.wires[0].baud, SM_DEFAULT_BAUD);
    ASSERT_STR_EQ(m.wires[1].gdb_path, "gdb");
}

static void test_rejects(void)
{
    sm_board_manifest_t m;
    /* missing board */
    ASSERT_INT_EQ(sm_board_manifest_from_json(
        "{\"wires\":[{\"role\":\"c\",\"link\":\"uart\",\"device\":\"/d\"}]}", &m), -1);
    /* no wires */
    ASSERT_INT_EQ(sm_board_manifest_from_json("{\"board\":\"b\",\"wires\":[]}", &m), -1);
    /* wire missing role */
    ASSERT_INT_EQ(sm_board_manifest_from_json(
        "{\"board\":\"b\",\"wires\":[{\"link\":\"uart\",\"device\":\"/d\"}]}", &m), -1);
    /* bad link */
    ASSERT_INT_EQ(sm_board_manifest_from_json(
        "{\"board\":\"b\",\"wires\":[{\"role\":\"c\",\"link\":\"spi\"}]}", &m), -1);
    /* uart without device */
    ASSERT_INT_EQ(sm_board_manifest_from_json(
        "{\"board\":\"b\",\"wires\":[{\"role\":\"c\",\"link\":\"uart\"}]}", &m), -1);
    /* not JSON */
    ASSERT_INT_EQ(sm_board_manifest_from_json("{nope", &m), -1);
}

static void test_socket_derivation(void)
{
    sm_board_manifest_t m;
    memset(&m, 0, sizeof(m));
    snprintf(m.board, sizeof(m.board), "myboard");

    sm_board_wire_t w;
    memset(&w, 0, sizeof(w));
    snprintf(w.role, sizeof(w.role), "console");

    setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1);
    char sock[SM_SOCK_PATH_MAX];
    ASSERT_INT_EQ(sm_board_wire_socket(&m, &w, sock, sizeof(sock)), 0);
    ASSERT_STR_EQ(sock, "/run/user/1000/smolmux-myboard-console.sock");

    /* explicit socket override wins */
    snprintf(w.socket, sizeof(w.socket), "/tmp/custom.sock");
    ASSERT_INT_EQ(sm_board_wire_socket(&m, &w, sock, sizeof(sock)), 0);
    ASSERT_STR_EQ(sock, "/tmp/custom.sock");

    /* Long board names shorten to a stable path that fits AF_UNIX bind. */
    w.socket[0] = '\0';
    memset(m.board, 'B', sizeof(m.board) - 1);
    m.board[sizeof(m.board) - 1] = '\0';
    setenv("XDG_RUNTIME_DIR", "/some/deep/runtime/directory/path", 1);
    ASSERT_INT_EQ(sm_board_wire_socket(&m, &w, sock, sizeof(sock)), 0);
    ASSERT(strlen(sock) <= SM_SOCK_FINAL_MAX, "long board socket fits final max");
    ASSERT(strstr(sock, ".sock") != NULL, "looks like a socket path");
    unsetenv("XDG_RUNTIME_DIR");
}

static void test_load_from_file(void)
{
    const char *tmp = getenv("TMPDIR");
    char path[512];
    snprintf(path, sizeof(path), "%s/smolmux-test.board.json",
             tmp && tmp[0] ? tmp : "/tmp");
    FILE *fp = fopen(path, "w");
    ASSERT_NOT_NULL(fp);
    if (fp) {
        fputs("{\"board\":\"fromfile\",\"wires\":["
              "{\"role\":\"c\",\"link\":\"uart\",\"device\":\"/dev/y\"}]}", fp);
        fclose(fp);
    }
    sm_board_manifest_t m;
    ASSERT_INT_EQ(sm_board_manifest_load(path, &m), 0);
    ASSERT_STR_EQ(m.board, "fromfile");
    ASSERT_INT_EQ((int)m.wire_count, 1);
    unlink(path);

    ASSERT_INT_EQ(sm_board_manifest_load("/nonexistent/x.board.json", &m), -1);
}

/* Every shipped example manifest must parse (guards configs/ against schema
 * drift and typos in new board examples). */
static void test_shipped_manifests_parse(void)
{
    const char *names[] = {
        "esp32-uart.board.json",
        "newboard.board.json",
        "samc21.board.json",
        "esp32-s3-touch-lcd-1.28.board.json",
        NULL
    };
    for (int i = 0; names[i]; i++) {
        char path[256];
        snprintf(path, sizeof(path), "configs/%s", names[i]);
        if (access(path, R_OK) != 0)
            snprintf(path, sizeof(path), "../configs/%s", names[i]);
        ASSERT(access(path, R_OK) == 0, "shipped manifest found");

        sm_board_manifest_t m;
        ASSERT_INT_EQ(sm_board_manifest_load(path, &m), 0);
        ASSERT(m.board[0], "manifest names its board");
        ASSERT(m.wire_count >= 1, "manifest has at least one wire");
    }
}

int main(void)
{
    printf("test_board_manifest\n");
    RUN_TEST(test_valid_manifest);
    RUN_TEST(test_shipped_manifests_parse);
    RUN_TEST(test_defaults);
    RUN_TEST(test_rejects);
    RUN_TEST(test_socket_derivation);
    RUN_TEST(test_load_from_file);
    TEST_REPORT();
}
