#include "test_main.h"
#include "protocol.h"
#include "util/base64.h"
#include "util/json_helpers.h"

static void test_encode_decode_hello(void)
{
    cJSON *msg = sm_msg_hello("test-client", "controller");
    size_t len;
    char *encoded = sm_msg_encode(msg, &len);
    ASSERT_NOT_NULL(encoded);
    ASSERT(len > 0, "encoded length > 0");

    sm_msg_t decoded = sm_msg_decode(encoded, len);
    ASSERT_INT_EQ(decoded.type, SM_MSG_HELLO);
    ASSERT_STR_EQ(sm_json_get_string(decoded.root, "name"), "test-client");
    ASSERT_STR_EQ(sm_json_get_string(decoded.root, "role"), "controller");

    sm_msg_free(&decoded);
    cJSON_Delete(msg);
    free(encoded);
}

static void test_encode_decode_welcome(void)
{
    cJSON *msg = sm_msg_welcome("0.1.0", "/dev/ttyUSB0", 115200, "controller");
    size_t len;
    char *encoded = sm_msg_encode(msg, &len);

    sm_msg_t decoded = sm_msg_decode(encoded, len);
    ASSERT_INT_EQ(decoded.type, SM_MSG_WELCOME);
    ASSERT_STR_EQ(sm_json_get_string(decoded.root, "broker_version"), "0.1.0");
    ASSERT_INT_EQ(sm_json_get_int(decoded.root, "baud", 0), 115200);

    sm_msg_free(&decoded);
    cJSON_Delete(msg);
    free(encoded);
}

static void test_encode_decode_output(void)
{
    const uint8_t data[] = "hello serial\r\n";
    cJSON *msg = sm_msg_output(data, sizeof(data) - 1, 1234567890.123);
    size_t len;
    char *encoded = sm_msg_encode(msg, &len);

    sm_msg_t decoded = sm_msg_decode(encoded, len);
    ASSERT_INT_EQ(decoded.type, SM_MSG_OUTPUT);

    /* Verify base64 data roundtrips */
    const char *b64 = sm_json_get_string(decoded.root, "data");
    ASSERT_NOT_NULL(b64);
    size_t dec_len;
    uint8_t *dec = sm_base64_decode(b64, strlen(b64), &dec_len);
    ASSERT_INT_EQ((int)dec_len, (int)(sizeof(data) - 1));
    ASSERT(memcmp(dec, data, dec_len) == 0, "data roundtrip");

    free(dec);
    sm_msg_free(&decoded);
    cJSON_Delete(msg);
    free(encoded);
}

static void test_encode_decode_send(void)
{
    const uint8_t data[] = "ls\n";
    cJSON *msg = sm_msg_send("req-001", data, 3);
    size_t len;
    char *encoded = sm_msg_encode(msg, &len);

    sm_msg_t decoded = sm_msg_decode(encoded, len);
    ASSERT_INT_EQ(decoded.type, SM_MSG_SEND);
    ASSERT_STR_EQ(sm_json_get_string(decoded.root, "id"), "req-001");

    sm_msg_free(&decoded);
    cJSON_Delete(msg);
    free(encoded);
}

static void test_encode_decode_send_expect(void)
{
    const uint8_t data[] = "uname\n";
    cJSON *msg = sm_msg_send_expect("req-002", data, 6, "Linux", 5000);
    size_t len;
    char *encoded = sm_msg_encode(msg, &len);

    sm_msg_t decoded = sm_msg_decode(encoded, len);
    ASSERT_INT_EQ(decoded.type, SM_MSG_SEND_EXPECT);
    ASSERT_STR_EQ(sm_json_get_string(decoded.root, "pattern"), "Linux");
    ASSERT_INT_EQ(sm_json_get_int(decoded.root, "timeout_ms", 0), 5000);

    sm_msg_free(&decoded);
    cJSON_Delete(msg);
    free(encoded);
}

static void test_encode_decode_expect_result(void)
{
    const uint8_t data[] = "Linux 5.15\r\n$ ";
    cJSON *msg = sm_msg_expect_result("req-002", 1, data, sizeof(data) - 1, "\\$\\s*$");
    size_t len;
    char *encoded = sm_msg_encode(msg, &len);

    sm_msg_t decoded = sm_msg_decode(encoded, len);
    ASSERT_INT_EQ(decoded.type, SM_MSG_EXPECT_RESULT);
    ASSERT_INT_EQ(sm_json_get_bool(decoded.root, "matched", 0), 1);

    sm_msg_free(&decoded);
    cJSON_Delete(msg);
    free(encoded);
}

static void test_encode_decode_error(void)
{
    cJSON *msg = sm_msg_error("req-003", "not authorized");
    size_t len;
    char *encoded = sm_msg_encode(msg, &len);

    sm_msg_t decoded = sm_msg_decode(encoded, len);
    ASSERT_INT_EQ(decoded.type, SM_MSG_ERROR);
    ASSERT_STR_EQ(sm_json_get_string(decoded.root, "message"), "not authorized");

    sm_msg_free(&decoded);
    cJSON_Delete(msg);
    free(encoded);
}

static void test_encode_decode_takeover_release(void)
{
    cJSON *msg = sm_msg_takeover("req-004");
    size_t len;
    char *encoded = sm_msg_encode(msg, &len);
    sm_msg_t decoded = sm_msg_decode(encoded, len);
    ASSERT_INT_EQ(decoded.type, SM_MSG_TAKEOVER);
    sm_msg_free(&decoded);
    cJSON_Delete(msg);
    free(encoded);

    msg = sm_msg_release("req-005");
    encoded = sm_msg_encode(msg, &len);
    decoded = sm_msg_decode(encoded, len);
    ASSERT_INT_EQ(decoded.type, SM_MSG_RELEASE);
    sm_msg_free(&decoded);
    cJSON_Delete(msg);
    free(encoded);
}

static void test_encode_decode_status(void)
{
    cJSON *msg = sm_msg_status_response("req-006", "/dev/ttyUSB0", 9600, 1, 0);
    size_t len;
    char *encoded = sm_msg_encode(msg, &len);

    sm_msg_t decoded = sm_msg_decode(encoded, len);
    ASSERT_INT_EQ(decoded.type, SM_MSG_STATUS_RESPONSE);
    ASSERT_INT_EQ(sm_json_get_int(decoded.root, "baud", 0), 9600);
    ASSERT_INT_EQ(sm_json_get_bool(decoded.root, "connected", 0), 1);
    ASSERT_INT_EQ(sm_json_get_bool(decoded.root, "suspended", 1), 0);

    sm_msg_free(&decoded);
    cJSON_Delete(msg);
    free(encoded);
}

static void test_encode_decode_suspend_resume(void)
{
    cJSON *msg = sm_msg_suspended("/dev/ttyUSB0", "test-client");
    size_t len;
    char *encoded = sm_msg_encode(msg, &len);
    sm_msg_t decoded = sm_msg_decode(encoded, len);
    ASSERT_INT_EQ(decoded.type, SM_MSG_SUSPENDED);
    sm_msg_free(&decoded);
    cJSON_Delete(msg);
    free(encoded);

    msg = sm_msg_resumed("/dev/ttyUSB0");
    encoded = sm_msg_encode(msg, &len);
    decoded = sm_msg_decode(encoded, len);
    ASSERT_INT_EQ(decoded.type, SM_MSG_RESUMED);
    sm_msg_free(&decoded);
    cJSON_Delete(msg);
    free(encoded);
}

static void test_encode_decode_history(void)
{
    cJSON *chunks = cJSON_CreateArray();
    cJSON *chunk = cJSON_CreateObject();
    cJSON_AddStringToObject(chunk, "data", "aGVsbG8=");
    cJSON_AddNumberToObject(chunk, "timestamp", 1234567890.0);
    cJSON_AddItemToArray(chunks, chunk);

    cJSON *msg = sm_msg_history_response("req-007", chunks);
    size_t len;
    char *encoded = sm_msg_encode(msg, &len);

    sm_msg_t decoded = sm_msg_decode(encoded, len);
    ASSERT_INT_EQ(decoded.type, SM_MSG_HISTORY_RESPONSE);

    sm_msg_free(&decoded);
    cJSON_Delete(msg);
    free(encoded);
}

static void test_encode_decode_anomaly(void)
{
    cJSON *msg = sm_msg_anomaly("inc-001", "kernel_panic", "critical",
                                 1234567890.0, "Kernel panic - not syncing",
                                 "previous output...");
    size_t len;
    char *encoded = sm_msg_encode(msg, &len);

    sm_msg_t decoded = sm_msg_decode(encoded, len);
    ASSERT_INT_EQ(decoded.type, SM_MSG_ANOMALY);
    ASSERT_STR_EQ(sm_json_get_string(decoded.root, "pattern_name"), "kernel_panic");
    ASSERT_STR_EQ(sm_json_get_string(decoded.root, "severity"), "critical");

    sm_msg_free(&decoded);
    cJSON_Delete(msg);
    free(encoded);
}

static void test_type_name(void)
{
    ASSERT_STR_EQ(sm_msg_type_name(SM_MSG_HELLO), "hello");
    ASSERT_STR_EQ(sm_msg_type_name(SM_MSG_OUTPUT), "output");
    ASSERT_STR_EQ(sm_msg_type_name(SM_MSG_UNKNOWN), "unknown");
}

static void test_decode_invalid(void)
{
    sm_msg_t msg = sm_msg_decode("not json", 8);
    ASSERT_NULL(msg.root);

    msg = sm_msg_decode("{\"type\":\"bogus\"}", 16);
    ASSERT_INT_EQ(msg.type, SM_MSG_UNKNOWN);
    sm_msg_free(&msg);
}

int main(void)
{
    printf("test_protocol\n");

    RUN_TEST(test_encode_decode_hello);
    RUN_TEST(test_encode_decode_welcome);
    RUN_TEST(test_encode_decode_output);
    RUN_TEST(test_encode_decode_send);
    RUN_TEST(test_encode_decode_send_expect);
    RUN_TEST(test_encode_decode_expect_result);
    RUN_TEST(test_encode_decode_error);
    RUN_TEST(test_encode_decode_takeover_release);
    RUN_TEST(test_encode_decode_status);
    RUN_TEST(test_encode_decode_suspend_resume);
    RUN_TEST(test_encode_decode_history);
    RUN_TEST(test_encode_decode_anomaly);
    RUN_TEST(test_type_name);
    RUN_TEST(test_decode_invalid);

    TEST_REPORT();
}
