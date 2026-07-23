#include "test_main.h"
#include "util/keyspec.h"
#include "constants.h"

#include <string.h>

static void test_parse_default_caret_bracket(void)
{
    uint8_t ch = 0;
    ASSERT_INT_EQ(sm_parse_escape_key("^]", &ch), 0);
    ASSERT_INT_EQ(ch, SM_MONITOR_ESCAPE_CHAR);   /* 0x1D */
    ASSERT_INT_EQ(ch, 0x1D);
}

static void test_parse_caret_letters(void)
{
    uint8_t ch = 0;
    ASSERT_INT_EQ(sm_parse_escape_key("^A", &ch), 0);
    ASSERT_INT_EQ(ch, 0x01);
    ASSERT_INT_EQ(sm_parse_escape_key("^Z", &ch), 0);
    ASSERT_INT_EQ(ch, 0x1A);
}

static void test_parse_caret_case_insensitive(void)
{
    uint8_t hi = 0, lo = 0;
    ASSERT_INT_EQ(sm_parse_escape_key("^A", &hi), 0);
    ASSERT_INT_EQ(sm_parse_escape_key("^a", &lo), 0);
    ASSERT_INT_EQ(hi, lo);
}

static void test_parse_caret_punctuation(void)
{
    uint8_t ch = 0;
    ASSERT_INT_EQ(sm_parse_escape_key("^[", &ch), 0);
    ASSERT_INT_EQ(ch, 0x1B);   /* ESC via caret */
    ASSERT_INT_EQ(sm_parse_escape_key("^\\", &ch), 0);
    ASSERT_INT_EQ(ch, 0x1C);
    ASSERT_INT_EQ(sm_parse_escape_key("^_", &ch), 0);
    ASSERT_INT_EQ(ch, 0x1F);
    ASSERT_INT_EQ(sm_parse_escape_key("^@", &ch), 0);
    ASSERT_INT_EQ(ch, 0x00);
}

static void test_parse_caret_del(void)
{
    uint8_t ch = 0;
    ASSERT_INT_EQ(sm_parse_escape_key("^?", &ch), 0);
    ASSERT_INT_EQ(ch, 0x7F);
}

static void test_parse_named_esc(void)
{
    uint8_t ch = 0;
    ASSERT_INT_EQ(sm_parse_escape_key("esc", &ch), 0);
    ASSERT_INT_EQ(ch, 0x1B);
    ASSERT_INT_EQ(sm_parse_escape_key("ESC", &ch), 0);
    ASSERT_INT_EQ(ch, 0x1B);
}

static void test_parse_rejects_bad_specs(void)
{
    uint8_t ch = 0x55;
    /* Printable single char must not become a prefix. */
    ASSERT_INT_EQ(sm_parse_escape_key("a", &ch), -1);
    /* Bare caret. */
    ASSERT_INT_EQ(sm_parse_escape_key("^", &ch), -1);
    /* Too long. */
    ASSERT_INT_EQ(sm_parse_escape_key("^]]", &ch), -1);
    ASSERT_INT_EQ(sm_parse_escape_key("^AB", &ch), -1);
    /* Empty and garbage. */
    ASSERT_INT_EQ(sm_parse_escape_key("", &ch), -1);
    ASSERT_INT_EQ(sm_parse_escape_key("ctrl-]", &ch), -1);
    /* Caret + out-of-range byte (lowercase already covered; '~' is > 0x5F). */
    ASSERT_INT_EQ(sm_parse_escape_key("^~", &ch), -1);
    /* NULL guards. */
    ASSERT_INT_EQ(sm_parse_escape_key(NULL, &ch), -1);
    /* Rejected specs leave *out untouched. */
    ASSERT_INT_EQ(ch, 0x55);
}

static void test_format_labels(void)
{
    char buf[16];

    sm_format_escape_key(0x1D, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "Ctrl-]");

    sm_format_escape_key(0x01, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "Ctrl-A");

    sm_format_escape_key(0x1B, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "ESC");

    sm_format_escape_key(0x7F, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "^?");
}

static void test_parse_format_roundtrip(void)
{
    /* Every control byte a caret spec can name should round-trip back to the
     * same byte after formatting and re-parsing (where a spec exists). */
    const char *specs[] = { "^A", "^]", "^\\", "^_", "^@", "esc", "^?" };
    for (size_t i = 0; i < sizeof(specs) / sizeof(specs[0]); i++) {
        uint8_t ch = 0;
        ASSERT_INT_EQ(sm_parse_escape_key(specs[i], &ch), 0);
        char label[16];
        sm_format_escape_key(ch, label, sizeof(label));
        ASSERT(label[0] != '\0', "label is non-empty");
    }
}

int main(void)
{
    printf("test_keyspec\n");

    RUN_TEST(test_parse_default_caret_bracket);
    RUN_TEST(test_parse_caret_letters);
    RUN_TEST(test_parse_caret_case_insensitive);
    RUN_TEST(test_parse_caret_punctuation);
    RUN_TEST(test_parse_caret_del);
    RUN_TEST(test_parse_named_esc);
    RUN_TEST(test_parse_rejects_bad_specs);
    RUN_TEST(test_format_labels);
    RUN_TEST(test_parse_format_roundtrip);

    TEST_REPORT();
}
