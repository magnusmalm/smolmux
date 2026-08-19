#include "test_main.h"
#include "util/base64.h"

static void test_encode_basic(void)
{
    char *enc = sm_base64_encode((const uint8_t *)"hello", 5);
    ASSERT_STR_EQ(enc, "aGVsbG8=");
    free(enc);
}

static void test_decode_basic(void)
{
    size_t len;
    uint8_t *dec = sm_base64_decode("aGVsbG8=", 8, &len);
    ASSERT_INT_EQ((int)len, 5);
    ASSERT(memcmp(dec, "hello", 5) == 0, "decoded matches");
    free(dec);
}

static void test_roundtrip(void)
{
    const char *input = "The quick brown fox jumps over the lazy dog";
    size_t input_len = strlen(input);

    char *enc = sm_base64_encode((const uint8_t *)input, input_len);
    ASSERT_NOT_NULL(enc);

    size_t dec_len;
    uint8_t *dec = sm_base64_decode(enc, strlen(enc), &dec_len);
    ASSERT_INT_EQ((int)dec_len, (int)input_len);
    ASSERT(memcmp(dec, input, input_len) == 0, "roundtrip matches");

    free(enc);
    free(dec);
}

static void test_empty(void)
{
    char *enc = sm_base64_encode((const uint8_t *)"", 0);
    ASSERT_STR_EQ(enc, "");
    free(enc);

    size_t len;
    uint8_t *dec = sm_base64_decode("", 0, &len);
    ASSERT_INT_EQ((int)len, 0);
    free(dec);
}

static void test_binary(void)
{
    uint8_t data[] = {0x00, 0x01, 0xFF, 0xFE, 0x80, 0x7F};
    char *enc = sm_base64_encode(data, sizeof(data));
    ASSERT_NOT_NULL(enc);

    size_t dec_len;
    uint8_t *dec = sm_base64_decode(enc, strlen(enc), &dec_len);
    ASSERT_INT_EQ((int)dec_len, (int)sizeof(data));
    ASSERT(memcmp(dec, data, sizeof(data)) == 0, "binary roundtrip");

    free(enc);
    free(dec);
}

static void test_padding(void)
{
    /* 1 byte → 2 chars of padding */
    char *enc1 = sm_base64_encode((const uint8_t *)"a", 1);
    ASSERT_STR_EQ(enc1, "YQ==");
    free(enc1);

    /* 2 bytes → 1 char of padding */
    char *enc2 = sm_base64_encode((const uint8_t *)"ab", 2);
    ASSERT_STR_EQ(enc2, "YWI=");
    free(enc2);

    /* 3 bytes → no padding */
    char *enc3 = sm_base64_encode((const uint8_t *)"abc", 3);
    ASSERT_STR_EQ(enc3, "YWJj");
    free(enc3);
}

int main(void)
{
    printf("test_base64\n");

    RUN_TEST(test_encode_basic);
    RUN_TEST(test_decode_basic);
    RUN_TEST(test_roundtrip);
    RUN_TEST(test_empty);
    RUN_TEST(test_binary);
    RUN_TEST(test_padding);

    TEST_REPORT();
}
